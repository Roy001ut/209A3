#include "seg_worker.h"
#include "protocol.h"
#include "ppm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ssize_t read_all(int fd, void *buf, size_t count)
{
    size_t   done = 0;
    uint8_t *p    = (uint8_t *)buf;
    while (done < count) {
        ssize_t n = read(fd, p + done, count - done);
        if (n < 0) { perror("seg_worker read_all"); return (ssize_t)done; }
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
        if (n < 0) { perror("seg_worker write_all"); return (ssize_t)done; }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

int run_seg_worker(int job_fd, int result_fd)
{
    seg_result_t result;
    memset(&result, 0, sizeof(result));

    /* Read job parameters */
    seg_job_t job;
    ssize_t nr = read_all(job_fd, &job, sizeof(seg_job_t));
    if (nr != (ssize_t)sizeof(seg_job_t)) {
        fprintf(stderr, "seg_worker: failed to read seg_job_t (got %zd)\n", nr);
        goto send_result;
    }

    /* Read complete pixel mask */
    uint32_t mask_bytes = job.img_width * job.img_height;
    uint8_t *mask = malloc(mask_bytes);
    if (!mask) {
        perror("seg_worker: malloc mask");
        goto send_result;
    }

    nr = read_all(job_fd, mask, mask_bytes);
    if (nr != (ssize_t)mask_bytes) {
        fprintf(stderr, "seg_worker: failed to read mask (got %zd, want %u)\n",
                nr, mask_bytes);
        free(mask);
        goto send_result;
    }
    close(job_fd);
    job_fd = -1;

    /* Load input image */
    ppm_t *img = ppm_read(job.infile);
    if (!img) {
        fprintf(stderr, "seg_worker: cannot read %s\n", job.infile);
        free(mask);
        goto send_result;
    }

    /* Compute statistics and centroid for assigned cluster */
    uint32_t n_pixels = job.img_width * job.img_height;
    uint32_t count    = 0;
    double   sum_lum  = 0.0, sum_lum2 = 0.0;
    double   sum_r    = 0.0, sum_g = 0.0, sum_b = 0.0;

    for (uint32_t i = 0; i < n_pixels; i++) {
        if (mask[i] != job.cluster_id) continue;
        uint8_t r = img->data[i*3 + 0];
        uint8_t g = img->data[i*3 + 1];
        uint8_t b = img->data[i*3 + 2];
        double lv = 0.299*r + 0.587*g + 0.114*b;
        sum_lum  += lv;
        sum_lum2 += lv * lv;
        sum_r    += r;
        sum_g    += g;
        sum_b    += b;
        count++;
    }

    ppm_free(img);
    free(mask);

    result.cluster_id  = job.cluster_id;
    result.pixel_count = count;
    if (count > 0) {
        double mean = sum_lum / count;
        double var  = sum_lum2 / count - mean * mean;
        if (var < 0.0) var = 0.0;
        result.lum_mean   = (float)mean;
        result.lum_stddev = (float)sqrt(var);
        result.centroid_r = (float)(sum_r / count);
        result.centroid_g = (float)(sum_g / count);
        result.centroid_b = (float)(sum_b / count);
    }

send_result:
    if (job_fd >= 0) close(job_fd);

    ssize_t nw = write_all(result_fd, &result, sizeof(seg_result_t));
    if (nw != (ssize_t)sizeof(seg_result_t))
        perror("seg_worker: write seg_result_t");

    close(result_fd);
    exit(0);
}
