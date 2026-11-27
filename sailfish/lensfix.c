/* lensfix.c -- see lensfix.h. */
#define _POSIX_C_SOURCE 200809L
#include "lensfix.h"
#include "yuvjpeg.h"
#include "../android/exifwrite.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jpeglib.h>

int lensfix_load(const char *path, ld_model_t *m, char *err, size_t errn)
{
    FILE *f = fopen(path, "rb");
    char *text;
    long len;
    int rc;
    if (!f) { snprintf(err, errn, "cannot open %s: %s", path, strerror(errno)); return -1; }
    if (fseek(f, 0, SEEK_END) || (len = ftell(f)) <= 0 || len > 1 << 20 || fseek(f, 0, SEEK_SET)) {
        fclose(f); snprintf(err, errn, "%s: empty or too large", path); return -1;
    }
    text = malloc((size_t)len + 1);
    if (!text) { fclose(f); snprintf(err, errn, "out of memory"); return -1; }
    if (fread(text, 1, (size_t)len, f) != (size_t)len) { free(text); fclose(f); snprintf(err, errn, "%s: short read", path); return -1; }
    fclose(f);
    text[len] = '\0';
    rc = ld_parse(text, m, err, errn);
    free(text);
    return rc;
}

/* --- the remap, on as many threads as there are cores ------------------- */
typedef struct {
    const ld_grid_t *g;
    const ir_yuv_t *in;
    ir_yuv_t *out;
    int y0, y1;                      /* luma rows; chroma rows are half */
    int failed;
} job_t;

static void *remap_job(void *arg)
{
    job_t *j = arg;
    const ir_yuv_t *in = j->in;
    ir_yuv_t *out = j->out;
    float *rx = malloc(sizeof(float) * (size_t)j->g->gw), *ry = malloc(sizeof(float) * (size_t)j->g->gw);
    int cw = in->w / 2, ch = in->h / 2;
    if (!rx || !ry) { free(rx); free(ry); j->failed = 1; return NULL; }
    ld_remap_plane(j->g, in->y, in->w, in->h, out->y, in->w, j->y0, j->y1, 0, 1, rx, ry);
    ld_remap_plane(j->g, in->u, cw, ch, out->u, cw, j->y0 / 2, j->y1 / 2, 1, 0, rx, ry);
    ld_remap_plane(j->g, in->v, cw, ch, out->v, cw, j->y0 / 2, j->y1 / 2, 1, 0, rx, ry);
    free(rx); free(ry);
    return NULL;
}

int lensfix_yuv(const ld_model_t *m, const double *field, const ir_yuv_t *in, ir_yuv_t *out)
{
    return lensfix_yuv_turned(m, field, 0, in, out);
}

int lensfix_yuv_turned(const ld_model_t *m, const double *field, int rot, const ir_yuv_t *in, ir_yuv_t *out)
{
    ld_view_t v;
    ld_grid_t g;
    job_t jobs[8];
    pthread_t th[8];
    int started[8] = { 0 };
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int n = ncpu < 1 ? 1 : ncpu > 8 ? 8 : (int)ncpu, k, failed = 0;
    size_t ny, nc;
    if (!in || !out || !in->y || !in->u || !in->v || (in->w & 1) || (in->h & 1)) return -1;
    *out = *in;
    out->y = out->u = out->v = NULL;
    rot = ((rot % 360) + 360) % 360;
    if (rot % 90) return -1;
    /* the view in the sensor's frame; the grid in the picture's, turned */
    if (ld_view_init(&v, m, rot == 90 || rot == 270 ? in->h : in->w, rot == 90 || rot == 270 ? in->w : in->h, field)) return -2;
    if (ld_grid_build_turned(&v, 16, rot, &g)) return -1;
    ny = (size_t)in->w * in->h; nc = ny / 4;
    out->y = malloc(ny); out->u = malloc(nc); out->v = malloc(nc);
    if (!out->y || !out->u || !out->v) { free(out->y); free(out->u); free(out->v); out->y = out->u = out->v = NULL; ld_grid_free(&g); return -1; }
    for (k = 0; k < n; ++k) {           /* bands of even rows */
        jobs[k].g = &g; jobs[k].in = in; jobs[k].out = out; jobs[k].failed = 0;
        jobs[k].y0 = (int)((long)in->h * k / n) & ~1;
        jobs[k].y1 = k == n - 1 ? in->h : (int)((long)in->h * (k + 1) / n) & ~1;
        if (k > 0) started[k] = pthread_create(&th[k], NULL, remap_job, &jobs[k]) == 0;
    }
    remap_job(&jobs[0]);
    for (k = 1; k < n; ++k) {
        if (started[k]) pthread_join(th[k], NULL);
        else remap_job(&jobs[k]);       /* no thread: done here */
    }
    for (k = 0; k < n; ++k) failed |= jobs[k].failed;
    ld_grid_free(&g);
    if (failed) { free(out->y); free(out->u); free(out->v); out->y = out->u = out->v = NULL; return -1; }
    return 0;
}

