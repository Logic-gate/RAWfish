/* Host test of the controller's focus behaviour (ctl_focus.c). A fake HAL
 * reports AF states frame by frame. Run by scripts/check-host.sh. */
#include "../ctl_focus.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static char cmds[128][64];
static int ncmds;
static int paused = -1, spot_calls = 0, spot_last = -1;
static ctl_focus_state_t st;

static void on_cmd(void *ctx, const char *c) { (void)ctx; if (ncmds < 128) snprintf(cmds[ncmds++], 64, "%s", c); }
static void on_state(void *ctx, const ctl_focus_state_t *s) { (void)ctx; st = *s; }
static void on_pause(void *ctx, int p) { (void)ctx; paused = p; }
static void on_spot(void *ctx, int on) { (void)ctx; spot_calls++; spot_last = on; }
static int count(const char *p) { int i, n = 0; for (i = 0; i < ncmds; ++i) if (!strncmp(cmds[i], p, strlen(p))) n++; return n; }
static const char *last(void) { return ncmds ? cmds[ncmds - 1] : ""; }

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } else printf("  ok: %s\n", msg); } while (0)

int main(void)
{
    ctl_focus_t *f = ctl_focus_new(on_cmd, on_state, on_pause, on_spot, NULL);
    double t = 0;
    int i;

    /* 1. once: a tap searches, a stale "locked" is ignored, the real lock holds. */
    ctl_focus_set_mode(f, CTL_FOCUS_ONCE);
    CHECK(!strcmp(last(), "focus auto"), "once: 'focus auto' applied");
    spot_calls = 0;
    ctl_focus_ref(f, 0.6, 0.4, t);
    CHECK(!strcmp(last(), "tap 0.6000 0.4000 af") && paused == 0 && st.searching && spot_calls == 0, "tap: 'tap af' only, exposure untouched, searching");
    ctl_focus_live(f, 4, 1.0, t += 50);                       /* the previous tap's lock, still reported */
    CHECK(st.searching && count("focus hold") == 0, "a 'locked' 50 ms after the trigger is the stale one: ignored");
    ctl_focus_live(f, 3, 1.0, t += 100);                      /* the scan */
    ctl_focus_live(f, 4, 1.0, t += 200);                      /* the lock */
    CHECK(!strcmp(last(), "focus hold") && st.circle == CTL_CIRCLE_SHARP && !st.searching && paused == 0,
          "lock: 'focus hold', circle sharp, exposure free");
    CHECK(spot_calls == 0, "after the search: the exposure still untouched (no light phase)");
    /* 2. a search that never answers: limit, then a late lock. */
    ctl_focus_ref(f, 0.3, 0.3, t += 1000);
    for (i = 0; i < 35; ++i) ctl_focus_live(f, 3, 1.0, t += 100);   /* 3.5 s of scanning */
    CHECK(!st.searching && st.circle == CTL_CIRCLE_UNSURE && paused == 0, "3 s without an answer: circle dashed, exposure free");

    ctl_focus_live(f, 4, 1.0, t += 100);
    CHECK(!strcmp(last(), "focus hold") && st.circle == CTL_CIRCLE_SHARP, "a lock after the limit still holds the lens");
    /* 3. continuous: a tap focuses on the point (settled = sharp); a long
     *    press sets the light point (a 5 % spot), which a focus tap keeps. */
    ctl_focus_set_mode(f, CTL_FOCUS_CONT);
    CHECK(!strcmp(last(), "focus continuous") && st.circle == CTL_CIRCLE_NONE && !st.light_ref, "mode change clears the reference");
    ctl_focus_ref(f, 0.5, 0.5, t += 100);
    CHECK(!strcmp(last(), "tap 0.5000 0.5000 af") && st.searching, "continuous tap: 'tap af' only, searching");
    ctl_focus_live(f, 1, 1.0, t += 100);                      /* passive scan */
    ctl_focus_live(f, 2, 1.0, t += 100);                      /* passive focused: the outcome */
    CHECK(!st.searching && st.circle == CTL_CIRCLE_SHARP, "continuous: settled on the point, circle sharp");
    ctl_focus_light(f, 0.2, 0.3);
    CHECK(!strcmp(last(), "tap 0.2000 0.3000 ae") && st.light_ref && spot_last == 1, "long press: 'tap ae', light point, spot on");
    ctl_focus_ref(f, 0.7, 0.7, t += 100);
    CHECK(!strcmp(last(), "tap 0.7000 0.7000 af") && st.light_ref, "a focus tap keeps the light point");
    ctl_focus_light_clear(f);
    CHECK(!strcmp(last(), "tap 0.5 0.5 aeclear") && !st.light_ref && spot_last == 0, "light cleared: 'aeclear', spot off");
    /* 4. manual: steps, and a tap adopts the distance found. */
    ctl_focus_set_mode(f, CTL_FOCUS_MANUAL);
    CHECK(!strncmp(last(), "focus 1.000m", 12), "manual: 'focus 1.000m' (1 D)");
    ctl_focus_step(f, -1); ctl_focus_step(f, -1);
    CHECK(fabs(st.diopters - 1.10) < 1e-9 && !strcmp(last(), "focus 0.909m nowait"), "two steps nearer: 1.10 D, sent without the lens wait");
    for (i = 0; i < 40; ++i) ctl_focus_step(f, +1);
    CHECK(st.diopters == 0 && !strcmp(last(), "focus inf nowait"), "stepping farther stops at infinity");
    ctl_focus_ref(f, 0.5, 0.5, t += 100);
    ctl_focus_live(f, 3, 2.0, t += 100);
    ctl_focus_live(f, 4, 2.02, t += 100);                     /* found 2.02 D */
    CHECK(fabs(st.diopters - 2.0) < 1e-9 && !strncmp(last(), "focus 0.500m", 12), "manual tap: the found distance adopted on the 0.05 D grid");
    /* 5. clear: the reference goes, the mode is restated. */
    ctl_focus_clear(f);
    CHECK(count("tap 0.5 0.5 afclear") >= 1 && st.circle == CTL_CIRCLE_NONE, "clear: the focus region reset, the circle gone");
    ctl_focus_free(f);

    printf(fails ? "focus: FAILED (%d)\n" : "focus: OK (6 rules)\n", fails);
    return fails ? 1 : 0;
}
