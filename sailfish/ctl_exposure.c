/*
 * ctl_exposure.c -- the exposure behaviour (see ctl_exposure.h). Ported from
 * app/qml/ExposureLogic.qml, rule for rule; the comments that explain a rule
 * came with it.
 */
#define _POSIX_C_SOURCE 200809L
#include "ctl_exposure.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ctl_exposure {
    ctl_cmd_fn cmd;
    ctl_state_fn state_cb;
    void *ctx;
    ctl_caps_t caps;
    int ready;

    int iso_manual, shutter_manual;
    int manual_iso;
    double manual_shutter_ns;
    int auto_iso;
    double auto_shutter_ns;
    int ev_steps;

    int raw_source;
    double target_raw, target_vf;
    int calibrated_raw, calibrated_vf;

    int live_iso;
    double live_exp_ns;
    int live_ae_state;

    double last_send_ms;
    char last_pair[64];
    int sent_iso;
    double sent_shutter_ns;
    int auto_sent;

    int paused;
    double offset_stops;
    int offset_valid;
    double ev100;

    double pv_target_ns;        /* a Σ preview frame: as short as this, ISO permitting */
    double pv_ns;               /* the preview frame time last sent */
    double pv_deficit;          /* photo exposure / preview frame's exposure (>= 1) */
    int pv_slow, too_dark;
    int pv_at_limit;            /* the preview frame is at the ceiling AND the maximum ISO */

    int spot_hold;
    int spot_iso;
    double spot_shutter_ns;
    int spot_steps;

    ctl_exp_state_t last_state;
    int have_state;
};

/* ---- helpers ------------------------------------------------------------- */
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static double log2d(double x) { return log2(x); }
static int full_auto(const ctl_exposure_t *e) { return !e->iso_manual && !e->shutter_manual; }
static int full_manual(const ctl_exposure_t *e) { return e->iso_manual && e->shutter_manual; }
static double ev_stops(const ctl_exposure_t *e) { return e->ev_steps * (e->caps.ev_step > 0 ? e->caps.ev_step : 1.0 / 3); }
static double shutter_max(const ctl_exposure_t *e) { return e->caps.shutter_max_ns > 0 ? e->caps.shutter_max_ns : 500000000.0; }
static int is_stacked(const ctl_exposure_t *e, double ns) { return ns > shutter_max(e) + 1; }
static int live_iso(const ctl_exposure_t *e) { return e->live_iso > 0 ? e->live_iso : e->manual_iso; }
static double live_shutter(const ctl_exposure_t *e) { return e->live_exp_ns > 0 ? e->live_exp_ns : e->manual_shutter_ns; }
/* "What automatic would choose", shifted by EV in the modes we drive. */
static double target(const ctl_exposure_t *e)
{
    double t = e->raw_source ? e->target_raw : e->target_vf;
    return t * (full_auto(e) ? 1.0 : pow(2.0, ev_stops(e)));
}
static int display_iso(const ctl_exposure_t *e)
{
    if (e->iso_manual) return e->manual_iso;
    if (e->shutter_manual) return e->auto_iso;
    return e->spot_hold ? e->spot_iso : live_iso(e);
}
static double display_shutter(const ctl_exposure_t *e)
{
    if (e->shutter_manual) return e->manual_shutter_ns;
    if (e->iso_manual) return e->auto_shutter_ns;
    return e->spot_hold ? e->spot_shutter_ns : live_shutter(e);
}

static void send(ctl_exposure_t *e, const char *cmd) { if (e->cmd) e->cmd(e->ctx, cmd); }

/* Pushes the state when it changed (the interface only draws). */
static void publish(ctl_exposure_t *e)
{
    ctl_exp_state_t s;
    ctl_exp_state(e, &s);
    if (e->have_state && memcmp(&s, &e->last_state, sizeof s) == 0) return;
    e->last_state = s;
    e->have_state = 1;
    if (e->state_cb) e->state_cb(e->ctx, &s);
}

/* ---- engine commands ------------------------------------------------------ */
/* Sends the preview pair. A stacked (Σ) shutter cannot be previewed as such
 * -- the sensor exposes 0.5 s at most --, so ONE frame stands for the stack,
 * with the same exposure (time x gain): as short as the target rate wants
 * (pv_target_ns, 1/10 s by default), the ISO raised accordingly; if that
 * ISO is beyond the sensor's maximum, the frame is lengthened just enough,
 * up to the ceiling. Beyond even that, the frame stays darker than the photo
 * by pv_deficit, which the meter compensates and the interface reports. The
 * only amplification is ISO, and it is reported. (The photo itself is a real
 * stack at the photo's ISO.) */
