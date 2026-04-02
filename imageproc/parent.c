#include "parent.h"
#include "protocol.h"
#include "ppm.h"
#include "kmeans.h"
#include "worker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* Robust pipe I/O helpers */
static ssize_t write_all(int fd, const void *buf, size_t count)
{
    size_t         done = 0;
    const uint8_t *p    = (const uint8_t *)buf;
    while (done < count) {
        ssize_t n = write(fd, p + done, count - done);
        if (n < 0) { perror("parent write_all"); return (ssize_t)done; }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

static ssize_t read_all(int fd, void *buf, size_t count)
{
    size_t   done = 0;
    uint8_t *p    = (uint8_t *)buf;
    while (done < count) {
        ssize_t n = read(fd, p + done, count - done);
        if (n < 0) { perror("parent read_all"); return (ssize_t)done; }
        if (n == 0) break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

/* Send command message to worker pipe */
static void send_cmd(int cmd_fd, uint8_t type, uint8_t cluster_id,
                     uint32_t W, uint32_t H, float param,
                     const char *infile, const char *outfile)
{
    cmd_t c;
    memset(&c, 0, sizeof(c));
    c.type       = type;
    c.cluster_id = cluster_id;
    c.img_width  = W;
    c.img_height = H;
    c.param      = param;
    if (infile)  strncpy(c.infile,  infile,  255);
    if (outfile) strncpy(c.outfile, outfile, 255);
    write_all(cmd_fd, &c, sizeof(cmd_t));
}

/* Read response message, returning 0 on success or -1 on short read */
static int read_resp(int resp_fd, resp_t *r)
{
    ssize_t nr = read_all(resp_fd, r, sizeof(resp_t));
    return (nr == (ssize_t)sizeof(resp_t)) ? 0 : -1;
}

/* Handle RESP_ERROR: print message, send terminate, and mark worker as failed */
static void handle_error_resp(int i, const resp_t *r,
                               int *cmd_pipe_w, int *failed)
{
    fprintf(stderr, "parent: worker %d RESP_ERROR: %s\n",
            i, r->message[0] ? r->message : "(no message)");
    if (cmd_pipe_w[i] >= 0) {
        /* Best-effort terminate — worker may already be gone */
        cmd_t term;
        memset(&term, 0, sizeof(term));
        term.type = CMD_TERMINATE;
        write(cmd_pipe_w[i], &term, sizeof(cmd_t));
        close(cmd_pipe_w[i]);
        cmd_pipe_w[i] = -1;
    }
    failed[i] = 1;
}

/* Gaussian pre-blur for k-means (sigma=1, 5-tap separable kernel) */
static void gaussian_blur_rgb(const uint8_t *src, uint8_t *out_buf,
                               uint32_t W, uint32_t H)
{
    static const float K[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };
    const int R = 2;

    uint8_t *tmp = malloc((size_t)W * H * 3);
    if (!tmp) {
        memcpy(out_buf, src, (size_t)W * H * 3);
        return;
    }

    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            float acc[3] = {0};
            for (int k = -R; k <= R; k++) {
                int nx = (int)x + k;
                if (nx < 0)       nx = 0;
                if (nx >= (int)W) nx = (int)W - 1;
                float w = K[k + R];
                acc[0] += w * src[(y * W + (uint32_t)nx) * 3 + 0];
                acc[1] += w * src[(y * W + (uint32_t)nx) * 3 + 1];
                acc[2] += w * src[(y * W + (uint32_t)nx) * 3 + 2];
            }
            tmp[(y * W + x) * 3 + 0] = (uint8_t)(acc[0] + 0.5f);
            tmp[(y * W + x) * 3 + 1] = (uint8_t)(acc[1] + 0.5f);
            tmp[(y * W + x) * 3 + 2] = (uint8_t)(acc[2] + 0.5f);
        }
    }

    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            float acc[3] = {0};
            for (int k = -R; k <= R; k++) {
                int ny = (int)y + k;
                if (ny < 0)       ny = 0;
                if (ny >= (int)H) ny = (int)H - 1;
                float w = K[k + R];
                acc[0] += w * tmp[((uint32_t)ny * W + x) * 3 + 0];
                acc[1] += w * tmp[((uint32_t)ny * W + x) * 3 + 1];
                acc[2] += w * tmp[((uint32_t)ny * W + x) * 3 + 2];
            }
            out_buf[(y * W + x) * 3 + 0] = (uint8_t)(acc[0] + 0.5f);
            out_buf[(y * W + x) * 3 + 1] = (uint8_t)(acc[1] + 0.5f);
            out_buf[(y * W + x) * 3 + 2] = (uint8_t)(acc[2] + 0.5f);
        }
    }

    free(tmp);
}

