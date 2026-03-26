#ifndef PPM_H
#define PPM_H

#include <stdint.h>

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *data;   /* interleaved RGB, row-major: data[(y*width+x)*3 + c] */
} ppm_t;

/* Read a full PPM (P6) image from disk. Returns NULL on error. */
ppm_t *ppm_read(const char *path);

/* Write a full PPM (P6) image to disk. Returns 0 on success, -1 on error. */
int    ppm_write(const ppm_t *img, const char *path);

/* Allocate a zeroed ppm_t of given dimensions. Returns NULL on malloc fail. */
ppm_t *ppm_alloc(uint32_t width, uint32_t height);

/* Free a ppm_t and its pixel data. */
void   ppm_free(ppm_t *img);

/*
 * Write rows [row_start, row_end) of img to path as a strip PPM.
 * The strip header uses height = (row_end - row_start), not img->height.
 * Returns 0 on success, -1 on error.
 */
int    ppm_write_strip(const ppm_t *img, const char *path,
                       uint32_t row_start, uint32_t row_end);

/*
 * Read rows [row_start, row_end) from a strip PPM file written by
 * ppm_write_strip. width must match the strip's width.
 * Returns a ppm_t with height = (row_end - row_start), or NULL on error.
 */
ppm_t *ppm_read_strip(const char *path, uint32_t width,
                      uint32_t row_start, uint32_t row_end);

#endif /* PPM_H */
