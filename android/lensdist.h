/*
 * lensdist.h -- lens distortion correction: the lens model of a camera (as
 * scripts/lens-calibrate.py measures it with OpenCV), and the remapping of
 * a frame to what a distortion-free lens would have drawn. Pure C, no
 * Android and no OpenCV: included by the engine (the taps), the CLI (the
 * photos) and the interface (C++: the viewfinder, drawn corrected by the
 * GPU), tested on a PC (android/tests/lensdist_test.c).
 *
 * The model is OpenCV's ("rational", of which the plain polynomial one is a
 * special case): for a ray (x, y) of the ideal pinhole camera, r2 = x^2+y^2,
 *   a  = (1 + k1 r2 + k2 r2^2 + k3 r2^3) / (1 + k4 r2 + k5 r2^2 + k6 r2^3)
 *   xd = x a + 2 p1 x y + p2 (r2 + 2 x^2)
 *   yd = y a + p1 (r2 + 2 y^2) + 2 p2 x y
 * and the sensor pixel is (fx xd + cx, fy yd + cy).
 *
 * ONE model per camera, for every output size: the file's matrix refers to
 * one frame (the 4:3 frame its samples were taken at, "width" x "height"),
 * which covers the whole active array; ld_bind() turns it into ACTIVE-ARRAY
 * pixels. Every frame the camera produces -- a 16:9 photo, a 12 MP or 50 MP
 * one, the viewfinder, zoomed or in the loupe -- shows a known rectangle of
 * that array (its "field", viewfield.h), so its correction follows from the
 * same model. (Measured on the JP2, Nov 2026: the 16:9 calibrations are the
 * 4:3 ones cropped to the middle band, within 0.5 % in focal length.)
 *
 * The corrected picture: a pinhole camera with square pixels, the same
 * optical centre, and the narrowest focal length at which the whole frame
 * still has picture under it (no black corners: OpenCV's alpha = 0). That
 * focal length is fitted on the frame's whole BAND (its shape at zoom 1),
 * so the viewfinder, a zoomed frame and the photo of one format all share
 * one corrected geometry, and the viewfinder shows what the photo will.
 *
 * Coordinates: "continuous" active-array pixels (pixel i spans [i, i+1),
 * its centre at i + 0.5) inside the model; pixel INDEX coordinates (the
 * centre of pixel i at i, OpenCV's convention) in and out of ld_src and
 * ld_dst.
 *
 * The file (JSON, written by scripts/lens-calibrate.py):
 *   "format": "rawfish-ir lens distortion 1"
 *   "width", "height": the frame the matrix refers to
 *   "fx", "fy", "cx", "cy": OpenCV's camera matrix for that frame
 *   "dist": [k1, k2, p1, p2, k3, k4, k5, k6] (4, 5 or 8 values; OpenCV order)
 * Everything else in it (the phone, the camera, the fit's error, the
 * samples) is information for people, not read here.
 */
#ifndef INDRAW_LENSDIST_H
#define INDRAW_LENSDIST_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "viewfield.h"

#define LD_FORMAT "rawfish-ir lens distortion 1"

typedef struct {
    int ok;                         /* parsed and bound                         */
    int ref_w, ref_h;               /* the frame the file's matrix refers to    */
    double ref_fx, ref_fy, ref_cx, ref_cy;   /* the file's, OpenCV convention    */
    double k[8];                    /* k1 k2 p1 p2 k3 k4 k5 k6                  */
    double r_fold;                  /* the ray radius past which the model folds back
                                     * (its radial curve stops rising); 9 if none   */
    int aw, ah;                     /* the active array the bound values are in */
    double fx, fy, cx, cy;          /* bound: active-array pixels, continuous   */
} ld_model_t;

