/* yuvproc.c -- see yuvproc.h. Plain C, pthreads over the rows. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "yuvproc.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

/* --- a parallel loop over row ranges ------------------------------------ */
typedef void (*row_fn)(void *ctx, int y0, int y1);
typedef struct { row_fn fn; void *ctx; int y0, y1; } job_t;
static void *job_run(void *p) { job_t *j = p; j->fn(j->ctx, j->y0, j->y1); return NULL; }
static int nthreads(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : n > 8 ? 8 : (int)n;
}
static void parallel_rows(row_fn fn, void *ctx, int h)
{
    pthread_t th[8]; job_t jobs[8];
    int n = nthreads(), i, started = 0;
    if (h < 64) n = 1;
    for (i = 0; i < n; ++i) {
        jobs[i].fn = fn; jobs[i].ctx = ctx; jobs[i].y0 = h * i / n; jobs[i].y1 = h * (i + 1) / n;
        if (i == n - 1 || pthread_create(&th[i], NULL, job_run, &jobs[i]) != 0) { job_run(&jobs[i]); } else started++;
    }
    for (i = 0; i < started; ++i) pthread_join(th[i], NULL);
}

/* --- box filter of a float plane by integral image, radius r ------------ */
static void box_filter(const float *src, float *dst, int w, int h, int r)
{
    /* row sums then column sums with a running window: O(w*h), no integral image */
    float *tmp = malloc((size_t)w * h * sizeof *tmp);
    int x, y;
    if (!tmp) { memcpy(dst, src, (size_t)w * h * sizeof *dst); return; }
    for (y = 0; y < h; ++y) {                      /* horizontal window        */
        const float *s = src + (size_t)y * w; float *t = tmp + (size_t)y * w;
        double acc = 0; int cnt = 0;
        for (x = 0; x <= r && x < w; ++x) { acc += s[x]; cnt++; }
        for (x = 0; x < w; ++x) {
            t[x] = (float)(acc / cnt);
            if (x + r + 1 < w) { acc += s[x + r + 1]; cnt++; }
            if (x - r >= 0) { acc -= s[x - r]; cnt--; }
        }
    }
    {   /* vertical window: column by column over a strip of rows */
        double *acc = calloc((size_t)w, sizeof *acc); int *cnt = calloc((size_t)w, sizeof *cnt);
        if (!acc || !cnt) { free(acc); free(cnt); memcpy(dst, tmp, (size_t)w * h * sizeof *dst); free(tmp); return; }
        for (y = 0; y <= r && y < h; ++y) for (x = 0; x < w; ++x) { acc[x] += tmp[(size_t)y * w + x]; cnt[x]++; }
        for (y = 0; y < h; ++y) {
            float *d = dst + (size_t)y * w;
            for (x = 0; x < w; ++x) d[x] = (float)(acc[x] / cnt[x]);
            if (y + r + 1 < h) { const float *a = tmp + (size_t)(y + r + 1) * w; for (x = 0; x < w; ++x) { acc[x] += a[x]; cnt[x]++; } }
            if (y - r >= 0) { const float *a = tmp + (size_t)(y - r) * w; for (x = 0; x < w; ++x) { acc[x] -= a[x]; cnt[x]--; } }
        }
        free(acc); free(cnt);
    }
    free(tmp);
}

/* Colour noise: a cross bilateral on the two chroma planes together. The
 * chroma noise is measured per brightness band (the median deviation of a
 * pixel from its four neighbours), then each pixel is averaged with its
 * neighbours weighted by how far their colour is from its own, against
 * 2.5 times that noise: blotches fall within it and merge, colour edges do
 * not and stay, even where the luminance is flat or textured. Two
 * separable passes, +-8 pixels. After ProCam's chromaBilateral (BSD-3). */
