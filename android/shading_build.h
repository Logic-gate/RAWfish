/*
 * shading_build.h -- build a measured lens-shading calibration from
 * flat-field DNGs, the way the on-phone assistant (and a PC) does it. The
 * READING side of shading.h: the assistant captures RAW through a diffuser
 * at several lens positions and under one or more lights, and this turns the
 * DNGs into the luminance-per-lens-position and colour-per-light grids
 * shading.h composes a photo's map from.
 *
 * Pure C, no Android: included by indraw.c (the CLI drives the captures and
 * calls this) and tested on a PC (android/tests/shading_build_test.c), which
 * writes synthetic flat-field DNGs with dng_write() and checks the grids come
 * back -- so it also exercises the DNG writer's pixels and GainMap.
 *
 * Method (Ric9k's RAWfish calibration script, re-expressed): an R x C grid
 * anchored at the image edges; each point the mean of a window around its
 * position, per CFA plane (R, the two greens, B); the gain is a 6 % centre
 * patch over the point; frames of the same (lens position, light) averaged.
 * The luminance grid is the green gain at a lens position; the colour grids
 * are R/G, G/G=1 and B/G relative to the brightest (longest-focus) daylight
 * green -- but here each grid is stored as measured and normalised when the
 * file is assembled, so this module only produces per-capture plane grids and
 * averages them; shading_build_assemble() lays them out as the file.
 */
#ifndef INDRAW_SHADING_BUILD_H
#define INDRAW_SHADING_BUILD_H

#include "shading.h"             /* shading_cal_t, SHADING_MAX_POINTS         */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

/* One flat-field measurement: the four CFA plane gain grids (R, Ge, Go, B,
 * centre ~1), plus the lens position and colour temperature it was taken at. */
typedef struct {
    int rows, cols;
    double hal_d;               /* the HAL's LENS_FOCUS_DISTANCE (lens position) */
    double cct;                 /* the colour temperature of the light          */
    int width, height;
    char cfa[8];
    float *r, *ge, *go, *b;     /* rows*cols each                               */
    double centre_level;        /* green centre level in DN (to flag clipping)  */
    double clip_fraction;       /* share of samples near white                  */
    double lopside;             /* |left-right| / mean of the green edges       */
    double corner_min_frac;     /* darkest inner corner as a fraction of centre
                                 * (the next-to-outermost ring: the extreme edge
                                 * is the lens circle's border, always dark)    */
} shading_capture_t;

static void shading_capture_free(shading_capture_t *c)
{
    free(c->r); free(c->ge); free(c->go); free(c->b);
    memset(c, 0, sizeof *c);
}

/* --- reading a flat-field DNG (the uncompressed 16-bit CFA ones indRAW
 * writes; the same minimal TIFF the writer emits) --------------------------- */
typedef struct { const uint8_t *p; size_t n; int big; } dng_reader_t;
static uint32_t dr_u32(const uint8_t *p, int big)
{
    return big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
               : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
static uint16_t dr_u16(const uint8_t *p, int big)
{
    return big ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]);
}
/* Finds tag in IFD0; returns its (type, count, value-or-offset field ptr). */
static const uint8_t *dr_tag(const dng_reader_t *d, uint16_t tag, int *type, uint32_t *count)
{
    uint32_t ifd = dr_u32(d->p + 4, d->big);
    uint16_t n, i;
    if (ifd + 2 > d->n) return NULL;
    n = dr_u16(d->p + ifd, d->big);
    for (i = 0; i < n; ++i) {
        const uint8_t *e = d->p + ifd + 2 + (size_t)i * 12;
        if (e + 12 > d->p + d->n) return NULL;
        if (dr_u16(e, d->big) == tag) { *type = dr_u16(e + 2, d->big); *count = dr_u32(e + 4, d->big); return e + 8; }
    }
    return NULL;
}
static uint32_t dr_int(const dng_reader_t *d, uint16_t tag, uint32_t dflt)
{
    int type;
    uint32_t count;
    const uint8_t *v = dr_tag(d, tag, &type, &count);
    if (!v) return dflt;
    if (type == 3) return dr_u16(v, d->big);
    if (type == 4) return dr_u32(v, d->big);
    return dflt;
}

/* Reads a flat-field DNG's pixels into the four plane grids of `out` at the
 * given lens position and colour temperature. 0 on success, -1 with a reason. */