/* --- the Exif thumbnail: an area average, then our encoder -------------- */
static void shrink_plane(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
    int x, y;
    for (y = 0; y < dh; ++y) {
        int y0 = (int)((long)y * sh / dh), y1 = (int)((long)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (x = 0; x < dw; ++x) {
            int x0 = (int)((long)x * sw / dw), x1 = (int)((long)(x + 1) * sw / dw), xx, yy;
            unsigned long sum = 0;
            if (x1 <= x0) x1 = x0 + 1;
            for (yy = y0; yy < y1; ++yy) for (xx = x0; xx < x1; ++xx) sum += src[(size_t)yy * sw + xx];
            dst[(size_t)y * dw + x] = (uint8_t)((sum + (unsigned long)((x1 - x0) * (y1 - y0)) / 2) / (unsigned long)((x1 - x0) * (y1 - y0)));
        }
    }
}

int lensfix_thumbnail(const ir_yuv_t *f, int max_w, uint8_t **jpg, unsigned long *len)
{
    ir_yuv_t t;
    int tw, th, rc;
    if (!f || !f->y || f->w < 16 || f->h < 16) return -1;
    tw = max_w < f->w ? max_w : f->w;
    th = (int)((long)tw * f->h / f->w);
    if (f->h > f->w) { th = max_w < f->h ? max_w : f->h; tw = (int)((long)th * f->w / f->h); }
    tw &= ~1; th &= ~1;
    if (tw < 16 || th < 16) return -1;
    memset(&t, 0, sizeof t);
    t.w = tw; t.h = th;
    t.y = malloc((size_t)tw * th); t.u = malloc((size_t)tw * th / 4); t.v = malloc((size_t)tw * th / 4);
    if (!t.y || !t.u || !t.v) { free(t.y); free(t.u); free(t.v); return -1; }
    shrink_plane(f->y, f->w, f->h, t.y, tw, th);
    shrink_plane(f->u, f->w / 2, f->h / 2, t.u, tw / 2, th / 2);
    shrink_plane(f->v, f->w / 2, f->h / 2, t.v, tw / 2, th / 2);
    rc = yuv_encode_jpeg(&t, 85, jpg, len);
    free(t.y); free(t.u); free(t.v);
    return rc;
}

/* --- decoding a JPEG into planar 4:2:0 ----------------------------------- */
typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } derr_t;
static void on_derror(j_common_ptr c) { longjmp(((derr_t *)c->err)->jb, 1); }