#define CB_BANDS 16
#define CB_RADIUS 8
typedef struct { const float *inu, *inv, *k; float *outu, *outv; int w, h, axis; const float *sp; } bil_ctx_t;
static void bilateral_rows(void *p, int y0, int y1)
{
    bil_ctx_t *c = p; int w = c->w, h = c->h, x, y, t;
    for (y = y0; y < y1; ++y)
        for (x = 0; x < w; ++x) {
            size_t i = (size_t)y * w + x;
            float u0 = c->inu[i], v0 = c->inv[i], kk = c->k[i], sw = 0, su = 0, sv = 0;
            for (t = -CB_RADIUS; t <= CB_RADIUS; ++t) {
                int xx = x, yy = y;
                size_t j; float du, dv, e, wgt;
                if (c->axis) { xx = x + t; if (xx < 0) xx = -xx; if (xx >= w) xx = 2 * w - 2 - xx; }
                else { yy = y + t; if (yy < 0) yy = -yy; if (yy >= h) yy = 2 * h - 2 - yy; }
                j = (size_t)yy * w + xx;
                du = c->inu[j] - u0; dv = c->inv[j] - v0;
                e = (du * du + dv * dv) * kk;           /* (d / sigma)^2 / 2 */
                wgt = e < 8.0f ? c->sp[t + CB_RADIUS] * expf(-e) : 0.0f;
                sw += wgt; su += wgt * c->inu[j]; sv += wgt * c->inv[j];
            }
            c->outu[i] = sw > 0 ? su / sw : u0; c->outv[i] = sw > 0 ? sv / sw : v0;
        }
}
static int bilateral_chroma(const uint8_t *y, int w, int h, uint8_t *u, uint8_t *v, float strength)
{
    const int cw = w / 2, ch = h / 2;
    size_t n = (size_t)cw * ch, i;
    float *a = malloc(n * sizeof *a), *b = malloc(n * sizeof *b), *c2 = malloc(n * sizeof *c2), *d = malloc(n * sizeof *d), *k = malloc(n * sizeof *k);
    uint8_t *band = malloc(n);
    float noise[CB_BANDS], sp[2 * CB_RADIUS + 1], last = -1;
    unsigned hist[CB_BANDS][256]; unsigned long cnt[CB_BANDS];
    int x, yy, bnd, t, pass;
    if (!a || !b || !c2 || !d || !k || !band) { free(a); free(b); free(c2); free(d); free(k); free(band); return -1; }
    for (yy = 0; yy < ch; ++yy)                 /* the brightness band of each chroma pixel */
        for (x = 0; x < cw; ++x) {
            const uint8_t *s = y + (size_t)(2 * yy) * w + 2 * x;
            band[(size_t)yy * cw + x] = (uint8_t)(((s[0] + s[1] + s[w] + s[w + 1]) / 4) * CB_BANDS / 256);
        }
    memset(hist, 0, sizeof hist); memset(cnt, 0, sizeof cnt);
    for (yy = 1; yy < ch - 1; yy += 2)          /* the noise per band: residual against the 4 neighbours */
        for (x = 1; x < cw - 1; x += 2) {
            size_t p = (size_t)yy * cw + x;
            float ru = u[p] - 0.25f * (u[p - 1] + u[p + 1] + u[p - cw] + u[p + cw]);
            float rv = v[p] - 0.25f * (v[p - 1] + v[p + 1] + v[p - cw] + v[p + cw]);
            int r = (int)(sqrtf(0.5f * (ru * ru + rv * rv)) * 8.0f + 0.5f);   /* eighths */
            if (r > 255) r = 255;
            hist[band[p]][r]++; cnt[band[p]]++;
        }
    for (bnd = 0; bnd < CB_BANDS; ++bnd) {
        unsigned long acc = 0; int r;
        if (cnt[bnd] < 32) { noise[bnd] = -1; continue; }
        for (r = 0; r < 256; ++r) { acc += hist[bnd][r]; if (acc * 2 >= cnt[bnd]) break; }
        noise[bnd] = (r / 8.0f) * 1.4826f / sqrtf(1.25f) * 1.2f;   /* MAD -> sigma; 4-neighbour residual has 1.25 sigma^2 */
    }
    for (bnd = 0; bnd < CB_BANDS; ++bnd) if (noise[bnd] >= 0) { last = noise[bnd]; break; }
    if (last < 0) last = 1.0f;
    for (bnd = 0; bnd < CB_BANDS; ++bnd) {
        if (noise[bnd] < 0) noise[bnd] = last;
        if (noise[bnd] < 0.05f) noise[bnd] = 0.05f;    /* a clipped area has no residual at all */
        last = noise[bnd];
    }
    for (i = 0; i < n; ++i) { float s = strength * noise[band[i]]; k[i] = 1.0f / (2.0f * s * s); a[i] = u[i]; b[i] = v[i]; }
    for (t = -CB_RADIUS; t <= CB_RADIUS; ++t) sp[t + CB_RADIUS] = expf(-0.5f * (float)t * t / 16.0f);   /* spatial sigma 4 */
    for (pass = 0; pass < 2; ++pass) {
        bil_ctx_t bc = { a, b, k, c2, d, cw, ch, pass == 0, sp };
        parallel_rows(bilateral_rows, &bc, ch);
        { float *tmp = a; a = c2; c2 = tmp; tmp = b; b = d; d = tmp; }
    }
    for (i = 0; i < n; ++i) {
        u[i] = (uint8_t)(a[i] < 0 ? 0 : a[i] > 255 ? 255 : a[i] + 0.5f);
        v[i] = (uint8_t)(b[i] < 0 ? 0 : b[i] > 255 ? 255 : b[i] + 0.5f);
    }
    free(a); free(b); free(c2); free(d); free(k); free(band);
    return 0;
}

