# CSC209 A3 — Adaptive Image Enhancement via Parallel Region Processing
## Claude Code Implementation Plan

---

## Project overview

Build a C program `imageproc` that enhances a PPM image using a three-level parallel
process tree. The parent segments the image into colour regions via k-means, forks one
worker per region, each worker forks tile subprocesses, and the parent stitches the
results into an enhanced output image.

All IPC must use pipes carrying fixed-width binary structs. No shared memory, no
signals for coordination, no external image libraries. C99 + POSIX only.

---

## Directory layout to create

```
imageproc/
├── Makefile
├── imageproc.c        # main() — arg parsing, orchestrates parent logic
├── ppm.h / ppm.c      # PPM read/write
├── kmeans.h / kmeans.c
├── protocol.h         # all 4 message structs
├── parent.h / parent.c
├── worker.h / worker.c
├── tile.h / tile.c
├── enhance.h / enhance.c   # histogram eq + unsharp mask
└── tests/
    ├── test_ppm.c
    ├── test_kmeans.c
    └── test_enhance.c
```

---

## Build steps — implement in this order

### Step 1 — protocol.h (no dependencies, do this first)

Define all four message structs. Every struct is fixed-width — no pointers, no
variable-length fields. The mask is sent separately (see Step 4).

```c
#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <stdint.h>

#define MAX_PATH   256
#define MAX_PIXELS (4096 * 4096)   /* hard ceiling for mask alloc */

/* MSG 1: parent -> worker */
typedef struct {
    uint8_t  cluster_id;
    uint32_t tile_count;
    uint32_t img_width;
    uint32_t img_height;
    char     infile[MAX_PATH];
    char     outfile[MAX_PATH];
    /* mask sent separately: write(fd, mask, width*height) after this struct */
} layer_job_t;

/* MSG 2: worker -> tile */
typedef struct {
    uint8_t  cluster_id;
    uint32_t row_start;
    uint32_t row_end;         /* exclusive */
    uint32_t img_width;
    uint32_t img_height;
    uint32_t mask_offset;     /* byte offset into mask for this tile's rows */
    char     infile[MAX_PATH];
    char     tmp_outfile[MAX_PATH];
} tile_job_t;

/* MSG 3: tile -> worker */
typedef struct {
    uint8_t  status;          /* 0=ok, 1=error */
    uint32_t rows_written;
    uint32_t pixels_owned;    /* pixels in tile belonging to this cluster */
    float    lum_mean;
    float    lum_stddev;
    char     tmp_outfile[MAX_PATH];
} tile_result_t;

/* MSG 4: worker -> parent */
typedef struct {
    uint8_t  status;          /* 0=complete, 1=partial, 2=failed */
    uint8_t  cluster_id;
    uint8_t  tiles_ok;
    uint32_t pixels_total;
    float    avg_lum_before;
    float    avg_lum_after;
    char     layer_file[MAX_PATH];
} layer_result_t;

#endif
```

---

### Step 2 — ppm.h / ppm.c

PPM is a trivially simple format. Parse entirely with fread/fwrite — no libraries.

**ppm.h:**
```c
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *data;   /* interleaved RGB, row-major: data[(y*width+x)*3 + c] */
} ppm_t;

ppm_t *ppm_read(const char *path);
int    ppm_write(const ppm_t *img, const char *path);
ppm_t *ppm_alloc(uint32_t width, uint32_t height);
void   ppm_free(ppm_t *img);

/* write/read a pixel strip (rows [row_start, row_end)) */
int    ppm_write_strip(const ppm_t *img, const char *path,
                       uint32_t row_start, uint32_t row_end);
ppm_t *ppm_read_strip(const char *path, uint32_t width,
                      uint32_t row_start, uint32_t row_end);
```

**PPM format to parse:**
```
P6\n
<width> <height>\n
255\n
<binary RGB data, 3 bytes per pixel, row-major>
```

Handle:
- Comment lines starting with `#` in the header (skip them)
- The header is ASCII text; binary pixel data follows immediately after the `\n`
  after `255`
- Use `fread` for the pixel data block, not `fscanf`

**Strip files:** tile subprocesses write only their row range. Format: write a minimal
PPM header with the strip's height (not the full image height), then the strip's rows.
The worker stitches strips back together in row order.

---

### Step 3 — kmeans.h / kmeans.c

Pure C k-means on RGB pixels. No random initialisation — use evenly spaced pixel
indices so output is deterministic.

