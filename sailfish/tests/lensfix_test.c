/* Host test of sailfish/lensfix.c: a JPEG corrected in place keeps its
 * size, its Exif (the tags written before, the orientation) and gets a new
 * thumbnail; the picture moves where the model says; a frame of the YUV
 * path the same. Run by check-host.sh. */
#define _POSIX_C_SOURCE 200809L
#include "../lensfix.h"
#include "../yuvjpeg.h"
#include "../../android/exifwrite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok: "); else { printf("  FAIL: "); ++fails; } printf(__VA_ARGS__); printf("\n"); } while (0)

static const char *k_model =
    "{\"format\": \"rawfish-ir lens distortion 1\", \"width\": 640, \"height\": 480,"
    " \"fx\": 271.6, \"fy\": 271.2, \"cx\": 320.3, \"cy\": 236.8,"
    " \"dist\": [-0.63654, 0.18024, -0.00057, 0.000059, 0.0, -0.66537, 0.20196, 0.0]}";

/* A grey frame with one white square of 4 x 4 at (x, y). */
static void frame_with_dot(ir_yuv_t *f, int w, int h, int x, int y)
{
    int i, j;
    memset(f, 0, sizeof *f);
    f->w = w; f->h = h;
    f->y = malloc((size_t)w * h); f->u = malloc((size_t)w * h / 4); f->v = malloc((size_t)w * h / 4);
    memset(f->y, 60, (size_t)w * h); memset(f->u, 128, (size_t)w * h / 4); memset(f->v, 128, (size_t)w * h / 4);
    for (j = -2; j < 2; ++j) for (i = -2; i < 2; ++i) f->y[(size_t)(y + j) * w + x + i] = 250;
}
/* The centre of the bright pixels of a luma plane. */
static void bright_centre(const uint8_t *p, int w, int h, double *cx, double *cy)
{
    double sx = 0, sy = 0, n = 0;
    int x, y;
    for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) if (p[(size_t)y * w + x] > 150) { double wt = p[(size_t)y * w + x] - 60; sx += x * wt; sy += y * wt; n += wt; }
    *cx = n ? sx / n : -1; *cy = n ? sy / n : -1;
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc((size_t)n);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return p;
}

