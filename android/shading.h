/*
 * shading.h -- a MEASURED lens-shading (vignetting) calibration, and the
 * gain map composed from it for one photo. Pure C, no Android: included by
 * indraw.c and tested on a PC (android/tests/shading_test.c).
 *
 * Why (measured on the JP2, October 2026, docs/VENDOR-TESTS.md): the HAL's
 * own shading map corrects only about half the falloff, does not follow the
 * lens position, and under-corrects the red in the corners under tungsten.
 * The flat-field measurements showed two independent effects:
 *   - the LUMINANCE falloff depends on the lens position (about 22 % less
 *     gain at the closest focus than at infinity), hardly on the light;
 *   - the COLOUR shading depends on the light (red 13 % above green in the
 *     corners in daylight, 23 % under tungsten), hardly on the focus.
 * So the calibration holds a luminance grid per lens position and a colour
 * factor grid per light, and a photo's map is their product at its own
 * lens position and colour temperature.
 *
 * The file (JSON, device-profiles/lens-shading/, named after the phone so
 * that users can exchange it): see android/data/lens-shading/ for the JP2's.
 *   "grid": {"rows": R, "cols": C}       edge-anchored, raw sensor coordinates
 *   "luminance": [{"hal_diopters": D, "gain": [R*C]}, ...]   green, centre ~1
 *   "colour":    [{"cct": K, "r": [R*C], "g": [R*C], "b": [R*C]}, ...]
 * hal_diopters is the HAL's own LENS_FOCUS_DISTANCE value (the lens
 * position), NOT a real distance: it needs no focus calibration and is the
 * same for every phone of the model.
 *
 * Composition, per grid point:  G = lum(D) x g(K),  R = lum(D) x r(K),
 * B = lum(D) x b(K); lum interpolated linearly in D between the two measured
 * positions around it (the nearest one outside the range), the colour in
 * mireds (1/K) between the two lights around K. Then the user's dosing, as
 * in Ric9k's RAWfish calibration script: gains clamped to [1, max_gain];
 * the pivot (balance 0..100) divides every gain by max_gain_found^(balance/
 * 100), so 0 only brightens the edges and 100 only darkens towards the
 * darkest corner (darktable clips values pushed above white: a pivot keeps
 * the edges below it); the strength (0..100) blends towards no correction.
 */
#ifndef INDRAW_SHADING_H
#define INDRAW_SHADING_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHADING_MAX_POINTS 16

typedef struct {
    int rows, cols;
    int width, height;                       /* the raw image the grid covers */
    char cfa[8];                             /* "RGGB", "BGGR"... as measured */
    int nlum;
    double lum_d[SHADING_MAX_POINTS];        /* sorted ascending              */
    float *lum[SHADING_MAX_POINTS];
    int ncol;
    double col_k[SHADING_MAX_POINTS];        /* sorted ascending              */
    float *col_r[SHADING_MAX_POINTS], *col_g[SHADING_MAX_POINTS], *col_b[SHADING_MAX_POINTS];
} shading_cal_t;

static void shading_free(shading_cal_t *c)
{
    int i;
    for (i = 0; i < c->nlum; ++i) free(c->lum[i]);
    for (i = 0; i < c->ncol; ++i) { free(c->col_r[i]); free(c->col_g[i]); free(c->col_b[i]); }
    memset(c, 0, sizeof *c);
}

/* --- a small scanner for this one file format (written by indRAW) ------- */
/* "key" as a JSON key: quoted, and not an escaped quote inside another
 * string (a "_comment" that names a key, \"gain\", is skipped). */
