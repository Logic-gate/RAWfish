/*
 * Host test of the JPEG writer (sailfish/yuvjpeg.c): the buffer it returns
 * must be a whole, valid JPEG that the caller owns.
 *
 * The images are noisy on purpose: their JPEGs are far larger than
 * libjpeg's first 4 KB output buffer, so libjpeg has to grow it and free
 * the old one. A writer that returned that first buffer (0.3.0 did) passes
 * a test on a small flat image and crashes on every real photo; here it is
 * caught, and at once under AddressSanitizer (check-host.sh runs this test
 * with -fsanitize=address,undefined).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <jpeglib.h>
#include "yuvjpeg.h"

static int failures;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); ++failures; } } while (0)

/* Decodes a JPEG held in memory; returns 0 and its size, and the mean of its
 * luminance, or -1 if libjpeg does not take it. */
static int decode(const uint8_t *jpg, unsigned long len, int *w, int *h, double *mean_y)
{
    struct jpeg_decompress_struct d;
    struct jpeg_error_mgr err;
    JSAMPROW row;
    double sum = 0;
    long n = 0;
    d.err = jpeg_std_error(&err);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, (unsigned char *)jpg, len);
    if (jpeg_read_header(&d, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&d); return -1; }
    d.out_color_space = JCS_YCbCr;
    jpeg_start_decompress(&d);
    *w = (int)d.output_width; *h = (int)d.output_height;
    row = malloc((size_t)d.output_width * 3);
    while (d.output_scanline < d.output_height) {
        JDIMENSION x;
        jpeg_read_scanlines(&d, &row, 1);
        for (x = 0; x < d.output_width; ++x) { sum += row[x * 3]; ++n; }
    }
    free(row);
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    *mean_y = n ? sum / n : 0;
    return 0;
}

/* One image of w x h, luminance = noise around `level`; encodes it, checks
 * the result, frees it. */
static void one(int w, int h, int level, int quality)
{
    ir_yuv_t f;
    uint8_t *jpg = NULL;
    unsigned long len = 0;
    unsigned seed = 12345u + (unsigned)(w * 31 + h);
    double mean_in = 0, mean_out = 0;
    int i, rc, dw = 0, dh = 0;
    char what[96];
    memset(&f, 0, sizeof f);
    f.w = w; f.h = h;
    f.y = malloc((size_t)w * h); f.u = malloc((size_t)(w / 2) * (h / 2)); f.v = malloc((size_t)(w / 2) * (h / 2));
    for (i = 0; i < w * h; ++i) {
        int v;
        seed = seed * 1103515245u + 12345u;
        v = level + (int)((seed >> 16) % 61) - 30;            /* +-30 of noise */
        f.y[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        mean_in += f.y[i];
    }
    mean_in /= (double)w * h;
    for (i = 0; i < (w / 2) * (h / 2); ++i) { seed = seed * 1103515245u + 12345u; f.u[i] = (uint8_t)(118 + (seed >> 16) % 21); f.v[i] = (uint8_t)(118 + (seed >> 20) % 21); }

    rc = yuv_encode_jpeg(&f, quality, &jpg, &len);
    snprintf(what, sizeof what, "%dx%d q%d: encoded (rc %d)", w, h, quality, rc);
    CHECK(rc == 0 && jpg != NULL && len > 0, what);
    if (rc == 0 && jpg) {
        if (w * h >= 256 * 256) {                             /* the point of the test: the buffer had to grow */
            snprintf(what, sizeof what, "%dx%d q%d: larger than libjpeg's first 4 KB buffer (%lu bytes)", w, h, quality, len);
            CHECK(len > 4096, what);
        }
        snprintf(what, sizeof what, "%dx%d q%d: starts FFD8, ends FFD9", w, h, quality);
        CHECK(jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg[len - 2] == 0xFF && jpg[len - 1] == 0xD9, what);
        snprintf(what, sizeof what, "%dx%d q%d: decodes to the same size", w, h, quality);
        CHECK(decode(jpg, len, &dw, &dh, &mean_out) == 0 && dw == w && dh == h, what);
        snprintf(what, sizeof what, "%dx%d q%d: same mean luminance (%.1f in, %.1f out)", w, h, quality, mean_in, mean_out);
        CHECK(mean_out > mean_in - 2.0 && mean_out < mean_in + 2.0, what);
    }
    free(jpg);                                                /* the caller owns it */
    free(f.y); free(f.u); free(f.v);
}

int main(void)
{
    ir_yuv_t bad;
    uint8_t *jpg = NULL;
    unsigned long len = 0;
    one(256, 256, 128, 95);        /* a small noisy image: already several buffers */
    one(1024, 768, 90, 95);        /* the buffer grows many times */
    one(640, 480, 200, 60);        /* a lower quality */
    one(50, 34, 128, 100);         /* sizes that are not multiples of 16 */
    memset(&bad, 0, sizeof bad);
    CHECK(yuv_encode_jpeg(&bad, 95, &jpg, &len) != 0 && jpg == NULL, "an empty frame is refused, nothing returned");
    if (failures) { printf("yuvjpeg: FAILED (%d)\n", failures); return 1; }
    printf("yuvjpeg: OK (4 images encoded, 3 grown past libjpeg's first buffer, decoded back; bad input refused)\n");
    return 0;
}