```c
/*
 * Segment `n_pixels` pixels into `k` clusters.
 * pixels: array of n_pixels * 3 bytes (R,G,B interleaved)
 * labels: output array of n_pixels uint8_t, caller allocates
 * centroids: output array of k * 3 floats (R,G,B), caller allocates
 * max_iter: stop after this many iterations even if not converged
 * returns: number of iterations run
 */
int kmeans(const uint8_t *pixels, uint32_t n_pixels, int k,
           uint8_t *labels, float *centroids, int max_iter);
```

**Algorithm:**
1. Init centroids: `centroid[i] = pixel[i * (n_pixels/k)]` for i in 0..k-1
2. Assign: for each pixel, find nearest centroid by squared L2 in RGB space
3. Update: recompute each centroid as mean of its assigned pixels
4. Repeat until no label changes or max_iter reached
5. Return iteration count

**Watch out for:** empty clusters (no pixels assigned). If a centroid has zero pixels,
reinitialise it to the pixel furthest from any centroid to avoid degeneracy.

---

### Step 4 — parent.c

Parent logic lives here. Called from `main()` after arg parsing.

```c
int run_parent(const char *infile, const char *outfile, int k, int tiles_per_worker);
```

**Parent flow:**

```
1. ppm_read(infile)
2. kmeans(img->data, width*height, k, labels, centroids, 50)
3. for each cluster i:
     a. pipe(job_pipe[i])    /* parent writes MSG1 + mask */
     b. pipe(result_pipe[i]) /* worker writes MSG4 */
     c. fork() -> worker process (see worker.c)
        parent side: close unused pipe ends
        child side:  call run_worker(job_fd, result_fd)
4. for each cluster i:
     write(job_pipe[i][1], &job, sizeof(layer_job_t))
     write(job_pipe[i][1], labels, width*height)   /* mask */
     close(job_pipe[i][1])
5. for each cluster i:
     read(result_pipe[i][0], &result, sizeof(layer_result_t))
     close(result_pipe[i][0])
6. stitch: read each layer_file, copy cluster-owned pixels into output image
7. ppm_write(output, outfile)
8. waitpid() all worker PIDs
9. print summary
```

**Stitching in step 6:**
Each layer file is a full-size PPM written by the worker containing only the pixels it
owns (other pixels zeroed or copied from original). For each pixel i, check
`labels[i] == cluster_id` and copy from that layer file. Because layers don't overlap,
each output pixel is written exactly once.

---

### Step 5 — worker.c

Worker logic. Each worker is a forked child; it calls `run_worker()` and exits.

```c
int run_worker(int job_fd, int result_fd);
```

**Worker flow:**

```
1. read(job_fd, &job, sizeof(layer_job_t))
2. mask = malloc(job.img_width * job.img_height)
   read(job_fd, mask, job.img_width * job.img_height)
   close(job_fd)

3. compute avg_lum_before: iterate owned pixels, compute mean luminance

4. divide rows into job.tile_count equal strips
   for each tile t:
     a. pipe(tile_job_pipe[t])
     b. pipe(tile_result_pipe[t])
     c. fork() -> tile process
        child: call run_tile(tile_job_fd, tile_result_fd)
        parent: close unused ends

5. for each tile t:
     build tile_job_t (row_start, row_end, mask_offset, tmp_outfile path)
     tmp path: e.g. "/tmp/imageproc_c{cluster}_t{tile}.ppm"
     write(tile_job_pipe[t][1], &tile_job, sizeof(tile_job_t))
     write(tile_job_pipe[t][1], mask + tile_job.mask_offset,
           (row_end - row_start) * img_width)
     close(tile_job_pipe[t][1])

6. collect tile results:
     for each tile t:
       read(tile_result_pipe[t][0], &tres, sizeof(tile_result_t))
       if EOF before full read: mark tile as failed
       close(tile_result_pipe[t][0])

7. assemble layer:
     stitch tile strip files into one full-size PPM layer file
     (only rows/pixels owned by this cluster are set; rest zeroed)

8. apply unsharp mask to the assembled layer (see enhance.c)
   strength = calibrate_strength(tile_results)  /* use lum_stddev */

9. compute avg_lum_after

10. build layer_result_t and write(result_fd, &result, sizeof(layer_result_t))
    close(result_fd)

11. waitpid() all tile PIDs
12. free(mask), exit(0)
```

