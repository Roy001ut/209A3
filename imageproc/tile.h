#ifndef TILE_H
#define TILE_H

/*
 * Entry point for a tile (grandchild) process.
 *
 * job_fd    : read end of pipe carrying tile_job_t + mask slice
 * result_fd : write end of pipe for tile_result_t
 *
 * Reads job struct, reads mask slice, loads its row strip from disk,
 * then dispatches on job.operation:
 *   TILE_OP_HISTEQ  — applies histogram equalisation to owned pixels
 *   TILE_OP_SHARPEN — applies unsharp mask to owned pixels
 * Writes the processed strip to disk, sends tile_result_t, and exits.
 *
 * Returns 0 on success, 1 on error (but normally calls exit() directly).
 */
int run_tile(int job_fd, int result_fd);

#endif /* TILE_H */
