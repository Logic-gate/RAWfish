/*
 * ctl_focus.c -- the focus behaviour (see ctl_focus.h). Ported from the
 * page's tap / search / hold / manual-pick sequences (app/qml/rawfish-ir.qml up
 * to ui 126), rule for rule.
 */
#define _POSIX_C_SOURCE 200809L
#include "ctl_focus.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STEP_D 0.05
#define MAX_D 10.0                  /* the closest distance until the caps say (10 D = 10 cm) */
#define SEARCH_LIMIT_MS 3000.0      /* then the exposure resumes, the circle is dashed */
#define STALE_MS 300.0              /* a "locked" this soon without a seen scan is the old one */
#define CONT_SETTLE_MIN_MS 400.0    /* continuous search: a settled state this soon may be the old one */
#define CONT_SETTLE_MAX_MS 1500.0   /* continuous search: locked by then whatever its state */

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
    double light_fx, light_fy;              /* the light point (long press), frame coordinates */
    int scene_on;

    int searching, seen_active, search_late;
    double search_start_ms;
    int hold_pending, manual_pick;
    double max_d;                   /* the closest distance, diopters (ctl_focus_set_max) */
    double step_pct;                /* volume steps above 1 D: this % of the value (0 = the constant 0.05 D) */
    /* The focus touch "continuous then lock" (ctl_focus_set_touch; the JP2's
     * 18 mm). Its AUTO search only scans from where the lens is towards
     * infinity: from our manual distance (1 m, near infinity on that lens) it
     * missed every closer subject (1 touch of 10, trace of 14 Nov), while its
     * CONTINUOUS search finds them. So, in manual focus, a touch on such a
     * camera: "tap X Y afc" (the continuous search on the zone), then, once
     * that search settled (state 2 focused or 6 unfocused, after a scan seen
     * or 400 ms), "aflock" (a start in a continuous mode locks the lens
     * where it is); the lock's outcome (4 or 5) is the touch's, as usual.
     * cont_phase: 0 none, 1 waiting for the search to settle, 2 locked,
     * waiting for 4 or 5. */
    int touch_cont, cont_phase, cont_seen_scan;
    int held;                   /* manual: lens held at a search's lock (see ctl_focus_state_t) */
    int no_search;              /* the camera takes no search of ours (ctl_focus_set_search) */
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

/* On the 0.05 D grid, between infinity (0) and the camera's closest
 * distance (f->max_d: its caps, through its calibration table; 10 D until
 * the caps come). Until 0.3.8 the closest was 10 D for every camera, while
 * the JP2's cameras 0 and 2 declare a lens that goes to 20 (HAL units): the
 * closest half of their range could not be reached by hand. */