static int shading_read_dng(const uint8_t *bytes, size_t len, int rows, int cols,
                            double hal_d, double cct, shading_capture_t *out,
                            char *err, size_t errn)
{
    dng_reader_t d;
    int type, i, j, k, pl, dy, dx;
    uint32_t count, w, h, white, black = 64, cfa_val = 3, strips, scnt;
    const uint8_t *v, *cfa_p, *off_p, *cnt_p;
    const uint16_t *px;
    double ref_sum, ref_n;
    static const char *const names4[4] = { "RGGB", "GRBG", "GBRG", "BGGR" };
    memset(out, 0, sizeof *out);
    if (len < 8) { snprintf(err, errn, "too short"); return -1; }
    d.p = bytes; d.n = len; d.big = bytes[0] == 'M';
    w = dr_int(&d, 256, 0); h = dr_int(&d, 257, 0);         /* ImageWidth / Length */
    if (!w || !h) { snprintf(err, errn, "no size"); return -1; }
    if (dr_int(&d, 258, 16) != 16 || dr_int(&d, 259, 1) != 1) { snprintf(err, errn, "not 16-bit uncompressed"); return -1; }
    white = dr_int(&d, 50717, 1023);                        /* WhiteLevel */
    v = dr_tag(&d, 50714, &type, &count);                   /* BlackLevel */
    if (v) {
        /* TIFF: a value that fits in the 4-byte field is stored inline,
         * otherwise the field holds an OFFSET to the values. indraw writes
         * BlackLevel as 4 SHORTs (one per CFA phase) = 8 bytes: an offset.
         * Take the first value (the phases are equal). Reading the field as
         * the value itself gave a black of 570 and emptied every corner. */
        size_t esz = type == 3 ? 2 : type == 4 ? 4 : type == 5 ? 8 : 1;
        const uint8_t *vp = (size_t)count * esz <= 4 ? v : bytes + dr_u32(v, d.big);
        if (vp + esz <= bytes + len) {
            if (type == 3) black = dr_u16(vp, d.big);
            else if (type == 4) black = dr_u32(vp, d.big);
            else if (type == 5) { uint32_t nu = dr_u32(vp, d.big), de = dr_u32(vp + 4, d.big); black = de ? nu / de : 64; }
        }
    }
    cfa_p = dr_tag(&d, 33422, &type, &count);               /* CFAPattern: 4 bytes, R=0 G=1 B=2, inline */
    if (cfa_p && count == 4) {
        if (cfa_p[0] == 0) cfa_val = 0; else if (cfa_p[0] == 2) cfa_val = 3;
        else cfa_val = cfa_p[1] == 0 ? 1 : 2;
    }
    off_p = dr_tag(&d, 273, &type, &strips);                /* StripOffsets  */
    cnt_p = dr_tag(&d, 279, &type, &scnt);                  /* StripByteCounts */
    if (!off_p || !cnt_p) { snprintf(err, errn, "no strips"); return -1; }
    (void)strips; (void)scnt; (void)cnt_p;
    {   /* indraw writes a single strip: the offset is the tag's value */
        uint32_t off = dr_u32(off_p, d.big);
        if ((size_t)off + (size_t)w * h * 2 > len) { snprintf(err, errn, "pixels past end"); return -1; }
        px = (const uint16_t *)(bytes + off);
    }
    out->rows = rows; out->cols = cols; out->hal_d = hal_d; out->cct = cct;
    out->width = (int)w; out->height = (int)h;
    snprintf(out->cfa, sizeof out->cfa, "%s", names4[cfa_val & 3]);
    out->r = calloc((size_t)rows * cols, sizeof(float));
    out->ge = calloc((size_t)rows * cols, sizeof(float));
    out->go = calloc((size_t)rows * cols, sizeof(float));
    out->b = calloc((size_t)rows * cols, sizeof(float));
    if (!out->r || !out->ge || !out->go || !out->b) { shading_capture_free(out); snprintf(err, errn, "no memory"); return -1; }

    /* plane order in the 2x2 quad, by CFA: which (dy,dx) is R, Ge (even row),
     * Go (odd row), B. names4 gives the top-left-first letters. */
    {
        int q[4]; /* q[k] in 0..3 = plane for quad position k (0 TL,1 TR,2 BL,3 BR) */
        const char *nm = names4[cfa_val & 3];
        int seen_g = 0;
        for (k = 0; k < 4; ++k) {
            if (nm[k] == 'R') q[k] = 0;
            else if (nm[k] == 'B') q[k] = 3;
            else q[k] = (seen_g++ == 0) ? 1 : 2;     /* first green = Ge, second = Go */
        }
        /* sample grid, edge-anchored, window half a cell */
        {
            int rh = (int)((double)h / rows / 2 * 0.5); if (rh < 1) rh = 1;
            int cw = (int)((double)w / cols / 2 * 0.5); if (cw < 1) cw = 1;
            for (i = 0; i < rows; ++i) {
                double yc = (double)i * (h - 1) / (rows - 1);
                for (j = 0; j < cols; ++j) {
                    double xc = (double)j * (w - 1) / (cols - 1);
                    double sum[4] = { 0, 0, 0, 0 };
                    long cnt_q[4] = { 0, 0, 0, 0 };
                    int y, x;
                    int y0 = (int)(yc - rh); if (y0 < 0) y0 = 0; y0 &= ~1;
                    int y1 = (int)(yc + rh); if (y1 > (int)h - 1) y1 = (int)h - 1;
                    int x0 = (int)(xc - cw); if (x0 < 0) x0 = 0; x0 &= ~1;
                    int x1 = (int)(xc + cw); if (x1 > (int)w - 1) x1 = (int)w - 1;
                    for (y = y0; y <= y1; ++y)
                        for (x = x0; x <= x1; ++x) {
                            int qp = (y & 1) * 2 + (x & 1);
                            double val = (double)px[(size_t)y * w + x] - black;
                            sum[q[qp]] += val; cnt_q[q[qp]] += 1;
                        }
                    out->r[i * cols + j]  = (float)(cnt_q[0] ? sum[0] / cnt_q[0] : 0);
                    out->ge[i * cols + j] = (float)(cnt_q[1] ? sum[1] / cnt_q[1] : 0);
                    out->go[i * cols + j] = (float)(cnt_q[2] ? sum[2] / cnt_q[2] : 0);
                    out->b[i * cols + j]  = (float)(cnt_q[3] ? sum[3] / cnt_q[3] : 0);
                }
            }
        }
        dy = dx = pl = 0; (void)dy; (void)dx; (void)pl; (void)v;
    }
    /* centre reference (6 % patch, green), clipping, left/right balance */
    {
        int y0 = (int)(h * 0.47), y1 = (int)(h * 0.53), x0 = (int)(w * 0.47), x1 = (int)(w * 0.53);
        int y, x;
        long clip = 0, tot = 0;
        ref_sum = ref_n = 0;
        for (y = y0; y <= y1; ++y)
            for (x = x0; x <= x1; ++x) {
                if (((y & 1) + (x & 1)) == 1) { ref_sum += (double)px[(size_t)y * w + x] - black; ref_n += 1; }
            }
        for (y = 0; y < (int)h; y += 8)
            for (x = 0; x < (int)w; x += 8) { tot++; if (px[(size_t)y * w + x] >= white - 4) clip++; }
        out->centre_level = ref_n ? ref_sum / ref_n : 0;
        out->clip_fraction = tot ? (double)clip / tot : 0;
    }
    /* turn levels into gains: centre green over the point, per plane */
    {
        double ref = out->centre_level > 1 ? out->centre_level : 1;
        double le = 0, ri = 0;
        int n = rows * cols;
        for (i = 0; i < n; ++i) {
            float *ap[4] = { &out->r[i], &out->ge[i], &out->go[i], &out->b[i] };
            for (k = 0; k < 4; ++k) {
                double lvl = *ap[k] > 1 ? *ap[k] : 1;
                *ap[k] = (float)(ref / lvl);
            }
        }
        for (i = 0; i < rows; ++i) { le += 1.0 / out->ge[i * cols + 0]; ri += 1.0 / out->ge[i * cols + cols - 1]; }
        out->lopside = le + ri > 0 ? fabs(le - ri) / ((le + ri) / 2) : 0;
        {   /* the darkest of the four INNER corners (ring 1, not the extreme
             * edge ring 0 which is the lens circle's dark border) */
            int cy[2] = { 1, rows - 2 }, cx[2] = { 1, cols - 2 }, a, bq;
            double worst = 1e9;
            for (a = 0; a < 2; ++a) for (bq = 0; bq < 2; ++bq) {
                double gg = out->ge[cy[a] * cols + cx[bq]];
                double frac = gg > 0 ? 1.0 / gg : 0;
                if (frac < worst) worst = frac;
            }
            out->corner_min_frac = worst;
        }
    }
    return 0;
}

