#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kmeans.h"

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; printf("  PASS: %s\n", msg); } \
    else       { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); } \
} while(0)

/* ---- Test 1: 3 clearly-blue + 3 clearly-red pixels, k=2 ---- */
static void test_two_clusters(void)
{
    printf("Test 1: 3 blue + 3 red pixels, k=2\n");

    /* 6 pixels: blue then red */
    uint8_t pixels[6*3] = {
        0,   0, 255,  /* blue */
        0,   0, 240,  /* blue */
        0,  10, 250,  /* blue */
        255,  0,   0,  /* red  */
        240,  5,   0,  /* red  */
        250,  0,  10,  /* red  */
    };
    uint8_t labels[6];
    float   centroids[2*3];

    int iters = kmeans(pixels, 6, 2, labels, centroids, 50);
    CHECK(iters > 0, "ran at least 1 iteration");

    /* All blues should share a label, all reds should share a different label */
    uint8_t blue_lbl = labels[0];
    uint8_t red_lbl  = labels[3];
    CHECK(blue_lbl != red_lbl, "blue and red in different clusters");
    CHECK(labels[1] == blue_lbl && labels[2] == blue_lbl,
          "all blue pixels share same label");
    CHECK(labels[4] == red_lbl && labels[5] == red_lbl,
          "all red pixels share same label");
}

/* ---- Test 2: all identical pixels, k=3 — must not crash ---- */
static void test_identical_pixels(void)
{
    printf("Test 2: all identical pixels (128,128,128), k=3 — no crash\n");

    uint8_t pixels[9*3];
    for (int i = 0; i < 9*3; i++) pixels[i] = 128;

    uint8_t labels[9];
    float   centroids[3*3];

    /* Should complete without segfault / infinite loop */
    int iters = kmeans(pixels, 9, 3, labels, centroids, 50);
    CHECK(iters >= 0, "kmeans returned without crash");

    /* Labels should all be valid cluster indices */
    int valid = 1;
    for (int i = 0; i < 9; i++) {
        if (labels[i] >= 3) { valid = 0; break; }
    }
    CHECK(valid, "all labels in range [0, k)");
}

int main(void)
{
    printf("=== test_kmeans ===\n");
    test_two_clusters();
    test_identical_pixels();
    printf("\nResults: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