static double snap(const ctl_focus_t *f, double d) { return fmax(0.0, fmin(f->max_d, round(d / STEP_D) * STEP_D)); }
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
    if (f) f->max_d = MAX_D;
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
    f->cont_phase = 0;
    f->light_ref = 0; f->circle = CTL_CIRCLE_NONE;
    /* A new session cannot be held where the old one's search was: manual
     * goes to the distance shown, through the calibration (as any manual
     * distance does); a new touch holds the lens again. */
    f->held = 0;
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
    f->searching = 0; f->search_late = 0; f->cont_phase = 0;
    set_paused(f, 0);
    if (f->hold_pending) {
        f->hold_pending = 0;
        if (st == 4 || st == 5) send(f, "focus hold");         /* a locked lens is a position */
    }
    if (f->manual_pick) {
        /* Manual focus, a touch: the lens STAYS where the HAL's search put it
         * ("focus hold"), and the distance shown is the HAL's own reading of
         * that position, as it is. Nothing is converted back and sent to the
         * lens: until 0.3.8 the distance found was rounded to the 0.05 D
         * grid and sent again through the calibration table, which moved the
         * lens away from the HAL's focus (seen: a manual photo softer than
         * the auto one everywhere). A search that did NOT focus leaves the
         * previous distance: state 6 (failed), and state 5 too, "locked but
         * not focused": the lens is then wherever the scan gave up, often at
         * infinity. 0357 held the lens on 5 as well, and a touch on a plain
         * floor at 3 m showed anything from 1.5 m to infinity depending on
         * where it was in the frame. Only 4, "focused and locked", is held. */
        f->manual_pick = 0;
        if (st == 4 && focus_real_d >= 0) {
            send(f, "focus hold");
            f->held = 1;
            f->diopters = focus_real_d;                          /* shown, not sent */
        } else {
            f->held = 0;
            send_manual(f);                                      /* back to the distance in force */
        }
    }
    (void)was_late;                                              /* the light is the hold's, not the search's */
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
        f->cont_phase = 0;                                       /* (a late lock still counts, below) */
        f->circle = CTL_CIRCLE_UNSURE;
        set_paused(f, 0);
        if (f->manual_pick) { f->manual_pick = 0; send_manual(f); }
        publish(f);
        return;
    }
    /* "Continuous then lock" (see touch_cont): first the continuous search
     * settles, then the lock is asked; until the lock answers (4 or 5), a
     * settled state (2, 6) is not an outcome. */
    if (f->cont_phase == 1) {
        double since = now_ms - f->search_start_ms;
        if (af_state == 1) f->cont_seen_scan = 1;
        if (((af_state == 2 || af_state == 6) && (f->cont_seen_scan || since > CONT_SETTLE_MIN_MS)) || since > CONT_SETTLE_MAX_MS) {
            f->cont_phase = 2;
            f->seen_active = 1;                                  /* a lock now is this search's own */
            send(f, "aflock");
        }
        return;
    }
    if (f->cont_phase == 2) {
        if (af_state == 4 || af_state == 5) outcome(f, af_state, focus_real_d);
        return;
    }
    /* Continuous focus has no lock: settled on the point (PASSIVE_FOCUSED)
     * is its outcome. */
    if (f->mode == CTL_FOCUS_CONT && af_state == 2 && f->seen_active) { outcome(f, 4, focus_real_d); return; }
    if (!done) { f->seen_active = 1; f->circle = CTL_CIRCLE_SEARCHING; publish(f); return; }
    if (f->seen_active || now_ms - f->search_start_ms > STALE_MS) outcome(f, af_state, focus_real_d);
}

void ctl_focus_set_mode(ctl_focus_t *f, int mode)
{
    if (mode < CTL_FOCUS_CONT || mode > CTL_FOCUS_MANUAL) return;
    f->mode = mode;
    f->held = 0;
    ctl_focus_clear(f);                                          /* a mode change drops the FOCUS point only:
                                                                    the light point is the exposure's, and stays */
    apply_mode(f);
    publish(f);
}
static int drop_ref(ctl_focus_t *f);
void ctl_focus_manual_search(ctl_focus_t *f, double now_ms)
{
    if (f->no_search) { ctl_focus_set_mode(f, CTL_FOCUS_MANUAL); return; }
    f->mode = CTL_FOCUS_MANUAL;
    f->held = 0;
    drop_ref(f);              /* the old point goes; the search's zone replaces its region */
    /* AF off where the lens is: the touch then goes from OFF to one AUTO
     * search with its trigger, as from a manual distance. (Straight from the
     * continuous mode the engine would make the zone a continuous one, with
     * no trigger and no lock to come.) */
    send(f, "focus hold");
    ctl_focus_ref(f, 0.5, 0.5, now_ms);
}
void ctl_focus_step(ctl_focus_t *f, int dir)                     /* > 0 = farther (fewer diopters) */
{
    if (f->mode != CTL_FOCUS_MANUAL) return;
    f->held = 0;                                                 /* a distance of the user's: the lens is set to it */
    {   /* A camera whose device profile gives focus_step_percent (the JP2's
         * 18 mm: 8) steps by that share of the value above about 1 D, on the
         * 0.05 grid: its near range took 180 presses, now 48 for 0 to 9 D.
         * Going farther, the step is the one that leads back to the value
         * below. Other cameras: the constant 0.05 D, fine in macro. */
        double d = f->diopters, step = STEP_D, p = f->step_pct / 100.0;
        if (p > 0) {
            if (dir < 0) step = fmax(STEP_D, round(d * p / STEP_D) * STEP_D);
            else         step = fmax(STEP_D, round(d / (1.0 + p) * p / STEP_D) * STEP_D);
        }
        f->diopters = snap(f, d - dir * step);
    }
    publish(f);                                                  /* the display first: no lag */
    send_manual_opt(f, 0);                                       /* the lens, without waiting */
}
void ctl_focus_set_step_pct(ctl_focus_t *f, double pct) { f->step_pct = pct > 0 && pct < 50 ? pct : 0; }
void ctl_focus_set_max(ctl_focus_t *f, double max_diopters)
{
    /* 0 or less: a fixed-focus camera (nothing to reach) or no caps: kept */
    if (max_diopters > 0.5) f->max_d = max_diopters;
    if (f->diopters > f->max_d) f->diopters = f->max_d;
}
void ctl_focus_set_distance(ctl_focus_t *f, double diopters)
{
    f->diopters = snap(f, diopters);
    f->held = 0;
    if (f->mode == CTL_FOCUS_MANUAL) send_manual(f);
    publish(f);
}