**Unsharp strength calibration from tile stats:**
Average the `lum_stddev` values across all successful tiles. Map stddev to strength:
- stddev < 15: flat region, strength = 0.5 (avoid noise amplification)
- 15 <= stddev < 40: moderate detail, strength = 1.2
- stddev >= 40: high detail (fur, edges), strength = 2.0

---

### Step 6 — tile.c

Tile logic. Each tile is a forked grandchild.

```c
int run_tile(int job_fd, int result_fd);
```

**Tile flow:**

```
1. read(job_fd, &job, sizeof(tile_job_t))
2. n_mask_bytes = (job.row_end - job.row_start) * job.img_width
   tile_mask = malloc(n_mask_bytes)
   read(job_fd, tile_mask, n_mask_bytes)
   close(job_fd)

3. ppm_read_strip(job.infile, job.img_width, job.row_start, job.row_end)

4. identify owned pixels: tile_mask[local_y * width + x] == job.cluster_id

5. build luminance histogram over owned pixels only (256 bins)

6. build CDF from histogram
   find cdf_min (first non-zero bin)
   build LUT: lut[v] = round((cdf[v] - cdf_min) / (n_owned - cdf_min) * 255)

7. apply LUT to owned pixels:
   for each owned pixel (r, g, b):
     lum = 0.299*r + 0.587*g + 0.114*b
     if lum == 0: skip
     scale = lut[lum] / lum
     new_r = clamp(r * scale, 0, 255)
     new_g = clamp(g * scale, 0, 255)
     new_b = clamp(b * scale, 0, 255)

8. ppm_write_strip(result, job.tmp_outfile, 0, row_end - row_start)

9. compute lum_mean and lum_stddev over owned pixels

10. build tile_result_t, write(result_fd, &tres, sizeof(tile_result_t))
    close(result_fd)

11. free everything, exit(0)
```

---

### Step 7 — enhance.c

Unsharp mask applied by the worker after tile assembly.

```c
/*
 * Apply unsharp mask in-place to owned pixels of layer.
 * mask: which pixels this worker owns
 * amount: sharpening strength (e.g. 1.5)
 * radius: blur kernel radius in pixels (use 2)
 */
void unsharp_mask(ppm_t *layer, const uint8_t *mask, uint8_t cluster_id,
                  float amount, int radius);
```

**Algorithm:**
1. Box blur: for each pixel, compute average of radius*2+1 square neighbourhood.
   Only average over pixels in the same cluster (mask check) to avoid bleed across
   region boundaries.
2. Detail = original - blurred (can be negative, use int16_t)
3. Sharpened = clamp(original + amount * detail, 0, 255)
4. Apply only to owned pixels.

---

### Step 8 — imageproc.c (main)

```c
int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "usage: %s <input.ppm> <output.ppm> [k] [tiles]\n", argv[0]);
        return 1;
    }
    const char *infile  = argv[1];
    const char *outfile = argv[2];
    int k     = argc >= 4 ? atoi(argv[3]) : 3;
    int tiles = argc >= 5 ? atoi(argv[4]) : 2;

    if (k < 1 || k > 8)     { fprintf(stderr, "k must be 1-8\n");     return 1; }
    if (tiles < 1 || tiles > 8) { fprintf(stderr, "tiles must be 1-8\n"); return 1; }

    return run_parent(infile, outfile, k, tiles);
}
```

---

### Step 9 — Makefile

```makefile
CC     = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -D_POSIX_C_SOURCE=200809L
SRCS   = imageproc.c ppm.c kmeans.c parent.c worker.c tile.c enhance.c
OBJS   = $(SRCS:.c=.o)
TARGET = imageproc

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) /tmp/imageproc_*.ppm

test: $(TARGET)
	./$(TARGET) tests/cat.ppm tests/output.ppm 3 2
```

---

## Pipe hygiene rules — apply everywhere

These are the most common source of hangs and unexpected EOF. Follow them exactly.

- After `fork()`, the parent must close the child-side ends of every pipe immediately.
  The child must close the parent-side ends.
- After finishing all writes to a pipe, `close()` the write end immediately. The
  reader will get EOF only when ALL write ends are closed.
- Never read from a pipe before closing your own write end of it (deadlock risk when
  the pipe buffer fills).
- Check every `read()` return value:
  - `== sizeof(struct)`: success
  - `== 0`: EOF — writer closed or died
  - `== -1`: error — log with perror and handle
- Check every `write()` return value. A short write means the reader closed early
  (broken pipe). Log it and handle gracefully.

---

## Error handling requirements