static void send_pair(ctl_exposure_t *e, int iso, double shutter_ns)
{
    double ns = shutter_ns, pv;
    int i = iso;
    char cmd[64];
    if (!e->ready) return;
    e->pv_deficit = 1.0;
    e->pv_slow = 0;
    e->pv_at_limit = 0;
    if (is_stacked(e, ns)) {
        double E = (double)iso * ns;                       /* the photo's exposure, ISO x ns */
        double target = clampd(e->pv_target_ns, e->caps.exp_min_ns, shutter_max(e));
        pv = target;
        if (E / pv > e->caps.iso_max) pv = fmin(shutter_max(e), E / e->caps.iso_max);   /* ISO limit: longer */
        if (E / pv < e->caps.iso_min) pv = fmax(e->caps.exp_min_ns, E / e->caps.iso_min); /* very bright: shorter */
        i = (int)lround(clampd(E / pv, e->caps.iso_min, e->caps.iso_max));
        ns = pv;
        e->pv_deficit = fmax(1.0, E / ((double)i * pv));
        e->pv_slow = pv > target * 1.05;
        e->pv_at_limit = pv >= shutter_max(e) * 0.999 && i >= e->caps.iso_max;
    }
    ns = clampd(ns, e->caps.exp_min_ns, shutter_max(e));
    i = (int)lround(clampd(i, e->caps.iso_min, e->caps.iso_max));
    e->pv_ns = ns;
    snprintf(cmd, sizeof cmd, "exposure exp=%.7f iso=%d", ns / 1e9, i);
    e->auto_sent = 0;
    if (!strcmp(cmd, e->last_pair)) return;
    snprintf(e->last_pair, sizeof e->last_pair, "%s", cmd);
    e->sent_iso = i;
    e->sent_shutter_ns = ns;
    send(e, cmd);
}
static void send_auto(ctl_exposure_t *e)
{
    if (!e->ready) return;
    e->last_pair[0] = '\0';
    e->sent_iso = 0;
    e->sent_shutter_ns = 0;
    if (e->auto_sent) return;      /* already in auto: a re-send would reset the HAL
                                    * and wipe a tap's AE region */
    e->auto_sent = 1;
    send(e, "exposure auto");
}
/* Was this frame exposed with the pair we last sent? The HAL applies a new
 * pair a few frames later; correcting on a frame still at the old settings
 * is what made the loop hunt (observed on the JP2, ui 65). */
static int settled(const ctl_exposure_t *e, int frame_iso, double frame_exp_ns)
{
    if (e->sent_iso <= 0) return 1;
    if (frame_iso <= 0 || frame_exp_ns <= 0) return 0;
    return fabs((double)frame_iso / e->sent_iso - 1) < 0.03
        && fabs(frame_exp_ns / e->sent_shutter_ns - 1) < 0.03;
}
static void send_ev(ctl_exposure_t *e)
{
    char cmd[32];
    if (!e->ready) return;
    snprintf(cmd, sizeof cmd, "ev %d", e->ev_steps);
    send(e, cmd);
}
static void send_meter(ctl_exposure_t *e)
{
    if (!e->ready) return;
    send(e, e->raw_source ? "meter on rate=5" : "meter off");
}
/* Applies the current mode to the engine. */
static void apply(ctl_exposure_t *e)
{
    if (!e->ready) return;
    if (e->spot_hold) { e->last_pair[0] = '\0'; send_pair(e, e->spot_iso, e->spot_shutter_ns); publish(e); return; }
    if (full_auto(e)) { send_auto(e); publish(e); return; }
    if (e->iso_manual && !e->shutter_manual) send_pair(e, e->manual_iso, e->auto_shutter_ns);
    else if (e->shutter_manual && !e->iso_manual) send_pair(e, e->auto_iso, e->manual_shutter_ns);
    else send_pair(e, e->manual_iso, e->manual_shutter_ns);
    publish(e);
}