/* False colour: a median on each chroma plane (3x3 or 5x5) removes the
 * colour specks of one or two pixels that the ISP's demosaicing invents on
 * fine periodic textures (a zipper's teeth, a weave). It also removes any
 * real colour detail of that size, which is why it is an option. */
typedef struct { const uint8_t *in; uint8_t *out; int w, h, r; } med_ctx_t;
static void median_rows(void *p, int y0, int y1)
{
    med_ctx_t *c = p; int w = c->w, h = c->h, r = c->r, x, y, i, j;
    for (y = y0; y < y1; ++y)
        for (x = 0; x < w; ++x) {
            unsigned hist[256] = { 0 }; int cnt = 0, acc = 0, v;
            for (j = -r; j <= r; ++j) {
                int yy = y + j; if (yy < 0) yy = -yy; if (yy >= h) yy = 2 * h - 2 - yy;
                for (i = -r; i <= r; ++i) {
                    int xx = x + i; if (xx < 0) xx = -xx; if (xx >= w) xx = 2 * w - 2 - xx;
                    hist[c->in[(size_t)yy * w + xx]]++; cnt++;
                }
            }
            for (v = 0; v < 256; ++v) { acc += hist[v]; if (acc * 2 > cnt) break; }
            c->out[(size_t)y * w + x] = (uint8_t)v;
        }
}
static int median_chroma(int w, int h, uint8_t *u, uint8_t *v, int radius)
{
    const int cw = w / 2, ch = h / 2;
    uint8_t *tmp = malloc((size_t)cw * ch);
    med_ctx_t c;
    if (!tmp) return -1;
    c.w = cw; c.h = ch; c.r = radius;
    c.in = u; c.out = tmp; parallel_rows(median_rows, &c, ch); memcpy(u, tmp, (size_t)cw * ch);
    c.in = v; c.out = tmp; parallel_rows(median_rows, &c, ch); memcpy(v, tmp, (size_t)cw * ch);
    free(tmp);
    return 0;
}

/* The luminance noise of a frame: the median absolute deviation of its
 * finest detail (each pixel minus the mean of its 3x3), on a grid of
 * samples, divided by 0.6745. Edges barely move a median. */
static float luma_sigma(const uint8_t *y, int w, int h)
{
    unsigned hist[256] = { 0 }; unsigned long total = 0, acc = 0;
    int x, yy, k;
    for (yy = 4; yy < h - 4; yy += 8)
        for (x = 4; x < w - 4; x += 8) {
            const uint8_t *s = y + (size_t)yy * w + x;
            int sum = 0, d;
            sum = s[-w - 1] + s[-w] + s[-w + 1] + s[-1] + s[1] + s[w - 1] + s[w] + s[w + 1];
            d = abs(s[0] * 8 - sum);                 /* 8 x (pixel - mean of the 8 around) */
            d = d / 8;
            if (d > 255) d = 255;
            hist[d]++; total++;
        }
    for (k = 0; k < 256; ++k) { acc += hist[k]; if (acc * 2 >= total) break; }
    /* pixel - mean of 8 neighbours has variance s^2 (1 + 1/8): correct */
    return (float)k / 0.6745f / sqrtf(1.125f);
}

