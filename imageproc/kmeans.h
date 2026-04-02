#ifndef KMEANS_H
#define KMEANS_H

#include <stdint.h>

/* Segment RGB pixels into k clusters using k-means.
 * Deterministically initializes centroids to evenly spaced pixels.
 * Recovers from empty clusters by picking the pixel furthest from all centroids.
 * Returns the number of iterations performed. */
int kmeans(const uint8_t *pixels, uint32_t n_pixels, int k,
           uint8_t *labels, float *centroids, int max_iter);

#endif /* KMEANS_H */