/* ---- construction ---------------------------------------------------------- */
ctl_exposure_t *ctl_exp_new(ctl_cmd_fn cmd, ctl_state_fn state, void *ctx)
{
    ctl_exposure_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->cmd = cmd; e->state_cb = state; e->ctx = ctx;
    e->manual_iso = 100; e->manual_shutter_ns = 16666667;
    e->auto_iso = 100; e->auto_shutter_ns = 16666667;
    e->spot_iso = 100; e->spot_shutter_ns = 16666667;
    e->target_raw = 0.18; e->target_vf = 0.18;
    e->ev100 = NAN;
    e->pv_target_ns = 1e8;                      /* 10 frames/s for a Σ preview */
    e->pv_deficit = 1.0;
    return e;
}
void ctl_exp_free(ctl_exposure_t *e) { free(e); }

void ctl_exp_set_caps(ctl_exposure_t *e, const ctl_caps_t *caps)
{
    e->caps = *caps;
    e->ready = caps->iso_min > 0;
    e->last_pair[0] = '\0';
    e->auto_sent = 0;
    send_meter(e);
    apply(e);
}
void ctl_exp_set_source(ctl_exposure_t *e, int raw)
{
    if (e->raw_source == !!raw) return;
    e->raw_source = !!raw;
    send_meter(e);
    e->offset_valid = 0;
    publish(e);
}
void ctl_exp_restart(ctl_exposure_t *e)
{
    e->last_pair[0] = '\0';
    e->auto_sent = 0;
    send_meter(e);
    apply(e);
}

/* ---- events ---------------------------------------------------------------- */
void ctl_exp_live(ctl_exposure_t *e, int iso, double exp_ns, int ae_state)
{
    e->live_iso = iso;
    e->live_exp_ns = exp_ns;
    e->live_ae_state = ae_state;
    publish(e);
}

/* The absolute light level of what is metered (the spot when a circle is
 * set), as EV at ISO 100: from the measured frame's pair, EV100 =
 * log2(N^2/t) - log2(ISO/100), plus how much brighter than the target the
 * measure is. Lightly smoothed for a steady display. */
static void update_metered_ev(ctl_exposure_t *e, double mean, int frame_iso, double frame_exp_ns)
{
    double n = e->caps.aperture, t = target(e) / (full_auto(e) ? 1.0 : pow(2.0, ev_stops(e)));
    double ev;
    if (!(n > 0) || !(mean > 0) || !(t > 0) || frame_iso <= 0 || !(frame_exp_ns > 0)) return;
    ev = log2d(n * n / (frame_exp_ns / 1e9)) - log2d(frame_iso / 100.0) + log2d(mean / t);
    e->ev100 = isnan(e->ev100) ? ev : 0.7 * e->ev100 + 0.3 * ev;
}

/* A light reference in automatic: a few bounded corrections, each on a
 * frame taken with the previous pair, until the spot sits at the level the
 * HAL's AE gives an average scene; then the pair is held. One correction:
 * time first up to a hand-held limit (1/30 s, or 1/focal for longer lenses),
 * then ISO, then time up to the ceiling. */
static void spot_correct(ctl_exposure_t *e, double mean, int frame_iso, double frame_exp_ns)
{
    double err, iso0, exp0, E, feq, handheld, exp_ns, iso;
    if (!settled(e, frame_iso, frame_exp_ns)) return;
    err = log2d(target(e) * pow(2.0, ev_stops(e)) / mean);
    if (fabs(err) < 0.12 || e->spot_steps <= 1) {
        /* There (or out of steps): hold. If nothing was sent yet the HAL's AE
         * is still free, so the pair of this frame is sent and kept. */
        if (e->sent_iso <= 0 && frame_iso > 0 && frame_exp_ns > 0) {
            e->spot_iso = frame_iso; e->spot_shutter_ns = frame_exp_ns;
            send_pair(e, e->spot_iso, e->spot_shutter_ns);
        }
        if (fabs(err) < 0.12) { e->spot_steps = 0; return; }
    }
    iso0 = frame_iso > 0 ? frame_iso : e->spot_iso;
    exp0 = frame_exp_ns > 0 ? frame_exp_ns : e->spot_shutter_ns;
    E = iso0 * exp0 * pow(2.0, err);
    feq = e->caps.focal_eq > 0 ? e->caps.focal_eq : 26;
    handheld = fmin(shutter_max(e), 1e9 / fmax(30.0, feq));
    exp_ns = clampd(E / e->caps.iso_min, e->caps.exp_min_ns, handheld);
    iso = clampd(E / exp_ns, e->caps.iso_min, e->caps.iso_max);
    if (iso >= e->caps.iso_max) exp_ns = clampd(E / e->caps.iso_max, e->caps.exp_min_ns, shutter_max(e));
    e->spot_iso = (int)lround(iso); e->spot_shutter_ns = exp_ns;
    e->spot_steps--;
    send_pair(e, e->spot_iso, e->spot_shutter_ns);
}

