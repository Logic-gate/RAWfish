/*
 * ctl_focus.h -- the camera's focus behaviour, in the controller
 * (docs/ARCHITECTURE.md, step 4). Pure logic, like ctl_exposure: the engine's
 * events and the interface's commands come in, engine commands and a state
 * go out through callbacks, time is a parameter.
 *
 * Modes:
 *   continuous  the HAL focuses by itself; a tap is light only (a sun)
 *   once        a tap runs one search on the point; when it has locked, the
 *               lens is held there ("focus hold") until the reference goes
 *   manual      a distance, stepped by 0.05 D (0 = infinity .. 10 D); a tap
 *               runs one search and adopts the distance found
 * A tap: "tap FX FY ae [af]" to the engine (the HAL's AE region, its meters'
 * spot, and the search), the exposure left alone while the search runs (the
 * pause callback), then the light phase (the spot callback) once the
 * search has ended or hit its 3 s limit; a lock reported later still
 * counts. The search's outcome is the first final AF state after the search
 * was seen to start: an AUTO search (once, manual) counts nothing before the
 * HAL shows its CANCEL (INACTIVE), so the previous search's scan and lock --
 * a touch made while it ran -- are not taken (0.3.8); a HAL that never shows
 * INACTIVE, or the scan, is accepted after 300 ms.
 */
#ifndef INDRAW_CTL_FOCUS_H
#define INDRAW_CTL_FOCUS_H

enum { CTL_FOCUS_CONT = 0, CTL_FOCUS_ONCE = 1, CTL_FOCUS_MANUAL = 2 };
/* The circle the interface draws: none, searching, sharp, locked but not
 * sharp (the HAL's "not focused"), failed/no answer, or a sun (light only). */
enum { CTL_CIRCLE_NONE = 0, CTL_CIRCLE_SEARCHING = 1, CTL_CIRCLE_SHARP = 4,
       CTL_CIRCLE_UNSURE = 5, CTL_CIRCLE_FAILED = 6, CTL_CIRCLE_SUN = 7 };

typedef struct {
    int mode;
    double diopters;            /* the manual distance (0 = infinity) */
    int circle;                 /* CTL_CIRCLE_* */
    int searching;              /* a search runs (exposure left alone) */
    int light_ref;              /* a reference measures the light (the spot) */
    int held;                   /* manual: the lens is held where a touch's search left it;
                                   the distance shown is the HAL's reading, and the photo
                                   keeps the lens there (it sends no distance) */
} ctl_focus_state_t;

typedef void (*ctl_focus_cmd_fn)(void *ctx, const char *engine_command);
typedef void (*ctl_focus_state_fn)(void *ctx, const ctl_focus_state_t *state);
/* Couplings with the exposure behaviour: leave the exposure alone (1) or
 * not (0); start / stop the spot measure of a light reference. */
typedef void (*ctl_focus_pause_fn)(void *ctx, int paused);
typedef void (*ctl_focus_spot_fn)(void *ctx, int on);

typedef struct ctl_focus ctl_focus_t;

ctl_focus_t *ctl_focus_new(ctl_focus_cmd_fn cmd, ctl_focus_state_fn state,
                           ctl_focus_pause_fn pause, ctl_focus_spot_fn spot, void *ctx);
void ctl_focus_free(ctl_focus_t *f);

/* The engine (re)started: apply the mode (the new session knows nothing). */
void ctl_focus_restart(ctl_focus_t *f);
/* The engine's "ae" line: AF state and the lens as a real distance (diopters,
 * < 0 unknown); now_ms drives the 3 s limit. */
void ctl_focus_live(ctl_focus_t *f, int af_state, double focus_real_d, double now_ms);

/* Commands from the interface. */
void ctl_focus_set_mode(ctl_focus_t *f, int mode);
/* The FOCUS button entering manual: manual focus, and one search in the
 * middle at once, from where the lens is ("focus hold", then the search),
 * instead of the mode's distance first (a lens trip of ~350 ms, then the
 * search started from there anyway). A camera that takes no search
 * (ctl_focus_set_search 0) gets the plain mode change: the distance shown. */
void ctl_focus_manual_search(ctl_focus_t *f, double now_ms);
void ctl_focus_step(ctl_focus_t *f, int dir);             /* manual: +1 farther, -1 nearer */
void ctl_focus_set_distance(ctl_focus_t *f, double diopters);
/* The camera's closest distance, in the diopters the focus works in (the
 * caps' min_focus_real: its HAL value through the calibration table, or the
 * HAL value with no table). Manual distances stop there. <= 0.5: kept. */
void ctl_focus_set_max(ctl_focus_t *f, double max_diopters);
/* Volume steps above about 1 D as a share of the value (device profile
 * focus_step_percent; 0 = the constant 0.05 D). */
void ctl_focus_set_step_pct(ctl_focus_t *f, double pct);
/* The caps' touch_focus: 0 = a camera our searches cannot focus (device
 * profile touch_focus_off; the JP2's 18 mm). No search is ever started on
 * it -- not a touch, not the FOCUS button entering manual: its automatic
 * mode is the HAL's own, its manual one the volume keys. 1 = the usual. */
void ctl_focus_set_search(ctl_focus_t *f, int allowed);
/* A long press: the exposure meters a 5 % spot at that point (the focus is
 * untouched); _light_clear: back to the whole image. */
void ctl_focus_light(ctl_focus_t *f, double fx, double fy);
void ctl_focus_light_clear(ctl_focus_t *f);
/* A tap, frame coords: one search. Returns 0 when this camera takes none
 * (ctl_focus_set_search 0): nothing sent, nothing changed. */
int ctl_focus_ref(ctl_focus_t *f, double fx, double fy, double now_ms);
void ctl_focus_clear(ctl_focus_t *f);                     /* the reference removed */
void ctl_focus_set_scene(ctl_focus_t *f, int scene_on);   /* a scene drives the exposure: no spot */

void ctl_focus_state(const ctl_focus_t *f, ctl_focus_state_t *out);

#endif
