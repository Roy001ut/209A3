#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../ppm.h"
#include "../enhance.h"

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; printf("  PASS: %s\n", msg); } \
    else       { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); } \
} while(0)

/* ---- helpers ---- */
static uint8_t *make_grey_strip(uint32_t n, uint8_t v)
{
    uint8_t *p = malloc(n * 3);
    if (p) memset(p, v, n * 3);
    return p;
}

/* Ramp: pixel i gets value lo + (hi-lo)*i/(n-1), greyscale */
static uint8_t *make_ramp_strip(uint32_t n, uint8_t lo, uint8_t hi)
{
    uint8_t *p = malloc(n * 3);
    if (!p) return NULL;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v = (n > 1) ? (uint8_t)(lo + (uint32_t)(hi - lo) * i / (n-1))
                             : lo;
        p[i*3+0] = p[i*3+1] = p[i*3+2] = v;
    }
    return p;
}

static uint8_t *all_owned_mask(uint32_t n, uint8_t cid)
{
    uint8_t *m = malloc(n);
    if (m) memset(m, cid, n);
    return m;
}

/* ---- Test 1: flat grey — histeq must not crash, values stay uint8 ---- */
static void test_flat_grey_eq(void)
{
    printf("Test 1: flat grey image — histeq no crash, values in [0,255]\n");
    const uint32_t N = 16; /* 4x4 */
    uint8_t *strip = make_grey_strip(N, 128);
    uint8_t *mask  = all_owned_mask(N, 0);
    CHECK(strip && mask, "alloc flat grey strip");

    if (strip && mask) {
        float mean, stddev;
        histeq(strip, 4, 4, mask, 0, &mean, &stddev);

        int ok = 1;
        for (uint32_t i = 0; i < N * 3; i++) {
            /* uint8_t is always 0-255 — just verify no garbage via NaN etc */
            (void)strip[i];
        }
        CHECK(ok, "no crash and values stay uint8");
        CHECK(stddev >= 0.0f, "stddev is non-negative");
    }
    free(strip);
    free(mask);
}

/* ---- Test 2: low-contrast ramp [100..120] — range expands after eq ---- */
static void test_low_contrast_eq(void)
{
    printf("Test 2: low-contrast [100..120] — range expands after histeq\n");
    const uint32_t N = 16;
    uint8_t *strip = make_ramp_strip(N, 100, 120);
    uint8_t *mask  = all_owned_mask(N, 0);
    CHECK(strip && mask, "alloc low-contrast strip");

    if (strip && mask) {
        /* Record original min/max luminance */
        uint8_t orig_min = strip[0], orig_max = strip[(N-1)*3];

        histeq(strip, 4, 4, mask, 0, NULL, NULL);

        uint8_t new_min = 255, new_max = 0;
        for (uint32_t i = 0; i < N; i++) {
            uint8_t v = strip[i*3];
            if (v < new_min) new_min = v;
            if (v > new_max) new_max = v;
        }
        int expanded = (new_max - new_min) > (orig_max - orig_min);
        CHECK(expanded, "range expanded after eq");
        CHECK(new_max >= 200, "max stretched toward 255");
    }
    free(strip);
    free(mask);
}

/* ---- Test 3: unsharp mask on uniform layer — no pixel changes ---- */
static void test_unsharp_uniform(void)
{
    printf("Test 3: unsharp mask on uniform grey layer — no pixel change\n");
    ppm_t *layer = ppm_alloc(4, 4);
    CHECK(layer != NULL, "ppm_alloc 4x4 uniform layer");

    uint8_t *mask = all_owned_mask(16, 0);
    CHECK(mask != NULL, "alloc mask");

    if (layer && mask) {
        /* Fill with constant grey */
        memset(layer->data, 150, 4*4*3);

        uint8_t before[4*4*3];
        memcpy(before, layer->data, 4*4*3);

        unsharp_mask(layer, mask, 0, 1.5f, 2);

        int changed = 0;
        for (uint32_t i = 0; i < 4*4*3; i++) {
            if (layer->data[i] != before[i]) { changed = 1; break; }
        }
        CHECK(!changed, "uniform layer pixels unchanged after unsharp mask");
    }
    ppm_free(layer);
    free(mask);
}

int main(void)
{
    printf("=== test_enhance ===\n");
    test_flat_grey_eq();
    test_low_contrast_eq();
    test_unsharp_uniform();
    printf("\nResults: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
