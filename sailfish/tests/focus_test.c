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
    ctl_focus_set_mode(f, CTL_FOCUS_MANUAL);
    ctl_focus_set_mode(f, CTL_FOCUS_CONT);
    CHECK(st.light_ref && count("aeclear") == 0, "a focus mode change keeps the light point (the exposure does not move)");
    ctl_focus_light_clear(f);
    CHECK(!strcmp(last(), "tap 0.5 0.5 aeclear") && !st.light_ref && spot_last == 0, "light cleared: 'aeclear', spot off");
    /* 4. manual: steps, and a tap adopts the distance found. */
    ctl_focus_set_mode(f, CTL_FOCUS_MANUAL);
    CHECK(!strncmp(last(), "focus 1.000m", 12), "manual: 'focus 1.000m' (1 D)");
    ctl_focus_step(f, -1); ctl_focus_step(f, -1);
    CHECK(fabs(st.diopters - 1.10) < 1e-9 && !strcmp(last(), "focus 0.909m nowait"), "two steps nearer: 1.10 D (constant 0.05 D steps), sent without the lens wait");
    {
        int before = count("focus inf nowait"), sends = ncmds;
        for (i = 0; i < 40; ++i) ctl_focus_step(f, +1);
        CHECK(st.diopters == 0 && !strcmp(last(), "focus inf nowait"), "stepping farther stops at infinity");
        CHECK(count("focus inf nowait") - before == 1 && ncmds - sends == 22,
              "a key held at the end: infinity sent once, nothing more (22 steps from 1.10 D, 18 repeats unsent)");
    }
    ctl_focus_ref(f, 0.5, 0.5, t += 100);
    ctl_focus_live(f, 0, 2.0, t += 50);                      /* the CANCEL shows: inactive */
    ctl_focus_live(f, 3, 2.0, t += 100);
    ctl_focus_live(f, 4, 2.02, t += 100);                     /* found 2.02 D */
    CHECK(!strcmp(last(), "focus hold") && st.held && fabs(st.diopters - 2.02) < 1e-9,
          "manual tap: the lens held where the HAL put it, its reading shown as is (2.02 D), nothing sent back");
    ctl_focus_step(f, -1);
    CHECK(!st.held && !strncmp(last(), "focus ", 6) && strstr(last(), "m nowait"), "a volume step after it: the user's distance, set through the calibration");
    ctl_focus_ref(f, 0.5, 0.5, t += 100);
    ctl_focus_live(f, 0, 2.0, t += 50);                      /* the CANCEL shows: inactive */
    ctl_focus_live(f, 3, 2.0, t += 100);
    ctl_focus_live(f, 6, -1.0, t += 100);                     /* nothing found */
    CHECK(!st.held && !strncmp(last(), "focus ", 6), "manual tap that finds nothing: back to the distance in force");
    {   /* locked but NOT focused (5): the lens is wherever the scan gave up
         * (here: infinity): not held, the distance in force stays */
        double before = st.diopters;
        ctl_focus_ref(f, 0.5, 0.5, t += 100);
        ctl_focus_live(f, 0, 1.0, t += 50);
        ctl_focus_live(f, 3, 1.0, t += 100);
        ctl_focus_live(f, 5, 0.0, t += 100);
        CHECK(!st.held && fabs(st.diopters - before) < 1e-9 && strcmp(last(), "focus hold") != 0,
              "manual tap locked but not focused (5): not held, the distance shown and set stays");
    }
    /* 4b. The closest distance per camera (0.3.8): 10 D until the caps say,
     *     then the camera's own (20 for the JP2's 18 mm, uncalibrated); a
     *     fixed-focus camera's 0 keeps it. */
    ctl_focus_set_mode(f, CTL_FOCUS_MANUAL);
    ctl_focus_set_distance(f, 15.0);
    CHECK(fabs(st.diopters - 10.0) < 1e-9, "manual: 10 D at most before the caps");
    ctl_focus_set_max(f, 20.0);
    ctl_focus_set_distance(f, 15.0);
    CHECK(fabs(st.diopters - 15.0) < 1e-9, "manual: 15 D once the camera reaches 20");
    ctl_focus_set_distance(f, 25.0);
    CHECK(fabs(st.diopters - 20.0) < 1e-9, "manual: never beyond the camera's 20");
    ctl_focus_set_max(f, 0.0);
    ctl_focus_set_distance(f, 18.0);
    CHECK(fabs(st.diopters - 18.0) < 1e-9, "a fixed-focus camera's 0: the closest distance kept");
    ctl_focus_set_max(f, 12.0);                                  /* another camera, closer limit */
    ctl_focus_step(f, 1);                                        /* one step farther (> 0 = farther) from the clamped 12 */
    CHECK(fabs(st.diopters - 11.95) < 1e-9, "another camera's closer limit: the distance clamped to it, then stepped from there");
    /* the state the next rules start from: manual, the closest at 20 D, the
     * lens held at a touch's lock (1.0 D) */
    ctl_focus_set_max(f, 20.0);
    ctl_focus_set_mode(f, CTL_FOCUS_MANUAL);
    ctl_focus_ref(f, 0.5, 0.5, t += 1000);
    ctl_focus_live(f, 0, -1, t += 50); ctl_focus_live(f, 3, -1, t += 100); ctl_focus_live(f, 4, 1.0, t += 100);
    /* 4c. A camera our searches cannot focus (caps touch_focus=0, the 18 mm):
     *     no search at all, whoever asks (a touch, the FOCUS button entering
     *     manual), nothing changed; the light point still meters. */
    {
        int before = ncmds;
        double d = st.diopters;
        ctl_focus_set_search(f, 0);
        CHECK(ctl_focus_ref(f, 0.5, 0.5, t += 1000) == 0 && ncmds == before && !st.searching && fabs(st.diopters - d) < 1e-9,
              "touch_focus 0: a search refused, nothing sent, the distance kept");
        ctl_focus_light(f, 0.3, 0.3);
        CHECK(count("tap 0.3000 0.3000 ae") == 1, "touch_focus 0: the hold still sets the light point");
        ctl_focus_light_clear(f);
        ctl_focus_set_search(f, 1);
    }
    /* 4d. The FOCUS button entering manual (fc mode manual search): AF off
     *     where the lens is, then the search -- no trip to the manual
     *     distance first; a camera without searches gets the distance. */
    {
        int before;
        ctl_focus_set_mode(f, CTL_FOCUS_CONT);
        before = ncmds;
        ctl_focus_manual_search(f, t += 1000);
        CHECK(ncmds - before == 2 && !strcmp(cmds[before], "focus hold") && !strcmp(cmds[before + 1], "tap 0.5000 0.5000 af")
              && st.mode == CTL_FOCUS_MANUAL && st.searching,
              "manual search: 'focus hold' then the search, no distance sent first");
        ctl_focus_live(f, 0, -1, t += 50);
        ctl_focus_live(f, 3, -1, t += 100);
        ctl_focus_live(f, 4, 2.5, t += 100);
        CHECK(st.held && fabs(st.diopters - 2.5) < 1e-9 && !strcmp(last(), "focus hold"), "manual search: locked -> held at the distance found");
        ctl_focus_set_mode(f, CTL_FOCUS_CONT);
        ctl_focus_set_search(f, 0);
        before = ncmds;
        ctl_focus_manual_search(f, t += 1000);
        CHECK(ncmds - before == 1 && !strncmp(cmds[before], "focus ", 6) && strcmp(cmds[before], "focus hold") && !st.searching,
              "manual search, camera without searches: the distance shown, no search");
        ctl_focus_set_search(f, 1);
        ctl_focus_set_mode(f, CTL_FOCUS_MANUAL);
    }
    /* 4e. A touch while a search still runs (log of 14 Nov, 08:35:17): the
     *     previous search's scan and lock, reported before the CANCEL shows
     *     (state 0), are not this touch's outcome; after the INACTIVE, the
     *     new scan's lock is. A HAL that never shows INACTIVE: the old rule
     *     after 300 ms. */
    {
        int holds;
        ctl_focus_set_mode(f, CTL_FOCUS_MANUAL);
        ctl_focus_ref(f, 0.5, 0.5, t += 1000);                   /* the first search */
        ctl_focus_live(f, 0, 10.25, t += 30);
        ctl_focus_live(f, 3, 10.25, t += 360);                   /* it scans... */
        ctl_focus_ref(f, 0.80, 0.44, t += 470);                  /* ...and a touch elsewhere */
        holds = count("focus hold");
        ctl_focus_live(f, 3, 10.80, t += 55);                    /* still the first scan */
        ctl_focus_live(f, 4, 10.425, t += 120);                  /* the first search's lock */
        CHECK(count("focus hold") == holds && st.searching, "a touch during a search: the previous lock is not its outcome");
        ctl_focus_live(f, 0, 10.425, t += 80);                   /* the CANCEL shows */
        ctl_focus_live(f, 3, 10.425, t += 120);                  /* the new scan */
        ctl_focus_live(f, 4, 7.5, t += 150);                     /* its lock */
        CHECK(count("focus hold") == holds + 1 && st.held && fabs(st.diopters - 7.5) < 1e-9,
              "...the new scan's lock, after the INACTIVE, is: held at 7.5");
        /* a HAL that never shows INACTIVE */
        ctl_focus_ref(f, 0.5, 0.5, t += 1000);
        holds = count("focus hold");
        ctl_focus_live(f, 3, 7.5, t += 100);
        ctl_focus_live(f, 4, 6.0, t += 100);                     /* 200 ms: may be the old one */
        CHECK(count("focus hold") == holds && st.searching, "no INACTIVE seen: a lock within 300 ms waits");
        ctl_focus_live(f, 4, 6.0, t += 150);                     /* 350 ms: the old rule */
        CHECK(count("focus hold") == holds + 1 && fabs(st.diopters - 6.0) < 1e-9, "no INACTIVE seen: after 300 ms, the lock counts");
    }
    /* 5. clear: the reference goes, the mode is restated. */
    ctl_focus_clear(f);
    CHECK(count("tap 0.5 0.5 afclear") >= 1 && st.circle == CTL_CIRCLE_NONE, "clear: the focus region reset, the circle gone");
    /* 6. a camera with focus_step_percent 8 (the 18 mm): the near range in
     *    about 45 presses, not 180 (last: it leaves the lens at 9 D) */
    ctl_focus_set_step_pct(f, 8);
    ctl_focus_set_mode(f, CTL_FOCUS_MANUAL);
    {   /* the near range: 0 to 9 D in about 45 presses, not 180 (the state
         * put back afterwards: the tests below start from it) */
        int presses = 0;
        ctl_focus_set_max(f, 9.0);
        ctl_focus_set_distance(f, 0.0);
        while (st.diopters < 9.0 - 1e-9 && presses < 400) { ctl_focus_step(f, -1); ++presses; }
        printf("  (0 to 9 D: %d presses)\n", presses);
        CHECK(presses >= 35 && presses <= 55, "0 to 9 D in about 45 presses (was 180)");
    }
    ctl_focus_free(f);

    printf(fails ? "focus: FAILED (%d)\n" : "focus: OK (10 rules)\n", fails);
    return fails ? 1 : 0;
}
