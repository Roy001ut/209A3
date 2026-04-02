#ifndef PARENT_H
#define PARENT_H

/* Orchestrate processing: group pixels with k-means, hand off clusters
 * to workers, stitch the results, and save the final image.
 * Returns 0 on success, 1 on error. */
int run_parent(const char *infile, const char *outfile,
               int k, int tiles_per_worker);

#endif /* PARENT_H */