/* Main parent process loop */
int run_parent(const char *infile, const char *outfile,
               int k, int tiles_per_worker)
{
    (void)tiles_per_worker;  /* tile count is fixed inside worker (2) */

    /* Read input image */
    ppm_t *img = ppm_read(infile);
    if (!img) {
        fprintf(stderr, "parent: cannot read input file: %s\n", infile);
        return 1;
    }

    uint32_t W        = img->width;
    uint32_t H        = img->height;
    uint32_t n_pixels = W * H;

    /* Run k-means clustering */
    uint8_t *labels    = malloc(n_pixels);
    float   *centroids = malloc((size_t)k * 3 * sizeof(float));
    if (!labels || !centroids) {
        perror("parent: malloc kmeans arrays");
        free(labels); free(centroids); ppm_free(img);
        return 1;
    }

    uint8_t *blur_buf = malloc((size_t)n_pixels * 3);
    if (!blur_buf) {
        perror("parent: malloc blur_buf");
        free(labels); free(centroids); ppm_free(img);
        return 1;
    }
    gaussian_blur_rgb(img->data, blur_buf, W, H);

    int iters = kmeans(blur_buf, n_pixels, k, labels, centroids, 50);
    free(blur_buf);
    fprintf(stderr, "parent: k-means converged in %d iteration(s)\n", iters);
    printf("[Parent] k-means => labels[]\n");
    fflush(stdout);
    free(centroids);
    ppm_free(img);
    img = NULL;

    /* Fork k persistent workers */
    int   cmd_pipe_w[8]   = {-1,-1,-1,-1,-1,-1,-1,-1};
    int   resp_pipe_r[8]  = {-1,-1,-1,-1,-1,-1,-1,-1};
    pid_t worker_pids[8]  = {0};
    int   failed[8]       = {0};   /* set to 1 on RESP_ERROR or fork failure */

    for (int i = 0; i < k; i++) {
        int cp[2], rp[2];
        if (pipe(cp) != 0 || pipe(rp) != 0) {
            perror("parent: pipe");
            free(labels);
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("parent: fork");
            close(cp[0]); close(cp[1]);
            close(rp[0]); close(rp[1]);
            free(labels);
            return 1;
        }

        if (pid == 0) {
            /* child */
            close(cp[1]);
            close(rp[0]);
            for (int j = 0; j < i; j++) {
                if (cmd_pipe_w[j]  >= 0) close(cmd_pipe_w[j]);
                if (resp_pipe_r[j] >= 0) close(resp_pipe_r[j]);
            }
            free(labels);
            run_worker(cp[0], rp[1]);
            /* never returns */
        }

        /* parent side */
        close(cp[0]);
        close(rp[1]);
        worker_pids[i]  = pid;
        cmd_pipe_w[i]   = cp[1];
        resp_pipe_r[i]  = rp[0];
    }

    /* Send CMD_LOAD to all workers, writing command then mask */
    for (int i = 0; i < k; i++) {
        if (failed[i]) continue;
        printf("[Parent] -> [Worker %d]: CMD_LOAD + WxH mask\n", i);
        fflush(stdout);
        send_cmd(cmd_pipe_w[i], CMD_LOAD, (uint8_t)i, W, H, 0.0f, infile, NULL);
        write_all(cmd_pipe_w[i], labels, n_pixels);  /* separate write for mask */
    }

    /* Wait for RESP_READY from all workers */
    for (int i = 0; i < k; i++) {
        if (failed[i]) continue;
        resp_t r;
        if (read_resp(resp_pipe_r[i], &r) != 0) {
            fprintf(stderr, "parent: worker %d: pipe error reading RESP_READY\n", i);
            failed[i] = 1;
            continue;
        }
        if (r.type == RESP_ERROR) {
            handle_error_resp(i, &r, cmd_pipe_w, failed);
            continue;
        }
        if (r.type != RESP_READY) {
            fprintf(stderr, "parent: worker %d: expected RESP_READY, got %u\n",
                    i, (unsigned)r.type);
            failed[i] = 1;
        }
    }

    /* Ask workers to analyze their clusters */
    for (int i = 0; i < k; i++) {
        if (failed[i]) continue;
        printf("[Parent] -> [Worker %d]: CMD_ANALYZE\n", i);
        fflush(stdout);
        send_cmd(cmd_pipe_w[i], CMD_ANALYZE, (uint8_t)i, W, H, 0.0f, NULL, NULL);
    }

    /* Collect stats from workers */
    resp_t stats[8];
    memset(stats, 0, sizeof(stats));

    for (int i = 0; i < k; i++) {
        if (failed[i]) continue;
        if (read_resp(resp_pipe_r[i], &stats[i]) != 0) {
            fprintf(stderr, "parent: worker %d: pipe error reading RESP_STATS\n", i);
            failed[i] = 1;
            continue;
        }
        if (stats[i].type == RESP_ERROR) {
            handle_error_resp(i, &stats[i], cmd_pipe_w, failed);
        }
    }

    /* Decide actions based on stats */
    int decision[8] = {0};

    printf("=== per-cluster decisions ===\n");
    for (int i = 0; i < k; i++) {
        if (failed[i]) {
            printf("cluster %d: FAILED — skipping\n", i);
            continue;
        }
        float mean   = stats[i].lum_mean;
        float stddev = stats[i].lum_stddev;
        const char *desc;

        if (mean < 60.0f) {
            decision[i] = 0;
            desc = "BRIGHTEN";
        } else if (stddev < 15.0f) {
            decision[i] = 1;
            desc = "SKIP";
        } else if (mean >= 200.0f) {
            decision[i] = 1;
            desc = "SKIP";
        } else if (stddev < 40.0f) {
            decision[i] = 2;
            desc = "EQUALIZE";
        } else {
            decision[i] = 3;
            desc = "EQUALIZE+SHARPEN";
        }

        printf("cluster %d: lum=%.1f stddev=%.1f -> %s\n",
               i, mean, stddev, desc);
    }
    printf("[Parent] decide\n");
    fflush(stdout);

    /* Issue work commands to each worker */
    for (int i = 0; i < k; i++) {
        if (failed[i]) continue;

        resp_t r;

        switch (decision[i]) {

        case 0: /* BRIGHTEN */
            printf("[Parent] -> [Worker %d]: CMD_BRIGHTEN (param=1.4)\n", i);
            fflush(stdout);
            send_cmd(cmd_pipe_w[i], CMD_BRIGHTEN, (uint8_t)i,
                     W, H, 1.4f, NULL, NULL);
            if (read_resp(resp_pipe_r[i], &r) != 0) { failed[i] = 1; break; }
            if (r.type == RESP_ERROR) { handle_error_resp(i, &r, cmd_pipe_w, failed); }
            break;

        case 1: /* SKIP */
            printf("[Parent] -> [Worker %d]: CMD_SKIP\n", i);
            fflush(stdout);
            send_cmd(cmd_pipe_w[i], CMD_SKIP, (uint8_t)i,
                     W, H, 0.0f, NULL, NULL);
            if (read_resp(resp_pipe_r[i], &r) != 0) { failed[i] = 1; break; }
            if (r.type == RESP_ERROR) { handle_error_resp(i, &r, cmd_pipe_w, failed); }
            break;

        case 2: /* EQUALIZE */
            printf("[Parent] -> [Worker %d]: CMD_EQUALIZE (param=0.5)\n", i);
            fflush(stdout);
            send_cmd(cmd_pipe_w[i], CMD_EQUALIZE, (uint8_t)i,
                     W, H, 0.5f, NULL, NULL);
            if (read_resp(resp_pipe_r[i], &r) != 0) { failed[i] = 1; break; }
            if (r.type == RESP_ERROR) { handle_error_resp(i, &r, cmd_pipe_w, failed); }
            break;

        case 3: /* EQUALIZE then SHARPEN */
            printf("[Parent] -> [Worker %d]: CMD_EQUALIZE (param=0.5)\n", i);
            fflush(stdout);
            send_cmd(cmd_pipe_w[i], CMD_EQUALIZE, (uint8_t)i,
                     W, H, 0.5f, NULL, NULL);
            if (read_resp(resp_pipe_r[i], &r) != 0) { failed[i] = 1; break; }
            if (r.type == RESP_ERROR) { handle_error_resp(i, &r, cmd_pipe_w, failed); break; }
            printf("[Parent] -> [Worker %d]: CMD_SHARPEN (param=1.2)\n", i);
            fflush(stdout);
            send_cmd(cmd_pipe_w[i], CMD_SHARPEN, (uint8_t)i,
                     W, H, 1.2f, NULL, NULL);
            if (read_resp(resp_pipe_r[i], &r) != 0) { failed[i] = 1; break; }
            if (r.type == RESP_ERROR) { handle_error_resp(i, &r, cmd_pipe_w, failed); }
            break;
        }
    }

    /* Save resulting layers */
    char layer_files[8][MAX_PATH];
    for (int i = 0; i < k; i++) {
        snprintf(layer_files[i], MAX_PATH,
                 "/tmp/imageproc_%d_layer%d.ppm", (int)getpid(), i);
        if (failed[i]) continue;
        printf("[Parent] -> [Worker %d]: CMD_SAVE (%s)\n", i, layer_files[i]);
        fflush(stdout);
        send_cmd(cmd_pipe_w[i], CMD_SAVE, (uint8_t)i,
                 W, H, 0.0f, NULL, layer_files[i]);
    }

    /* Read RESP_DONE from all workers after CMD_SAVE */
    int save_ok[8] = {0};
    for (int i = 0; i < k; i++) {
        if (failed[i]) continue;
        resp_t r;
        if (read_resp(resp_pipe_r[i], &r) != 0) {
            fprintf(stderr, "parent: worker %d: pipe error reading CMD_SAVE resp\n", i);
            failed[i] = 1;
            continue;
        }
        if (r.type == RESP_ERROR) {
            handle_error_resp(i, &r, cmd_pipe_w, failed);
            continue;
        }
        if (r.type == RESP_DONE && r.status == 0)
            save_ok[i] = 1;
        else
            fprintf(stderr, "parent: worker %d CMD_SAVE failed (status=%u)\n",
                    i, (unsigned)r.status);
    }

    /* Terminate workers */
    for (int i = 0; i < k; i++) {
        if (cmd_pipe_w[i] < 0) continue;  /* already closed by handle_error_resp */
        printf("[Parent] -> [Worker %d]: CMD_TERMINATE\n", i);
        fflush(stdout);
        send_cmd(cmd_pipe_w[i], CMD_TERMINATE, (uint8_t)i,
                 W, H, 0.0f, NULL, NULL);
        close(cmd_pipe_w[i]);
        cmd_pipe_w[i] = -1;
    }

    /* Reap child processes */
    for (int i = 0; i < k; i++) {
        if (worker_pids[i] > 0)
            waitpid(worker_pids[i], NULL, 0);
    }

    /* Stitch output layers together */
    ppm_t *orig = ppm_read(infile);
    if (!orig) {
        fprintf(stderr, "parent: cannot re-read input for stitch: %s\n", infile);
        free(labels);
        return 1;
    }

    ppm_t *output = ppm_alloc(W, H);
    if (!output) {
        perror("parent: ppm_alloc output");
        ppm_free(orig); free(labels);
        return 1;
    }
    memcpy(output->data, orig->data, (size_t)n_pixels * 3);
    ppm_free(orig);

    for (int i = 0; i < k; i++) {
        if (!save_ok[i]) {
            fprintf(stderr, "parent: cluster %d failed — skipping layer\n", i);
            continue;
        }

        ppm_t *layer = ppm_read(layer_files[i]);
        if (!layer) {
            fprintf(stderr, "parent: cannot read layer file: %s\n", layer_files[i]);
            continue;
        }

        for (uint32_t p = 0; p < n_pixels; p++) {
            if (labels[p] == (uint8_t)i) {
                output->data[p*3+0] = layer->data[p*3+0];
                output->data[p*3+1] = layer->data[p*3+1];
                output->data[p*3+2] = layer->data[p*3+2];
            }
        }
        ppm_free(layer);
        unlink(layer_files[i]);
    }

    printf("[Parent] stitch k layers => output.ppm\n");
    fflush(stdout);

    /* Write result to disk */
    if (ppm_write(output, outfile) != 0) {
        fprintf(stderr, "parent: failed to write output: %s\n", outfile);
        ppm_free(output); free(labels);
        return 1;
    }
    ppm_free(output);

    /* Final execution summary */
    fprintf(stderr, "\n=== imageproc summary ===\n");
    fprintf(stderr, "  Input : %s  (%ux%u)\n", infile, W, H);
    fprintf(stderr, "  Output: %s\n", outfile);
    fprintf(stderr, "  k=%d\n\n", k);
    for (int i = 0; i < k; i++) {
        static const char *dnames[] = {"BRIGHTEN","SKIP","EQUALIZE","EQUALIZE+SHARPEN"};
        if (failed[i] && !save_ok[i]) {
            fprintf(stderr, "  cluster %d: FAILED\n", i);
        } else {
            fprintf(stderr, "  cluster %d: %s  pixels=%u  lum=%.1f stddev=%.1f\n",
                    i, dnames[decision[i]],
                    stats[i].pixel_count,
                    stats[i].lum_mean,
                    stats[i].lum_stddev);
        }
    }

    /* Close any still-open resp pipe read ends */
    for (int i = 0; i < k; i++) {
        if (resp_pipe_r[i] >= 0) close(resp_pipe_r[i]);
    }

    free(labels);
    return 0;
}