| Situation | Required behaviour |
|---|---|
| Input file not found | print error to stderr, exit 1 |
| Tile subprocess crashes (EOF on result pipe before full read) | worker marks tile failed, continues with remaining tiles, sets status=partial in MSG4 |
| Worker crashes (EOF on result pipe) | parent marks cluster failed, stitches without that layer, prints warning |
| Malloc failure | print error, exit(1) — don't continue with null pointer |
| write() returns < expected | print error with perror, treat as failure for that pipe |

---

## Temporary file naming convention

Use a predictable pattern that includes PID to avoid collisions if run concurrently:

```c
/* tile tmp files */
snprintf(job.tmp_outfile, MAX_PATH,
         "/tmp/imageproc_%d_c%d_t%d.ppm", getpid(), cluster_id, tile_idx);

/* worker layer files */
snprintf(result.layer_file, MAX_PATH,
         "/tmp/imageproc_%d_layer%d.ppm", getppid(), cluster_id);
```

Clean up all `/tmp/imageproc_*.ppm` files on successful exit. On error exit, leave
them for debugging.

---

## Testing plan

### Unit tests (no forking needed)

1. `test_ppm.c` — write a 4x4 PPM, read it back, compare pixel-by-pixel
2. `test_ppm.c` — write a strip (rows 1-2 of a 4x4), read strip, verify dimensions
3. `test_kmeans.c` — 6 pixels: 3 clearly blue, 3 clearly red, k=2. Assert labels split correctly.
4. `test_kmeans.c` — all identical pixels, k=3. Assert no crash (empty cluster handling).
5. `test_enhance.c` — flat grey image, histogram eq should be identity (already full range)
6. `test_enhance.c` — low-contrast image (all pixels 100-120), verify eq stretches to ~0-255
7. `test_enhance.c` — unsharp mask on uniform region (stddev 0) should change nothing

### Integration tests

```bash
# Basic run — should exit 0 and produce output file
./imageproc tests/cat.ppm /tmp/out.ppm 3 2
test -f /tmp/out.ppm && echo PASS

# k=1 — single worker, single cluster, should work as a no-op segmentation
./imageproc tests/cat.ppm /tmp/out_k1.ppm 1 2

# k=5 — stress test more workers
./imageproc tests/cat.ppm /tmp/out_k5.ppm 5 2

# Output should differ from input (enhancement actually did something)
# Compare file sizes as a rough proxy — output should not be identical bytes
cmp -s tests/cat.ppm /tmp/out.ppm && echo "FAIL: output identical to input" || echo PASS
```

### Valgrind check

```bash
valgrind --leak-check=full --track-fds=yes ./imageproc tests/cat.ppm /tmp/val_out.ppm 3 2
```

Target: zero definitely lost, zero file descriptors open at exit beyond stdin/stdout/stderr.

---

## Common pitfalls to avoid

- **Forgetting to send the mask separately.** The mask array is `width*height` bytes.
  It cannot fit in the fixed-width struct. Write the struct first, then write the mask
  bytes in a second write() call. The reader does two reads.
- **Row count mismatch in strip files.** The strip PPM header must say the strip's
  height, not the full image height. A common bug is writing the full height in the
  header and only writing partial pixel data — ppm_read will then block waiting for
  more bytes.
- **Box blur crossing cluster boundaries.** When blurring in `unsharp_mask()`, only
  include neighbours that belong to the same cluster. Without this check, a sharp
  boundary between clusters blurs across the boundary and creates visible halos.
- **Integer overflow in histogram CDF.** A 4096x4096 image has 16M pixels. The CDF
  accumulates counts — use `uint32_t` not `uint16_t` for CDF bins.
- **Float precision in lum_mean/stddev.** Accumulate in `double`, cast to `float`
  only when writing into the struct.
- **Not calling waitpid().** After collecting all pipe results, always call waitpid()
  on every forked PID. Without it, zombie processes accumulate.

---

## Suggested implementation order

1. `protocol.h` — structs only, no logic
2. `ppm.c` + unit test — get I/O working before anything parallel
3. `kmeans.c` + unit test — verify segmentation on simple cases
4. `enhance.c` + unit test — verify hist eq and unsharp independently
5. `tile.c` — simplest process level, reads job and writes result
6. `worker.c` — forks tiles, collects results, assembles layer
7. `parent.c` — forks workers, runs k-means, stitches final output
8. `imageproc.c` — wire everything together with arg parsing
9. Integration test + valgrind

Build and test each step before moving to the next. The parallel layers are much
easier to debug when the underlying image and enhancement code is already verified.