/* Is a capture's centre level consistent with the measurement so far?
 * Checks the two ways the light or the framing can move:
 *  - within one lens position, against that position's first frame: the
 *    frames of a position agree within ~0.5 % on a steady setup, so 5 % is
 *    a change of light or a moved phone/diffuser;
 *  - from one position to the next, the first frame of a position against
 *    the first frame of the previous one: the falloff legitimately moves the
 *    centre by a few % per step (2.8-7 % measured on the JP2 over six
 *    positions), so 15 % is a light that changed between positions.
 * pos_first: the first level at this position (< 0: this IS the first);
 * prev_first: the previous position's first level (< 0: none yet).
 * Returns 0 if consistent, else 1 (within a position) or 2 (between
 * positions), with a reason. Pure: tested on the host. */
#define SHADING_WITHIN_POSITION 0.05
#define SHADING_BETWEEN_POSITIONS 0.15
static int shading_level_check(double level, double pos_first, double prev_first,
                               char *err, size_t errn)
{
    if (pos_first > 0) {                          /* a later frame of this position */
        if (fabs(level - pos_first) / pos_first > SHADING_WITHIN_POSITION) {
            snprintf(err, errn, "the light or framing changed during a lens position (%.0f %%)",
                     100.0 * fabs(level - pos_first) / pos_first);
            return 1;
        }
    } else if (prev_first > 0) {                  /* the first frame of a new position */
        if (fabs(level - prev_first) / prev_first > SHADING_BETWEEN_POSITIONS) {
            snprintf(err, errn, "the light changed between two lens positions (%.0f %%)",
                     100.0 * fabs(level - prev_first) / prev_first);
            return 2;
        }
    }
    return 0;
}

