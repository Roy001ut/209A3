#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAX_PATH   256
#define MAX_PIXELS (4096 * 4096)   /* hard ceiling for mask alloc */

/* MSG 1: parent -> worker
 * Sent as two writes:
 *   write(fd, &job, sizeof(layer_job_t))
 *   write(fd, mask, img_width * img_height)   <- mask bytes follow separately
 */
typedef struct {
    uint8_t  cluster_id;
    uint32_t tile_count;
    uint32_t img_width;
    uint32_t img_height;
    char     infile[MAX_PATH];
    char     outfile[MAX_PATH];
} layer_job_t;

/* MSG 2: worker -> tile
 * Sent as two writes:
 *   write(fd, &job, sizeof(tile_job_t))
 *   write(fd, mask_slice, (row_end - row_start) * img_width)
 */
typedef struct {
    uint8_t  cluster_id;
    uint32_t row_start;
    uint32_t row_end;           /* exclusive */
    uint32_t img_width;
    uint32_t img_height;
    uint32_t mask_offset;       /* byte offset into full mask for this tile */
    char     infile[MAX_PATH];
    char     tmp_outfile[MAX_PATH];
} tile_job_t;

/* MSG 3: tile -> worker */
typedef struct {
    uint8_t  status;            /* 0=ok, 1=error */
    uint32_t rows_written;
    uint32_t pixels_owned;     /* pixels in this tile belonging to the cluster */
    float    lum_mean;
    float    lum_stddev;
    char     tmp_outfile[MAX_PATH];
} tile_result_t;

/* MSG 4: worker -> parent */
typedef struct {
    uint8_t  status;            /* 0=complete, 1=partial, 2=failed */
    uint8_t  cluster_id;
    uint8_t  tiles_ok;
    uint32_t pixels_total;
    float    avg_lum_before;
    float    avg_lum_after;
    char     layer_file[MAX_PATH];
} layer_result_t;

#endif /* PROTOCOL_H */