/* Global shift between two frames: the sum of absolute differences on the
 * luminance reduced 8 times over a search of +-8 steps (+-64 pixels), then
 * refined at 4 times and at full size by +-2 pixels. Hand shake between
 * frames 50 ms apart is a few pixels; rotation is not corrected (the
 * corners merge a little less). */
static void downsample(const uint8_t *y, int w, int h, int f, uint8_t *out, int *ow, int *oh)
{
    int x, yy, i, j;
    *ow = w / f; *oh = h / f;
    for (yy = 0; yy < *oh; ++yy)
        for (x = 0; x < *ow; ++x) {
            unsigned s = 0;
            for (j = 0; j < f; ++j) for (i = 0; i < f; ++i) s += y[(size_t)(yy * f + j) * w + x * f + i];
            out[(size_t)yy * *ow + x] = (uint8_t)(s / (f * f));
        }
}
/* The mean absolute difference, each pixel's capped at 24: a moving
 * object, however bright, cannot pull the alignment towards itself; the
 * still background decides. */
static double sad(const uint8_t *a, const uint8_t *b, int w, int h, int dy, int dx, int step)
{
    double s = 0; long cnt = 0; int x, y;
    for (y = 32; y < h - 32; y += step)
        for (x = 32; x < w - 32; x += step) {
            int by = y - dy, bx = x - dx, d;
            if (by < 0 || bx < 0 || by >= h || bx >= w) continue;
            d = abs((int)a[(size_t)y * w + x] - (int)b[(size_t)by * w + bx]);
            s += d > 24 ? 24 : d; cnt++;
        }
    return cnt ? s / cnt : 1e9;
}
static void find_shift(const uint8_t *ya, const uint8_t *yb, int w, int h, int *dy, int *dx)
{
    int w8, h8, w4, h4, by = 0, bx = 0, y, x;
    uint8_t *a8 = malloc((size_t)w * h / 64 + 16), *b8 = malloc((size_t)w * h / 64 + 16);
    uint8_t *a4 = malloc((size_t)w * h / 16 + 16), *b4 = malloc((size_t)w * h / 16 + 16);
    double best;
    *dy = *dx = 0;
    if (!a8 || !b8 || !a4 || !b4) { free(a8); free(b8); free(a4); free(b4); return; }
    downsample(ya, w, h, 8, a8, &w8, &h8); downsample(yb, w, h, 8, b8, &w8, &h8);
    best = 1e18;
    for (y = -8; y <= 8; ++y) for (x = -8; x <= 8; ++x) { double s = sad(a8, b8, w8, h8, y, x, 2); if (s < best) { best = s; by = y; bx = x; } }
    downsample(ya, w, h, 4, a4, &w4, &h4); downsample(yb, w, h, 4, b4, &w4, &h4);
    { int cy = by * 2, cx = bx * 2; best = 1e18;
      for (y = cy - 2; y <= cy + 2; ++y) for (x = cx - 2; x <= cx + 2; ++x) { double s = sad(a4, b4, w4, h4, y, x, 2); if (s < best) { best = s; by = y; bx = x; } } }
    { int cy = by * 4, cx = bx * 4; best = 1e18;
      for (y = cy - 3; y <= cy + 3; ++y) for (x = cx - 3; x <= cx + 3; ++x) { double s = sad(ya, yb, w, h, y, x, 7); if (s < best) { best = s; by = y; bx = x; } } }
    *dy = by; *dx = bx;
    free(a8); free(b8); free(a4); free(b4);
}

/* Merging the frames: each later frame is shifted onto the first, then
 * added where its luminance differs from the first by no more than the
 * noise allows (a soft threshold on the local mean difference); where it
 * differs more, something moved, and the first frame stands alone. The
 * weights are the same for the colour planes. */
