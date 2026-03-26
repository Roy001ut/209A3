#include "enhance.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static inline uint8_t clamp_u8(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* Compute luminance (0-255 range) from RGB. */
static inline float lum_f(uint8_t r, uint8_t g, uint8_t b)
{
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

/* --------------------------------------------------------------------------
 * Histogram equalisation
 * -------------------------------------------------------------------------- */

/*
 * Build a global 256-entry LUT from all owned pixels in the full image.
 * Returns the average luminance of owned pixels.
 */
float histeq_build_lut(const uint8_t *pixels, uint32_t n_pixels,
                       const uint8_t *mask, uint8_t cluster_id,
                       uint8_t *lut)
{
    uint32_t hist[256] = {0};
    uint32_t n_owned = 0;
    double   sum_lum = 0.0;

    for (uint32_t i = 0; i < n_pixels; i++) {
        if (mask[i] != cluster_id) continue;
        uint8_t r = pixels[i*3+0], g = pixels[i*3+1], b = pixels[i*3+2];
        float lv = lum_f(r, g, b);
        hist[(uint8_t)(lv + 0.5f)]++;
        sum_lum += lv;
        n_owned++;
    }

    if (n_owned == 0) {
        for (int v = 0; v < 256; v++) lut[v] = (uint8_t)v;
        return 0.0f;
    }

    uint32_t cdf[256];
    cdf[0] = hist[0];
    for (int v = 1; v < 256; v++) cdf[v] = cdf[v-1] + hist[v];

    uint32_t cdf_min = 0;
    for (int v = 0; v < 256; v++) {
        if (cdf[v] > 0) { cdf_min = cdf[v]; break; }
    }

    uint32_t denom = n_owned - cdf_min;
    for (int v = 0; v < 256; v++) {
        if (denom == 0) {
            lut[v] = (uint8_t)v;
        } else {
            double mapped = (double)(cdf[v] - cdf_min) / denom * 255.0;
            if (mapped < 0.0)   mapped = 0.0;
            if (mapped > 255.0) mapped = 255.0;
            lut[v] = (uint8_t)(mapped + 0.5);
        }
    }

    return (float)(sum_lum / n_owned);
}

void histeq(uint8_t *strip, uint32_t width, uint32_t height,
            const uint8_t *mask, uint8_t cluster_id,
            float blend, const uint8_t *lut,
            float *lum_mean_out, float *lum_stddev_out)
{
    uint32_t n_pixels = width * height;
    uint32_t n_owned  = 0;
    for (uint32_t i = 0; i < n_pixels; i++)
        if (mask[i] == cluster_id) n_owned++;

    if (n_owned == 0) {
        if (lum_mean_out)   *lum_mean_out   = 0.0f;
        if (lum_stddev_out) *lum_stddev_out = 0.0f;
        return;
    }

    /* --- Apply LUT via luminance scaling --- */
    for (uint32_t i = 0; i < n_pixels; i++) {
        if (mask[i] != cluster_id) continue;

        uint8_t r = strip[i*3 + 0];
        uint8_t g = strip[i*3 + 1];
        uint8_t b = strip[i*3 + 2];

        float lv = lum_f(r, g, b);
        if (lv < 1.0f) continue;  /* avoid divide-by-zero for pure black */

        uint8_t lv_u8   = (uint8_t)(lv + 0.5f);
        float   new_lv  = blend * (float)lut[lv_u8] + (1.0f - blend) * lv;
        float   scale   = new_lv / lv;

        strip[i*3 + 0] = clamp_u8((int)(r * scale + 0.5f));
        strip[i*3 + 1] = clamp_u8((int)(g * scale + 0.5f));
        strip[i*3 + 2] = clamp_u8((int)(b * scale + 0.5f));
    }

    /* --- Compute lum stats over owned pixels AFTER equalisation --- */
    if (lum_mean_out || lum_stddev_out) {
        double sum  = 0.0;
        double sum2 = 0.0;
        for (uint32_t i = 0; i < n_pixels; i++) {
            if (mask[i] != cluster_id) continue;
            double lv = lum_f(strip[i*3], strip[i*3+1], strip[i*3+2]);
            sum  += lv;
            sum2 += lv * lv;
        }
        double mean   = sum / n_owned;
        double var    = sum2 / n_owned - mean * mean;
        if (var < 0.0) var = 0.0;  /* floating point rounding guard */

        if (lum_mean_out)   *lum_mean_out   = (float)mean;
        if (lum_stddev_out) *lum_stddev_out = (float)sqrt(var);
    }
}

/* --------------------------------------------------------------------------
 * Unsharp mask
 * -------------------------------------------------------------------------- */

void unsharp_mask(ppm_t *layer, const uint8_t *mask, uint8_t cluster_id,
                  float amount, int radius)
{
    uint32_t W = layer->width;
    uint32_t H = layer->height;
    uint8_t *data = layer->data;

    /* Allocate blurred buffer (same size as image) */
    uint8_t *blurred = malloc((size_t)W * H * 3);
    if (!blurred) {
        perror("unsharp_mask: malloc blurred");
        return;
    }
    memcpy(blurred, data, (size_t)W * H * 3);

    /* --- Box blur over same-cluster neighbours only --- */
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            uint32_t idx = y * W + x;
            if (mask[idx] != cluster_id) continue;

            double sum_r = 0, sum_g = 0, sum_b = 0;
            int    count = 0;

            int y0 = (int)y - radius;
            int y1 = (int)y + radius;
            int x0 = (int)x - radius;
            int x1 = (int)x + radius;

            for (int ny = y0; ny <= y1; ny++) {
                if (ny < 0 || ny >= (int)H) continue;
                for (int nx = x0; nx <= x1; nx++) {
                    if (nx < 0 || nx >= (int)W) continue;
                    uint32_t nidx = (uint32_t)ny * W + (uint32_t)nx;
                    /* Only include same-cluster neighbours */
                    if (mask[nidx] != cluster_id) continue;
                    sum_r += data[nidx*3 + 0];
                    sum_g += data[nidx*3 + 1];
                    sum_b += data[nidx*3 + 2];
                    count++;
                }
            }

            if (count > 0) {
                blurred[idx*3 + 0] = clamp_u8((int)(sum_r / count + 0.5));
                blurred[idx*3 + 1] = clamp_u8((int)(sum_g / count + 0.5));
                blurred[idx*3 + 2] = clamp_u8((int)(sum_b / count + 0.5));
            }
        }
    }

    /* --- Sharpen: original + amount * (original - blurred) --- */
    /* Pixels within radius of a cluster boundary get half strength.       */
    for (uint32_t i = 0; i < W * H; i++) {
        if (mask[i] != cluster_id) continue;

        uint32_t y = i / W;
        uint32_t x = i % W;
        int at_boundary = 0;
        int y0 = (int)y - radius, y1 = (int)y + radius;
        int x0 = (int)x - radius, x1 = (int)x + radius;
        for (int ny = y0; ny <= y1 && !at_boundary; ny++) {
            if (ny < 0 || ny >= (int)H) continue;
            for (int nx = x0; nx <= x1 && !at_boundary; nx++) {
                if (nx < 0 || nx >= (int)W) continue;
                if (mask[(uint32_t)ny * W + (uint32_t)nx] != cluster_id)
                    at_boundary = 1;
            }
        }

        float eff_amount = at_boundary ? amount * 0.5f : amount;
        for (int c = 0; c < 3; c++) {
            int16_t detail = (int16_t)data[i*3+c] - (int16_t)blurred[i*3+c];
            int     sharp  = (int)data[i*3+c] + (int)(eff_amount * detail);
            data[i*3+c]    = clamp_u8(sharp);
        }
    }

    free(blurred);
}

/* --------------------------------------------------------------------------
 * Strength calibration
 * -------------------------------------------------------------------------- */

float calibrate_strength(float avg_lum_stddev)
{
    float s;
    if (avg_lum_stddev < 15.0f)      s = 0.3f;
    else if (avg_lum_stddev < 40.0f) s = 0.6f;
    else                             s = 0.5f;

    /* Allow override via SHARP_MAX env var (e.g. SHARP_MAX=0.8) */
    const char *env = getenv("SHARP_MAX");
    if (env) {
        float cap = (float)atof(env);
        if (cap >= 0.0f && s > cap) s = cap;
    }
    return s;
}