/* --- the file ------------------------------------------------------------ */
/* The value after "key": (a number or the start of an array/string), or NULL. */
static inline const char *ld_after_key(const char *s, const char *key)
{
    size_t n = strlen(key);
    const char *p = s;
    while ((p = strchr(p, '"')) != NULL) {
        if (!strncmp(p + 1, key, n) && p[n + 1] == '"') {
            const char *q = p + n + 2;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') ++q;
            if (*q == ':') {
                ++q;
                while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') ++q;
                return q;
            }
        }
        ++p;
    }
    return NULL;
}
static inline int ld_num(const char *s, const char *key, double *v)
{
    const char *p = ld_after_key(s, key);
    char *end;
    if (!p) return 0;
    *v = strtod(p, &end);
    return end != p && isfinite(*v);
}

static inline double ld_radial(const ld_model_t *m, double r2)
{
    const double *k = m->k;
    double num = 1 + r2 * (k[0] + r2 * (k[1] + r2 * k[4]));
    double den = 1 + r2 * (k[5] + r2 * (k[6] + r2 * k[7]));
    return num / den;
}

/* Where the radial curve rd(r) = r a(r^2) stops rising (the model is only
 * meaningful inside it), 9 if it rises up to there. */
static inline double ld_fold_radius(const ld_model_t *m)
{
    double r, prev = 0;
    for (r = 0.001; r <= 9.0; r += 0.001) {
        double r2 = r * r, den = 1 + r2 * (m->k[5] + r2 * (m->k[6] + r2 * m->k[7]));
        double rd = r * ld_radial(m, r2);
        if (den <= 1e-9 || rd <= prev) return r - 0.001;
        prev = rd;
    }
    return 9.0;
}

/* Parses the file's text into m (not bound yet). 0, or -1 with the reason. */
static inline int ld_parse(const char *text, ld_model_t *m, char *err, size_t errn)
{
    const char *p;
    double w, h;
    int n = 0;
    memset(m, 0, sizeof *m);
    p = ld_after_key(text, "format");
    if (!p || *p != '"' || strncmp(p + 1, "rawfish-ir lens distortion ", 27)) {
        snprintf(err, errn, "not a lens distortion file (\"format\": \"%s\")", LD_FORMAT); return -1;
    }
    if (atoi(p + 28) != 1) { snprintf(err, errn, "unknown version of the lens distortion format"); return -1; }
    if (!ld_num(text, "width", &w) || !ld_num(text, "height", &h) || w < 16 || h < 16 || w > 65535 || h > 65535) {
        snprintf(err, errn, "no valid width / height"); return -1;
    }
    m->ref_w = (int)w; m->ref_h = (int)h;
    if (!ld_num(text, "fx", &m->ref_fx) || !ld_num(text, "fy", &m->ref_fy) ||
        !ld_num(text, "cx", &m->ref_cx) || !ld_num(text, "cy", &m->ref_cy) ||
        !(m->ref_fx > 0) || !(m->ref_fy > 0)) {
        snprintf(err, errn, "no valid fx, fy, cx, cy"); return -1;
    }
    p = ld_after_key(text, "dist");
    if (!p || *p != '[') { snprintf(err, errn, "no \"dist\" list"); return -1; }
    ++p;
    while (n < 8) {
        char *end;
        double v;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') ++p;
        if (*p == ']') break;
        v = strtod(p, &end);
        if (end == p || !isfinite(v)) { snprintf(err, errn, "a bad number in \"dist\""); return -1; }
        m->k[n++] = v;
        p = end;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != ']' || !(n == 4 || n == 5 || n == 8)) { snprintf(err, errn, "\"dist\" takes 4, 5 or 8 values"); return -1; }
    m->r_fold = ld_fold_radius(m);
    return 0;
}

/* Binds the model to an active array of aw x ah pixels (<= 0: the file's own
 * frame, e.g. on a PC): the file's frame is the largest rectangle of its
 * shape centred in the array (normally the whole array). */
static inline void ld_bind(ld_model_t *m, int aw, int ah)
{
    vf_rect_t a, r;
    double s;
    if (aw <= 0 || ah <= 0) { aw = m->ref_w; ah = m->ref_h; }
    a.x = 0; a.y = 0; a.w = aw; a.h = ah;
    r = vf_field(a, m->ref_w, m->ref_h);
    s = r.w / m->ref_w;
    m->aw = aw; m->ah = ah;
    m->fx = m->ref_fx * s; m->fy = m->ref_fy * s;
    m->cx = r.x + (m->ref_cx + 0.5) * s;
    m->cy = r.y + (m->ref_cy + 0.5) * s;
    m->ok = 1;
}

