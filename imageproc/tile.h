#ifndef TILE_H
#define TILE_H

/* Tile worker entry point.
 * Reads job parameters and a mask slice, processes the assigned rows
 * (either histogram equalisation or sharpening), writes the result to disk,
 * and reports statistics back through result_fd before exiting. */
int run_tile(int job_fd, int result_fd);

#endif /* TILE_H */
