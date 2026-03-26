#include "worker.h"
#include "protocol.h"
#include "ppm.h"
#include "enhance.h"
#include "tile.h"

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
 * Compute average luminance over owned pixels
 * -------------------------------------------------------------------------- */
static float avg_luminance(const ppm_t *img, const uint8_t *mask,
                            uint8_t cluster_id)
{
    uint32_t n = img->width * img->height;
    double   sum   = 0.0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (mask[i] != cluster_id) continue;
        double lv = 0.299 * img->data[i*3]
                  + 0.587 * img->data[i*3+1]
                  + 0.114 * img->data[i*3+2];
        sum += lv;
        count++;
    }
    return (count > 0) ? (float)(sum / count) : 0.0f;
}

/* --------------------------------------------------------------------------
 * run_worker
 * -------------------------------------------------------------------------- */
int run_worker(int job_fd, int result_fd)
{
    layer_result_t lresult;
    memset(&lresult, 0, sizeof(lresult));

    /* ------------------------------------------------------------------ */
    /* 1. Read layer_job_t                                                 */
    /* ------------------------------------------------------------------ */
    layer_job_t job;
    ssize_t nr = read_all(job_fd, &job, sizeof(layer_job_t));
    if (nr != (ssize_t)sizeof(layer_job_t)) {
        fprintf(stderr, "worker: failed to read layer_job_t (got %zd)\n", nr);
        lresult.status = 2;
        goto send_result;
    }

    /* ------------------------------------------------------------------ */
    /* 2. Read full mask                                                   */
    /* ------------------------------------------------------------------ */
    uint32_t mask_bytes = job.img_width * job.img_height;
    uint8_t *mask = malloc(mask_bytes);
    if (!mask) {
        perror("worker: malloc mask");
        lresult.status = 2;
        goto send_result;
    }

    nr = read_all(job_fd, mask, mask_bytes);
    if (nr != (ssize_t)mask_bytes) {
        fprintf(stderr, "worker: failed to read mask (got %zd, want %u)\n",
                nr, mask_bytes);
        free(mask);
        lresult.status = 2;
        goto send_result;
    }
    close(job_fd);
    job_fd = -1;

    /* ------------------------------------------------------------------ */
    /* 3. Compute avg_lum_before over owned pixels in original image       */
    /* ------------------------------------------------------------------ */
    ppm_t *orig = ppm_read(job.infile);
    if (!orig) {
        fprintf(stderr, "worker: cannot read input %s\n", job.infile);
        free(mask);
        lresult.status = 2;
        goto send_result;
    }
    float avg_lum_before = avg_luminance(orig, mask, job.cluster_id);
    ppm_free(orig);

    /* ------------------------------------------------------------------ */
    /* 4. Divide rows into tile_count equal strips                        */
    /* ------------------------------------------------------------------ */
    uint32_t tc         = job.tile_count;
    uint32_t H          = job.img_height;
    uint32_t W          = job.img_width;
    pid_t    tile_pids[8]         = {0};
    int      tile_job_w[8]        = {-1,-1,-1,-1,-1,-1,-1,-1};
    int      tile_result_r[8]     = {-1,-1,-1,-1,-1,-1,-1,-1};

    for (uint32_t t = 0; t < tc; t++) {
        uint32_t row_start = t * (H / tc);
        uint32_t row_end   = (t == tc - 1) ? H : (t + 1) * (H / tc);

        /* Build tile_job_t */
        tile_job_t tjob;
        memset(&tjob, 0, sizeof(tjob));
        tjob.cluster_id  = job.cluster_id;
        tjob.row_start   = row_start;
        tjob.row_end     = row_end;
        tjob.img_width   = W;
        tjob.img_height  = H;
        tjob.mask_offset = row_start * W;
        strncpy(tjob.infile, job.infile, MAX_PATH - 1);
        snprintf(tjob.tmp_outfile, MAX_PATH,
                 "/tmp/imageproc_%d_c%d_t%u.ppm",
                 getpid(), (int)job.cluster_id, t);

        /* Create pipes */
        int job_pipe[2], res_pipe[2];
        if (pipe(job_pipe) != 0 || pipe(res_pipe) != 0) {
            perror("worker: pipe");
            free(mask);
            lresult.status = 2;
            goto send_result;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("worker: fork tile");
            close(job_pipe[0]); close(job_pipe[1]);
            close(res_pipe[0]); close(res_pipe[1]);
            free(mask);
            lresult.status = 2;
            goto send_result;
        }

        if (pid == 0) {
            /* ---- child (tile process) ---- */
            close(job_pipe[1]);   /* close write end of job pipe */
            close(res_pipe[0]);   /* close read end of result pipe */
            /* close all other tile pipes inherited from prior iterations */
            for (uint32_t tt = 0; tt < t; tt++) {
                if (tile_job_w[tt]    >= 0) close(tile_job_w[tt]);
                if (tile_result_r[tt] >= 0) close(tile_result_r[tt]);
            }
            if (job_fd >= 0)  close(job_fd);
            close(result_fd);
            run_tile(job_pipe[0], res_pipe[1]);
            /* run_tile calls exit() — never returns */
        }

        /* ---- parent side ---- */
        close(job_pipe[0]);  /* close read end — child owns it */
        close(res_pipe[1]);  /* close write end — child owns it */

        tile_pids[t]      = pid;
        tile_job_w[t]     = job_pipe[1];
        tile_result_r[t]  = res_pipe[0];
    }

    /* ------------------------------------------------------------------ */
    /* 5. Send tile_job_t + mask slice to each tile, then close write end  */
    /* ------------------------------------------------------------------ */
    for (uint32_t t = 0; t < tc; t++) {
        /* Rebuild job to get tmp_outfile path (already set above in tjob,
         * but we need to re-derive it here — easier to reconstruct) */
        tile_job_t tjob;
        memset(&tjob, 0, sizeof(tjob));
        tjob.cluster_id  = job.cluster_id;
        tjob.row_start   = t * (H / tc);
        tjob.row_end     = (t == tc - 1) ? H : (t + 1) * (H / tc);
        tjob.img_width   = W;
        tjob.img_height  = H;
        tjob.mask_offset = tjob.row_start * W;
        strncpy(tjob.infile, job.infile, MAX_PATH - 1);
        snprintf(tjob.tmp_outfile, MAX_PATH,
                 "/tmp/imageproc_%d_c%d_t%u.ppm",
                 getpid(), (int)job.cluster_id, t);

        uint32_t slice_bytes = (tjob.row_end - tjob.row_start) * W;

        ssize_t nw = write_all(tile_job_w[t], &tjob, sizeof(tile_job_t));
        if (nw != (ssize_t)sizeof(tile_job_t)) {
            fprintf(stderr, "worker: short write tile_job_t to tile %u\n", t);
        }

        nw = write_all(tile_job_w[t],
                       mask + tjob.mask_offset, slice_bytes);
        if (nw != (ssize_t)slice_bytes) {
            fprintf(stderr, "worker: short write mask slice to tile %u\n", t);
        }

        close(tile_job_w[t]);
        tile_job_w[t] = -1;
    }

    /* ------------------------------------------------------------------ */
    /* 6. Collect tile_result_t from each tile                            */
    /* ------------------------------------------------------------------ */
    tile_result_t tresults[8];
    memset(tresults, 0, sizeof(tresults));
    uint8_t tiles_ok = 0;

    for (uint32_t t = 0; t < tc; t++) {
        nr = read_all(tile_result_r[t],
                      &tresults[t], sizeof(tile_result_t));
        close(tile_result_r[t]);
        tile_result_r[t] = -1;

        if (nr != (ssize_t)sizeof(tile_result_t)) {
            fprintf(stderr, "worker: tile %u: EOF before full result "
                    "(got %zd)\n", t, nr);
            tresults[t].status = 1;
        } else if (tresults[t].status == 0) {
            tiles_ok++;
        }
    }

    /* ------------------------------------------------------------------ */
    /* 7. Stitch tile strip files into full-size layer PPM                */
    /*    Only cluster-owned pixels are set; rest are zeroed.             */
    /* ------------------------------------------------------------------ */
    ppm_t *layer = ppm_alloc(W, H);
    if (!layer) {
        perror("worker: ppm_alloc layer");
        free(mask);
        lresult.status = 2;
        goto send_result;
    }

    for (uint32_t t = 0; t < tc; t++) {
        if (tresults[t].status != 0) continue;  /* skip failed tiles */

        uint32_t row_start = t * (H / tc);
        uint32_t row_end   = (t == tc - 1) ? H : (t + 1) * (H / tc);
        uint32_t strip_h   = row_end - row_start;

        ppm_t *strip = ppm_read_strip(tresults[t].tmp_outfile, W, 0, strip_h);
        if (!strip) {
            fprintf(stderr, "worker: cannot read strip %s\n",
                    tresults[t].tmp_outfile);
            continue;
        }

        /* Copy only owned pixels into layer */
        for (uint32_t row = 0; row < strip_h; row++) {
            uint32_t img_row = row_start + row;
            for (uint32_t x = 0; x < W; x++) {
                uint32_t img_idx   = img_row * W + x;
                uint32_t strip_idx = row    * W + x;
                if (mask[img_idx] == job.cluster_id) {
                    layer->data[img_idx*3 + 0] = strip->data[strip_idx*3 + 0];
                    layer->data[img_idx*3 + 1] = strip->data[strip_idx*3 + 1];
                    layer->data[img_idx*3 + 2] = strip->data[strip_idx*3 + 2];
                }
            }
        }
        ppm_free(strip);

        /* Clean up tmp strip file */
        unlink(tresults[t].tmp_outfile);
    }

    /* ------------------------------------------------------------------ */
    /* 8. Apply unsharp mask with calibrated strength                     */
    /* ------------------------------------------------------------------ */
    float avg_stddev = 0.0f;
    uint8_t good = 0;
    for (uint32_t t = 0; t < tc; t++) {
        if (tresults[t].status == 0) {
            avg_stddev += tresults[t].lum_stddev;
            good++;
        }
    }
    if (good > 0) avg_stddev /= good;

    float strength = calibrate_strength(avg_stddev);
    unsharp_mask(layer, mask, job.cluster_id, strength, 2);

    /* ------------------------------------------------------------------ */
    /* 9. Compute avg_lum_after                                           */
    /* ------------------------------------------------------------------ */
    float avg_lum_after = avg_luminance(layer, mask, job.cluster_id);

    /* ------------------------------------------------------------------ */
    /* 10. Write layer file                                               */
    /* ------------------------------------------------------------------ */
    char layer_file[MAX_PATH];
    snprintf(layer_file, MAX_PATH,
             "/tmp/imageproc_%d_layer%d.ppm",
             getppid(), (int)job.cluster_id);

    if (ppm_write(layer, layer_file) != 0) {
        fprintf(stderr, "worker: ppm_write layer failed: %s\n", layer_file);
        ppm_free(layer);
        free(mask);
        lresult.status = 2;
        goto send_result;
    }
    ppm_free(layer);

    /* ------------------------------------------------------------------ */
    /* 11. Count total owned pixels                                       */
    /* ------------------------------------------------------------------ */
    uint32_t pixels_total = 0;
    for (uint32_t t = 0; t < tc; t++)
        pixels_total += tresults[t].pixels_owned;

    /* ------------------------------------------------------------------ */
    /* 12. Build layer_result_t                                           */
    /* ------------------------------------------------------------------ */
    lresult.status        = (tiles_ok == tc) ? 0 : 1;
    lresult.cluster_id    = job.cluster_id;
    lresult.tiles_ok      = tiles_ok;
    lresult.pixels_total  = pixels_total;
    lresult.avg_lum_before = avg_lum_before;
    lresult.avg_lum_after  = avg_lum_after;
    strncpy(lresult.layer_file, layer_file, MAX_PATH - 1);
    lresult.layer_file[MAX_PATH - 1] = '\0';

    free(mask);

    /* ------------------------------------------------------------------ */
    /* 13. waitpid all tile PIDs                                          */
    /* ------------------------------------------------------------------ */
    for (uint32_t t = 0; t < tc; t++) {
        if (tile_pids[t] > 0)
            waitpid(tile_pids[t], NULL, 0);
    }

send_result:
    if (job_fd >= 0) close(job_fd);

    /* Close any still-open tile pipe ends on error paths */
    for (int t = 0; t < 8; t++) {
        if (tile_job_w[t]    >= 0) close(tile_job_w[t]);
        if (tile_result_r[t] >= 0) close(tile_result_r[t]);
    }

    ssize_t nw = write_all(result_fd, &lresult, sizeof(layer_result_t));
    if (nw != (ssize_t)sizeof(layer_result_t))
        perror("worker: write layer_result_t");

    close(result_fd);
    exit(lresult.status == 0 ? 0 : 1);
}