/* A tap: the FOCUS only, in every mode (continuous: focus kept on the
 * point; once: one scan, then held; manual: one scan, the distance found
 * becomes the manual one, to adjust by hand). The exposure is not touched:
 * it stays the whole image's, or the held light point's (ctl_focus_light). */
int ctl_focus_ref(ctl_focus_t *f, double fx, double fy, double now_ms)
{
    char c[64];
    /* The 18 mm (0.3.8): its one-way search passes the sharp position and
     * often reports a success on a blurred image. The interface stopped
     * sending its touches (0403), but the FOCUS button entering manual still
     * started a search in the middle (review, 14 Nov 2026): refused here,
     * whoever asks, so that manual starts from the distance shown. */
    if (f->no_search) return 0;
    f->tap_fx = fx; f->tap_fy = fy;
    f->search_late = 0; f->seen_active = 0;
    f->manual_pick = f->mode == CTL_FOCUS_MANUAL;
    f->hold_pending = f->mode == CTL_FOCUS_ONCE;
    f->searching = 1;
    f->search_start_ms = now_ms;
    f->circle = CTL_CIRCLE_SEARCHING;
    f->cont_phase = 0;
    if (f->mode == CTL_FOCUS_MANUAL && f->touch_cont) {         /* continuous search, then lock */
        f->cont_phase = 1; f->cont_seen_scan = 0;
        snprintf(c, sizeof c, "tap %.4f %.4f afc", fx, fy);
    } else snprintf(c, sizeof c, "tap %.4f %.4f af", fx, fy);
    send(f, c);
    publish(f);
    return 1;
}

void ctl_focus_set_touch(ctl_focus_t *f, int mode) { f->touch_cont = mode == 2; }
void ctl_focus_set_search(ctl_focus_t *f, int allowed) { f->no_search = !allowed; }

/* A long press: the LIGHT at that point (a 5 % spot): the HAL's exposure
 * meters there, and in a manual or priority exposure the spot meter reads
 * there (the light reference). The focus is not touched. */
void ctl_focus_light(ctl_focus_t *f, double fx, double fy)
{
    char c[64];
    f->light_fx = fx; f->light_fy = fy;
    f->light_ref = 1;
    if (f->spot) f->spot(f->ctx, 0);                             /* a new point: measured afresh */
    snprintf(c, sizeof c, "tap %.4f %.4f ae", fx, fy);
    send(f, c);
    light_phase(f);
    publish(f);
}
void ctl_focus_light_clear(ctl_focus_t *f)
{
    if (!f->light_ref) return;
    f->light_ref = 0;
    if (f->spot) f->spot(f->ctx, 0);                             /* the exposure back to the whole image */
    send(f, "tap 0.5 0.5 aeclear");
    publish(f);
}

/* The focus circle cleared: the focus back to the mode's (the light point,
 * if any, stays; ctl_focus_light_clear removes it). A mode change clears
 * both. */
/* The focus point dropped (fields only); returns whether there was one. */
static int drop_ref(ctl_focus_t *f)
{
    int had = f->searching || f->search_late || f->circle != CTL_CIRCLE_NONE;
    f->searching = 0; f->search_late = 0;
    f->hold_pending = 0; f->manual_pick = 0;
    f->cont_phase = 0;                                           /* no lock to come */
    f->circle = CTL_CIRCLE_NONE;
    set_paused(f, 0);
    return had;
}
void ctl_focus_clear(ctl_focus_t *f)
{
    if (drop_ref(f)) {
        send(f, "tap 0.5 0.5 afclear");
        /* the mode again; but a manual lens held at a search stays where it is
         * (removing the circle does not move the lens) */
        if (!(f->mode == CTL_FOCUS_MANUAL && f->held)) apply_mode(f);
    }
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
    out->held = f->held;
}
