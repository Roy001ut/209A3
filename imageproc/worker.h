#ifndef WORKER_H
#define WORKER_H

/*
 * Entry point for a persistent worker (child) process.
 *
 * cmd_fd    : read end of pipe carrying cmd_t structs from parent
 * resp_fd   : write end of pipe for resp_t structs back to parent
 *
 * Runs a while(1) loop reading commands:
 *   CMD_LOAD      — read mask from pipe, load image into memory → RESP_READY
 *   CMD_ANALYZE   — compute lum stats over owned pixels → RESP_STATS
 *   CMD_BRIGHTEN  — multiply owned RGB by param → RESP_DONE
 *   CMD_EQUALIZE  — fork tile subprocesses (TILE_OP_HISTEQ) → RESP_DONE
 *   CMD_SHARPEN   — fork tile subprocesses (TILE_OP_SHARPEN) → RESP_DONE
 *   CMD_SKIP      — no-op → RESP_DONE
 *   CMD_SAVE      — write pixels to outfile as PPM → RESP_DONE
 *   CMD_TERMINATE — free memory, exit(0)
 *
 * EOF on cmd_fd (parent died) → exit(1)
 */
int run_worker(int cmd_fd, int resp_fd);

#endif /* WORKER_H */