static const char *sh_key(const char *s, const char *end, const char *key)
{
    size_t n = strlen(key);
    const char *start = s;
    for (; s && s + n + 2 <= end; ++s)
        if (s[0] == '"' && !strncmp(s + 1, key, n) && s[n + 1] == '"' && (s == start || s[-1] != '\\')) return s + n + 2;
    return NULL;
}
static int sh_number(const char *s, const char *end, const char *key, double *v)
{
    const char *p = sh_key(s, end, key);
    char *e;
    if (!p || !(p = strchr(p, ':')) || p >= end) return -1;
    *v = strtod(p + 1, &e);
    return e == p + 1 ? -1 : 0;
}
static int sh_string(const char *s, const char *end, const char *key, char *out, size_t n)
{
    const char *p = sh_key(s, end, key), *q;
    if (!p || !(p = strchr(p, ':')) || !(p = strchr(p, '"')) || p >= end || !(q = strchr(p + 1, '"'))) return -1;
    if ((size_t)(q - p - 1) >= n) return -1;
    memcpy(out, p + 1, (size_t)(q - p - 1)); out[q - p - 1] = '\0';
    return 0;
}
/* n floats of the array after "key"; 0 if exactly n were read. */
static int sh_floats(const char *s, const char *end, const char *key, float *out, int n)
{
    const char *p = sh_key(s, end, key);
    int k = 0;
    if (!p || !(p = strchr(p, '[')) || p >= end) return -1;
    ++p;
    while (k < n && p < end) {
        char *e;
        double v;
        while (p < end && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
        if (*p == ']') break;
        v = strtod(p, &e);
        if (e == p) return -1;
        out[k++] = (float)v;
        p = e;
    }
    return k == n ? 0 : -1;
}
/* The end of the JSON object or array opened at *s ('{' or '['). */
static const char *sh_close(const char *s, const char *end)
{
    int depth = 0;
    for (; s < end; ++s) {
        if (*s == '{' || *s == '[') ++depth;
        else if (*s == '}' || *s == ']') { if (--depth == 0) return s + 1; }
    }
    return NULL;
}

/* Parses a calibration file's text; 0 on success, else -1 with a reason. */
static int shading_parse(const char *text, shading_cal_t *c, char *err, size_t errn)
{
    const char *end = text + strlen(text), *arr, *aend, *o, *oend;
    double v;
    int n, i, j;
    memset(c, 0, sizeof *c);
#define SH_FAIL(msg) do { snprintf(err, errn, "%s", msg); shading_free(c); return -1; } while (0)
    if (sh_number(text, end, "rows", &v)) SH_FAIL("no grid rows");
    c->rows = (int)v;
    if (sh_number(text, end, "cols", &v)) SH_FAIL("no grid cols");
    c->cols = (int)v;
    if (c->rows < 2 || c->cols < 2 || c->rows * c->cols > 64 * 64) SH_FAIL("bad grid size");
    n = c->rows * c->cols;
    if (sh_number(text, end, "width", &v)) SH_FAIL("no width");
    c->width = (int)v;
    if (sh_number(text, end, "height", &v)) SH_FAIL("no height");
    c->height = (int)v;
    if (sh_string(text, end, "cfa", c->cfa, sizeof c->cfa)) SH_FAIL("no cfa");
    /* luminance: an array of {hal_diopters, gain} */
    if (!(arr = sh_key(text, end, "luminance")) || !(arr = strchr(arr, '[')) || !(aend = sh_close(arr, end)))
        SH_FAIL("no luminance");
    for (o = arr + 1; (o = strchr(o, '{')) && o < aend; o = oend) {
        if (c->nlum >= SHADING_MAX_POINTS) SH_FAIL("too many lens positions");
        if (!(oend = sh_close(o, aend))) SH_FAIL("broken luminance entry");
        if (sh_number(o, oend, "hal_diopters", &v)) SH_FAIL("a luminance entry has no hal_diopters");
        c->lum_d[c->nlum] = v;
        if (!(c->lum[c->nlum] = malloc(sizeof(float) * (size_t)n))) SH_FAIL("no memory");
        if (sh_floats(o, oend, "gain", c->lum[c->nlum], n)) { ++c->nlum; SH_FAIL("a luminance grid is not rows x cols"); }
        ++c->nlum;
    }
    /* colour: an array of {cct, r, g, b} */
    if (!(arr = sh_key(text, end, "colour")) || !(arr = strchr(arr, '[')) || !(aend = sh_close(arr, end)))
        SH_FAIL("no colour");
    for (o = arr + 1; (o = strchr(o, '{')) && o < aend; o = oend) {
        if (c->ncol >= SHADING_MAX_POINTS) SH_FAIL("too many lights");
        if (!(oend = sh_close(o, aend))) SH_FAIL("broken colour entry");
        if (sh_number(o, oend, "cct", &v) || v < 1000 || v > 20000) SH_FAIL("a colour entry has no valid cct");
        c->col_k[c->ncol] = v;
        c->col_r[c->ncol] = malloc(sizeof(float) * (size_t)n);
        c->col_g[c->ncol] = malloc(sizeof(float) * (size_t)n);
        c->col_b[c->ncol] = malloc(sizeof(float) * (size_t)n);
        ++c->ncol;
        if (!c->col_r[c->ncol - 1] || !c->col_g[c->ncol - 1] || !c->col_b[c->ncol - 1]) SH_FAIL("no memory");
        if (sh_floats(o, oend, "r", c->col_r[c->ncol - 1], n) || sh_floats(o, oend, "g", c->col_g[c->ncol - 1], n) ||
            sh_floats(o, oend, "b", c->col_b[c->ncol - 1], n)) SH_FAIL("a colour grid is not rows x cols");
    }
    if (c->nlum < 1 || c->ncol < 1) SH_FAIL("needs at least one lens position and one light");
    /* sort both by their key (insertion sort: a few entries) */
    for (i = 1; i < c->nlum; ++i)
        for (j = i; j > 0 && c->lum_d[j - 1] > c->lum_d[j]; --j) {
            double td = c->lum_d[j]; float *tg = c->lum[j];
            c->lum_d[j] = c->lum_d[j - 1]; c->lum[j] = c->lum[j - 1]; c->lum_d[j - 1] = td; c->lum[j - 1] = tg;
        }
    for (i = 1; i < c->ncol; ++i)
        for (j = i; j > 0 && c->col_k[j - 1] > c->col_k[j]; --j) {
            double tk = c->col_k[j]; float *tr = c->col_r[j], *tg = c->col_g[j], *tb = c->col_b[j];
            c->col_k[j] = c->col_k[j - 1]; c->col_r[j] = c->col_r[j - 1]; c->col_g[j] = c->col_g[j - 1]; c->col_b[j] = c->col_b[j - 1];
            c->col_k[j - 1] = tk; c->col_r[j - 1] = tr; c->col_g[j - 1] = tg; c->col_b[j - 1] = tb;
        }
    /* Two entries with the same key would make an interval of zero width
     * (sh_bracket never divides by it -- the search stops before -- but the
     * second entry would be dead data): refused, like non-increasing focus
     * calibration points. The engine's add-a-light replaces an entry within
     * 50 K instead of adding one (indraw_shading_calibrate). */
    for (i = 1; i < c->nlum; ++i) if (c->lum_d[i] == c->lum_d[i - 1]) SH_FAIL("two luminance entries have the same hal_diopters");
    for (i = 1; i < c->ncol; ++i) if (c->col_k[i] == c->col_k[i - 1]) SH_FAIL("two colour entries have the same cct");
#undef SH_FAIL
    return 0;
}

/* The two entries around x in a sorted key list, and the weight of the
 * upper one (0 below the first, 1 past the last: the nearest is used). */
static void sh_bracket(const double *keys, int n, double x, int *lo, int *hi, double *w)
{
    int i;
    if (n == 1 || x <= keys[0]) { *lo = *hi = 0; *w = 0; return; }
    if (x >= keys[n - 1]) { *lo = *hi = n - 1; *w = 0; return; }
    for (i = 1; i < n && keys[i] < x; ++i) {}
    *lo = i - 1; *hi = i;
    *w = (x - keys[i - 1]) / (keys[i] - keys[i - 1]);
}

/* A photo's gain map: rows x cols points, 4 gains each [R, Ge, Go, B] (both
 * greens alike), for lens position hal_d and colour temperature cct_k, with
 * the user's dosing (balance and strength 0..100, max_gain >= 1). */
static void shading_compose(const shading_cal_t *c, double hal_d, double cct_k,
                            double balance, double strength, double max_gain, float *out)
{
    int n = c->rows * c->cols, i, lo, hi, clo, chi;
    double w, cw, gmax = 1.0, pivot, st;
    sh_bracket(c->lum_d, c->nlum, hal_d, &lo, &hi, &w);
    {   /* colour: interpolated in mireds (1/K); keys ascending in K are
           descending in mireds, so bracket on K and weigh by mireds */
        sh_bracket(c->col_k, c->ncol, cct_k > 0 ? cct_k : c->col_k[c->ncol - 1], &clo, &chi, &cw);
        if (clo != chi) {
            double m = 1.0 / cct_k, m0 = 1.0 / c->col_k[clo], m1 = 1.0 / c->col_k[chi];
            cw = (m - m0) / (m1 - m0);
        }
    }
    if (max_gain < 1.0) max_gain = 1.0;
    for (i = 0; i < n; ++i) {
        double l = c->lum[lo][i] * (1 - w) + c->lum[hi][i] * w;
        double r = c->col_r[clo][i] * (1 - cw) + c->col_r[chi][i] * cw;
        double g = c->col_g[clo][i] * (1 - cw) + c->col_g[chi][i] * cw;
        double b = c->col_b[clo][i] * (1 - cw) + c->col_b[chi][i] * cw;
        double gr = l * r, gg = l * g, gb = l * b;
        gr = gr < 1 ? 1 : gr > max_gain ? max_gain : gr;
        gg = gg < 1 ? 1 : gg > max_gain ? max_gain : gg;
        gb = gb < 1 ? 1 : gb > max_gain ? max_gain : gb;
        out[i * 4 + 0] = (float)gr; out[i * 4 + 1] = (float)gg; out[i * 4 + 2] = (float)gg; out[i * 4 + 3] = (float)gb;
        if (gr > gmax) gmax = gr;
        if (gg > gmax) gmax = gg;
        if (gb > gmax) gmax = gb;
    }
    balance = balance < 0 ? 0 : balance > 100 ? 100 : balance;
    st = (strength < 0 ? 0 : strength > 100 ? 100 : strength) / 100.0;
    pivot = pow(gmax, balance / 100.0);
    for (i = 0; i < n * 4; ++i) {
        double v = 1.0 + st * (out[i] / pivot - 1.0);
        out[i] = (float)(v < 1e-3 ? 1e-3 : v);
    }
}

#endif
