/* Host test of the controller's exposure behaviour (ctl_exposure.c): a
 * simulated scene and a HAL that applies a sent pair two frames later, as
 * the real one does. No Android, no Qt. Run by scripts/check-host.sh.
 * Each check names the rule it protects. */
#include "../ctl_exposure.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the fake engine: records commands, simulates the sensor --- */
static char cmds[256][64];
static int ncmds;
static ctl_exp_state_t last_state;
static int nstates;

static void on_cmd(void *ctx, const char *c) { (void)ctx; if (ncmds < 256) snprintf(cmds[ncmds++], 64, "%s", c); }
static void on_state(void *ctx, const ctl_exp_state_t *s) { (void)ctx; last_state = *s; nstates++; }

static int count(const char *prefix)
{
    int i, n = 0;
    for (i = 0; i < ncmds; ++i) if (!strncmp(cmds[i], prefix, strlen(prefix))) n++;
    return n;
}
static const char *last_cmd(void) { return ncmds ? cmds[ncmds - 1] : ""; }

/* The scene: a mean of L at ISO 100, 1/60 s. The HAL: a queue of pairs, the
 * frame measured now was exposed with the pair sent two samples ago. */
static double scene_L = 0.18;
static struct { int iso; double ns; } halq[4];
static int hal_auto = 1;
static int hal_iso = 400; static double hal_ns = 16666667;   /* the HAL's AE choice */

static void hal_apply_cmd(void)
{
    /* the last command sent, if an exposure one, enters the queue */
    const char *c = last_cmd();
    int iso; double sec;
    if (sscanf(c, "exposure exp=%lf iso=%d", &sec, &iso) == 2) { halq[3].iso = iso; halq[3].ns = sec * 1e9; hal_auto = 0; }
    else if (!strcmp(c, "exposure auto")) hal_auto = 1;
}
static void frame(ctl_exposure_t *e, double now_ms)
{
    int iso; double ns, mean;
    /* shift the queue: the pair sent two frames ago is what this frame got */
    if (!hal_auto) { halq[0] = halq[1]; halq[1] = halq[2]; halq[2] = halq[3]; iso = halq[1].iso; ns = halq[1].ns; }
    else { iso = hal_iso; ns = hal_ns; }
    if (iso <= 0) { iso = hal_iso; ns = hal_ns; }
    mean = scene_L * (iso / 100.0) * (ns / 16666667.0);
    ctl_exp_live(e, iso, ns, hal_auto ? 2 : 0);
    ctl_exp_sample(e, mean, iso, ns, now_ms);
    hal_apply_cmd();
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } else printf("  ok: %s\n", msg); } while (0)

