/*
 * ctl_focus.c -- the focus behaviour (see ctl_focus.h). Ported from the
 * page's tap / search / hold / manual-pick sequences (app/qml/indraw.qml up
 * to ui 126), rule for rule.
 */
#define _POSIX_C_SOURCE 200809L
#include "ctl_focus.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STEP_D 0.05
#define MAX_D 10.0
#define SEARCH_LIMIT_MS 3000.0      /* then the exposure resumes, the circle is dashed */
#define STALE_MS 300.0              /* a "locked" this soon without a seen scan is the old one */

struct ctl_focus {
    ctl_focus_cmd_fn cmd;
    ctl_focus_state_fn state_cb;
    ctl_focus_pause_fn pause;
    ctl_focus_spot_fn spot;
    void *ctx;

    int mode;
    double diopters;
    int circle;
    int light_ref;
    int scene_on;

    int searching, seen_active, search_late;
    double search_start_ms;
    int hold_pending, manual_pick;
    double tap_fx, tap_fy;

    ctl_focus_state_t last;
    int have_last;
};

static void send(ctl_focus_t *f, const char *c) { if (f->cmd) f->cmd(f->ctx, c); }
static void publish(ctl_focus_t *f)
{
    ctl_focus_state_t s;
    ctl_focus_state(f, &s);
    if (f->have_last && memcmp(&s, &f->last, sizeof s) == 0) return;
    f->last = s; f->have_last = 1;
    if (f->state_cb) f->state_cb(f->ctx, &s);
}
static void set_paused(ctl_focus_t *f, int p) { if (f->pause) f->pause(f->ctx, p); }

static double snap(double d) { return fmax(0.0, fmin(MAX_D, round(d / STEP_D) * STEP_D)); }
/* wait = 0 for live stepping: the engine returns without waiting for the
 * lens, so steps never pile up; elsewhere (a mode change, the distance a
 * search found) the lens is waited for, as a photo would. */
static void send_manual_opt(ctl_focus_t *f, int wait)
{
    char c[48];
    if (f->diopters <= 0) snprintf(c, sizeof c, "focus inf%s", wait ? "" : " nowait");
    else snprintf(c, sizeof c, "focus %.3fm%s", 1.0 / f->diopters, wait ? "" : " nowait");
    send(f, c);
}
static void send_manual(ctl_focus_t *f) { send_manual_opt(f, 1); }
/* The mode, to the live preview (no photo). */
static void apply_mode(ctl_focus_t *f)
{
    switch (f->mode) {
    case CTL_FOCUS_CONT: send(f, "focus continuous"); break;
    case CTL_FOCUS_MANUAL: send_manual(f); break;
    default: send(f, "focus auto"); break;
    }
}

ctl_focus_t *ctl_focus_new(ctl_focus_cmd_fn cmd, ctl_focus_state_fn state,
                           ctl_focus_pause_fn pause, ctl_focus_spot_fn spot, void *ctx)
{
    ctl_focus_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->cmd = cmd; f->state_cb = state; f->pause = pause; f->spot = spot; f->ctx = ctx;
    f->mode = CTL_FOCUS_CONT;
    f->diopters = 1.0;
    return f;
}
void ctl_focus_free(ctl_focus_t *f) { free(f); }

void ctl_focus_restart(ctl_focus_t *f)
{
    /* a new session: no reference, no search in flight */
    f->searching = 0; f->search_late = 0; f->hold_pending = 0; f->manual_pick = 0;
    f->light_ref = 0; f->circle = CTL_CIRCLE_NONE;
    set_paused(f, 0);
    apply_mode(f);
    publish(f);
}

/* The light phase after the search (its outcome or its limit). */
static void light_phase(ctl_focus_t *f)
{
    if (f->light_ref && !f->scene_on && f->spot) f->spot(f->ctx, 1);
}

/* The search's outcome, read once. */
static void outcome(ctl_focus_t *f, int st, double focus_real_d)
{
    int was_late = f->search_late;
    f->circle = (st == 4) ? CTL_CIRCLE_SHARP : (st == 5) ? CTL_CIRCLE_UNSURE : CTL_CIRCLE_FAILED;
    f->searching = 0; f->search_late = 0;
    set_paused(f, 0);
    if (f->hold_pending) {
        f->hold_pending = 0;
        if (st == 4 || st == 5) send(f, "focus hold");         /* a locked lens is a position */
    }
    if (f->manual_pick) {
        f->manual_pick = 0;
        if (st == 4 && focus_real_d >= 0) f->diopters = snap(focus_real_d);  /* the distance found */
        send_manual(f);                                          /* manual again, there */
    }
    if (!was_late) light_phase(f);                               /* a late lock: it already ran */
    publish(f);
}

