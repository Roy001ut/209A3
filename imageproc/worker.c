#include "worker.h"
#include "protocol.h"
#include "ppm.h"
#include "enhance.h"
#include "tile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* --------------------------------------------------------------------------
 * Robust pipe I/O helpers
 * -------------------------------------------------------------------------- */
static ssize_t read_all(int fd, void *buf, size_t count)
{
    size_t   done = 0;
    uint8_t *p    = (uint8_t *)buf;
    while (done < count) {
        ssize_t n = read(fd, p + done, count - done);
        if (n < 0) { perror("worker read_all"); return (ssize_t)done; }
        if (n == 0) break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

static ssize_t write_all(int fd, const void *buf, size_t count)
{
    size_t         done = 0;
    const uint8_t *p    = (const uint8_t *)buf;
    while (done < count) {
        ssize_t n = write(fd, p + done, count - done);
        if (n < 0) { perror("worker write_all"); return (ssize_t)done; }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

/* --------------------------------------------------------------------------
 * send_resp — write a resp_t to resp_fd
 * -------------------------------------------------------------------------- */
static void send_resp(int resp_fd, uint8_t type, uint8_t status)
{
    resp_t r;
    memset(&r, 0, sizeof(r));
    r.type   = type;
    r.status = status;
    write_all(resp_fd, &r, sizeof(resp_t));
}

/* --------------------------------------------------------------------------
 * fork_tiles — fork tile_count tile subprocesses for one operation.
 *
 * operation : TILE_OP_HISTEQ or TILE_OP_SHARPEN
 * param     : blend ratio (histeq) or amount (sharpen)
 * pixels    : full image pixel array (RGB, width*height*3 bytes)
 * mask      : full label array (width*height bytes)
 * cluster_id, W, H, infile : image metadata
 * eq_lut    : pre-built LUT (used only for TILE_OP_HISTEQ)
 * tile_count: number of tiles to fork
 *
 * On success, stitches processed strips back into pixels[] and returns
 * the mean output_variance across tiles (quality metric).
 * Returns -1.0f on fatal error.
 * -------------------------------------------------------------------------- */
static float fork_tiles(uint8_t operation, float param,
                        uint8_t *pixels, const uint8_t *mask,
                        uint8_t cluster_id, uint32_t W, uint32_t H,
                        const char *infile, const uint8_t *eq_lut,
                        uint32_t tile_count)
{
    pid_t tile_pids[8]        = {0};
    int   tile_job_w[8]       = {-1,-1,-1,-1,-1,-1,-1,-1};
    int   tile_result_r[8]    = {-1,-1,-1,-1,-1,-1,-1,-1};

    /* ---- fork all tiles ---- */
    for (uint32_t t = 0; t < tile_count; t++) {
        uint32_t row_start = t * (H / tile_count);
        uint32_t row_end   = (t == tile_count - 1) ? H : (t + 1) * (H / tile_count);

        tile_job_t tjob;
        memset(&tjob, 0, sizeof(tjob));
        tjob.operation  = operation;
        tjob.cluster_id = cluster_id;
        tjob.row_start  = row_start;
        tjob.row_end    = row_end;
        tjob.img_width  = W;
        tjob.img_height = H;
        tjob.param      = param;
        if (eq_lut) memcpy(tjob.eq_lut, eq_lut, 256);
        strncpy(tjob.infile, infile, MAX_PATH - 1);
        snprintf(tjob.tmp_outfile, MAX_PATH,
                 "/tmp/imageproc_%d_c%d_op%d_t%u.ppm",
                 getpid(), (int)cluster_id, (int)operation, t);

        int jp[2], rp[2];
        if (pipe(jp) != 0 || pipe(rp) != 0) {
            perror("worker: pipe for tile");
            return -1.0f;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("worker: fork tile");
            close(jp[0]); close(jp[1]);
            close(rp[0]); close(rp[1]);
            return -1.0f;
        }

        if (pid == 0) {
            /* child */
            close(jp[1]);
            close(rp[0]);
            for (uint32_t tt = 0; tt < t; tt++) {
                if (tile_job_w[tt]    >= 0) close(tile_job_w[tt]);
                if (tile_result_r[tt] >= 0) close(tile_result_r[tt]);
            }
            run_tile(jp[0], rp[1]);
            /* never returns */
        }

        close(jp[0]);
        close(rp[1]);
        tile_pids[t]     = pid;
        tile_job_w[t]    = jp[1];
        tile_result_r[t] = rp[0];
    }

    /* ---- send tile_job_t + mask slice to each tile ---- */
    for (uint32_t t = 0; t < tile_count; t++) {
        uint32_t row_start  = t * (H / tile_count);
        uint32_t row_end    = (t == tile_count - 1) ? H : (t + 1) * (H / tile_count);
        uint32_t slice_bytes = (row_end - row_start) * W;

        /* Rebuild tjob to get correct tmp_outfile */
        tile_job_t tjob;
        memset(&tjob, 0, sizeof(tjob));
        tjob.operation  = operation;
        tjob.cluster_id = cluster_id;
        tjob.row_start  = row_start;
        tjob.row_end    = row_end;
        tjob.img_width  = W;
        tjob.img_height = H;
        tjob.param      = param;
        if (eq_lut) memcpy(tjob.eq_lut, eq_lut, 256);
        strncpy(tjob.infile, infile, MAX_PATH - 1);
        snprintf(tjob.tmp_outfile, MAX_PATH,
                 "/tmp/imageproc_%d_c%d_op%d_t%u.ppm",
                 getpid(), (int)cluster_id, (int)operation, t);

        printf("[Worker %d] -> [Tile %d]: tile_job_t + mask slice\n", cluster_id, t);
        fflush(stdout);
        write_all(tile_job_w[t], &tjob, sizeof(tile_job_t));
        write_all(tile_job_w[t], mask + row_start * W, slice_bytes);
        close(tile_job_w[t]);
        tile_job_w[t] = -1;
    }

    /* ---- collect tile_result_t ---- */
    tile_result_t tresults[8];
    memset(tresults, 0, sizeof(tresults));

    for (uint32_t t = 0; t < tile_count; t++) {
        ssize_t nr = read_all(tile_result_r[t],
                              &tresults[t], sizeof(tile_result_t));
        close(tile_result_r[t]);
        tile_result_r[t] = -1;
        if (nr != (ssize_t)sizeof(tile_result_t)) {
            fprintf(stderr, "worker: tile %u: short result (got %zd)\n", t, nr);
            tresults[t].status = 1;
        }
    }

    /* ---- stitch strips back into pixels[] ---- */
    printf("[Worker %d] stitch; waitpid()\n", cluster_id);
    fflush(stdout);
    for (uint32_t t = 0; t < tile_count; t++) {
        if (tresults[t].status != 0) continue;

        uint32_t row_start = t * (H / tile_count);
        uint32_t row_end   = (t == tile_count - 1) ? H : (t + 1) * (H / tile_count);
        uint32_t strip_h   = row_end - row_start;

        ppm_t *strip = ppm_read_strip(tresults[t].tmp_outfile, W, 0, strip_h);
        if (!strip) {
            fprintf(stderr, "worker: cannot read strip %s\n",
                    tresults[t].tmp_outfile);
            continue;
        }

        /* Copy only owned pixels back into the full pixel buffer */
        for (uint32_t row = 0; row < strip_h; row++) {
            uint32_t img_row = row_start + row;
            for (uint32_t x = 0; x < W; x++) {
                uint32_t img_idx   = img_row * W + x;
                uint32_t strip_idx = row    * W + x;
                if (mask[img_idx] == cluster_id) {
                    pixels[img_idx*3 + 0] = strip->data[strip_idx*3 + 0];
                    pixels[img_idx*3 + 1] = strip->data[strip_idx*3 + 1];
                    pixels[img_idx*3 + 2] = strip->data[strip_idx*3 + 2];
                }
            }
        }
        ppm_free(strip);
        unlink(tresults[t].tmp_outfile);
    }

    /* ---- waitpid ---- */
    for (uint32_t t = 0; t < tile_count; t++) {
        if (tile_pids[t] > 0)
            waitpid(tile_pids[t], NULL, 0);
    }

    /* ---- compute mean output_variance ---- */
    float total_var = 0.0f;
    uint32_t good   = 0;
    for (uint32_t t = 0; t < tile_count; t++) {
        if (tresults[t].status == 0) {
            total_var += tresults[t].output_variance;
            good++;
        }
    }
    return (good > 0) ? total_var / good : 0.0f;
}

/* --------------------------------------------------------------------------
 * run_worker — persistent command loop
 * -------------------------------------------------------------------------- */
int run_worker(int cmd_fd, int resp_fd)
{
    /* Persistent state across commands */
    uint8_t *pixels     = NULL;   /* full image RGB, W*H*3 bytes */
    uint8_t *mask       = NULL;   /* cluster label per pixel, W*H bytes */
    uint8_t  cluster_id = 0;
    uint32_t W          = 0;
    uint32_t H          = 0;
    char     infile[MAX_PATH] = {0};

    for (;;) {
        cmd_t cmd;
        ssize_t nr = read_all(cmd_fd, &cmd, sizeof(cmd_t));
        if (nr == 0) {
            /* EOF — parent died */
            fprintf(stderr, "worker: EOF on cmd_fd, parent died\n");
            free(pixels); free(mask);
            exit(1);
        }
        if (nr != (ssize_t)sizeof(cmd_t)) {
            fprintf(stderr, "worker: short read cmd_t (got %zd)\n", nr);
            free(pixels); free(mask);
            exit(1);
        }

        switch (cmd.type) {

        /* ---------------------------------------------------------------- */
        case CMD_LOAD: {
            /* Free any previous state */
            free(pixels); pixels = NULL;
            free(mask);   mask   = NULL;

            cluster_id = cmd.cluster_id;
            W          = cmd.img_width;
            H          = cmd.img_height;
            strncpy(infile, cmd.infile, MAX_PATH - 1);
            infile[MAX_PATH - 1] = '\0';

            uint32_t mask_bytes = W * H;
            mask = malloc(mask_bytes);
            if (!mask) {
                perror("worker: malloc mask");
                send_resp(resp_fd, RESP_ERROR, 1);
                break;
            }

            nr = read_all(cmd_fd, mask, mask_bytes);
            if (nr != (ssize_t)mask_bytes) {
                fprintf(stderr, "worker: short mask read (got %zd, want %u)\n",
                        nr, mask_bytes);
                free(mask); mask = NULL;
                send_resp(resp_fd, RESP_ERROR, 1);
                break;
            }

            ppm_t *img = ppm_read(infile);
            if (!img) {
                fprintf(stderr, "worker: cannot read %s\n", infile);
                free(mask); mask = NULL;
                send_resp(resp_fd, RESP_ERROR, 1);
                break;
            }

            pixels = malloc((size_t)W * H * 3);
            if (!pixels) {
                perror("worker: malloc pixels");
                ppm_free(img);
                free(mask); mask = NULL;
                send_resp(resp_fd, RESP_ERROR, 1);
                break;
            }
            memcpy(pixels, img->data, (size_t)W * H * 3);
            ppm_free(img);

            printf("[Worker %d] -> [Parent]: RESP_READY\n", cluster_id);
            fflush(stdout);
            send_resp(resp_fd, RESP_READY, 0);
            break;
        }

        /* ---------------------------------------------------------------- */
        case CMD_ANALYZE: {
            resp_t r;
            memset(&r, 0, sizeof(r));
            r.type = RESP_STATS;

            if (!pixels || !mask) {
                r.status = 1;
                write_all(resp_fd, &r, sizeof(resp_t));
                break;
            }

            uint32_t n_pixels = W * H;
            uint32_t count    = 0;
            double   sum_lum  = 0.0, sum_lum2 = 0.0;

            for (uint32_t i = 0; i < n_pixels; i++) {
                if (mask[i] != cluster_id) continue;
                double lv = 0.299 * pixels[i*3+0]
                          + 0.587 * pixels[i*3+1]
                          + 0.114 * pixels[i*3+2];
                sum_lum  += lv;
                sum_lum2 += lv * lv;
                count++;
            }

            if (count > 0) {
                double mean = sum_lum / count;
                double var  = sum_lum2 / count - mean * mean;
                if (var < 0.0) var = 0.0;
                r.lum_mean    = (float)mean;
                r.lum_stddev  = (float)sqrt(var);
                r.pixel_count = count;
            }
            printf("[Worker %d] -> [Parent]: RESP_STATS (lum_mean=%.1f, lum_stddev=%.1f)\n", cluster_id, r.lum_mean, r.lum_stddev);
            fflush(stdout);
            write_all(resp_fd, &r, sizeof(resp_t));
            break;
        }

        /* ---------------------------------------------------------------- */
        case CMD_BRIGHTEN: {
            if (!pixels || !mask) {
                send_resp(resp_fd, RESP_DONE, 1);
                break;
            }
            float factor = cmd.param;
            uint32_t n_pixels = W * H;
            for (uint32_t i = 0; i < n_pixels; i++) {
                if (mask[i] != cluster_id) continue;
                for (int c = 0; c < 3; c++) {
                    int v = (int)(pixels[i*3+c] * factor + 0.5f);
                    pixels[i*3+c] = (v > 255) ? 255 : (uint8_t)v;
                }
            }
            printf("[Worker %d] -> [Parent]: RESP_DONE\n", cluster_id);
            fflush(stdout);
            send_resp(resp_fd, RESP_DONE, 0);
            break;
        }

        /* ---------------------------------------------------------------- */
        case CMD_EQUALIZE: {
            if (!pixels || !mask) {
                send_resp(resp_fd, RESP_DONE, 1);
                break;
            }

            /* Build global LUT from in-memory pixels */
            uint8_t eq_lut[256];
            histeq_build_lut(pixels, W * H, mask, cluster_id, eq_lut);
            printf("[Worker %d] build eq_lut[256]; fork tiles\n", cluster_id);
            fflush(stdout);

            float quality = fork_tiles(TILE_OP_HISTEQ, cmd.param,
                                       pixels, mask, cluster_id, W, H,
                                       infile, eq_lut, 2);

            resp_t r;
            memset(&r, 0, sizeof(r));
            r.type    = RESP_DONE;
            r.status  = (quality < 0.0f) ? 1 : 0;
            r.quality = (quality >= 0.0f) ? quality : 0.0f;
            printf("[Worker %d] -> [Parent]: RESP_DONE (quality=%.2f)\n", cluster_id, r.quality);
            fflush(stdout);
            write_all(resp_fd, &r, sizeof(resp_t));
            break;
        }

        /* ---------------------------------------------------------------- */
        case CMD_SHARPEN: {
            if (!pixels || !mask) {
                send_resp(resp_fd, RESP_DONE, 1);
                break;
            }
            printf("[Worker %d] fork tiles\n", cluster_id);
            fflush(stdout);

            float quality = fork_tiles(TILE_OP_SHARPEN, cmd.param,
                                       pixels, mask, cluster_id, W, H,
                                       infile, NULL, 2);

            resp_t r;
            memset(&r, 0, sizeof(r));
            r.type    = RESP_DONE;
            r.status  = (quality < 0.0f) ? 1 : 0;
            r.quality = (quality >= 0.0f) ? quality : 0.0f;
            write_all(resp_fd, &r, sizeof(resp_t));
            break;
        }

        /* ---------------------------------------------------------------- */
        case CMD_SKIP:
            printf("[Worker %d] -> [Parent]: RESP_DONE\n", cluster_id);
            fflush(stdout);
            send_resp(resp_fd, RESP_DONE, 0);
            break;

        /* ---------------------------------------------------------------- */
        case CMD_SAVE: {
            if (!pixels) {
                send_resp(resp_fd, RESP_DONE, 1);
                break;
            }
            /* Write only owned pixels into a full-size PPM.
             * Non-owned pixels are left as zero (black). */
            ppm_t *out = ppm_alloc(W, H);
            if (!out) {
                perror("worker: ppm_alloc for save");
                send_resp(resp_fd, RESP_DONE, 1);
                break;
            }
            uint32_t n_pixels = W * H;
            for (uint32_t i = 0; i < n_pixels; i++) {
                if (mask[i] == cluster_id) {
                    out->data[i*3+0] = pixels[i*3+0];
                    out->data[i*3+1] = pixels[i*3+1];
                    out->data[i*3+2] = pixels[i*3+2];
                }
            }
            printf("[Worker %d] write pixel buf\n", cluster_id);
            fflush(stdout);
            int rc = ppm_write(out, cmd.outfile);
            ppm_free(out);
            printf("[Worker %d] -> [Parent]: RESP_DONE\n", cluster_id);
            fflush(stdout);
            send_resp(resp_fd, RESP_DONE, rc != 0 ? 1 : 0);
            break;
        }

        /* ---------------------------------------------------------------- */
        case CMD_TERMINATE:
            printf("[Worker %d] free(); exit(0)\n", cluster_id);
            fflush(stdout);
            free(pixels);
            free(mask);
            close(cmd_fd);
            close(resp_fd);
            exit(0);

        /* ---------------------------------------------------------------- */
        default:
            fprintf(stderr, "worker: unknown cmd type %u\n",
                    (unsigned)cmd.type);
            send_resp(resp_fd, RESP_ERROR, 1);
            break;
        }
    }

    /* unreachable */
    return 0;
}