/* Averages the captures sharing a (lens position, light), then lays the
 * calibration out: one luminance grid (green) per distinct lens position,
 * one colour triple (R/G, 1, B/G) per distinct light, the colour normalised
 * to each capture's own green so it is the pure colour shading. The result
 * fills a shading_cal_t exactly as shading.h expects. 0, or -1 with a reason. */
static int shading_build_assemble(const shading_capture_t *caps, int ncap,
                                  shading_cal_t *out, char *err, size_t errn)
{
    int i, j, n, rows = 0, cols = 0;
    memset(out, 0, sizeof *out);
    if (ncap < 1) { snprintf(err, errn, "no captures"); return -1; }
    rows = caps[0].rows; cols = caps[0].cols; n = rows * cols;
    snprintf(out->cfa, sizeof out->cfa, "%s", caps[0].cfa);
    out->rows = rows; out->cols = cols; out->width = caps[0].width; out->height = caps[0].height;
    /* distinct lens positions (luminance) and lights (colour), within tolerance */
    for (i = 0; i < ncap; ++i) {
        int li = -1, ci = -1;
        for (j = 0; j < out->nlum; ++j) if (fabs(out->lum_d[j] - caps[i].hal_d) < 0.02) li = j;
        if (li < 0) {
            if (out->nlum >= SHADING_MAX_POINTS) { snprintf(err, errn, "too many lens positions"); shading_free(out); return -1; }
            li = out->nlum++; out->lum_d[li] = caps[i].hal_d;
            out->lum[li] = calloc((size_t)n, sizeof(float));
        }
        for (j = 0; j < out->ncol; ++j) if (fabs(out->col_k[j] - caps[i].cct) < 50) ci = j;
        if (ci < 0) {
            if (out->ncol >= SHADING_MAX_POINTS) { snprintf(err, errn, "too many lights"); shading_free(out); return -1; }
            ci = out->ncol++; out->col_k[ci] = caps[i].cct;
            out->col_r[ci] = calloc((size_t)n, sizeof(float));
            out->col_g[ci] = calloc((size_t)n, sizeof(float));
            out->col_b[ci] = calloc((size_t)n, sizeof(float));
        }
    }
    /* accumulate (green for luminance; colour = plane / green, averaged) */
    {
        int *ln = calloc((size_t)out->nlum, sizeof(int));
        int *cn = calloc((size_t)out->ncol, sizeof(int));
        if (!ln || !cn) { free(ln); free(cn); snprintf(err, errn, "no memory"); shading_free(out); return -1; }
        for (i = 0; i < ncap; ++i) {
            int li = 0, ci = 0, idx;
            for (j = 0; j < out->nlum; ++j) if (fabs(out->lum_d[j] - caps[i].hal_d) < 0.02) li = j;
            for (j = 0; j < out->ncol; ++j) if (fabs(out->col_k[j] - caps[i].cct) < 50) ci = j;
            for (idx = 0; idx < n; ++idx) {
                double g = (caps[i].ge[idx] + caps[i].go[idx]) / 2;
                out->lum[li][idx] += (float)g;
                out->col_r[ci][idx] += (float)(g > 0 ? caps[i].r[idx] / g : 1);
                out->col_g[ci][idx] += 1.0f;
                out->col_b[ci][idx] += (float)(g > 0 ? caps[i].b[idx] / g : 1);
            }
            ln[li]++; cn[ci]++;
        }
        for (j = 0; j < out->nlum; ++j) for (i = 0; i < n; ++i) if (ln[j]) out->lum[j][i] /= ln[j];
        for (j = 0; j < out->ncol; ++j) for (i = 0; i < n; ++i) if (cn[j]) {
            out->col_r[j][i] /= cn[j]; out->col_g[j][i] /= cn[j]; out->col_b[j][i] /= cn[j];
        }
        /* The colour factors must be the colour SHADING only: 1 at the centre,
         * the corners' relative drift. plane/green as averaged above still
         * carries the light's white balance (R/G ~ 2 on the JP2), so divide
         * each colour grid by its own centre value. (Without this a built
         * file put R = 2.04 and B = 1.43 at the centre: the white balance
         * baked into the shading.) */
        for (j = 0; j < out->ncol; ++j) {
            int ci = (rows / 2) * cols + cols / 2;
            double rc = out->col_r[j][ci], gc = out->col_g[j][ci], bc = out->col_b[j][ci];
            if (rc > 0 && gc > 0 && bc > 0)
                for (i = 0; i < n; ++i) {
                    out->col_r[j][i] = (float)(out->col_r[j][i] / rc);
                    out->col_g[j][i] = (float)(out->col_g[j][i] / gc);
                    out->col_b[j][i] = (float)(out->col_b[j][i] / bc);
                }
        }
        free(ln); free(cn);
    }
    return 0;
}