void ctl_focus_live(ctl_focus_t *f, int af_state, double focus_real_d, double now_ms)
{
    int done = af_state == 4 || af_state == 5 || af_state == 6;
    if (f->search_late) {                                        /* after the limit: a late lock counts */
        if (done) outcome(f, af_state, focus_real_d);
        return;
    }
    if (!f->searching) return;
    if (now_ms - f->search_start_ms > SEARCH_LIMIT_MS) {
        /* No answer in time (a dark or featureless point): the exposure is
         * free again, the circle dashed, but the search goes on in the HAL
         * and a late outcome is still taken. Manual focus goes back to its
         * distance now. */
        f->searching = 0; f->search_late = 1;
        f->circle = CTL_CIRCLE_UNSURE;
        set_paused(f, 0);
        if (f->manual_pick) { f->manual_pick = 0; send_manual(f); }
        light_phase(f);
        publish(f);
        return;
    }
    if (!done) { f->seen_active = 1; f->circle = CTL_CIRCLE_SEARCHING; publish(f); return; }
    if (f->seen_active || now_ms - f->search_start_ms > STALE_MS) outcome(f, af_state, focus_real_d);
}

void ctl_focus_set_mode(ctl_focus_t *f, int mode)
{
    if (mode < CTL_FOCUS_CONT || mode > CTL_FOCUS_MANUAL) return;
    f->mode = mode;
    ctl_focus_clear(f);                                          /* a mode change drops the reference */
    apply_mode(f);
    publish(f);
}
void ctl_focus_step(ctl_focus_t *f, int dir)                     /* > 0 = farther (fewer diopters) */
{
    if (f->mode != CTL_FOCUS_MANUAL) return;
    f->diopters = snap(f->diopters - dir * STEP_D);
    publish(f);                                                  /* the display first: no lag */
    send_manual_opt(f, 0);                                       /* the lens, without waiting */
}
void ctl_focus_set_distance(ctl_focus_t *f, double diopters)
{
    f->diopters = snap(diopters);
    if (f->mode == CTL_FOCUS_MANUAL) send_manual(f);
    publish(f);
}

void ctl_focus_ref(ctl_focus_t *f, double fx, double fy, double now_ms)
{
    char c[64];
    int light_only = f->mode == CTL_FOCUS_CONT;
    f->tap_fx = fx; f->tap_fy = fy;
    f->search_late = 0; f->seen_active = 0;
    f->hold_pending = 0; f->manual_pick = 0;
    f->light_ref = 1;
    if (f->spot) f->spot(f->ctx, 0);                             /* a held pair: the HAL's AE back for the search */
    if (light_only) {
        snprintf(c, sizeof c, "tap %.4f %.4f ae", fx, fy);
        send(f, c);
        f->circle = CTL_CIRCLE_SUN;
        f->searching = 0;
        light_phase(f);
        publish(f);
        return;
    }
    f->manual_pick = f->mode == CTL_FOCUS_MANUAL;
    f->hold_pending = f->mode == CTL_FOCUS_ONCE;
    f->searching = 1;
    f->search_start_ms = now_ms;
    f->circle = CTL_CIRCLE_SEARCHING;
    set_paused(f, 1);                                            /* no exposure change during the search */
    snprintf(c, sizeof c, "tap %.4f %.4f ae af", fx, fy);
    send(f, c);
    publish(f);
}

void ctl_focus_clear(ctl_focus_t *f)
{
    int had = f->light_ref || f->searching || f->search_late || f->circle != CTL_CIRCLE_NONE;
    f->light_ref = 0; f->searching = 0; f->search_late = 0;
    f->hold_pending = 0; f->manual_pick = 0;
    f->circle = CTL_CIRCLE_NONE;
    set_paused(f, 0);
    if (f->spot) f->spot(f->ctx, 0);                             /* the held exposure goes back to the HAL */
    if (had) { send(f, "tap 0.5 0.5 clear"); apply_mode(f); }   /* and the held lens: the mode again */
    publish(f);
}
void ctl_focus_set_scene(ctl_focus_t *f, int scene_on) { f->scene_on = !!scene_on; }

void ctl_focus_state(const ctl_focus_t *f, ctl_focus_state_t *out)
{
    memset(out, 0, sizeof *out);
    out->mode = f->mode;
    out->diopters = f->diopters;
    out->circle = f->circle;
    out->searching = f->searching;
    out->light_ref = f->light_ref;
}