int lensfix_decode(const uint8_t *jpg, size_t len, ir_yuv_t *out)
{
    struct jpeg_decompress_struct d;
    derr_t err;
    /* what longjmp may come back to: heap blocks whose pointers do not
     * change after setjmp (as yuvjpeg.c's output) */
    struct bufs { uint8_t *rows, *line; } *b = calloc(1, sizeof *b);
    int w, h;
    if (!b) return -1;
    memset(out, 0, sizeof *out);
    d.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_derror;
    if (setjmp(err.jb)) {
        jpeg_destroy_decompress(&d);
        free(b->rows); free(b->line); free(b);
        free(out->y); free(out->u); free(out->v); memset(out, 0, sizeof *out);
        return -2;
    }
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, (unsigned char *)jpg, (unsigned long)len);
    jpeg_read_header(&d, TRUE);
    w = (int)d.image_width; h = (int)d.image_height;
    if (w < 16 || h < 16 || (w & 1) || (h & 1) || d.num_components != 3) { jpeg_destroy_decompress(&d); free(b); return -3; }
    out->w = w; out->h = h;
    out->y = malloc((size_t)w * h); out->u = malloc((size_t)w * h / 4); out->v = malloc((size_t)w * h / 4);
    if (!out->y || !out->u || !out->v) { jpeg_destroy_decompress(&d); free(b); free(out->y); free(out->u); free(out->v); memset(out, 0, sizeof *out); return -1; }
    if (d.jpeg_color_space == JCS_YCbCr &&
        d.comp_info[0].h_samp_factor == 2 && d.comp_info[0].v_samp_factor == 2 &&
        d.comp_info[1].h_samp_factor == 1 && d.comp_info[1].v_samp_factor == 1 &&
        d.comp_info[2].h_samp_factor == 1 && d.comp_info[2].v_samp_factor == 1) {
        /* 4:2:0 as the camera writes it: the planes as they are (raw data
         * out), 16 luma rows at a time into padded rows, then copied */
        JSAMPROW ry[16], ru[8], rv[8];
        JSAMPARRAY planes[3] = { ry, ru, rv };
        size_t yw = (size_t)d.comp_info[0].width_in_blocks * DCTSIZE, cw = (size_t)d.comp_info[1].width_in_blocks * DCTSIZE;
        int i;
        if (yw < (size_t)w) yw = (size_t)w;
        if (cw < (size_t)w / 2) cw = (size_t)w / 2;
        d.raw_data_out = TRUE;
        d.out_color_space = JCS_YCbCr;
        d.dct_method = JDCT_ISLOW;
        b->rows = malloc(yw * 16 + cw * 16);
        if (!b->rows) longjmp(err.jb, 1);
        for (i = 0; i < 16; ++i) ry[i] = b->rows + yw * i;
        for (i = 0; i < 8; ++i) { ru[i] = b->rows + yw * 16 + cw * i; rv[i] = b->rows + yw * 16 + cw * (8 + i); }
        jpeg_start_decompress(&d);
        while (d.output_scanline < d.output_height) {
            int y0 = (int)d.output_scanline, n;
            if (jpeg_read_raw_data(&d, planes, 16) == 0) break;
            for (n = 0; n < 16 && y0 + n < h; ++n) memcpy(out->y + (size_t)(y0 + n) * w, ry[n], (size_t)w);
            for (n = 0; n < 8 && y0 / 2 + n < h / 2; ++n) {
                memcpy(out->u + (size_t)(y0 / 2 + n) * (w / 2), ru[n], (size_t)w / 2);
                memcpy(out->v + (size_t)(y0 / 2 + n) * (w / 2), rv[n], (size_t)w / 2);
            }
        }
    } else {
        /* any other layout: decoded to full YCbCr, the chroma averaged 2 x 2 */
        int y, x;
        d.out_color_space = JCS_YCbCr;
        jpeg_start_decompress(&d);
        b->line = malloc((size_t)w * 3 * 2);
        if (!b->line) longjmp(err.jb, 1);
        for (y = 0; y < h; y += 2) {
            JSAMPROW r[1];
            uint8_t *l0 = b->line, *l1 = b->line + (size_t)w * 3;
            r[0] = l0; jpeg_read_scanlines(&d, r, 1);
            r[0] = l1; jpeg_read_scanlines(&d, r, 1);
            for (x = 0; x < w; ++x) { out->y[(size_t)y * w + x] = l0[3 * x]; out->y[(size_t)(y + 1) * w + x] = l1[3 * x]; }
            for (x = 0; x < w / 2; ++x) {
                const uint8_t *a = l0 + 6 * x, *c = l1 + 6 * x;
                out->u[(size_t)(y / 2) * (w / 2) + x] = (uint8_t)((a[1] + a[4] + c[1] + c[4] + 2) / 4);
                out->v[(size_t)(y / 2) * (w / 2) + x] = (uint8_t)((a[2] + a[5] + c[2] + c[5] + 2) / 4);
            }
        }
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    free(b->rows); free(b->line); free(b);
    return 0;
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *p;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    p = malloc((size_t)n);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *len = (size_t)n;
    return p;
}

/* One plane of w x h turned clockwise by deg into dst. */
static void rotate_plane(const uint8_t *src, int w, int h, int deg, uint8_t *dst)
{
    int x, y;
    if (deg == 90) {                     /* out H x W: (ox, oy) <- (oy, H-1-ox) */
        for (y = 0; y < w; ++y) for (x = 0; x < h; ++x) dst[(size_t)y * h + x] = src[(size_t)(h - 1 - x) * w + y];
    } else if (deg == 180) {
        for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) dst[(size_t)y * w + x] = src[(size_t)(h - 1 - y) * w + (w - 1 - x)];
    } else if (deg == 270) {             /* out H x W: (ox, oy) <- (W-1-oy, ox) */
        for (y = 0; y < w; ++y) for (x = 0; x < h; ++x) dst[(size_t)y * h + x] = src[(size_t)x * w + (w - 1 - y)];
    } else memcpy(dst, src, (size_t)w * h);
}

