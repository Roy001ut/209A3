#ifndef ENHANCE_H
#define ENHANCE_H

#include "ppm.h"
#include <stdint.h>

/*
 * Apply histogram equalisation in-place to the owned pixels of a strip.
 *
 * strip      : pixel data (height x width, RGB interleaved)
 * width      : image width (pixels per row)
 * height     : number of rows in this strip
 * mask       : cluster label for each pixel in the strip (height * width bytes)
 * cluster_id : which label to operate on
 *
 * Only pixels where mask[y*width+x] == cluster_id are modified.
 * Luminance is computed as 0.299*R + 0.587*G + 0.114*B.
 * Equalisation is applied via per-pixel luminance scaling so hue is preserved.
 *
 * lum_mean_out / lum_stddev_out : optional outputs (pass NULL to skip).
 * Stats are computed over owned pixels AFTER equalisation.
 */
void histeq(uint8_t *strip, uint32_t width, uint32_t height,
            const uint8_t *mask, uint8_t cluster_id,
            float *lum_mean_out, float *lum_stddev_out);

/*
 * Apply unsharp mask in-place to owned pixels of a full-size layer image.
 *
 * layer      : full PPM image (worker-assembled layer)
 * mask       : cluster label per pixel (img->width * img->height bytes)
 * cluster_id : which label to operate on
 * amount     : sharpening strength (e.g. 0.5, 1.2, 2.0)
 * radius     : box blur kernel half-size in pixels (use 2 → 5x5 kernel)
 *
 * Only owned pixels are sharpened. Blur kernel only averages same-cluster
 * neighbours to prevent halo bleeding across region boundaries.
 */
void unsharp_mask(ppm_t *layer, const uint8_t *mask, uint8_t cluster_id,
                  float amount, int radius);

/*
 * Calibrate unsharp strength from average luminance stddev across tiles.
 *   stddev < 15  → 0.5  (flat region)
 *   15 <= s < 40 → 1.2  (moderate detail)
 *   s >= 40      → 2.0  (high detail / edges)
 */
float calibrate_strength(float avg_lum_stddev);

#endif /* ENHANCE_H */
