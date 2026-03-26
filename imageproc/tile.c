#include "tile.h"
#include "protocol.h"
#include "ppm.h"
#include "enhance.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Robust pipe read: keeps reading until all `count` bytes are received,
 * or EOF / error occurs.
 * Returns total bytes read (< count means EOF/error).
 * -------------------------------------------------------------------------- */
static ssize_t read_all(int fd, void *buf, size_t count)
{
    size_t  done = 0;
    uint8_t *p   = (uint8_t *)buf;
    while (done < count) {
        ssize_t n = read(fd, p + done, count - done);
        if (n < 0) { perror("read_all"); return (ssize_t)done; }
        if (n == 0) break;  /* EOF */
        done += (size_t)n;
    }
    return (ssize_t)done;
}

/* --------------------------------------------------------------------------
 * run_tile
 * -------------------------------------------------------------------------- */
int run_tile(int job_fd, int result_fd)
{
    tile_result_t result;
    memset(&result, 0, sizeof(result));

    /* ------------------------------------------------------------------ */
    /* 1. Read tile_job_t                                                  */
    /* ------------------------------------------------------------------ */
    tile_job_t job;
    ssize_t nr = read_all(job_fd, &job, sizeof(tile_job_t));
    if (nr != (ssize_t)sizeof(tile_job_t)) {
        fprintf(stderr, "tile: failed to read tile_job_t (got %zd)\n", nr);
        result.status = 1;
        goto send_result;
    }

    /* ------------------------------------------------------------------ */
    /* 2. Read mask slice                                                  */
    /* ------------------------------------------------------------------ */
    uint32_t strip_rows  = job.row_end - job.row_start;
    uint32_t mask_bytes  = strip_rows * job.img_width;

    uint8_t *tile_mask = malloc(mask_bytes);
    if (!tile_mask) {
        perror("tile: malloc tile_mask");
        result.status = 1;
        goto send_result;
    }

    nr = read_all(job_fd, tile_mask, mask_bytes);
    if (nr != (ssize_t)mask_bytes) {
        fprintf(stderr, "tile: failed to read mask slice (got %zd, want %u)\n",
                nr, mask_bytes);
        free(tile_mask);
        result.status = 1;
        goto send_result;
    }
    close(job_fd);
    job_fd = -1;

    /* ------------------------------------------------------------------ */
    /* 3. Load row strip from input file                                   */
    /* ------------------------------------------------------------------ */
    ppm_t *strip = ppm_read_strip(job.infile, job.img_width,
                                  job.row_start, job.row_end);
    if (!strip) {
        fprintf(stderr, "tile: ppm_read_strip failed for %s rows [%u,%u)\n",
                job.infile, job.row_start, job.row_end);
        free(tile_mask);
        result.status = 1;
        goto send_result;
    }

    /* ------------------------------------------------------------------ */
    /* 4+5+6+7. Histogram equalisation on owned pixels                    */
    /* Also collects lum_mean and lum_stddev over owned pixels.           */
    /* ------------------------------------------------------------------ */

    /* Count owned pixels for result */
    uint32_t n_owned = 0;
    for (uint32_t i = 0; i < strip_rows * job.img_width; i++) {
        if (tile_mask[i] == job.cluster_id) n_owned++;
    }

    float lum_mean   = 0.0f;
    float lum_stddev = 0.0f;

    /* EQ_BLEND: 0.0 = no equalization, 1.0 = full (default 1.0) */
    float eq_blend = 1.0f;
    const char *blend_env = getenv("EQ_BLEND");
    if (blend_env) eq_blend = (float)atof(blend_env);

    /* Use global LUT from worker; skip if cluster is too dark */
    if (!job.skip_histeq) {
        histeq(strip->data, job.img_width, strip_rows,
               tile_mask, job.cluster_id,
               eq_blend, job.eq_lut,
               &lum_mean, &lum_stddev);
    } else {
        /* Dark cluster: compute stats from original pixels, no modification */
        double sum_lum = 0.0, sum2 = 0.0;
        uint32_t cnt = 0;
        for (uint32_t i = 0; i < strip_rows * job.img_width; i++) {
            if (tile_mask[i] != job.cluster_id) continue;
            double lv = 0.299*strip->data[i*3+0]
                      + 0.587*strip->data[i*3+1]
                      + 0.114*strip->data[i*3+2];
            sum_lum += lv;
            sum2    += lv * lv;
            cnt++;
        }
        if (cnt > 0) {
            lum_mean = (float)(sum_lum / cnt);
            double var = sum2/cnt - (sum_lum/cnt)*(sum_lum/cnt);
            if (var < 0.0) var = 0.0;
            lum_stddev = (float)sqrt(var);
        }
    }

    /* ------------------------------------------------------------------ */
    /* 8. Write processed strip to tmp_outfile                            */
    /* ------------------------------------------------------------------ */
    if (ppm_write_strip(strip, job.tmp_outfile, 0, strip_rows) != 0) {
        fprintf(stderr, "tile: ppm_write_strip failed for %s\n",
                job.tmp_outfile);
        ppm_free(strip);
        free(tile_mask);
        result.status = 1;
        goto send_result;
    }

    /* ------------------------------------------------------------------ */
    /* 9. Build and send tile_result_t                                    */
    /* ------------------------------------------------------------------ */
    result.status       = 0;
    result.rows_written = strip_rows;
    result.pixels_owned = n_owned;
    result.lum_mean     = lum_mean;
    result.lum_stddev   = lum_stddev;
    strncpy(result.tmp_outfile, job.tmp_outfile, MAX_PATH - 1);
    result.tmp_outfile[MAX_PATH - 1] = '\0';

    ppm_free(strip);
    free(tile_mask);

send_result:
    if (job_fd >= 0) close(job_fd);

    ssize_t nw = write(result_fd, &result, sizeof(tile_result_t));
    if (nw != (ssize_t)sizeof(tile_result_t)) {
        perror("tile: write result");
        close(result_fd);
        exit(1);
    }
    close(result_fd);
    exit(result.status == 0 ? 0 : 1);
}