int main(void)
{
    const char *path = "/tmp/indraw_lensfix_test.jpg";
    ld_model_t m;
    ld_view_t v;
    char err[300];
    ir_yuv_t f, out, back;
    uint8_t *jpg, *file;
    unsigned long jlen;
    size_t flen;
    exif_info_t ex;
    double su, sv, u, vv, cx, cy;
    const int dx = 40, dy = 40;                       /* the dot near a corner: where the lens bends most */

    printf("lensfix:\n");
    CHECK(ld_parse(k_model, &m, err, sizeof err) == 0, "the model parses");
    ld_bind(&m, 0, 0);
    ld_view_init(&v, &m, 640, 480, NULL);
    ld_dst(&v, dx - 0.5, dy - 0.5, &u, &vv);         /* where the dot should land, corrected */

    /* --- the YUV path --- */
    frame_with_dot(&f, 640, 480, dx, dy);
    snprintf(f.make, sizeof f.make, "Jolla");
    CHECK(lensfix_yuv(&m, NULL, &f, &out) == 0 && out.w == 640 && out.h == 480 && !strcmp(out.make, "Jolla"),
          "a YUV frame corrected, its size and fields kept");
    bright_centre(out.y, out.w, out.h, &cx, &cy);
    CHECK(fabs(cx - u) < 0.6 && fabs(cy - vv) < 0.6, "the dot at (%.1f, %.1f) moved to (%.1f, %.1f), the model says (%.1f, %.1f)",
          dx - 0.5, dy - 0.5, cx, cy, u, vv);
    ld_src(&v, cx, cy, &su, &sv);
    CHECK(hypot(su - (dx - 0.5), sv - (dy - 0.5)) < 0.6, "and the model takes it back to the lens' point");
    free(out.y); free(out.u); free(out.v);

    /* --- a JPEG with Exif, corrected in place --- */
    CHECK(yuv_encode_jpeg(&f, 95, &jpg, &jlen) == 0, "a test JPEG");
    memset(&ex, 0, sizeof ex);
    ex.make = "Jolla"; ex.model = "JP2601"; ex.software = "RAWfish-ir test"; ex.datetime = "2026:11:25 10:00:00";
    ex.exposure_ns = 10000000; ex.iso = 400; ex.orientation = 6; ex.wb_manual = 1; ex.exposure_manual = 1;
    CHECK(exif_write_jpeg(path, jpg, jlen, &ex) == 0, "with its Exif (orientation 6, manual exposure and white)");
    free(jpg);
    CHECK(lensfix_jpeg_file(&m, NULL, -1, path, path, 92, err, sizeof err) == 0, "corrected in place (%s)", err);
    file = read_all(path, &flen);
    CHECK(file && exif_read_orientation(file, flen) == 6, "its orientation kept");
    {
        int have_make = 0, have_sw = 0, thumb = 0;
        size_t i;
        for (i = 0; file && i + 16 < flen && i < 65536; ++i) {
            if (!memcmp(file + i, "JP2601", 6)) have_make = 1;
            if (!memcmp(file + i, "RAWfish-ir test", 15)) have_sw = 1;
            if (i > 4 && file[i] == 0xFF && file[i + 1] == 0xD8 && file[i + 2] == 0xFF) thumb = 1;
        }
        CHECK(have_make && have_sw, "its Make / Model and Software kept");
        CHECK(thumb, "a thumbnail inside the Exif (the corrected picture's)");
    }
    {   /* the manual white balance and exposure mode (always written by
         * exif_write_jpeg) are the file's, not reset to automatic */
        uint8_t tag_wb[4] = { 0x03, 0xA4, 0x03, 0x00 }, tag_em[4] = { 0x02, 0xA4, 0x03, 0x00 };
        size_t i;
        int wb = -1, em = -1;
        for (i = 0; file && i + 10 < flen && i < 65536; ++i) {
            if (!memcmp(file + i, tag_wb, 4)) wb = file[i + 8];
            if (!memcmp(file + i, tag_em, 4)) em = file[i + 8];
        }
        CHECK(wb == 1 && em == 1, "WhiteBalance and ExposureMode still manual (%d, %d)", wb, em);
    }
    CHECK(file && lensfix_decode(file, flen, &back) == 0 && back.w == 640 && back.h == 480, "the same size");
    if (back.y) {
        bright_centre(back.y, back.w, back.h, &cx, &cy);
        CHECK(fabs(cx - u) < 1.0 && fabs(cy - vv) < 1.0, "the dot where the model says (%.1f, %.1f)", cx, cy);
        free(back.y); free(back.u); free(back.v);
    }
    free(file);
    /* turned pixels (the driver rotated them): refused, the file untouched */
    {
        ir_yuv_t t;
        frame_with_dot(&t, 480, 640, 40, 40);
        yuv_encode_jpeg(&t, 90, &jpg, &jlen);
        memset(&ex, 0, sizeof ex);
        exif_write_jpeg(path, jpg, jlen, &ex);
        CHECK(lensfix_jpeg_file(&m, NULL, -1, path, path, 92, err, sizeof err) != 0, "a portrait JPEG of a landscape sensor, rotation unknown: refused (%s)", err);
        free(jpg); free(t.y); free(t.u); free(t.v);
    }
    /* turned upright by the driver (90 clockwise, no Exif orientation, as
     * the JP2's camera JPEG): turned back, corrected, turned upright again */
    {
        ir_yuv_t r;
        double ex_x = 480 - 1 - vv, ex_y = u;            /* the corrected dot, turned 90 clockwise */
        CHECK(lensfix_rotate(&f, 90, &r) == 0 && r.w == 480 && r.h == 640, "a frame turned 90 clockwise: 480 x 640");
        yuv_encode_jpeg(&r, 95, &jpg, &jlen);
        memset(&ex, 0, sizeof ex);
        ex.make = "Jolla";
        exif_write_jpeg(path, jpg, jlen, &ex);
        free(jpg); free(r.y); free(r.u); free(r.v);
        err[0] = '\0';
        CHECK(lensfix_jpeg_file(&m, NULL, 90, path, path, 92, err, sizeof err) == 0, "corrected with its rotation, 90 (%s)", err);
        file = read_all(path, &flen);
        if (file && lensfix_decode(file, flen, &back) == 0) {
            bright_centre(back.y, back.w, back.h, &cx, &cy);
            CHECK(back.w == 480 && back.h == 640 && fabs(cx - ex_x) < 1.0 && fabs(cy - ex_y) < 1.0,
                  "still upright, the dot at (%.1f, %.1f), expected (%.1f, %.1f)", cx, cy, ex_x, ex_y);
            free(back.y); free(back.u); free(back.v);
        } else CHECK(0, "read back");
        free(file);
    }
    {   /* the rotations are each other's inverse */
        ir_yuv_t a1, a2;
        int d, same = 1;
        for (d = 90; d < 360; d += 90) {
            lensfix_rotate(&f, d, &a1); lensfix_rotate(&a1, 360 - d, &a2);
            if (a2.w != f.w || memcmp(a2.y, f.y, (size_t)f.w * f.h) || memcmp(a2.u, f.u, (size_t)f.w * f.h / 4)) same = 0;
            free(a1.y); free(a1.u); free(a1.v); free(a2.y); free(a2.u); free(a2.v);
        }
        CHECK(same, "turning by 90, 180, 270 and back gives the frame back");
    }
    free(f.y); free(f.u); free(f.v);
    remove(path);
    printf(fails ? "lensfix: %d FAILED\n" : "lensfix: all passed\n", fails);
    return fails != 0;
}
