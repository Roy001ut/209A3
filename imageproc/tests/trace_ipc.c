#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "protocol.h"

void dump_hex(const char *label, const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    printf("--- %s (%zu bytes) ---\n", label, size);
    for (size_t i = 0; i < size; i++) {
        printf("%02x ", p[i]);
        if ((i + 1) % 16 == 0) printf("\n");
        else if ((i + 1) % 4 == 0) printf("  ");
    }
    printf("\n\n");
}

int main() {
    printf("=== imageproc IPC DATA FLOW TRACE ===\n\n");

    /* Generate and display cmd_t payload */
    cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOAD;
    cmd.cluster_id = 2;
    cmd.img_width = 1920;
    cmd.img_height = 1080;
    strncpy(cmd.infile, "input.ppm", 255);

    printf("STEP 1: Parent constructs CMD_LOAD structure\n");
    printf("  - Type: %d (CMD_LOAD)\n", cmd.type);
    printf("  - Cluster: %d\n", cmd.cluster_id);
    printf("  - Size: %dx%d\n", cmd.img_width, cmd.img_height);
    printf("  - File: %s\n", cmd.infile);
    dump_hex("Binary Packet (cmd_t)", &cmd, sizeof(cmd_t));

    /* Generate and display resp_t payload */
    resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = RESP_STATS;
    resp.status = 0;
    resp.lum_mean = 127.5f;
    resp.lum_stddev = 45.2f;
    resp.pixel_count = 500000;

    printf("STEP 2: Worker responds with RESP_STATS\n");
    printf("  - Type: %d (RESP_STATS)\n", resp.type);
    printf("  - Mean: %.2f\n", resp.lum_mean);
    printf("  - StdDev: %.2f\n", resp.lum_stddev);
    printf("  - Count: %u\n", resp.pixel_count);
    dump_hex("Binary Packet (resp_t)", &resp, sizeof(resp_t));

    /* Generate and display tile_job_t payload */
    tile_job_t job;
    memset(&job, 0, sizeof(job));
    job.operation = TILE_OP_SHARPEN;
    job.row_start = 0;
    job.row_end = 540;
    job.param = 1.25f;

    printf("STEP 3: Worker creates TILE_JOB for grandchild\n");
    printf("  - Op: %d (SHARPEN)\n", job.operation);
    printf("  - Rows: [%u, %u)\n", job.row_start, job.row_end);
    printf("  - Param: %.2f\n", job.param);
    dump_hex("Binary Packet (tile_job_t)", &job, sizeof(tile_job_t));

    return 0;
}
