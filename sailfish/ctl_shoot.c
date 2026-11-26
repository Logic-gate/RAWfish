/*
 * ctl_shoot.c -- see ctl_shoot.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "ctl_shoot.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int is_stacked(double ns, double shutter_max_ns) { return ns > shutter_max_ns + 1; }

/* Appends to a capture line of CTL_SHOOT_LINE bytes, *len its length so far.
 * Returns 0 when the text does not fit: the caller then refuses the whole
 * press (returns 0, no line), because a cut line could still run -- with
 * its exp= or iso= missing or cut short. Until 0.3.8 a second snprintf
 * wrote at lines[0] + n with n the length WANTED by the first, which past
 * the end was undefined behaviour (audit, November 2026; not reachable with
 * today's names and options, at most ~200 bytes). */
static int put(char *line, size_t *len, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*len >= CTL_SHOOT_LINE) return 0;
    va_start(ap, fmt);
    n = vsnprintf(line + *len, CTL_SHOOT_LINE - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= CTL_SHOOT_LINE - *len) { line[0] = '\0'; return 0; }
    *len += (size_t)n;
    return 1;
}

/* "focus=0.500m" / "focus=inf" for a manual focus, "" otherwise. */
static void focus_arg(const ctl_focus_state_t *fs, char *out, size_t n)
{
    /* not manual, or a manual lens held where a touch's search left it: the
     * photo sends no distance and keeps the lens as it is */
    if (!fs || fs->mode != CTL_FOCUS_MANUAL || fs->held) { out[0] = '\0'; return; }
    if (fs->diopters <= 0) snprintf(out, n, " focus=inf");
    else snprintf(out, n, " focus=%.3fm", 1.0 / fs->diopters);
}

/* A bracket's shots in turn, from lines[at]: three real exposures at -N, 0,
 * +N stops around the pair (bracket N), or nx at the same exposure
 * (bracket 0, x3 / x5 / x7), each clamped to the single-exposure range;
 * "raw" lines (raw = 1) or "jpeg" lines with the quality and processing (q).
 * Every name carries its milliseconds (ms=1): the series is named alike.
 * Returns the number of lines, 0 if one would not fit (in the line, or in
 * CTL_SHOOT_MAX_LINES). */
static int bracket_shots(const ctl_shoot_req_t *req, int raw, double ns, int iso, int nx, double smax,
                         double exp_min_ns, const char *farg, const char *q,
                         char lines[CTL_SHOOT_MAX_LINES][CTL_SHOOT_LINE], int at)
{
    int stops = req->bracket, k, n = stops == 0 ? nx : 3;
    if (at + n > CTL_SHOOT_MAX_LINES) return 0;
    for (k = 0; k < n; ++k) {
        double e = ns * pow(2.0, n == 3 && stops > 0 ? (k - 1) * stops : 0);
        size_t len = 0;
        if (e < exp_min_ns) e = exp_min_ns;
        if (e > smax) e = smax;
        if (!put(lines[at + k], &len, "%s %s%s%s%s exp=%.7f iso=%d ms=1", raw ? "raw" : "jpeg", req->name, farg,
                 raw ? "" : q, !raw && req->snap ? " label=snap" : "", e / 1e9, iso))
            return 0;
    }
    return n;
}

