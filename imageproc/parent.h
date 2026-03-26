#ifndef PARENT_H
#define PARENT_H

/*
 * Top-level orchestrator called from main().
 *
 * Reads infile, runs k-means, forks one worker per cluster,
 * collects layer_result_t from each worker, stitches the final
 * output image, and writes it to outfile.
 *
 * Returns 0 on success, 1 on error.
 */
int run_parent(const char *infile, const char *outfile,
               int k, int tiles_per_worker);

#endif /* PARENT_H */
