#ifndef WORKER_H
#define WORKER_H

/*
 * Entry point for a worker (child) process.
 *
 * job_fd    : read end of pipe carrying layer_job_t + full mask
 * result_fd : write end of pipe for layer_result_t
 *
 * Reads the job, forks tile_count tile subprocesses, collects their
 * results, stitches strip files into a full layer PPM, applies unsharp
 * mask, then sends layer_result_t and exits.
 *
 * Returns 0 on success, 1 on error (but normally calls exit() directly).
 */
int run_worker(int job_fd, int result_fd);

#endif /* WORKER_H */
