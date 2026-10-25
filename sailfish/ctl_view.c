/*
 * ctl_view.c -- the viewfinder's geometry (see ctl_view.h). Ported from the
 * page's "Coordinates" model (app/qml/indraw.qml up to ui 127).
 */
#define _POSIX_C_SOURCE 200809L
#include "ctl_view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOUPE_MAX 12.0
#define LOUPE_SEND_MS 80.0          /* at most one loupe command per 80 ms */

struct ctl_view {
    ctl_view_cmd_fn cmd;
    ctl_view_state_fn state_cb;
    ctl_view_ref_fn ref_cb;
    void *ctx;
    double zoom_max;
    int crop_free;
    double zoom, loupe, cx, cy;
    int gesture;
    int has_ref;
    double ref_ax, ref_ay;      /* the reference point, array coordinates */
    char last_loupe_cmd[64];
    char pending_cmd[64];
    double last_loupe_ms;
    ctl_view_state_t last;
    int have_last;
};

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static void send(ctl_view_t *v, const char *c) { if (v->cmd) v->cmd(v->ctx, c); }

/* A window of size s centred on (cx, cy), kept inside `in` (the engine's
 * preview_crop clamps the same way). */
static ctl_rect_t window_in(double s, double cx, double cy, ctl_rect_t in)
{
    ctl_rect_t r;
    r.w = s; r.h = s;
    r.x = clampd(cx - s / 2, in.x, in.x + in.w - s);
    r.y = clampd(cy - s / 2, in.y, in.y + in.h - s);
    return r;
}
static ctl_rect_t zoom_window(const ctl_view_t *v)
{
    ctl_rect_t r;
    double s = 1.0 / fmax(1.0, v->zoom);
    r.x = (1 - s) / 2; r.y = (1 - s) / 2; r.w = s; r.h = s;
    return r;
}
/* How much of the loupe the HAL crops. */
static double hal_loupe(const ctl_view_t *v)
{
    double z = fmax(1.0, v->zoom), h;
    if (v->gesture) return 1.0;
    h = fmin(v->loupe, v->zoom_max / z);
    if (!v->crop_free) {
        double s = 1.0 / (z * v->loupe);
        double off = fmax(fabs(v->cx - 0.5), fabs(v->cy - 0.5));
        h = fmin(h, 1.0 / (z * fmin(1.0 / z, 2 * off + s)));   /* keep the view inside the HAL window */
    }
    return fmax(1.0, h);
}
ctl_rect_t ctl_view_frame(const ctl_view_t *v)
{
    double f = 1.0 / (fmax(1.0, v->zoom) * hal_loupe(v));
    if (v->crop_free) return window_in(f, v->cx, v->cy, zoom_window(v));
    {
        ctl_rect_t r;                              /* the HAL centres it */
        r.x = (1 - f) / 2; r.y = (1 - f) / 2; r.w = f; r.h = f;
        return r;
    }
}
static ctl_rect_t display_window(const ctl_view_t *v)
{
    return window_in(1.0 / (fmax(1.0, v->zoom) * v->loupe), v->cx, v->cy, ctl_view_frame(v));
}

static void publish(ctl_view_t *v)
{
    ctl_view_state_t s;
    ctl_view_state(v, &s);
    if (v->have_last && memcmp(&s, &v->last, sizeof s) == 0) return;
    v->last = s; v->have_last = 1;
    if (v->state_cb) v->state_cb(v->ctx, &s);
}

/* The loupe command, sent only when its HAL part changes and not more than
 * every 80 ms (a pending one goes at the next change or tick). */
static void loupe_command(ctl_view_t *v, double now_ms)
{
    char c[64];
    double h = hal_loupe(v);
    snprintf(c, sizeof c, "loupe %.3f %.4f %.4f", h, v->crop_free ? v->cx : 0.5, v->crop_free ? v->cy : 0.5);
    if (!strcmp(c, v->last_loupe_cmd)) { v->pending_cmd[0] = '\0'; return; }
    if (now_ms - v->last_loupe_ms < LOUPE_SEND_MS) { snprintf(v->pending_cmd, sizeof v->pending_cmd, "%s", c); return; }
    snprintf(v->last_loupe_cmd, sizeof v->last_loupe_cmd, "%s", c);
    v->pending_cmd[0] = '\0';
    v->last_loupe_ms = now_ms;
    send(v, c);
}
void ctl_view_tick(ctl_view_t *v, double now_ms)
{
    if (v->pending_cmd[0] && now_ms - v->last_loupe_ms >= LOUPE_SEND_MS) loupe_command(v, now_ms);
}
static void update(ctl_view_t *v, double now_ms)
{
    loupe_command(v, now_ms);
    publish(v);
}

