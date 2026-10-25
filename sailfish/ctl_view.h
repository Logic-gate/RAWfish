/*
 * ctl_view.h -- the viewfinder's geometry, in the controller
 * (docs/ARCHITECTURE.md, step 5). Pure logic, like the other modules.
 *
 * "Array" coordinates are normalised in the sensor's active array (0..1).
 * The digital zoom shows a centred window 1/zoom wide (the photo's crop);
 * the focus loupe a window 1/(zoom x loupe) wide centred on (cx, cy). The
 * loupe is a REAL crop of the preview by the HAL ("loupe" engine command) up
 * to the HAL's maximum zoom over the digital zoom, a display enlargement
 * beyond. Two kinds of HAL: FREEFORM crops anywhere (the HAL window follows
 * the pan); CENTER_ONLY re-centres every crop, so the HAL window is kept
 * wide enough to contain the panned view and the display does the rest.
 * While a finger moves the loupe (gesture) the HAL crop stays at the zoom
 * window: one request change when it starts, one when it ends.
 *
 * The interface works in DISPLAY-window coordinates (0..1 across what it
 * shows, the view rotation undone by the interface): taps and pans come in
 * that way; the view window to show, and the reference point to draw, go out
 * that way.
 */
#ifndef INDRAW_CTL_VIEW_H
#define INDRAW_CTL_VIEW_H

typedef struct { double x, y, w, h; } ctl_rect_t;

typedef struct {
    double zoom, loupe;
    ctl_rect_t view;            /* what the interface shows, array-normalised */
    int has_ref;
    double ref_u, ref_v;        /* the reference point in display-window coordinates */
} ctl_view_state_t;

typedef void (*ctl_view_cmd_fn)(void *ctx, const char *engine_command);
typedef void (*ctl_view_state_fn)(void *ctx, const ctl_view_state_t *state);
/* A tap converted to viewfinder-frame coordinates (the engine's "tap" takes
 * these): the controller hands it to the focus behaviour. */
typedef void (*ctl_view_ref_fn)(void *ctx, double fx, double fy);

typedef struct ctl_view ctl_view_t;

ctl_view_t *ctl_view_new(ctl_view_cmd_fn cmd, ctl_view_state_fn state, ctl_view_ref_fn ref, void *ctx);
void ctl_view_free(ctl_view_t *v);

void ctl_view_set_caps(ctl_view_t *v, double zoom_max, int crop_free);
void ctl_view_restart(ctl_view_t *v);                 /* a new session: zoom re-sent, loupe closed */

void ctl_view_set_zoom(ctl_view_t *v, double zoom, double now_ms);
void ctl_view_set_loupe(ctl_view_t *v, double loupe, double now_ms);   /* 1 closes it */
void ctl_view_pan(ctl_view_t *v, double du, double dv, double now_ms); /* in display-window units */
void ctl_view_gesture(ctl_view_t *v, int on, double now_ms);
void ctl_view_tap(ctl_view_t *v, double u, double vv);               /* display-window point */
void ctl_view_clear_ref(ctl_view_t *v);
void ctl_view_tick(ctl_view_t *v, double now_ms);                    /* flushes a throttled loupe command */

void ctl_view_state(const ctl_view_t *v, ctl_view_state_t *out);
/* The current viewfinder frame window (the preview crop), array-normalised. */
ctl_rect_t ctl_view_frame(const ctl_view_t *v);

#endif
