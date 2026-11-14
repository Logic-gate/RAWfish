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
    /* A touch's AUTO search starts with a CANCEL (the engine's trigger),
     * which the HAL shows as INACTIVE (0) before its new scan: until then
     * the states reported are the PREVIOUS search's. A touch made while a
     * search still ran took that search's lock as its own (log of 14 Nov,
     * 08:35:17: a lock 175 ms after the touch, from frames still carrying
     * the old zone; our "focus hold" then cut the new scan). wants_cancel:
     * this search starts with one; seen_cancel: its INACTIVE was seen. */
    int wants_cancel, seen_cancel;
    double search_start_ms;
    int hold_pending, manual_pick;
    double max_d;                   /* the closest distance, diopters (ctl_focus_set_max) */
    double step_pct;                /* volume steps above 1 D: this % of the value (0 = the constant 0.05 D) */
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
/* The manual distance, to the lens. Never waited for ("nowait"): the
 * engine's wait for the lens (up to 1.2 s, about 400 ms seen) ran on the
 * controller's reader thread, which then read no AF state, no meter and no
 * command meanwhile -- after a search that failed, a mode change, each
 * volume step. A photo waits for its own lens (its focus= option). */
static void send_manual(ctl_focus_t *f)
{
    char c[48];
    if (f->diopters <= 0) snprintf(c, sizeof c, "focus inf nowait");
    else snprintf(c, sizeof c, "focus %.3fm nowait", 1.0 / f->diopters);
    send(f, c);
}
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
    f->searching = 0; f->search_late = 0;
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
    if (f->wants_cancel && !f->seen_cancel && now_ms - f->search_start_ms <= SEARCH_LIMIT_MS) {
        /* Before the CANCEL shows (state 0), the states are the previous
         * search's: none of them counts. A HAL that never shows INACTIVE
         * falls back to the old rule after STALE_MS (a lock that old is the
         * new search's, or still there after it). */
        if (af_state == 0) { f->seen_cancel = 1; return; }
        if (now_ms - f->search_start_ms <= STALE_MS) return;
        f->seen_cancel = 1;
    }
    if (now_ms - f->search_start_ms > SEARCH_LIMIT_MS) {
        /* No answer in time (a dark or featureless point): the exposure is
         * free again, the circle dashed, but the search goes on in the HAL
         * and a late outcome is still taken. Manual focus goes back to its
         * distance now. */
        f->searching = 0; f->search_late = 1;                    /* (a late lock still counts, above) */
        f->circle = CTL_CIRCLE_UNSURE;
        set_paused(f, 0);
        if (f->manual_pick) { f->manual_pick = 0; send_manual(f); }
        publish(f);
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
    int was_held = f->held;
    double before = f->diopters;
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
    /* At an end (infinity, or the closest), a key held down repeats the
     * same distance: nothing to send -- the lens is there already. Each
     * send re-sent the camera's repeating request (162 times at the
     * closest stop in the log of 14 Nov). A lens held where a search left
     * it is set to the distance even when the number is the same. */
    if (!was_held && f->diopters == before) return;
    send_manual(f);
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
    /* the engine's AUTO search (once, manual) starts with a CANCEL; the
     * continuous mode's has no trigger */
    f->wants_cancel = f->mode != CTL_FOCUS_CONT;
    f->seen_cancel = 0;
    snprintf(c, sizeof c, "tap %.4f %.4f af", fx, fy);
    send(f, c);
    publish(f);
    return 1;
}

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
