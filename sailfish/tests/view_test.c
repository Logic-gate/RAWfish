/* Host test of the controller's view geometry (ctl_view.c): zoom, loupe,
 * centre-only and freeform HALs, gestures, taps. Run by check-host.sh. */
#include "../ctl_view.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static char cmds[128][64];
static int ncmds;
static ctl_view_state_t st;
static double ref_fx = -1, ref_fy = -1;
static void on_cmd(void *ctx, const char *c) { (void)ctx; if (ncmds < 128) snprintf(cmds[ncmds++], 64, "%s", c); }
static void on_state(void *ctx, const ctl_view_state_t *s) { (void)ctx; st = *s; }
static void on_ref(void *ctx, double fx, double fy) { (void)ctx; ref_fx = fx; ref_fy = fy; }
static const char *last(void) { return ncmds ? cmds[ncmds - 1] : ""; }
static int count(const char *p) { int i, n = 0; for (i = 0; i < ncmds; ++i) if (!strncmp(cmds[i], p, strlen(p))) n++; return n; }
static int near(double a, double b, double eps) { return fabs(a - b) < eps; }

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } else printf("  ok: %s\n", msg); } while (0)

int main(void)
{
    ctl_view_t *v = ctl_view_new(on_cmd, on_state, on_ref, NULL);
    double t = 0;
    int i, before;

    /* 1. centre-only HAL (the JP2), zoom 1. */
    ctl_view_set_caps(v, 4.0, 0);
    ctl_view_restart(v);
    CHECK(count("zoom 1.00") == 1 && near(st.view.w, 1, 1e-9), "restart: zoom re-sent, the whole frame shown");
    ctl_view_set_loupe(v, 3.0, t += 100);
    CHECK(!strcmp(last(), "loupe 3.000 0.5000 0.5000") && near(st.view.w, 1.0 / 3, 1e-9), "loupe 3 at the centre: the HAL crops 3x, view 1/3");
    /* a gesture: the HAL crop goes back to the zoom window, the view stays */
    ctl_view_gesture(v, 1, t += 100);
    CHECK(!strcmp(last(), "loupe 1.000 0.5000 0.5000") && near(st.view.w, 1.0 / 3, 1e-9), "gesture: HAL crop 1, the view window unchanged");
    before = ncmds;
    for (i = 0; i < 20; ++i) ctl_view_pan(v, -0.05, 0, t += 10);       /* a pan to the right, 10 ms apart */
    CHECK(ncmds == before, "pan during the gesture: no loupe command at all");
    CHECK(near(st.view.x, 0.5 + 1.0 / 6, 1e-6) && near(st.view.w, 1.0 / 3, 1e-9), "pan: the view moved right and stopped at the edge");
    ctl_view_gesture(v, 0, t += 100);
    CHECK(near(st.view.x, 0.5 + 1.0 / 6, 1e-6), "release: the view stays where it was panned");
    CHECK(!strncmp(last(), "loupe 1.", 8), "release near the edge on a centre-only HAL: the HAL cannot crop (1.0)");
    ctl_view_pan(v, 0.05 * 20, 0, t += 100);                            /* back to the centre */
    ctl_view_tick(v, t += 100);
    CHECK(!strcmp(last(), "loupe 3.000 0.5000 0.5000"), "back at the centre: the HAL crops 3x again");
    /* 2. a tap in the loupe: frame coordinates for the engine, the circle in display terms. */
    ctl_view_tap(v, 0.25, 0.75);
    CHECK(near(ref_fx, 0.25, 1e-6) && near(ref_fy, 0.75, 1e-6), "tap at centre loupe 3 (HAL crop = view): frame coords = display coords");
    CHECK(st.has_ref && near(st.ref_u, 0.25, 1e-6) && near(st.ref_v, 0.75, 1e-6), "the reference point reported in display coordinates");
    ctl_view_pan(v, -0.2, 0, t += 100); ctl_view_tick(v, t += 100);
    CHECK(st.has_ref && st.ref_u < 0.25, "after a pan the point stays on the scene (moves in the display)");
    /* 3. zoom + loupe: the product stays within the HAL's maximum. */
    ctl_view_set_loupe(v, 1, t += 100);
    ctl_view_set_zoom(v, 2.0, t += 100);
    CHECK(!strcmp(cmds[ncmds - 2], "zoom 2.00") || !strcmp(last(), "zoom 2.00"), "zoom 2 sent");
    ctl_view_set_loupe(v, 4.0, t += 100);
    CHECK(!strncmp(last(), "loupe 2.000", 11) && near(st.view.w, 1.0 / 8, 1e-9), "zoom 2 x loupe 4: the HAL crops 2 (max 4 / zoom 2), the view is 1/8");
    /* 4. freeform HAL: the HAL window follows the pan. */
    ctl_view_set_caps(v, 4.0, 1);
    ctl_view_set_zoom(v, 1.0, t += 100);
    ctl_view_set_loupe(v, 4.0, t += 100);
    ctl_view_pan(v, -0.1, 0, t += 100); ctl_view_tick(v, t += 100);
    CHECK(!strncmp(last(), "loupe 4.000 0.5250", 18), "freeform: 'loupe 4' with the centre moved");
    /* 5. throttle: two changes within 80 ms give one command, the second flushed by a tick. */
    before = ncmds;
    ctl_view_set_loupe(v, 2.0, t += 100);
    ctl_view_set_loupe(v, 3.0, t += 10);
    CHECK(ncmds - before == 1 && !strncmp(last(), "loupe 2.000", 11), "two loupe changes 10 ms apart: one command (the first)");
    ctl_view_tick(v, t += 100);
    CHECK(ncmds - before == 2 && !strncmp(last(), "loupe 3.000", 11), "the tick flushes the pending one");
    ctl_view_set_loupe(v, 6.0, t += 100);
    CHECK(!strncmp(last(), "loupe 4.000", 11) && near(st.view.w, 1.0 / 6, 1e-9), "loupe 6: the HAL crops its max 4, the display the rest");
    ctl_view_free(v);

    printf(fails ? "view: FAILED (%d)\n" : "view: OK (5 rules)\n", fails);
    return fails ? 1 : 0;
}