int lensfix_rotate(const ir_yuv_t *f, int deg, ir_yuv_t *out)
{
    size_t ny = (size_t)f->w * f->h, nc = ny / 4;
    deg = ((deg % 360) + 360) % 360;
    if (deg % 90) return -1;
    *out = *f;
    out->y = malloc(ny); out->u = malloc(nc); out->v = malloc(nc);
    if (!out->y || !out->u || !out->v) { free(out->y); free(out->u); free(out->v); out->y = out->u = out->v = NULL; return -1; }
    rotate_plane(f->y, f->w, f->h, deg, out->y);
    rotate_plane(f->u, f->w / 2, f->h / 2, deg, out->u);
    rotate_plane(f->v, f->w / 2, f->h / 2, deg, out->v);
    if (deg == 90 || deg == 270) { out->w = f->h; out->h = f->w; }
    return 0;
}

static void yuv_free3(ir_yuv_t *f) { free(f->y); free(f->u); free(f->v); f->y = f->u = f->v = NULL; }

int lensfix_jpeg_file(const ld_model_t *m, lensfix_field_fn field_for, int rot_deg, const char *in_path,
                      const char *out_path, int quality, char *msg, size_t msgn)
{
    double fbuf[4], *field = NULL;
    int32_t fi[4];
    size_t olen = 0;
    uint8_t *orig = read_all(in_path, &olen), *jpg = NULL, *thumb = NULL;
    unsigned long jlen = 0, tlen = 0;
    ir_yuv_t src, fixed;
    exif_info_t ex;
    int rc, turned = 0, landscape_model, sw, sh;
    memset(&fixed, 0, sizeof fixed);
    if (!orig) { snprintf(msg, msgn, "cannot read %s", in_path); return -1; }
    if ((rc = lensfix_decode(orig, olen, &src)) != 0) {
        free(orig);
        snprintf(msg, msgn, rc == -3 ? "not a colour JPEG of even size" : "cannot decode the JPEG");
        return -1;
    }
    /* The model is the sensor's. A JPEG with an Exif orientation has the
     * sensor's pixels; one without may have had them turned upright by the
     * camera driver (the JP2's does), by the rotation the photo was asked
     * to carry: corrected as it is, upright, the grid turned with it
     * (ld_grid_build_turned: no copy turned back and forth). */
    landscape_model = m->aw >= m->ah;
    if (rot_deg > 0 && exif_read_orientation(orig, olen) <= 1) turned = ((rot_deg % 360) + 360) % 360;
    if ((turned == 90 || turned == 270) && (src.w > src.h) == landscape_model && src.w != src.h) turned = 0;   /* not turned after all */
    sw = turned == 90 || turned == 270 ? src.h : src.w;     /* the sensor's frame */
    sh = turned == 90 || turned == 270 ? src.w : src.h;
    if ((sw > sh) != landscape_model && sw != sh) {
        free(orig); yuv_free3(&src);
        snprintf(msg, msgn, "the JPEG's pixels are turned (%dx%d) against the sensor, by an unknown rotation: not corrected", src.w, src.h);
        return -1;
    }
    if (field_for && field_for(sw, sh, fi) == 0 && fi[2] > 0 && fi[3] > 0) {
        fbuf[0] = fi[0]; fbuf[1] = fi[1]; fbuf[2] = fi[2]; fbuf[3] = fi[3];
        field = fbuf;
    }
    rc = lensfix_yuv_turned(m, field, turned, &src, &fixed);
    yuv_free3(&src);
    if (rc) { free(orig); snprintf(msg, msgn, rc == -2 ? "the lens model cannot correct this frame" : "out of memory"); return -1; }
    rc = yuv_encode_jpeg(&fixed, quality, &jpg, &jlen);
    if (rc == 0 && lensfix_thumbnail(&fixed, 320, &thumb, &tlen)) { thumb = NULL; tlen = 0; }
    yuv_free3(&fixed);
    if (rc) { free(orig); snprintf(msg, msgn, "cannot compress the corrected picture"); return -1; }
    memset(&ex, 0, sizeof ex);
    ex.keep_existing = 1;                    /* the file's own Exif, only the thumbnail new */
    ex.thumb = thumb; ex.thumb_len = tlen;
    rc = exif_rewrite_jpeg(out_path, jpg, jlen, orig, olen, &ex);
    free(orig); free(jpg); free(thumb);
    if (rc) { snprintf(msg, msgn, "cannot write %s: %s", out_path, strerror(rc)); return -1; }
    return 0;
}
