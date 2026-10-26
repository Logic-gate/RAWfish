/*
 * ctl_shoot.c -- see ctl_shoot.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "ctl_shoot.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int is_stacked(double ns, double shutter_max_ns) { return ns > shutter_max_ns + 1; }

/* "focus=0.500m" / "focus=inf" for a manual focus, "" otherwise. */
static void focus_arg(const ctl_focus_state_t *fs, char *out, size_t n)
{
    if (!fs || fs->mode != CTL_FOCUS_MANUAL) { out[0] = '\0'; return; }
    if (fs->diopters <= 0) snprintf(out, n, " focus=inf");
    else snprintf(out, n, " focus=%.3fm", 1.0 / fs->diopters);
}

int ctl_shoot_lines(const ctl_shoot_req_t *req, const ctl_exp_state_t *xs,
                    double shutter_max_ns, double exp_min_ns,
                    const ctl_focus_state_t *fs, char lines[CTL_SHOOT_MAX_LINES][CTL_SHOOT_LINE])
{
    char q[96] = "", farg[32], head[224];
    double ns, smax = shutter_max_ns > 0 ? shutter_max_ns : 500000000.0;
    int iso, n = 0;
    if (!req || !xs || !lines || !req->name[0]) return 0;
    ns = xs->shutter_ns; iso = xs->iso;
    if (!req->raw) {
        snprintf(q, sizeof q, " q=%d%s%s%s%s", req->quality > 0 ? req->quality : 95,
                 req->noise[0] ? " " : "", req->noise, req->edge[0] ? " " : "", req->edge);
    }
    focus_arg(fs, farg, sizeof farg);
    /* A stacked shutter with a pair: a stack. */
    if (!xs->full_auto && is_stacked(ns, smax)) {
        snprintf(lines[0], CTL_SHOOT_LINE, "stack %s film=%.3f iso=%d stk_denoise=%d",
                 req->name, ns / 1e9, iso, req->denoise > 1 ? req->denoise : 1);
        return 1;
    }
    snprintf(head, sizeof head, "%s %s%s%s", req->raw ? "raw" : "jpeg", req->name, farg, req->raw ? "" : q);
    /* x3 in JPEG: one burst; the pair goes along when held. */
    if (req->bracket == 0 && !req->raw) {
        n = snprintf(lines[0], CTL_SHOOT_LINE, "burst %s n=3%s", req->name, q);
        if (req->gap > 0 && n < CTL_SHOOT_LINE)       /* the sensor spaces the frames */
            n += snprintf(lines[0] + n, CTL_SHOOT_LINE - n, " gap=%.3f", req->gap);
        if (xs->fixed_pair && n < CTL_SHOOT_LINE) snprintf(lines[0] + n, CTL_SHOOT_LINE - n, " exp=%.7f iso=%d", ns / 1e9, iso);
        return 1;
    }
    /* Bracket: three real exposures around the pair (x3 in raw: three the same). */
    if (req->bracket >= 0) {
        int stops = req->bracket, k;
        const int list[3] = { -stops, 0, stops };
        for (k = 0; k < 3; ++k) {
            double e = ns * pow(2.0, list[k]);
            if (e < exp_min_ns) e = exp_min_ns;
            if (e > smax) e = smax;
            snprintf(lines[k], CTL_SHOOT_LINE, "%s exp=%.7f iso=%d", head, e / 1e9, iso);
        }
        return 3;
    }
    /* One photo: with the held pair, or in the camera's AE. */
    if (xs->fixed_pair) snprintf(lines[0], CTL_SHOOT_LINE, "%s exp=%.7f iso=%d", head, ns / 1e9, iso);
    else snprintf(lines[0], CTL_SHOOT_LINE, "%s", head);
    return 1;
}
