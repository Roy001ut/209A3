#include "kmeans.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/* Squared L2 distance between a pixel (3 uint8_t) and a centroid (3 float). */
static float sq_dist(const uint8_t *px, const float *c)
{
    float dr = (float)px[0] - c[0];
    float dg = (float)px[1] - c[1];
    float db = (float)px[2] - c[2];
    return dr*dr + dg*dg + db*db;
}

/*
 * Assign each pixel to its nearest centroid.
 * Returns 1 if any label changed, 0 if converged.
 */
static int assign(const uint8_t *pixels, uint32_t n_pixels, int k,
                  const float *centroids, uint8_t *labels)
{
    int changed = 0;
    for (uint32_t i = 0; i < n_pixels; i++) {
        const uint8_t *px = pixels + i * 3;
        float best_dist = FLT_MAX;
        uint8_t best_c  = 0;
        for (int c = 0; c < k; c++) {
            float d = sq_dist(px, centroids + c * 3);
            if (d < best_dist) {
                best_dist = d;
                best_c    = (uint8_t)c;
            }
        }
        if (labels[i] != best_c) {
            labels[i] = best_c;
            changed = 1;
        }
    }
    return changed;
}

/*
 * Recompute each centroid as the mean of its assigned pixels.
 * Detects empty clusters and returns a bitmask of empty cluster indices.
 */
static uint8_t update(const uint8_t *pixels, uint32_t n_pixels, int k,
                      const uint8_t *labels, float *centroids)
{
    double sums[8][3] = {{0}};
    uint32_t counts[8] = {0};

    for (uint32_t i = 0; i < n_pixels; i++) {
        int c = labels[i];
        sums[c][0] += pixels[i*3 + 0];
        sums[c][1] += pixels[i*3 + 1];
        sums[c][2] += pixels[i*3 + 2];
        counts[c]++;
    }

    uint8_t empty_mask = 0;
    for (int c = 0; c < k; c++) {
        if (counts[c] == 0) {
            empty_mask |= (uint8_t)(1 << c);
        } else {
            centroids[c*3 + 0] = (float)(sums[c][0] / counts[c]);
            centroids[c*3 + 1] = (float)(sums[c][1] / counts[c]);
            centroids[c*3 + 2] = (float)(sums[c][2] / counts[c]);
        }
    }
    return empty_mask;
}

/*
 * For each empty cluster in empty_mask, reinitialise its centroid to the
 * pixel that is furthest from any current (non-empty) centroid.
 */
static void fix_empty(const uint8_t *pixels, uint32_t n_pixels, int k,
                      uint8_t empty_mask, float *centroids)
{
    for (int c = 0; c < k; c++) {
        if (!(empty_mask & (1 << c)))
            continue;

        /* find pixel with maximum min-distance to any centroid */
        float max_min_dist = -1.0f;
        uint32_t best_px   = 0;

        for (uint32_t i = 0; i < n_pixels; i++) {
            const uint8_t *px = pixels + i * 3;
            float min_d = FLT_MAX;
            for (int cc = 0; cc < k; cc++) {
                if (empty_mask & (1 << cc)) continue; /* skip other empties */
                float d = sq_dist(px, centroids + cc * 3);
                if (d < min_d) min_d = d;
            }
            if (min_d > max_min_dist) {
                max_min_dist = min_d;
                best_px      = i;
            }
        }

        centroids[c*3 + 0] = (float)pixels[best_px*3 + 0];
        centroids[c*3 + 1] = (float)pixels[best_px*3 + 1];
        centroids[c*3 + 2] = (float)pixels[best_px*3 + 2];

        /* clear this bit so subsequent empty clusters can use it as reference */
        empty_mask &= (uint8_t)~(1 << c);
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int kmeans(const uint8_t *pixels, uint32_t n_pixels, int k,
           uint8_t *labels, float *centroids, int max_iter)
{
    if (n_pixels == 0 || k <= 0) return 0;

    /* Clamp k to available pixels to avoid degenerate init */
    if ((uint32_t)k > n_pixels)
        k = (int)n_pixels;

    /* --- Initialise centroids: evenly spaced pixel indices --- */
    for (int c = 0; c < k; c++) {
        uint32_t idx = (uint32_t)c * (n_pixels / (uint32_t)k);
        centroids[c*3 + 0] = (float)pixels[idx*3 + 0];
        centroids[c*3 + 1] = (float)pixels[idx*3 + 1];
        centroids[c*3 + 2] = (float)pixels[idx*3 + 2];
    }

    /* Initialise all labels to 0 so assign() detects first-pass changes */
    memset(labels, 0, n_pixels);

    /* --- Main loop --- */
    int iter;
    for (iter = 0; iter < max_iter; iter++) {
        int changed = assign(pixels, n_pixels, k, centroids, labels);

        uint8_t empty_mask = update(pixels, n_pixels, k, labels, centroids);
        if (empty_mask)
            fix_empty(pixels, n_pixels, k, empty_mask, centroids);

        if (!changed)
            break;
    }

    return iter;
}