int ctl_shoot_lines(const ctl_shoot_req_t *req, const ctl_exp_state_t *xs,
                    double shutter_max_ns, double exp_min_ns,
                    const ctl_focus_state_t *fs, char lines[CTL_SHOOT_MAX_LINES][CTL_SHOOT_LINE])
{
    char q[96] = "", farg[32];
    double ns, smax = shutter_max_ns > 0 ? shutter_max_ns : 500000000.0;
    int iso, nx;
    size_t len = 0;
    if (!req || !xs || !lines || !req->name[0]) return 0;
    ns = xs->shutter_ns; iso = xs->iso;
    if (!req->raw) {
        snprintf(q, sizeof q, " q=%d%s%s%s%s", req->quality > 0 ? req->quality : 95,
                 req->noise[0] ? " " : "", req->noise, req->edge[0] ? " " : "", req->edge);
    }
    /* the same-exposure series: 3, 5 or 7 frames (anything else: 3) */
    nx = req->bracket_n == 5 || req->bracket_n == 7 ? req->bracket_n : 3;
    focus_arg(fs, farg, sizeof farg);
    /* Our JPEG from the YUV stream: one command, the frames merged there.
     * It is ONE photo: a request that carries a bracket is not ours to take
     * (it used to be taken, and its bracket dropped in silence); it goes on
     * to the camera's JPEG below, burst or three exposures. */
    if (req->yuv && req->bracket < 0) {
        /* snap from our YUV: one light photo (light=1), never with a DNG */
        int nf = req->frames == 2 || req->frames == 4 || req->frames == 8 ? req->frames : 1;
        char nr[24] = "", lu[12] = "";
        if (req->nr_tiles && nf > 1) snprintf(nr, sizeof nr, " nr=tiles");
        /* the luminance filter when the interface sent one (it does, this
         * camera's choice or default); none: the CLI's default, the device
         * profile's (jpg_luma_nr) */
        if (req->luma_set && req->luma >= 0 && req->luma <= 3) snprintf(lu, sizeof lu, " luma=%d", req->luma);
        if (!put(lines[0], &len, "yuv %s n=%d%s colour=%d median=%d%s sharpen=%d q=%d%s%s%s", req->name,
                 nf, nr, req->colour, req->median, lu, req->sharpen,
                 req->quality > 0 ? req->quality : 95, req->dng && !req->snap ? " dng=1" : "", req->snap ? " light=1" : "", farg))
            return 0;
        if (xs->fixed_pair && !put(lines[0], &len, " exp=%.7f iso=%d", ns / 1e9, iso)) return 0;
        return 1;
    }
    /* A stacked shutter with a pair: a stack. */
    if (!xs->full_auto && is_stacked(ns, smax)) {
        if (!put(lines[0], &len, "stack %s film=%.3f iso=%d stk_denoise=%d",
                 req->name, ns / 1e9, iso, req->denoise > 1 ? req->denoise : 1))
            return 0;
        return 1;
    }
    /* A bracket: the camera's JPEGs (type jpg) or DNGs (type raw) -- and
     * with JPG + DNG (Settings, req->dng), the camera's JPEGs first, then
     * the same series again as DNGs (0.3.11, patch 0553): two series one
     * after the other, not one exposure for both (our own JPG of each shot
     * would have spaced the shots by its processing, about 1 s each).
     *  - x3 / x5 / x7 in JPEG: one burst; the pair goes along when held;
     *  - otherwise (EV steps in JPEG, every bracket in DNG): shots in turn,
     *    see bracket_shots. */
    if (req->bracket >= 0) {
        int at = 0, k;
        if (!req->raw) {
            if (req->bracket == 0) {
                if (!put(lines[0], &len, "burst %s n=%d%s", req->name, nx, q)) return 0;
                if (xs->fixed_pair && !put(lines[0], &len, " exp=%.7f iso=%d", ns / 1e9, iso)) return 0;
                at = 1;
            } else if ((at = bracket_shots(req, 0, ns, iso, nx, smax, exp_min_ns, farg, q, lines, 0)) == 0) return 0;
            if (!req->dng) return at;
        }
        if ((k = bracket_shots(req, 1, ns, iso, nx, smax, exp_min_ns, farg, q, lines, at)) == 0) return 0;
        return at + k;
    }
    /* One photo: with the held pair, or in the camera's AE. */
    if (!put(lines[0], &len, "%s %s%s%s%s", req->raw ? "raw" : "jpeg", req->name, farg,
             req->raw ? "" : q, !req->raw && req->snap ? " label=snap" : ""))
        return 0;
    if (xs->fixed_pair && !put(lines[0], &len, " exp=%.7f iso=%d", ns / 1e9, iso)) return 0;
    return 1;
}