ctl_view_t *ctl_view_new(ctl_view_cmd_fn cmd, ctl_view_state_fn state, ctl_view_ref_fn ref, void *ctx)
{
    ctl_view_t *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->cmd = cmd; v->state_cb = state; v->ref_cb = ref; v->ctx = ctx;
    v->zoom_max = 1; v->zoom = 1; v->loupe = 1; v->cx = 0.5; v->cy = 0.5;
    v->last_loupe_ms = -1000;
    return v;
}
void ctl_view_free(ctl_view_t *v) { free(v); }

void ctl_view_set_caps(ctl_view_t *v, double zoom_max, int crop_free)
{
    v->zoom_max = zoom_max > 1 ? zoom_max : 1;
    v->crop_free = !!crop_free;
}
void ctl_view_restart(ctl_view_t *v)
{
    char c[32];
    v->loupe = 1; v->cx = 0.5; v->cy = 0.5; v->gesture = 0;
    v->has_ref = 0;
    v->last_loupe_cmd[0] = '\0'; v->pending_cmd[0] = '\0'; v->last_loupe_ms = -1000;
    snprintf(c, sizeof c, "zoom %.2f", v->zoom);
    send(v, c);
    update(v, 0);
}

void ctl_view_set_zoom(ctl_view_t *v, double zoom, double now_ms)
{
    char c[32];
    v->zoom = clampd(zoom, 1.0, v->zoom_max);
    snprintf(c, sizeof c, "zoom %.2f", v->zoom);
    send(v, c);
    /* the loupe lives inside the zoom window: re-placed */
    {
        ctl_rect_t z = zoom_window(v);
        double s = 1.0 / (v->zoom * v->loupe);
        v->cx = clampd(v->cx, z.x + s / 2, z.x + z.w - s / 2);
        v->cy = clampd(v->cy, z.y + s / 2, z.y + z.h - s / 2);
    }
    update(v, now_ms);
}
void ctl_view_set_loupe(ctl_view_t *v, double loupe, double now_ms)
{
    v->loupe = clampd(loupe, 1.0, LOUPE_MAX);
    if (v->loupe <= 1.01) { v->loupe = 1; v->cx = 0.5; v->cy = 0.5; }
    update(v, now_ms);
}
void ctl_view_pan(ctl_view_t *v, double du, double dv, double now_ms)
{
    ctl_rect_t z = zoom_window(v);
    double s = 1.0 / (fmax(1.0, v->zoom) * v->loupe);
    if (v->loupe <= 1.01) return;
    v->cx = clampd(v->cx - du * s, z.x + s / 2, z.x + z.w - s / 2);
    v->cy = clampd(v->cy - dv * s, z.y + s / 2, z.y + z.h - s / 2);
    update(v, now_ms);
}
void ctl_view_gesture(ctl_view_t *v, int on, double now_ms)
{
    v->gesture = !!on;
    update(v, now_ms);
}

/* A tap: display-window point -> array point (kept, for the circle) ->
 * viewfinder-frame point, handed to the focus behaviour. */
void ctl_view_tap(ctl_view_t *v, double u, double vv)
{
    ctl_rect_t d = display_window(v), f = ctl_view_frame(v);
    double ax = d.x + u * d.w, ay = d.y + vv * d.h;
    v->ref_ax = ax; v->ref_ay = ay; v->has_ref = 1;
    if (v->ref_cb) v->ref_cb(v->ctx, (ax - f.x) / f.w, (ay - f.y) / f.h);
    publish(v);
}
void ctl_view_clear_ref(ctl_view_t *v) { v->has_ref = 0; publish(v); }

void ctl_view_state(const ctl_view_t *v, ctl_view_state_t *out)
{
    ctl_rect_t d = display_window(v);
    memset(out, 0, sizeof *out);
    out->zoom = v->zoom;
    out->loupe = v->loupe;
    out->view = d;
    out->has_ref = v->has_ref;
    if (v->has_ref) { out->ref_u = (v->ref_ax - d.x) / d.w; out->ref_v = (v->ref_ay - d.y) / d.h; }
}
