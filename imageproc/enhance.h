#ifndef ENHANCE_H
#define ENHANCE_H

#include "ppm.h"
#include <stdint.h>

/* Apply histogram equalization in-place to pixels within a specific cluster.
 * Scales luminance per-pixel to preserve hue.
 * Computes post-equalization luminance statistics if pointers are provided. */
/* Build 256-entry equalization LUT from the full image for one cluster, returning average luminance. */
float histeq_build_lut(const uint8_t *pixels, uint32_t n_pixels,
                       const uint8_t *mask, uint8_t cluster_id,
                       uint8_t *lut);

/* blend: 0.0 = no equalization, 1.0 = full equalization */
void histeq(uint8_t *strip, uint32_t width, uint32_t height,
            const uint8_t *mask, uint8_t cluster_id,
            float blend, const uint8_t *lut,
            float *lum_mean_out, float *lum_stddev_out);

/* Apply unsharp masking to a specific cluster within the image.
 * Averages only same-cluster neighbors to prevent halo bleeding across boundaries. */
void unsharp_mask(ppm_t *layer, const uint8_t *mask, uint8_t cluster_id,
                  float amount, int radius);

/* Calibrate unsharp mask strength based on local detail (luminance stddev). */
float calibrate_strength(float avg_lum_stddev);

#endif /* ENHANCE_H */
