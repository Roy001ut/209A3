#include "ppm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/*
 * Read a token from a PPM header, skipping '#' comment lines.
 * Returns 1 on success, 0 on EOF/error.
 */
static int read_token(FILE *f, char *buf, size_t bufsize)
{
    int c;

    /* skip whitespace and comments */
    for (;;) {
        /* skip whitespace */
        while ((c = fgetc(f)) != EOF && (c == ' ' || c == '\t' ||
                                          c == '\n' || c == '\r'))
            ;
        if (c == EOF)
            return 0;
        if (c == '#') {
            /* skip rest of comment line */
            while ((c = fgetc(f)) != EOF && c != '\n')
                ;
        } else {
            ungetc(c, f);
            break;
        }
    }

    /* read token until whitespace */
    size_t i = 0;
    while ((c = fgetc(f)) != EOF && c != ' ' && c != '\t' &&
           c != '\n' && c != '\r') {
        if (i + 1 < bufsize)
            buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return (i > 0) ? 1 : 0;
}

/*
 * Write a minimal P6 PPM header to an open file.
 */
static int write_header(FILE *f, uint32_t width, uint32_t height)
{
    return fprintf(f, "P6\n%u %u\n255\n", width, height) > 0 ? 0 : -1;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

ppm_t *ppm_alloc(uint32_t width, uint32_t height)
{
    ppm_t *img = malloc(sizeof(ppm_t));
    if (!img) {
        perror("ppm_alloc: malloc ppm_t");
        return NULL;
    }
    img->width  = width;
    img->height = height;
    img->data   = calloc((size_t)width * height, 3);
    if (!img->data) {
        perror("ppm_alloc: calloc data");
        free(img);
        return NULL;
    }
    return img;
}

void ppm_free(ppm_t *img)
{
    if (!img) return;
    free(img->data);
    free(img);
}

ppm_t *ppm_read(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }

    char tok[64];

    /* magic */
    if (!read_token(f, tok, sizeof(tok)) || strcmp(tok, "P6") != 0) {
        fprintf(stderr, "ppm_read: %s is not a P6 PPM\n", path);
        fclose(f);
        return NULL;
    }

    /* width */
    if (!read_token(f, tok, sizeof(tok))) goto bad_header;
    uint32_t width = (uint32_t)atoi(tok);

    /* height */
    if (!read_token(f, tok, sizeof(tok))) goto bad_header;
    uint32_t height = (uint32_t)atoi(tok);

    /* maxval — must be 255 */
    if (!read_token(f, tok, sizeof(tok))) goto bad_header;
    if (atoi(tok) != 255) {
        fprintf(stderr, "ppm_read: %s has maxval %s (only 255 supported)\n",
                path, tok);
        fclose(f);
        return NULL;
    }

    if (width == 0 || height == 0) {
        fprintf(stderr, "ppm_read: %s has zero dimension\n", path);
        fclose(f);
        return NULL;
    }

    ppm_t *img = ppm_alloc(width, height);
    if (!img) { fclose(f); return NULL; }

    size_t npix = (size_t)width * height * 3;
    if (fread(img->data, 1, npix, f) != npix) {
        fprintf(stderr, "ppm_read: %s truncated pixel data\n", path);
        ppm_free(img);
        fclose(f);
        return NULL;
    }

    fclose(f);
    return img;

bad_header:
    fprintf(stderr, "ppm_read: %s bad header\n", path);
    fclose(f);
    return NULL;
}

int ppm_write(const ppm_t *img, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }

    if (write_header(f, img->width, img->height) != 0) {
        fprintf(stderr, "ppm_write: failed writing header to %s\n", path);
        fclose(f);
        return -1;
    }

    size_t npix = (size_t)img->width * img->height * 3;
    if (fwrite(img->data, 1, npix, f) != npix) {
        fprintf(stderr, "ppm_write: failed writing pixels to %s\n", path);
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

int ppm_write_strip(const ppm_t *img, const char *path,
                    uint32_t row_start, uint32_t row_end)
{
    if (row_start >= row_end || row_end > img->height) {
        fprintf(stderr, "ppm_write_strip: invalid row range [%u, %u) for height %u\n",
                row_start, row_end, img->height);
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }

    uint32_t strip_height = row_end - row_start;

    /* Header uses strip height, NOT full image height */
    if (write_header(f, img->width, strip_height) != 0) {
        fprintf(stderr, "ppm_write_strip: failed writing header to %s\n", path);
        fclose(f);
        return -1;
    }

    size_t row_bytes = (size_t)img->width * 3;
    size_t nbytes    = row_bytes * strip_height;
    const uint8_t *src = img->data + (size_t)row_start * row_bytes;

    if (fwrite(src, 1, nbytes, f) != nbytes) {
        fprintf(stderr, "ppm_write_strip: failed writing pixels to %s\n", path);
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

ppm_t *ppm_read_strip(const char *path, uint32_t width,
                      uint32_t row_start, uint32_t row_end)
{
    if (row_start >= row_end) {
        fprintf(stderr, "ppm_read_strip: invalid row range [%u, %u)\n",
                row_start, row_end);
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }

    char tok[64];

    if (!read_token(f, tok, sizeof(tok)) || strcmp(tok, "P6") != 0) {
        fprintf(stderr, "ppm_read_strip: %s is not a P6 PPM\n", path);
        fclose(f);
        return NULL;
    }

    if (!read_token(f, tok, sizeof(tok))) goto bad;
    uint32_t file_width = (uint32_t)atoi(tok);

    if (!read_token(f, tok, sizeof(tok))) goto bad;
    uint32_t file_height = (uint32_t)atoi(tok);

    if (!read_token(f, tok, sizeof(tok))) goto bad; /* maxval */

    uint32_t strip_height = row_end - row_start;

    if (file_width != width) {
        fprintf(stderr, "ppm_read_strip: %s width %u != expected %u\n",
                path, file_width, width);
        fclose(f);
        return NULL;
    }
    if (file_height < strip_height) {
        fprintf(stderr, "ppm_read_strip: %s height %u < strip height %u\n",
                path, file_height, strip_height);
        fclose(f);
        return NULL;
    }

    ppm_t *img = ppm_alloc(width, strip_height);
    if (!img) { fclose(f); return NULL; }

    size_t nbytes = (size_t)width * strip_height * 3;
    if (fread(img->data, 1, nbytes, f) != nbytes) {
        fprintf(stderr, "ppm_read_strip: %s truncated\n", path);
        ppm_free(img);
        fclose(f);
        return NULL;
    }

    fclose(f);
    return img;

bad:
    fprintf(stderr, "ppm_read_strip: %s bad header\n", path);
    fclose(f);
    return NULL;
}
