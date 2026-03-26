#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ppm.h"

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; printf("  PASS: %s\n", msg); } \
    else       { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); } \
} while(0)

/* ---- helper: build a 4x4 test image with known pixel values ---- */
static ppm_t *make_4x4(void)
{
    ppm_t *img = ppm_alloc(4, 4);
    if (!img) return NULL;
    for (uint32_t i = 0; i < 16; i++) {
        img->data[i*3 + 0] = (uint8_t)(i * 16);        /* R */
        img->data[i*3 + 1] = (uint8_t)(255 - i * 16);  /* G */
        img->data[i*3 + 2] = (uint8_t)(i % 3 * 80);    /* B */
    }
    return img;
}

/* ---- Test 1: write full image, read back, compare pixel-by-pixel ---- */
static void test_write_read_roundtrip(void)
{
    printf("Test 1: full image write/read roundtrip\n");
    const char *path = "/tmp/test_ppm_full.ppm";

    ppm_t *orig = make_4x4();
    CHECK(orig != NULL, "ppm_alloc 4x4");

    CHECK(ppm_write(orig, path) == 0, "ppm_write");

    ppm_t *back = ppm_read(path);
    CHECK(back != NULL, "ppm_read back");

    if (back) {
        CHECK(back->width  == 4, "width matches");
        CHECK(back->height == 4, "height matches");

        int match = 1;
        for (uint32_t i = 0; i < 16 * 3; i++) {
            if (orig->data[i] != back->data[i]) { match = 0; break; }
        }
        CHECK(match, "pixel data matches byte-for-byte");
        ppm_free(back);
    }
    ppm_free(orig);
}

/* ---- Test 2: write strip (rows 1-2 of 4x4), read strip, verify ---- */
static void test_strip_write_read(void)
{
    printf("Test 2: strip write/read (rows 1-2 of 4x4)\n");
    const char *path = "/tmp/test_ppm_strip.ppm";

    ppm_t *orig = make_4x4();
    CHECK(orig != NULL, "ppm_alloc 4x4 for strip test");

    /* Write only rows 1..3 (exclusive), i.e. rows 1 and 2 */
    CHECK(ppm_write_strip(orig, path, 1, 3) == 0, "ppm_write_strip rows 1-2");

    /* Read as a 2-row strip */
    ppm_t *strip = ppm_read_strip(path, 4, 0, 2);
    CHECK(strip != NULL, "ppm_read_strip");

    if (strip) {
        CHECK(strip->width  == 4, "strip width == 4");
        CHECK(strip->height == 2, "strip height == 2");

        /* Compare against rows 1-2 of original */
        int match = 1;
        for (uint32_t row = 0; row < 2; row++) {
            for (uint32_t x = 0; x < 4; x++) {
                uint32_t oi = (1 + row) * 4 + x;
                uint32_t si = row * 4 + x;
                for (int c = 0; c < 3; c++) {
                    if (orig->data[oi*3+c] != strip->data[si*3+c]) {
                        match = 0;
                        break;
                    }
                }
            }
        }
        CHECK(match, "strip pixel data matches original rows 1-2");
        ppm_free(strip);
    }
    ppm_free(orig);
}

int main(void)
{
    printf("=== test_ppm ===\n");
    test_write_read_roundtrip();
    test_strip_write_read();
    printf("\nResults: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
