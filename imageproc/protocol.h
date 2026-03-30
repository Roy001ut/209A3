#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAX_PATH   256
#define MAX_PIXELS (4096 * 4096)   /* hard ceiling for mask alloc */

/* ------------------------------------------------------------------
 * Parent -> Worker commands
 * ------------------------------------------------------------------ */
#define CMD_LOAD      1
#define CMD_ANALYZE   2
#define CMD_BRIGHTEN  3
#define CMD_EQUALIZE  4
#define CMD_SHARPEN   5
#define CMD_SKIP      6
#define CMD_SAVE      7
#define CMD_TERMINATE 8

typedef struct {
    uint8_t  type;          /* CMD_* constant */
    uint8_t  cluster_id;
    uint32_t img_width;
    uint32_t img_height;
    float    param;         /* CMD_BRIGHTEN: multiply factor
                               CMD_SHARPEN:  amount (e.g. 1.2)
                               CMD_EQUALIZE: blend ratio (e.g. 0.5) */
    char     infile[256];   /* CMD_LOAD: path to input PPM */
    char     outfile[256];  /* CMD_SAVE: path to write layer PPM */
} cmd_t;

/* ------------------------------------------------------------------
 * Worker -> Parent responses
 * ------------------------------------------------------------------ */
#define RESP_READY  1
#define RESP_STATS  2
#define RESP_DONE   3
#define RESP_ERROR  4

typedef struct {
    uint8_t  type;          /* RESP_* constant */
    uint8_t  status;        /* 0=ok, 1=error */
    float    lum_mean;      /* RESP_STATS: mean luminance of owned pixels */
    float    lum_stddev;    /* RESP_STATS: luminance standard deviation */
    uint32_t pixel_count;   /* RESP_STATS: how many pixels this worker owns */
    float    quality;       /* RESP_DONE after processing: output variance */
    char     message[64];   /* RESP_ERROR: description */
} resp_t;

/* ------------------------------------------------------------------
 * Worker -> Tile job
 * Sent as two writes:
 *   write(fd, &job, sizeof(tile_job_t))
 *   write(fd, mask_slice, (row_end - row_start) * img_width)
 * ------------------------------------------------------------------ */
#define TILE_OP_HISTEQ  1
#define TILE_OP_SHARPEN 2

typedef struct {
    uint8_t  operation;         /* TILE_OP_HISTEQ or TILE_OP_SHARPEN */
    uint8_t  cluster_id;
    uint32_t row_start;
    uint32_t row_end;           /* exclusive */
    uint32_t img_width;
    uint32_t img_height;
    float    param;             /* histeq: blend ratio; sharpen: amount */
    uint8_t  eq_lut[256];       /* pre-computed global equalization LUT
                                   (filled by worker; used for TILE_OP_HISTEQ) */
    char     infile[MAX_PATH];
    char     tmp_outfile[MAX_PATH];
} tile_job_t;

/* ------------------------------------------------------------------
 * Tile -> Worker result
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t  status;            /* 0=ok, 1=error */
    uint32_t rows_written;
    uint32_t pixels_owned;
    float    lum_mean;
    float    lum_stddev;
    float    output_variance;   /* quality metric */
    char     tmp_outfile[MAX_PATH];
} tile_result_t;

#endif /* PROTOCOL_H */