/* A ray of the ideal camera -> the lens' (normalised coordinates). */
static inline void ld_distort(const ld_model_t *m, double x, double y, double *xd, double *yd)
{
    double r2 = x * x + y * y, a = ld_radial(m, r2), xy = x * y;
    *xd = x * a + 2 * m->k[2] * xy + m->k[3] * (r2 + 2 * x * x);
    *yd = y * a + m->k[2] * (r2 + 2 * y * y) + 2 * m->k[3] * xy;
}

/* --- one frame's correction ------------------------------------------------ */
typedef struct {
    int ok;
    int w, h;                       /* the frame (and the corrected one), pixels      */
    double fx0, fy0, fw, fh;        /* the field it shows, active-array pixels        */
    double fo, cxo, cyo;            /* the corrected camera, active-array pixels      */
    ld_model_t m;
} ld_view_t;

/* Does every point of the border of b (corrected camera fo) fall on the
 * lens' picture of b? */
static inline int ld_covers(const ld_model_t *m, vf_rect_t b, double fo)
{
    const int n = 48;
    int i, side;
    double rf2 = m->r_fold * m->r_fold, eps = 1e-6 * (b.w + b.h);
    for (side = 0; side < 4; ++side)
        for (i = 0; i <= n; ++i) {
            double t = (double)i / n, X, Y, x, y, xd, yd, sx, sy;
            switch (side) {
            case 0: X = b.x + t * b.w; Y = b.y; break;
            case 1: X = b.x + t * b.w; Y = b.y + b.h; break;
            case 2: X = b.x; Y = b.y + t * b.h; break;
            default: X = b.x + b.w; Y = b.y + t * b.h; break;
            }
            x = (X - m->cx) / fo; y = (Y - m->cy) / fo;
            if (x * x + y * y >= rf2) return 0;
            ld_distort(m, x, y, &xd, &yd);
            sx = m->fx * xd + m->cx; sy = m->fy * yd + m->cy;
            if (sx < b.x - eps || sx > b.x + b.w + eps || sy < b.y - eps || sy > b.y + b.h + eps) return 0;
        }
    return 1;
}

/* The correction of a frame of w x h pixels showing `field` (active-array
 * pixels x, y, w, h; NULL: the frame's whole band, i.e. zoom 1). 0, or -1
 * when the model cannot correct this frame (it folds inside it, or would
 * crop more than 25 % of it). */
static inline int ld_view_init(ld_view_t *v, const ld_model_t *m, int w, int h, const double *field)
{
    vf_rect_t a = { 0, 0, 0, 0 }, b;
    double lo, hi;
    int i;
    memset(v, 0, sizeof *v);
    if (!m || !m->ok || w < 2 || h < 2) return -1;
    v->m = *m; v->w = w; v->h = h;
    a.w = m->aw; a.h = m->ah;
    b = vf_field(a, w, h);                       /* the band: the frame's shape at zoom 1 */
    if (field && field[2] > 0 && field[3] > 0) { v->fx0 = field[0]; v->fy0 = field[1]; v->fw = field[2]; v->fh = field[3]; }
    else { v->fx0 = b.x; v->fy0 = b.y; v->fw = b.w; v->fh = b.h; }
    /* the narrowest corrected focal length that leaves no corner empty */
    lo = 0.2 * m->fx; hi = 5.0 * m->fx;
    if (!ld_covers(m, b, hi)) return -1;
    if (ld_covers(m, b, lo)) hi = lo;
    else for (i = 0; i < 60; ++i) {
        double mid = 0.5 * (lo + hi);
        if (ld_covers(m, b, mid)) hi = mid; else lo = mid;
    }
    /* A sound model crops a few per cent (the JP2's: 1 to 2 %); one that
     * needs much more is a bad fit (it folds near the frame's middle). */
    if (hi > 1.25 * m->fx) return -1;
    v->fo = hi; v->cxo = m->cx; v->cyo = m->cy;
    v->ok = 1;
    return 0;
}