void ctl_exp_sample(ctl_exposure_t *e, double mean, int frame_iso, double frame_exp_ns, double now_ms)
{
    double err, gain;
    if (!e->ready || !(mean > 0) || e->paused) return;
    update_metered_ev(e, mean, frame_iso, frame_exp_ns);
    if (e->spot_hold) {
        if (e->spot_steps > 0) spot_correct(e, mean, frame_iso, frame_exp_ns);
        publish(e);
        return;
    }
    if (full_auto(e)) {
        /* Calibrate: the HAL's converged AE (2 CONVERGED, 3 LOCKED) defines
         * "well exposed" for this source; the HAL compensation is undone so
         * the target is the EV 0 choice. */
        if (e->live_ae_state == 2 || e->live_ae_state == 3) {
            double m0 = mean / pow(2.0, ev_stops(e));
            if (e->raw_source) { e->target_raw = e->calibrated_raw ? 0.8 * e->target_raw + 0.2 * m0 : m0; e->calibrated_raw = 1; }
            else               { e->target_vf  = e->calibrated_vf  ? 0.8 * e->target_vf  + 0.2 * m0 : m0; e->calibrated_vf  = 1; }
        }
        publish(e);
        return;
    }
    /* A Σ preview frame darker than the photo (ISO and ceiling at their
     * limits): what the photo would read is the frame's mean times the
     * deficit. "Too dark to measure" only AT that limit -- the frame at the
     * 0.5 s ceiling and the maximum ISO, its signal still under about 4 DN of
     * a 10-bit range (0.004, provisional, to be set from measurements): the
     * preview cannot gain any more. Below the limit a dark frame only means
     * a dark photo (manual: the +/- offset says it; priority: the loop opens
     * up), not a measure that fails (0.47.17 flagged it there too). */
    {
        int stacked = e->shutter_manual && is_stacked(e, e->manual_shutter_ns);
        int dark = stacked && e->pv_at_limit && mean < 0.004;
        if (dark != e->too_dark) { e->too_dark = dark; publish(e); }
        if (dark) return;                   /* leave the pair alone: no measure */
        if (stacked) mean *= e->pv_deficit;
    }
    err = log2d(target(e) / mean);          /* stops to add to reach the target */
    if (full_manual(e)) {
        e->offset_stops = -err;             /* + = brighter than automatic would pick */
        e->offset_valid = 1;
        publish(e);
        return;
    }
    if (!settled(e, frame_iso, frame_exp_ns)) return;   /* old settings: wait */
    if (fabs(err) < 0.12) return;                       /* dead band: no hunting */
    if (now_ms - e->last_send_ms < 120) return;
    e->last_send_ms = now_ms;
    /* The settle check above already prevents overshoot, so the step can be
     * close to the full error (0.6 felt slow on the JP2, ~2-3 Hz). */
    gain = pow(2.0, err * 0.85);
    if (e->iso_manual) {
        double base = frame_exp_ns > 0 ? frame_exp_ns : e->auto_shutter_ns;
        e->auto_shutter_ns = clampd(base * gain, e->caps.exp_min_ns, shutter_max(e));
        send_pair(e, e->manual_iso, e->auto_shutter_ns);
    } else {
        double base_iso = frame_iso > 0 ? frame_iso : e->auto_iso;
        /* A stacked shutter previews one frame of the same exposure (send_pair):
         * the photo's ISO is the frame's exposure, times the deficit, over the
         * photo's time. */
        if (frame_iso > 0 && frame_exp_ns > 0 && is_stacked(e, e->manual_shutter_ns))
            base_iso = frame_iso * frame_exp_ns * e->pv_deficit / e->manual_shutter_ns;
        e->auto_iso = (int)lround(clampd(base_iso * gain, e->caps.iso_min, e->caps.iso_max));
        send_pair(e, e->auto_iso, e->manual_shutter_ns);
    }
    publish(e);
}

