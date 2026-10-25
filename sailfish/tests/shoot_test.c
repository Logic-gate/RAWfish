/* Host test of the controller's shooting decisions (ctl_shoot.c). */
#include "../ctl_shoot.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } else printf("  ok: %s\n", msg); } while (0)

int main(void)
{
    char lines[CTL_SHOOT_MAX_LINES][CTL_SHOOT_LINE];
    ctl_shoot_req_t r;
    ctl_exp_state_t xs;
    ctl_focus_state_t fs;
    int n;
    memset(&r, 0, sizeof r); memset(&xs, 0, sizeof xs); memset(&fs, 0, sizeof fs);
    snprintf(r.name, sizeof r.name, "photo");
    r.quality = 95; snprintf(r.noise, sizeof r.noise, "off_noise"); snprintf(r.edge, sizeof r.edge, "fast_edge");
    r.bracket = -1; r.denoise = 1;
    xs.full_auto = 1; xs.fixed_pair = 0; xs.iso = 400; xs.shutter_ns = 16666667;
    fs.mode = CTL_FOCUS_CONT; fs.diopters = 1.0;

    /* 1. automatic, one JPEG: the camera's AE at the shot, no exp. */
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "jpeg photo q=95 off_noise fast_edge"), "auto JPEG: quality and processing, no pair");
    /* 2. a held pair goes with the photo; manual focus restates its distance. */
    xs.fixed_pair = 1; xs.full_auto = 0; xs.iso_manual = 1;
    fs.mode = CTL_FOCUS_MANUAL; fs.diopters = 2.0;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "jpeg photo focus=0.500m q=95 off_noise fast_edge exp=0.0166667 iso=400"), "held pair + manual focus: focus= and exp= iso=");
    /* 3. raw: no quality, no processing. */
    r.raw = 1;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "raw photo focus=0.500m exp=0.0166667 iso=400"), "raw: no q, no modes");
    /* 4. a stacked shutter with a pair: a stack with the denoise factor. */
    xs.shutter_ns = 2e9; r.denoise = 2;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "stack photo film=2.000 iso=400 stk_denoise=2"), "Σ2s: a stack, denoise x2");
    /* 5. bracket x3 in JPEG: one burst with the pair; in raw three shots. */
    xs.shutter_ns = 16666667; r.denoise = 1; r.bracket = 0; r.raw = 0; fs.mode = CTL_FOCUS_ONCE;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "burst photo n=3 q=95 off_noise fast_edge exp=0.0166667 iso=400"), "x3 JPEG: a burst, the pair along");
    r.raw = 1;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 3 && !strcmp(lines[0], lines[2]) && !strncmp(lines[1], "raw photo exp=0.0166667", 23), "x3 raw: three shots at the same exposure");
    /* 6. bracket 2 stops: three real exposures, clamped at the ceiling. */
    r.raw = 0; r.bracket = 2; xs.shutter_ns = 2e8;                 /* 1/5 s: +2 stops would be 0.8 s */
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 3 && strstr(lines[0], "exp=0.0500000") && strstr(lines[1], "exp=0.2000000") && strstr(lines[2], "exp=0.5000000"),
          "bracket +-2: 1/20, 1/5, and the ceiling 1/2");
    printf(fails ? "shoot: FAILED (%d)\n" : "shoot: OK (6 rules)\n", fails);
    return fails ? 1 : 0;
}