/* Corrected frame pixel (u, v) -> the source frame's pixel showing it
 * (index coordinates; may fall outside the frame when zoomed). */
static inline void ld_src(const ld_view_t *v, double u, double vv, double *su, double *sv)
{
    const ld_model_t *m = &v->m;
    double X = v->fx0 + (u + 0.5) * v->fw / v->w, Y = v->fy0 + (vv + 0.5) * v->fh / v->h;
    double x = (X - v->cxo) / v->fo, y = (Y - v->cyo) / v->fo, xd, yd;
    ld_distort(m, x, y, &xd, &yd);
    *su = (m->fx * xd + m->cx - v->fx0) * v->w / v->fw - 0.5;
    *sv = (m->fy * yd + m->cy - v->fy0) * v->h / v->fh - 0.5;
}

/* The inverse: a source frame pixel -> where it lands in the corrected
 * frame (Newton's method on ld_src; a few iterations at most). */
static inline void ld_dst(const ld_view_t *v, double su, double sv, double *u, double *vv)
{
    double x = su, y = sv;
    int i;
    for (i = 0; i < 30; ++i) {
        double fu, fv, ax, ay, bx, by, ex, ey, det, dx, dy;
        ld_src(v, x, y, &fu, &fv);
        ex = fu - su; ey = fv - sv;
        if (fabs(ex) < 1e-4 && fabs(ey) < 1e-4) break;
        ld_src(v, x + 0.5, y, &ax, &ay);
        ld_src(v, x, y + 0.5, &bx, &by);
        ax = (ax - fu) * 2; ay = (ay - fv) * 2; bx = (bx - fu) * 2; by = (by - fv) * 2;   /* the Jacobian */
        det = ax * by - bx * ay;
        if (fabs(det) < 1e-12) break;
        dx = (ex * by - ey * bx) / det; dy = (ax * ey - ay * ex) / det;
        x -= dx; y -= dy;
    }
    *u = x; *vv = y;
}

/* --- the remap: a coarse grid of exact points, interpolated -------------- */
/* The map is smooth: evaluated exactly every `step` pixels, bilinear in
 * between (at 16 px on a 4096 px frame, well under 0.01 px off). */
typedef struct { int gw, gh, step; float *x, *y; } ld_grid_t;

static inline void ld_grid_free(ld_grid_t *g) { free(g->x); free(g->y); memset(g, 0, sizeof *g); }

static inline int ld_grid_build(const ld_view_t *v, int step, ld_grid_t *g)
{
    int i, j;
    memset(g, 0, sizeof *g);
    if (!v->ok || step < 1) return -1;
    g->step = step;
    g->gw = (v->w - 1 + step - 1) / step + 2;
    g->gh = (v->h - 1 + step - 1) / step + 2;
    g->x = (float *)malloc(sizeof(float) * (size_t)g->gw * g->gh);
    g->y = (float *)malloc(sizeof(float) * (size_t)g->gw * g->gh);
    if (!g->x || !g->y) { ld_grid_free(g); return -1; }
    for (j = 0; j < g->gh; ++j)
        for (i = 0; i < g->gw; ++i) {
            double su, sv;
            ld_src(v, (double)i * step, (double)j * step, &su, &sv);
            g->x[(size_t)j * g->gw + i] = (float)su;
            g->y[(size_t)j * g->gw + i] = (float)sv;
        }
    return 0;
}

/* One row of the grid's map at luma row coordinate gv: rx, ry hold gw nodes. */
static inline void ld_grid_row(const ld_grid_t *g, double gv, float *rx, float *ry)
{
    double t = gv / g->step;
    int j = (int)floor(t), i;
    float f;
    if (j < 0) j = 0;
    if (j > g->gh - 2) j = g->gh - 2;
    f = (float)(t - j);
    for (i = 0; i < g->gw; ++i) {
        const size_t a = (size_t)j * g->gw + i, b = a + (size_t)g->gw;
        rx[i] = g->x[a] + (g->x[b] - g->x[a]) * f;
        ry[i] = g->y[a] + (g->y[b] - g->y[a]) * f;
    }
}

