#ifndef WORKER_H
#define WORKER_H

/* Worker process entry point.
 * Reads commands from cmd_fd and writes responses to resp_fd.
 * Supported commands include CMD_LOAD, CMD_ANALYZE, CMD_BRIGHTEN,
 * CMD_EQUALIZE, CMD_SHARPEN, CMD_SKIP, CMD_SAVE, and CMD_TERMINATE.
 * Exits cleanly on CMD_TERMINATE or if the parent pipe closes. */
int run_worker(int cmd_fd, int resp_fd);

#endif /* WORKER_H */