typedef struct {
    const ir_yuv_t *ref, *f; int dy, dx, w, h;
    float *ay, *au, *av, *wy, *wc;              /* accumulators (Y, U, V) and weights (luma, chroma) */
    const float *d;                             /* local mean |diff| (box 7x7) at full res */
    float T; double merged;                     /* sum of weights, for the share */
} merge_ctx_t;
static void merge_rows(void *p, int y0, int y1)
{
    merge_ctx_t *c = p; int w = c->w, h = c->h, x, y; double m = 0;
    for (y = y0; y < y1; ++y) {
        int sy = y - c->dy;
        for (x = 0; x < w; ++x) {
            int sx = x - c->dx; float wgt;
            if (sy < 0 || sy >= h || sx < 0 || sx >= w) continue;
            { float dd = c->d[(size_t)y * w + x]; wgt = (2 * c->T - dd) / c->T; wgt = wgt < 0 ? 0 : wgt > 1 ? 1 : wgt; }
            c->ay[(size_t)y * w + x] += wgt * c->f->y[(size_t)sy * w + sx];
            c->wy[(size_t)y * w + x] += wgt; m += wgt;
            if (!(y & 1) && !(x & 1)) {
                int cw = w / 2, cy = y / 2, cx = x / 2, csy = sy / 2, csx = sx / 2;
                c->au[(size_t)cy * cw + cx] += wgt * c->f->u[(size_t)csy * cw + csx];
                c->av[(size_t)cy * cw + cx] += wgt * c->f->v[(size_t)csy * cw + csx];
                c->wc[(size_t)cy * cw + cx] += wgt;
            }
        }
    }
    /* merged share: not thread-safe to sum here; each thread writes its slice's sum into d? keep simple: atomic-ish add */
    { static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mu); c->merged += m; pthread_mutex_unlock(&mu); }
}
typedef struct { const uint8_t *a, *b; int dy, dx, w, h; float *d; } diff_ctx_t;
static void diff_rows(void *p, int y0, int y1)
{
    diff_ctx_t *c = p; int x, y;
    for (y = y0; y < y1; ++y) {
        int sy = y - c->dy;
        for (x = 0; x < c->w; ++x) {
            int sx = x - c->dx;
            c->d[(size_t)y * c->w + x] = (sy < 0 || sy >= c->h || sx < 0 || sx >= c->w) ? 1e6f
                : (float)abs((int)c->a[(size_t)y * c->w + x] - (int)c->b[(size_t)sy * c->w + sx]);
        }
    }
}

int yuv_denoise(const ir_yuv_t *frames, int n, int colour, int median, ir_yuv_t *out, yuv_denoise_report_t *rep)
{
    static const float strengths[6] = { 0.0f, 1.0f, 2.0f, 3.0f, 5.0f, 8.0f };   /* times the measured noise; 5 and 8 are deliberately too much */
    const ir_yuv_t *ref = &frames[0];
    int w = ref->w, h = ref->h, k;
    size_t np = (size_t)w * h, nc = (size_t)(w / 2) * (h / 2);
    double t0;
    memset(rep, 0, sizeof *rep);
    if (n < 1 || n > 4 || !ref->y) return -1;
    *out = *ref;
    out->y = malloc(np); out->u = malloc(nc); out->v = malloc(nc);
    if (!out->y || !out->u || !out->v) { free(out->y); free(out->u); free(out->v); out->y = out->u = out->v = NULL; return -1; }
    rep->sigma = luma_sigma(ref->y, w, h);
    rep->merged_pct[0] = 100.0f;
    if (n == 1) {
        memcpy(out->y, ref->y, np); memcpy(out->u, ref->u, nc); memcpy(out->v, ref->v, nc);
    } else {
        float *ay = calloc(np, sizeof *ay), *au = calloc(nc, sizeof *au), *av = calloc(nc, sizeof *av);
        float *wy = calloc(np, sizeof *wy), *wc = calloc(nc, sizeof *wc), *d = malloc(np * sizeof *d), *db = malloc(np * sizeof *db);
        size_t i;
        if (!ay || !au || !av || !wy || !wc || !d || !db) { free(ay); free(au); free(av); free(wy); free(wc); free(d); free(db); free(out->y); free(out->u); free(out->v); out->y = out->u = out->v = NULL; return -1; }
        for (i = 0; i < np; ++i) { ay[i] = ref->y[i]; wy[i] = 1; }
        for (i = 0; i < nc; ++i) { au[i] = ref->u[i]; av[i] = ref->v[i]; wc[i] = 1; }
        for (k = 1; k < n; ++k) {
            merge_ctx_t mc; diff_ctx_t dc;
            t0 = now_ms();
            find_shift(ref->y, frames[k].y, w, h, &rep->shift_dy[k], &rep->shift_dx[k]);
            rep->ms_align += now_ms() - t0;
            t0 = now_ms();
            dc.a = ref->y; dc.b = frames[k].y; dc.dy = rep->shift_dy[k]; dc.dx = rep->shift_dx[k]; dc.w = w; dc.h = h; dc.d = d;
            parallel_rows(diff_rows, &dc, h);
            box_filter(d, db, w, h, 3);              /* local mean |diff|, 7x7 */
            mc.ref = ref; mc.f = &frames[k]; mc.dy = dc.dy; mc.dx = dc.dx; mc.w = w; mc.h = h;
            mc.ay = ay; mc.au = au; mc.av = av; mc.wy = wy; mc.wc = wc; mc.d = db;
            mc.T = 2.5f * rep->sigma * 1.4142f * 0.8f; mc.merged = 0;
            parallel_rows(merge_rows, &mc, h);
            rep->merged_pct[k] = (float)(100.0 * mc.merged / (double)np);
            rep->ms_merge += now_ms() - t0;
        }
        for (i = 0; i < np; ++i) { float o = ay[i] / wy[i]; out->y[i] = (uint8_t)(o < 0 ? 0 : o > 255 ? 255 : o + 0.5f); }
        for (i = 0; i < nc; ++i) {
            float ou = au[i] / wc[i], ov = av[i] / wc[i];
            out->u[i] = (uint8_t)(ou < 0 ? 0 : ou > 255 ? 255 : ou + 0.5f);
            out->v[i] = (uint8_t)(ov < 0 ? 0 : ov > 255 ? 255 : ov + 0.5f);
        }
        free(ay); free(au); free(av); free(wy); free(wc); free(d); free(db);
    }
    if (median > 0 || colour > 0) {
        t0 = now_ms();
        if (median > 0 && median_chroma(w, h, out->u, out->v, median > 4 ? 4 : median)) return -1;   /* 3x3 .. 9x9 */
        if (colour > 0 && bilateral_chroma(out->y, w, h, out->u, out->v, strengths[colour > 5 ? 5 : colour])) return -1;
        rep->ms_colour = now_ms() - t0;
    }
    return 0;
}

