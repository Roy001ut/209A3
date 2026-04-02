#include "tile.h"
#include "protocol.h"
#include "ppm.h"
#include "enhance.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Robust pipe I/O helper */
static ssize_t read_all(int fd, void *buf, size_t count)
{
    size_t  done = 0;
    uint8_t *p   = (uint8_t *)buf;
    while (done < count) {
        ssize_t n = read(fd, p + done, count - done);
        if (n < 0) { perror("tile read_all"); return (ssize_t)done; }
        if (n == 0) break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

/* Main tile process logic */
int run_tile(int job_fd, int result_fd)
{
    tile_result_t result;
    memset(&result, 0, sizeof(result));

    /* Read job parameters */
    tile_job_t job;
    ssize_t nr = read_all(job_fd, &job, sizeof(tile_job_t));
    if (nr != (ssize_t)sizeof(tile_job_t)) {
        fprintf(stderr, "tile: failed to read tile_job_t (got %zd)\n", nr);
        result.status = 1;
        goto send_result;
    }

    /* Read image mask block */
    uint32_t strip_rows = job.row_end - job.row_start;
    uint32_t mask_bytes = strip_rows * job.img_width;

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

    /* Read image strip from file */
    ppm_t *strip = ppm_read_strip(job.infile, job.img_width,
                                  job.row_start, job.row_end);
    if (!strip) {
        fprintf(stderr, "tile: ppm_read_strip failed for %s rows [%u,%u)\n",
                job.infile, job.row_start, job.row_end);
        free(tile_mask);
        result.status = 1;
        goto send_result;
    }

    /* Count valid pixels for this tile */
    uint32_t n_owned = 0;
    for (uint32_t i = 0; i < strip_rows * job.img_width; i++) {
        if (tile_mask[i] == job.cluster_id) n_owned++;
    }

    float lum_mean   = 0.0f;
    float lum_stddev = 0.0f;

    /* Perform required operation */
    if (job.operation == TILE_OP_HISTEQ) {
        /* Histogram equalisation using pre-built LUT from worker */
        histeq(strip->data, job.img_width, strip_rows,
               tile_mask, job.cluster_id,
               job.param, job.eq_lut,
               &lum_mean, &lum_stddev);
    } else if (job.operation == TILE_OP_SHARPEN) {
        /* Unsharp mask — need full-size layer ppm_t for unsharp_mask(),
         * but we only have a strip.  Build a temporary ppm_t wrapper
         * that covers only this strip (mask coords are strip-local).   */
        ppm_t strip_img;
        strip_img.width  = job.img_width;
        strip_img.height = strip_rows;
        strip_img.data   = strip->data;
        unsharp_mask(&strip_img, tile_mask, job.cluster_id, job.param, 2);

        /* Compute lum stats after sharpening */
        if (n_owned > 0) {
            double sum = 0.0, sum2 = 0.0;
            for (uint32_t i = 0; i < strip_rows * job.img_width; i++) {
                if (tile_mask[i] != job.cluster_id) continue;
                double lv = 0.299 * strip->data[i*3+0]
                          + 0.587 * strip->data[i*3+1]
                          + 0.114 * strip->data[i*3+2];
                sum  += lv;
                sum2 += lv * lv;
            }
            double mean = sum / n_owned;
            double var  = sum2 / n_owned - mean * mean;
            if (var < 0.0) var = 0.0;
            lum_mean   = (float)mean;
            lum_stddev = (float)sqrt(var);
        }
    } else {
        fprintf(stderr, "tile: unknown operation %u\n", (unsigned)job.operation);
        ppm_free(strip);
        free(tile_mask);
        result.status = 1;
        goto send_result;
    }

    /* Calculate output variance */
    float output_variance = lum_stddev * lum_stddev;

    /* Write processed image strip to temporary file */
    printf("[Tile] %s; write %s\n", job.operation == TILE_OP_HISTEQ ? "histeq" : "sharpen", job.tmp_outfile);
    fflush(stdout);
    if (ppm_write_strip(strip, job.tmp_outfile, 0, strip_rows) != 0) {
        fprintf(stderr, "tile: ppm_write_strip failed for %s\n",
                job.tmp_outfile);
        ppm_free(strip);
        free(tile_mask);
        result.status = 1;
        goto send_result;
    }

    /* Construct and send result back to parent */
    result.status          = 0;
    result.rows_written    = strip_rows;
    result.pixels_owned    = n_owned;
    result.lum_mean        = lum_mean;
    result.lum_stddev      = lum_stddev;
    result.output_variance = output_variance;
    strncpy(result.tmp_outfile, job.tmp_outfile, MAX_PATH - 1);
    result.tmp_outfile[MAX_PATH - 1] = '\0';

    ppm_free(strip);
    free(tile_mask);

send_result:
    if (job_fd >= 0) close(job_fd);

    printf("[Tile] -> [Worker %d]: tile_result_t\n", job.cluster_id);
    fflush(stdout);
    ssize_t nw = write(result_fd, &result, sizeof(tile_result_t));
    if (nw != (ssize_t)sizeof(tile_result_t)) {
        perror("tile: write result");
        close(result_fd);
        exit(1);
    }
    close(result_fd);
    exit(result.status == 0 ? 0 : 1);
}
