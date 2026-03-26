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

/* --------------------------------------------------------------------------
 * Robust pipe I/O
 * -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 * Gaussian pre-blur for k-means (sigma=1, 5-tap separable kernel)
 * Writes into out_buf (caller allocates, same size as img->data).
 * img->data is never modified.
 * -------------------------------------------------------------------------- */
static void gaussian_blur_rgb(const uint8_t *src, uint8_t *out_buf,
                               uint32_t W, uint32_t H)
{
    /* 5-tap kernel: [1/16, 4/16, 6/16, 4/16, 1/16] */
    static const float K[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };
    const int R = 2;  /* kernel half-width */

    uint8_t *tmp = malloc((size_t)W * H * 3);
    if (!tmp) {
        /* fallback: copy unchanged */
        memcpy(out_buf, src, (size_t)W * H * 3);
        return;
    }

    /* Horizontal pass: src -> tmp */
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            float acc[3] = {0};
            for (int k = -R; k <= R; k++) {
                int nx = (int)x + k;
                if (nx < 0)        nx = 0;
                if (nx >= (int)W)  nx = (int)W - 1;
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

    /* Vertical pass: tmp -> out_buf */
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            float acc[3] = {0};
            for (int k = -R; k <= R; k++) {
                int ny = (int)y + k;
                if (ny < 0)        ny = 0;
                if (ny >= (int)H)  ny = (int)H - 1;
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

/* --------------------------------------------------------------------------
 * run_parent
 * -------------------------------------------------------------------------- */
int run_parent(const char *infile, const char *outfile,
               int k, int tiles_per_worker)
{
    /* ------------------------------------------------------------------ */
    /* 1. Read input image                                                 */
    /* ------------------------------------------------------------------ */
    ppm_t *img = ppm_read(infile);
    if (!img) {
        fprintf(stderr, "parent: cannot read input file: %s\n", infile);
        return 1;
    }

    uint32_t W = img->width;
    uint32_t H = img->height;
    uint32_t n_pixels = W * H;

    /* ------------------------------------------------------------------ */
    /* 2. Run k-means                                                     */
    /* ------------------------------------------------------------------ */
    uint8_t *labels    = malloc(n_pixels);
    float   *centroids = malloc((size_t)k * 3 * sizeof(float));
    if (!labels || !centroids) {
        perror("parent: malloc kmeans arrays");
        free(labels); free(centroids); ppm_free(img);
        return 1;
    }

    /* Pre-blur image for k-means to reduce noise-driven boundary instability */
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
    free(centroids);

    /* ------------------------------------------------------------------ */
    /* 3. Create pipes and fork one worker per cluster                    */
    /* ------------------------------------------------------------------ */
    int    job_pipe_w[8]    = {-1,-1,-1,-1,-1,-1,-1,-1}; /* parent writes */
    int    result_pipe_r[8] = {-1,-1,-1,-1,-1,-1,-1,-1}; /* parent reads  */
    pid_t  worker_pids[8]   = {0};

    for (int i = 0; i < k; i++) {
        int job_pipe[2], result_pipe[2];
        if (pipe(job_pipe) != 0 || pipe(result_pipe) != 0) {
            perror("parent: pipe");
            free(labels); ppm_free(img);
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("parent: fork worker");
            close(job_pipe[0]); close(job_pipe[1]);
            close(result_pipe[0]); close(result_pipe[1]);
            free(labels); ppm_free(img);
            return 1;
        }

        if (pid == 0) {
            /* ---- child (worker) ---- */
            close(job_pipe[1]);      /* close write end */
            close(result_pipe[0]);   /* close read end  */

            /* close all parent-side pipe ends inherited from prior forks */
            for (int j = 0; j < i; j++) {
                if (job_pipe_w[j]    >= 0) close(job_pipe_w[j]);
                if (result_pipe_r[j] >= 0) close(result_pipe_r[j]);
            }

            /* free parent-only resources before exec-like call */
            free(labels);
            ppm_free(img);

            run_worker(job_pipe[0], result_pipe[1]);
            /* run_worker calls exit() — never returns */
        }

        /* ---- parent side ---- */
        close(job_pipe[0]);      /* close read end  — child owns it */
        close(result_pipe[1]);   /* close write end — child owns it */

        worker_pids[i]    = pid;
        job_pipe_w[i]     = job_pipe[1];
        result_pipe_r[i]  = result_pipe[0];
    }

    /* ------------------------------------------------------------------ */
    /* 4. Send layer_job_t + full mask to each worker                     */
    /* ------------------------------------------------------------------ */
    for (int i = 0; i < k; i++) {
        layer_job_t job;
        memset(&job, 0, sizeof(job));
        job.cluster_id  = (uint8_t)i;
        job.tile_count  = (uint32_t)tiles_per_worker;
        job.img_width   = W;
        job.img_height  = H;
        strncpy(job.infile, infile, MAX_PATH - 1);
        /* outfile field unused by worker; worker derives its own layer path */
        strncpy(job.outfile, outfile, MAX_PATH - 1);

        ssize_t nw = write_all(job_pipe_w[i], &job, sizeof(layer_job_t));
        if (nw != (ssize_t)sizeof(layer_job_t)) {
            fprintf(stderr, "parent: short write layer_job_t to worker %d\n", i);
        }

        nw = write_all(job_pipe_w[i], labels, n_pixels);
        if (nw != (ssize_t)n_pixels) {
            fprintf(stderr, "parent: short write mask to worker %d\n", i);
        }

        close(job_pipe_w[i]);
        job_pipe_w[i] = -1;
    }

    /* ------------------------------------------------------------------ */
    /* 5. Collect layer_result_t from each worker                         */
    /* ------------------------------------------------------------------ */
    layer_result_t results[8];
    memset(results, 0, sizeof(results));

    for (int i = 0; i < k; i++) {
        ssize_t nr = read_all(result_pipe_r[i],
                              &results[i], sizeof(layer_result_t));
        close(result_pipe_r[i]);
        result_pipe_r[i] = -1;

        if (nr != (ssize_t)sizeof(layer_result_t)) {
            fprintf(stderr, "parent: worker %d: EOF before full result "
                    "(got %zd) — treating as failed\n", i, nr);
            results[i].status = 2;
        }
    }

    /* ------------------------------------------------------------------ */
    /* 6. Stitch: for each pixel, copy from the owning cluster layer      */
    /* ------------------------------------------------------------------ */
    ppm_t *output = ppm_alloc(W, H);
    if (!output) {
        perror("parent: ppm_alloc output");
        free(labels); ppm_free(img);
        return 1;
    }

    /* Pre-fill output with original image pixels as fallback */
    memcpy(output->data, img->data, (size_t)n_pixels * 3);

    /* Load each layer file and copy owned pixels */
    for (int i = 0; i < k; i++) {
        if (results[i].status == 2) {
            fprintf(stderr, "parent: cluster %d failed — skipping layer\n", i);
            continue;
        }

        ppm_t *layer = ppm_read(results[i].layer_file);
        if (!layer) {
            fprintf(stderr, "parent: cannot read layer file: %s\n",
                    results[i].layer_file);
            continue;
        }

        for (uint32_t p = 0; p < n_pixels; p++) {
            if (labels[p] == (uint8_t)i) {
                output->data[p*3 + 0] = layer->data[p*3 + 0];
                output->data[p*3 + 1] = layer->data[p*3 + 1];
                output->data[p*3 + 2] = layer->data[p*3 + 2];
            }
        }
        ppm_free(layer);

        /* Clean up layer temp file */
        unlink(results[i].layer_file);
    }

    /* ------------------------------------------------------------------ */
    /* 7. Write final output                                              */
    /* ------------------------------------------------------------------ */
    if (ppm_write(output, outfile) != 0) {
        fprintf(stderr, "parent: failed to write output: %s\n", outfile);
        ppm_free(output); ppm_free(img); free(labels);
        return 1;
    }
    ppm_free(output);

    /* ------------------------------------------------------------------ */
    /* 8. waitpid all workers                                             */
    /* ------------------------------------------------------------------ */
    for (int i = 0; i < k; i++) {
        if (worker_pids[i] > 0) {
            int wstatus;
            waitpid(worker_pids[i], &wstatus, 0);
        }
    }

    /* ------------------------------------------------------------------ */
    /* 9. Print summary                                                   */
    /* ------------------------------------------------------------------ */
    fprintf(stderr, "\n=== imageproc summary ===\n");
    fprintf(stderr, "  Input : %s  (%ux%u)\n", infile, W, H);
    fprintf(stderr, "  Output: %s\n", outfile);
    fprintf(stderr, "  k=%d  tiles_per_worker=%d\n\n", k, tiles_per_worker);
    for (int i = 0; i < k; i++) {
        const char *s = results[i].status == 0 ? "OK"
                      : results[i].status == 1 ? "PARTIAL"
                      :                          "FAILED";
        fprintf(stderr, "  cluster %d: %s  pixels=%u  "
                "lum %.1f -> %.1f  tiles_ok=%u\n",
                i, s,
                results[i].pixels_total,
                results[i].avg_lum_before,
                results[i].avg_lum_after,
                results[i].tiles_ok);
    }

    free(labels);
    ppm_free(img);
    return 0;
}
