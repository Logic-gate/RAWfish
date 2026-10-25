/*
 * ctl_exposure.h -- the camera's exposure behaviour, in the controller
 * (docs/ARCHITECTURE.md, step 3). A faithful port of the app's
 * ExposureLogic.qml, as pure logic: no I/O, no threads, no clock of its own.
 * The caller feeds it the engine's events (live values, meter samples) and
 * the interface's commands, and gives it two callbacks: one for the engine
 * commands it decides ("exposure exp=... iso=...", "exposure auto", "ev N",
 * "meter on|off"), one for the state the interface shows. Time comes in as a
 * parameter, so a host test can drive it frame by frame.
 *
 * Modes:
 *   automatic   neither wheel pinned: the HAL's AE drives; the state shows
 *               the live pair; EV is the HAL's compensation. A light
 *               reference (spot) makes the controller set the exposure from
 *               the spot in a few bounded corrections, then hold the pair.
 *   priority    one wheel pinned: the other value is computed here from the
 *               meter and re-sent as the scene moves (an AE loop of our own on
 *               real data, the preview in true manual). EV shifts its target.
 *   manual      both pinned: the pair as is; the offset from what automatic
 *               would choose is measured and reported.
 * The target ("well exposed") is calibrated per source (RAW meter / viewfinder
 * meter) from the HAL's converged AE while in automatic; 18 % before.
 */
#ifndef INDRAW_CTL_EXPOSURE_H
#define INDRAW_CTL_EXPOSURE_H

#include <stddef.h>

typedef struct {
    int iso_min, iso_max;
    double exp_min_ns;
    double shutter_max_ns;      /* the single-exposure ceiling (stacks beyond) */
    double ev_step;             /* EV per HAL compensation step */
    double focal_eq;            /* 35 mm-equivalent focal length (hand-held limit) */
    double aperture;            /* f-number, 0 if unknown (no EV100 then) */
} ctl_caps_t;

/* The state the interface shows (ctl_exp_state()). */
typedef struct {
    int iso;                    /* what the ISO wheel shows */
    double shutter_ns;          /* what the speed wheel shows */
    int iso_manual, shutter_manual;
    int full_auto, full_manual;
    int fixed_pair;             /* a pair the controller holds (photos carry it) */
    int spot_hold;
    int ev_steps;
    double offset_stops;        /* full manual: + brighter than automatic */
    int offset_valid;
    double ev100;               /* the metered light level; NAN without aperture */
    /* A Σ (stacked) shutter's preview: one frame of pv_ms at a raised ISO
     * stands for the stack (exposure = time x gain). pv_slow: the ISO limit
     * forced a frame longer than the target rate's; deficit_stops > 0: even
     * at the ceiling and the maximum ISO the frame is that much darker than
     * the photo (the meter compensates, the histogram cannot); too_dark: the
     * frame's signal is down in the noise, nothing reliable can be measured. */
    double pv_ms;
    int pv_slow;
    double deficit_stops;
    int too_dark;
} ctl_exp_state_t;

typedef void (*ctl_cmd_fn)(void *ctx, const char *engine_command);
typedef void (*ctl_state_fn)(void *ctx, const ctl_exp_state_t *state);

typedef struct ctl_exposure ctl_exposure_t;

ctl_exposure_t *ctl_exp_new(ctl_cmd_fn cmd, ctl_state_fn state, void *ctx);
void ctl_exp_free(ctl_exposure_t *e);

/* The engine's capabilities (from the caps event); the loop runs only once
 * they are known. Re-applies the mode (as on a restart). */
void ctl_exp_set_caps(ctl_exposure_t *e, const ctl_caps_t *caps);
/* raw = 1: the RAW meter is the source (type raw), else the viewfinder's. */
void ctl_exp_set_source(ctl_exposure_t *e, int raw);
/* The engine (re)started: nothing is known to be sent; apply the mode. */
void ctl_exp_restart(ctl_exposure_t *e);

/* Events from the engine. */
void ctl_exp_live(ctl_exposure_t *e, int iso, double exp_ns, int ae_state);
void ctl_exp_sample(ctl_exposure_t *e, double mean, int frame_iso, double frame_exp_ns, double now_ms);

/* Commands from the interface. */
void ctl_exp_touch_iso(ctl_exposure_t *e);        /* the ISO wheel touched: pinned where it is */
void ctl_exp_touch_shutter(ctl_exposure_t *e);
void ctl_exp_set_iso(ctl_exposure_t *e, int iso); /* a notch picked: pinned to it */
void ctl_exp_set_shutter(ctl_exposure_t *e, double ns);
void ctl_exp_set_ev(ctl_exposure_t *e, int steps);
void ctl_exp_reset_auto(ctl_exposure_t *e);       /* the AE button */
void ctl_exp_spot_start(ctl_exposure_t *e);       /* a light reference, in automatic */
void ctl_exp_spot_stop(ctl_exposure_t *e);
void ctl_exp_set_paused(ctl_exposure_t *e, int paused);
/* The target frame rate of a Σ preview (frames per second, 1..30; 10 by
 * default): the ISO is raised so that one frame this short stands for the
 * stack, within the sensor's maximum ISO. */
void ctl_exp_set_preview_fps(ctl_exposure_t *e, double fps);

/* The current state (also pushed through the state callback on change). */
void ctl_exp_state(const ctl_exposure_t *e, ctl_exp_state_t *out);

#endif