/* ---- commands -------------------------------------------------------------- */
void ctl_exp_touch_iso(ctl_exposure_t *e)
{
    if (e->iso_manual) return;
    e->manual_iso = display_iso(e);
    if (!e->shutter_manual) e->auto_shutter_ns = live_shutter(e);
    e->iso_manual = 1;
    e->spot_hold = 0; e->spot_steps = 0;            /* a pinned wheel ends a held spot */
    apply(e);
}
void ctl_exp_touch_shutter(ctl_exposure_t *e)
{
    if (e->shutter_manual) return;
    e->manual_shutter_ns = display_shutter(e);
    if (!e->iso_manual) e->auto_iso = live_iso(e);
    e->shutter_manual = 1;
    e->spot_hold = 0; e->spot_steps = 0;
    apply(e);
}
void ctl_exp_set_iso(ctl_exposure_t *e, int iso)
{
    if (!e->iso_manual) { if (!e->shutter_manual) e->auto_shutter_ns = live_shutter(e); e->iso_manual = 1; }
    e->manual_iso = iso;
    e->spot_hold = 0; e->spot_steps = 0;
    apply(e);
}
void ctl_exp_set_shutter(ctl_exposure_t *e, double ns)
{
    if (!e->shutter_manual) { if (!e->iso_manual) e->auto_iso = live_iso(e); e->shutter_manual = 1; }
    e->manual_shutter_ns = ns;
    e->spot_hold = 0; e->spot_steps = 0;
    apply(e);
}
void ctl_exp_set_ev(ctl_exposure_t *e, int steps)
{
    e->ev_steps = steps;
    if (full_auto(e)) send_ev(e);       /* the HAL's compensation */
    if (e->spot_hold) e->spot_steps = 4; /* a new target: measure again */
    /* priority modes: the loop target moved; the next sample acts */
    publish(e);
}
void ctl_exp_reset_auto(ctl_exposure_t *e)
{
    e->iso_manual = 0; e->shutter_manual = 0; e->ev_steps = 0;
    e->offset_valid = 0;
    e->spot_hold = 0; e->spot_steps = 0;
    send_ev(e);
    send_auto(e);
    publish(e);
}
void ctl_exp_spot_start(ctl_exposure_t *e)
{
    if (!full_auto(e)) return;
    e->spot_iso = live_iso(e); e->spot_shutter_ns = live_shutter(e);
    e->last_pair[0] = '\0'; e->sent_iso = 0; e->sent_shutter_ns = 0;
    e->spot_steps = 4;
    e->spot_hold = 1;
    publish(e);
}
void ctl_exp_spot_stop(ctl_exposure_t *e)
{
    if (!e->spot_hold) return;
    e->spot_hold = 0; e->spot_steps = 0;
    e->auto_sent = 0;
    if (full_auto(e)) send_auto(e);
    publish(e);
}
void ctl_exp_set_paused(ctl_exposure_t *e, int paused) { e->paused = !!paused; }
void ctl_exp_set_preview_fps(ctl_exposure_t *e, double fps)
{
    fps = clampd(fps, 1.0, 30.0);
    e->pv_target_ns = 1e9 / fps;
    if (e->shutter_manual && is_stacked(e, e->manual_shutter_ns)) { e->last_pair[0] = '\0'; apply(e); }
}

void ctl_exp_state(const ctl_exposure_t *e, ctl_exp_state_t *out)
{
    memset(out, 0, sizeof *out);
    out->iso = display_iso(e);
    out->shutter_ns = display_shutter(e);
    out->iso_manual = e->iso_manual;
    out->shutter_manual = e->shutter_manual;
    out->full_auto = full_auto(e);
    out->full_manual = full_manual(e);
    out->spot_hold = e->spot_hold;
    out->fixed_pair = !full_auto(e) || e->spot_hold;
    out->ev_steps = e->ev_steps;
    out->offset_stops = e->offset_stops;
    out->offset_valid = e->offset_valid;
    out->ev100 = e->ev100;
    {
        int stacked = e->shutter_manual && is_stacked(e, e->manual_shutter_ns);
        out->pv_ms = stacked ? e->pv_ns / 1e6 : 0;
        out->pv_slow = stacked && e->pv_slow;
        out->deficit_stops = stacked && e->pv_deficit > 1.035 ? log2d(e->pv_deficit) : 0;
        out->too_dark = stacked && e->too_dark;
    }
}