/* Serialises a shading_cal_t to the JSON file format shading.h reads.
 * `extra` is copied verbatim into a "measured" object (may be NULL). Returns
 * the number of bytes written, or -1. */
static int shading_write_file(const char *path, const shading_cal_t *c,
                              const char *make, const char *model, const char *camera_id)
{
    FILE *f = fopen(path, "wb");
    int i, k, n = c->rows * c->cols;
    if (!f) return -1;
    fprintf(f, "{\n \"format\":\"indraw-lens-shading\",\"version\":1,\n");
    fprintf(f, " \"make\":\"%s\",\"model\":\"%s\",\"camera_id\":\"%s\",\"cfa\":\"%s\",\n",
            make ? make : "", model ? model : "", camera_id ? camera_id : "0", c->cfa);
    fprintf(f, " \"width\":%d,\"height\":%d,\n \"grid\":{\"rows\":%d,\"cols\":%d,\"anchored\":\"edges\"},\n",
            c->width, c->height, c->rows, c->cols);
    fprintf(f, " \"luminance\":[\n");
    for (k = 0; k < c->nlum; ++k) {
        fprintf(f, "  {\"hal_diopters\":%.4f,\"gain\":[", c->lum_d[k]);
        for (i = 0; i < n; ++i) fprintf(f, "%s%.4f", i ? "," : "", c->lum[k][i]);
        fprintf(f, "]}%s\n", k < c->nlum - 1 ? "," : "");
    }
    fprintf(f, " ],\n \"colour\":[\n");
    for (k = 0; k < c->ncol; ++k) {
        fprintf(f, "  {\"cct\":%.0f,\"r\":[", c->col_k[k]);
        for (i = 0; i < n; ++i) fprintf(f, "%s%.4f", i ? "," : "", c->col_r[k][i]);
        fprintf(f, "],\"g\":[");
        for (i = 0; i < n; ++i) fprintf(f, "%s%.4f", i ? "," : "", c->col_g[k][i]);
        fprintf(f, "],\"b\":[");
        for (i = 0; i < n; ++i) fprintf(f, "%s%.4f", i ? "," : "", c->col_b[k][i]);
        fprintf(f, "]}%s\n", k < c->ncol - 1 ? "," : "");
    }
    fprintf(f, " ]\n}\n");
    {
        long end = ftell(f);
        fclose(f);
        return (int)end;
    }
}

#endif