/* Sharpening: the luminance minus its 3x3 mean is the fine detail; a
 * part of it (the strength) is added back, except where it is smaller
 * than the noise, where nothing is added: grain is not sharpened. Done
 * after the merge, on a frame with less noise than any single one. */
typedef struct { const uint8_t *src; uint8_t *dst; int w, h; float amount, thresh; } sharp_ctx_t;
static void sharpen_rows(void *p, int y0, int y1)
{
    sharp_ctx_t *c = p; int w = c->w, h = c->h, x, y;
    for (y = y0; y < y1; ++y) {
        const uint8_t *s = c->src + (size_t)y * w; uint8_t *d = c->dst + (size_t)y * w;
        if (y == 0 || y == h - 1) { memcpy(d, s, (size_t)w); continue; }
        d[0] = s[0]; d[w - 1] = s[w - 1];
        for (x = 1; x < w - 1; ++x) {
            int sum = s[x - w - 1] + s[x - w] + s[x - w + 1] + s[x - 1] + s[x + 1] + s[x + w - 1] + s[x + w] + s[x + w + 1];
            float detail = s[x] - (sum + s[x]) / 9.0f, ad = fabsf(detail), o;
            if (ad <= c->thresh) { d[x] = s[x]; continue; }
            /* a soft start: the part above the threshold counts, so the rule has no step */
            detail = detail > 0 ? detail - c->thresh : detail + c->thresh;
            o = s[x] + c->amount * detail;
            d[x] = (uint8_t)(o < 0 ? 0 : o > 255 ? 255 : o + 0.5f);
        }
    }
}
void yuv_sharpen(ir_yuv_t *f, int level, float sigma)
{
    static const float amounts[6] = { 0.0f, 0.5f, 1.0f, 1.5f, 2.5f, 4.0f };   /* 2.5 and 4 are deliberately too much */
    sharp_ctx_t c; uint8_t *out;
    if (!f || !f->y || level < 1 || level > 5) return;
    out = malloc((size_t)f->w * f->h);
    if (!out) return;
    c.src = f->y; c.dst = out; c.w = f->w; c.h = f->h; c.amount = amounts[level];
    c.thresh = 1.0f * sigma;                         /* the fine detail of pure noise: about 0.9 sigma */
    parallel_rows(sharpen_rows, &c, f->h);
    memcpy(f->y, out, (size_t)f->w * f->h);
    free(out);
}