int main(void)
{
    ctl_caps_t caps = { 100, 3200, 1000000.0, 500000000.0, 1.0 / 3, 26, 1.88 };
    ctl_exposure_t *e;
    double t = 0;
    int i, before;

    /* 1. Calibration in automatic: the target follows the HAL's converged mean. */
    e = ctl_exp_new(on_cmd, on_state, NULL);
    ctl_exp_set_caps(e, &caps);
    CHECK(count("meter off") == 1 && count("exposure auto") == 1, "caps: meter off + exposure auto sent once");
    hal_iso = 400; hal_ns = 16666667; scene_L = 0.10;            /* HAL's AE lands the scene at 0.40 */
    for (i = 0; i < 10; ++i) frame(e, t += 100);
    /* 2. ISO priority: the loop drives the speed to the calibrated target, with latency. */
    before = ncmds;
    ctl_exp_touch_iso(e);                                        /* pinned at the live 400 */
    scene_L = 0.025;                                             /* the scene goes 2 stops darker */
    for (i = 0; i < 40; ++i) frame(e, t += 100);
    CHECK(last_state.iso == 400 && last_state.iso_manual && !last_state.shutter_manual, "priority: ISO pinned at 400, speed free");
    CHECK(fabs(last_state.shutter_ns / 66666667.0 - 1) < 0.2, "priority: speed settled near 1/15 s (2 stops longer)");
    CHECK(ncmds - before < 12, "priority: converged in a few sends, no hunting (settle check)");
    /* 3. Full manual: the offset reads the scene. */
    ctl_exp_touch_shutter(e);
    for (i = 0; i < 5; ++i) frame(e, t += 100);
    CHECK(last_state.full_manual && last_state.fixed_pair, "manual: both pinned, fixed pair");
    CHECK(last_state.offset_valid && fabs(last_state.offset_stops) < 0.5, "manual: offset ~0 right after the loop had settled");
    scene_L *= 4;
    for (i = 0; i < 5; ++i) frame(e, t += 100);
    CHECK(fabs(last_state.offset_stops - 2.0) < 0.3, "manual: scene 2 stops brighter -> offset +2");
    CHECK(!isnan(last_state.ev100), "manual: EV100 available (aperture known)");
    /* 4. Back to automatic: ev 0 and exposure auto, once. */
    before = ncmds;
    ctl_exp_reset_auto(e);
    ctl_exp_reset_auto(e);
    CHECK(count("exposure auto") - 1 == 1 && ncmds - before == 3, "reset: 'ev 0' + 'exposure auto' once, second reset sends only ev");
    CHECK(last_state.full_auto && !last_state.fixed_pair, "reset: automatic, no fixed pair");
    /* 5. Spot in automatic: bounded corrections, then held; photos carry the pair. */
    hal_iso = 400; hal_ns = 16666667; scene_L = 0.10;
    for (i = 0; i < 5; ++i) frame(e, t += 100);
    before = ncmds;
    scene_L = 0.025;                                             /* a dark spot: 2 stops under the calibrated 0.40 */
    ctl_exp_spot_start(e);
    for (i = 0; i < 30; ++i) frame(e, t += 100);
    CHECK(last_state.spot_hold && last_state.fixed_pair && last_state.full_auto, "spot: held in automatic, fixed pair");
    CHECK(ncmds - before >= 1 && ncmds - before <= 4, "spot: at most 4 corrections");
    CHECK(fabs(last_state.shutter_ns * last_state.iso / (16666667.0 * 400) - 4.0) < 1.0, "spot: the pair opened ~2 stops for the dark spot");
    before = ncmds;
    for (i = 0; i < 20; ++i) frame(e, t += 100);
    CHECK(ncmds == before, "spot: nothing more sent while held");
    ctl_exp_spot_stop(e);
    CHECK(!strcmp(last_cmd(), "exposure auto") && !last_state.spot_hold, "spot stop: back to the HAL's AE");
    /* 6. Stacked (Σ) shutter: one preview frame of the same exposure, as
     *    short as the target rate (10 /s) allows within the maximum ISO. */
    hal_iso = 400; hal_ns = 16666667;
    for (i = 0; i < 5; ++i) frame(e, t += 100);                  /* the HAL's AE is at 400 again */
    ctl_exp_reset_auto(e);
    ctl_exp_set_iso(e, 100);
    ctl_exp_set_shutter(e, 2e9);                                 /* Σ2s at ISO 100: E = 2e11 */
    {
        double sec; int iso;
        CHECK(sscanf(last_cmd(), "exposure exp=%lf iso=%d", &sec, &iso) == 2 && fabs(sec - 0.1) < 1e-6 && iso == 2000,
              "Σ2s ISO 100: preview 1/10 s at ISO 2000 (same exposure, 10 frames/s)");
        CHECK(!last_state.pv_slow && last_state.deficit_stops == 0 && fabs(last_state.pv_ms - 100) < 0.5,
              "Σ2s ISO 100: not slowed, no deficit");
    }
    ctl_exp_set_iso(e, 400);                                     /* E = 8e11: 1/10 s would need ISO 8000 */
    {
        double sec; int iso;
        CHECK(sscanf(last_cmd(), "exposure exp=%lf iso=%d", &sec, &iso) == 2 && fabs(sec - 0.25) < 1e-6 && iso == 3200,
              "Σ2s ISO 400: the ISO limit lengthens the frame to 1/4 s at ISO 3200");
        CHECK(last_state.pv_slow && last_state.deficit_stops == 0, "Σ2s ISO 400: slowed, no deficit");
    }
    ctl_exp_set_shutter(e, 8e9);                                 /* Σ8s at ISO 400: 2x beyond 0.5 s at 3200 */
    CHECK(fabs(last_state.deficit_stops - 1.0) < 0.02, "Σ8s ISO 400: one stop darker than the photo");
    CHECK(fabs(last_state.shutter_ns - 8e9) < 1 && last_state.shutter_manual, "Σ8s: the wheel shows 8 s");
    /* speed priority: the free ISO converges for the photo, through the deficit */
    ctl_exp_reset_auto(e);
    hal_iso = 400; hal_ns = 16666667; scene_L = 0.10;
    for (i = 0; i < 10; ++i) frame(e, t += 100);                 /* calibrate: target 0.40 */
    scene_L = 0.10 / 64;                                         /* much darker: needs 64x the exposure */
    ctl_exp_set_shutter(e, 2e9);                                 /* Σ2s, ISO free */
    for (i = 0; i < 60; ++i) frame(e, t += 150);
    {
        /* the photo needs 400 x 1/60 s x 64 = Σ2s at ISO ~213 */
        CHECK(fabs(last_state.iso / 213.0 - 1) < 0.25, "Σ2s speed priority: the photo's ISO settles near 213 through the adaptive preview");
    }
    /* a deficit: the preview frame stays 3 stops under the photo, the meter compensates */
    ctl_exp_set_shutter(e, 8e9);                                 /* Σ8s, ISO free */
    scene_L = 0.10 / 2048;                                       /* photo: Σ8s at ISO ~1707 */
    for (i = 0; i < 80; ++i) frame(e, t += 150);
    CHECK(last_state.deficit_stops > 2.5 && fabs(last_state.iso / 1707.0 - 1) < 0.25,
          "Σ8s in a deficit (~3 stops): the photo's ISO still settles near 1707");
    ctl_exp_set_shutter(e, 2e9);
    scene_L = 0.10 / 64;
    for (i = 0; i < 40; ++i) frame(e, t += 150);
    /* too dark: the loop opens up to the limit (0.5 s, the maximum ISO), then
     * flags it and sends nothing more */
    scene_L = 1e-7;
    for (i = 0; i < 40; ++i) frame(e, t += 150);
    CHECK(last_state.too_dark && strstr(last_cmd(), "exp=0.5000000 iso=3200"), "Σ too dark: flagged only at the limit (0.5 s, ISO max)");
    before = ncmds;
    for (i = 0; i < 10; ++i) frame(e, t += 150);
    CHECK(ncmds == before, "Σ too dark at the limit: the loop sends nothing");
    scene_L = 0.10 / 64;
    for (i = 0; i < 5; ++i) frame(e, t += 150);
    CHECK(!last_state.too_dark, "light back: the flag goes");
    /* manual, under-exposed but NOT at the limit: a dark photo, not a failed
     * measure -- no red, the offset says how dark (the 0.47.17 bug) */
    ctl_exp_set_iso(e, 100);
    ctl_exp_set_shutter(e, 4e9);                                 /* Σ4s ISO 100: preview 1/10 s at ISO 4000 */
    scene_L = 1e-6;                                              /* very dim: the frame mean ~0.001 */
    for (i = 0; i < 10; ++i) frame(e, t += 150);
    CHECK(!last_state.too_dark && last_state.offset_valid && last_state.offset_stops < -5,
          "Σ4s manual, very dim, not at the limit: no red, the offset shows the under-exposure");
    /* 7. Paused: no sends. */
    ctl_exp_reset_auto(e);
    ctl_exp_touch_iso(e);
    ctl_exp_set_paused(e, 1);
    before = ncmds; scene_L = 0.3;
    for (i = 0; i < 10; ++i) frame(e, t += 100);
    CHECK(ncmds == before, "paused: the loop sends nothing");
    ctl_exp_free(e);

    printf(fails ? "exposure: FAILED (%d)\n" : "exposure: OK (10 rules)\n", fails);
    return fails ? 1 : 0;
}
