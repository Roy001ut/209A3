#ifndef KMEANS_H
#define KMEANS_H

#include <stdint.h>

/*
 * Segment n_pixels RGB pixels into k clusters.
 *
 * pixels   : input, n_pixels * 3 bytes interleaved (R,G,B)
 * n_pixels : number of pixels
 * k        : number of clusters (1 <= k <= 8)
 * labels   : output, n_pixels uint8_t — caller allocates
 * centroids: output, k * 3 floats (R,G,B per centroid) — caller allocates
 * max_iter : iteration cap
 *
 * Initialisation: centroid[i] = pixel[i * (n_pixels / k)]  (deterministic)
 * Empty cluster recovery: reinitialise to pixel furthest from any centroid.
 *
 * Returns the number of iterations run.
 */
int kmeans(const uint8_t *pixels, uint32_t n_pixels, int k,
           uint8_t *labels, float *centroids, int max_iter);

#endif /* KMEANS_H */