static inline uint8_t ld_clip8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }

/* Rows [y0, y1) of a corrected plane of dw x dh pixels from the source plane
 * (sw x sh, the same size), through grid g (built on the luma frame).
 * chroma: the plane is at half resolution (4:2:0). cubic: Catmull-Rom
 * (sharper) instead of bilinear. Outside the source: its edge, repeated. */
static inline void ld_remap_plane(const ld_grid_t *g, const uint8_t *src, int sw, int sh,
                                  uint8_t *dst, int dw, int y0, int y1, int chroma, int cubic,
                                  float *rx, float *ry)
{
    int x, y;
    for (y = y0; y < y1; ++y) {
        uint8_t *out = dst + (size_t)y * dw;
        double gv = chroma ? 2.0 * y + 0.5 : y;
        ld_grid_row(g, gv, rx, ry);
        for (x = 0; x < dw; ++x) {
            double gu = chroma ? 2.0 * x + 0.5 : x, t = gu / g->step;
            int i = (int)t;
            float f, su, sv;
            if (i > g->gw - 2) i = g->gw - 2;
            f = (float)(t - i);
            su = rx[i] + (rx[i + 1] - rx[i]) * f;
            sv = ry[i] + (ry[i + 1] - ry[i]) * f;
            if (chroma) { su = (su - 0.5f) * 0.5f; sv = (sv - 0.5f) * 0.5f; }
            if (su < 0) su = 0;
            if (sv < 0) sv = 0;
            if (su > sw - 1) su = (float)(sw - 1);
            if (sv > sh - 1) sv = (float)(sh - 1);
            if (cubic) {
                int ix = (int)su, iy = (int)sv, k, l;
                float tx = su - ix, ty = sv - iy, wx[4], wy[4], acc = 0;
                wx[0] = ((-tx + 2) * tx - 1) * tx * 0.5f; wx[1] = ((3 * tx - 5) * tx * tx + 2) * 0.5f;
                wx[2] = ((-3 * tx + 4) * tx + 1) * tx * 0.5f; wx[3] = (tx - 1) * tx * tx * 0.5f;
                wy[0] = ((-ty + 2) * ty - 1) * ty * 0.5f; wy[1] = ((3 * ty - 5) * ty * ty + 2) * 0.5f;
                wy[2] = ((-3 * ty + 4) * ty + 1) * ty * 0.5f; wy[3] = (ty - 1) * ty * ty * 0.5f;
                if (ix >= 1 && iy >= 1 && ix + 2 < sw && iy + 2 < sh) {
                    const uint8_t *p = src + (size_t)(iy - 1) * sw + (ix - 1);
                    for (l = 0; l < 4; ++l, p += sw)
                        acc += wy[l] * (wx[0] * p[0] + wx[1] * p[1] + wx[2] * p[2] + wx[3] * p[3]);
                } else {
                    for (l = 0; l < 4; ++l) {
                        int yy = iy - 1 + l;
                        float r = 0;
                        yy = yy < 0 ? 0 : yy >= sh ? sh - 1 : yy;
                        for (k = 0; k < 4; ++k) {
                            int xx = ix - 1 + k;
                            xx = xx < 0 ? 0 : xx >= sw ? sw - 1 : xx;
                            r += wx[k] * src[(size_t)yy * sw + xx];
                        }
                        acc += wy[l] * r;
                    }
                }
                out[x] = ld_clip8(acc);
            } else {
                int ix = (int)su, iy = (int)sv;
                float tx, ty;
                const uint8_t *p;
                if (ix > sw - 2) ix = sw - 2;
                if (iy > sh - 2) iy = sh - 2;
                tx = su - ix; ty = sv - iy;
                p = src + (size_t)iy * sw + ix;
                out[x] = ld_clip8((p[0] * (1 - tx) + p[1] * tx) * (1 - ty) + (p[sw] * (1 - tx) + p[sw + 1] * tx) * ty);
            }
        }
    }
}

#endif
