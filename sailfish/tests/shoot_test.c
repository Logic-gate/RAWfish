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
    fs.held = 1;                                              /* manual, lens held at a touch's search */
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && strstr(lines[0], "focus=") == NULL, "manual lens held at a search: the photo sends no distance (keeps the lens)");
    fs.held = 0;
    /* 3. raw: no quality, no processing. */
    r.raw = 1;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "raw photo focus=0.500m exp=0.0166667 iso=400"), "raw: no q, no modes");
    /* 3b. yuv: our command, the frames and filters, the pair and the distance. */
    r.raw = 0; r.yuv = 1; r.frames = 4; r.colour = 2; r.median = 1; r.sharpen = 2;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "yuv photo n=4 colour=2 median=1 sharpen=2 q=95 focus=0.500m exp=0.0166667 iso=400"), "yuv: frames, filters, quality, focus, pair");
    r.luma_set = 1; r.luma = 0;                   /* an explicit 0 is sent (it overrides a device default) */
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "yuv photo n=4 colour=2 median=1 luma=0 sharpen=2 q=95 focus=0.500m exp=0.0166667 iso=400"), "yuv: an explicit luma=0");
    r.luma = 2;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "yuv photo n=4 colour=2 median=1 luma=2 sharpen=2 q=95 focus=0.500m exp=0.0166667 iso=400"), "yuv: luma=2");
    r.luma_set = 0;
    r.dng = 1;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && strstr(lines[0], "q=95 dng=1 focus=") != NULL, "yuv with a DNG: dng=1");
    /* 3b''. 8 frames, the tile merge; tiles with one frame is the classic */
    r.dng = 0; r.frames = 8; r.nr_tiles = 1;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strncmp(lines[0], "yuv photo n=8 nr=tiles colour=2 ", 32), "yuv: 8 frames by tiles");
    r.frames = 4; r.nr_tiles = 0;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && strstr(lines[0], "n=4 colour=") != NULL && !strstr(lines[0], "nr="), "yuv classic: no nr=");
    r.frames = 1; r.nr_tiles = 1;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && strstr(lines[0], "n=1 colour=") != NULL && !strstr(lines[0], "nr="), "yuv tiles of one frame: the classic");
    r.frames = 4; r.nr_tiles = 0;
    r.yuv = 0; r.dng = 0;
    /* 3b'. a yuv request WITH a bracket is not one photo: it becomes the
     *      camera's JPEG, a burst (x3) or three exposures (EV). */
    r.yuv = 1; r.bracket = 0;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strncmp(lines[0], "burst ", 6), "yuv with x3: a burst of the camera's JPEG, not one yuv photo");
    r.bracket = 1;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 3 && !strncmp(lines[0], "jpeg ", 5) && !strncmp(lines[2], "jpeg ", 5), "yuv with an EV bracket: three jpeg exposures");
    r.yuv = 0; r.bracket = -1;
    /* 3c'. snap from our YUV: light=1, and no DNG even if asked. */
    r.yuv = 1; r.snap = 1; r.dng = 1; r.bracket = -1;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strncmp(lines[0], "yuv ", 4) && strstr(lines[0], " light=1") && !strstr(lines[0], "dng=1"), "snap from yuv: light=1, no DNG");
    r.yuv = 0; r.snap = 0; r.dng = 0;
    /* 3c. snap: the HAL JPEG with its label for the Exif. */
    r.snap = 1;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strncmp(lines[0], "jpeg ", 5) && strstr(lines[0], " label=snap") != NULL, "snap: a jpeg line with label=snap");
    r.snap = 0;
    /* 4. a stacked shutter with a pair: a stack with the denoise factor. */
    xs.shutter_ns = 2e9; r.denoise = 2;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "stack photo film=2.000 iso=400 stk_denoise=2"), "Σ2s: a stack, denoise x2");
    /* 4b. Σ with a bracket that reached us anyway (the interface sends
     *     "off" with Σ): still one stack, never three (the stack comes first). */
    r.bracket = 2; r.raw = 1;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strncmp(lines[0], "stack ", 6), "Σ with a bracket: one stack, the bracket ignored");
    r.bracket = -1; r.raw = 0;
    /* 5. bracket x3 in JPEG: one burst with the pair; in raw three shots. */
    xs.shutter_ns = 16666667; r.denoise = 1; r.bracket = 0; r.raw = 0; fs.mode = CTL_FOCUS_ONCE;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "burst photo n=3 q=95 off_noise fast_edge exp=0.0166667 iso=400"), "x3 JPEG: a burst, the pair along");
    r.raw = 1;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 3 && !strcmp(lines[0], lines[2]) && !strncmp(lines[1], "raw photo exp=0.0166667", 23), "x3 raw: three shots at the same exposure");
    /* 5b. x5 / x7: a burst of five or seven in JPEG, as many shots in raw;
     * any other count is three. */
    r.raw = 0; r.bracket_n = 5;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && !strcmp(lines[0], "burst photo n=5 q=95 off_noise fast_edge exp=0.0166667 iso=400"), "x5 JPEG: a burst of five");
    r.raw = 1; r.bracket_n = 7;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 7 && !strcmp(lines[0], lines[6]) && !strncmp(lines[3], "raw photo exp=0.0166667", 23), "x7 raw: seven shots at the same exposure");
    r.raw = 0; r.bracket_n = 4;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 1 && strstr(lines[0], "burst photo n=3 "), "a count other than 3, 5 or 7: three");
    /* 6. bracket 2 stops: three real exposures, clamped at the ceiling --
     * whatever the series' count says. */
    r.raw = 0; r.bracket = 2; r.bracket_n = 7; xs.shutter_ns = 2e8;   /* 1/5 s: +2 stops would be 0.8 s */
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 3 && strstr(lines[0], "exp=0.0500000") && strstr(lines[1], "exp=0.2000000") && strstr(lines[2], "exp=0.5000000"),
          "bracket +-2: 1/20, 1/5, and the ceiling 1/2");
    CHECK(strstr(lines[0], " ms=1") && strstr(lines[1], " ms=1") && strstr(lines[2], " ms=1"),
          "bracket: the milliseconds always in its names (ms=1)");
    /* 6b. JPG + DNG (dng with a bracket in JPEG): the camera's JPEG series
     * first, then the same series in raw -- x7: the burst and seven raw
     * shots (8 lines, the most); EV steps: three jpeg then three raw. */
    r.dng = 1; r.bracket = 0; r.bracket_n = 7; xs.shutter_ns = 16666667;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 8 && !strncmp(lines[0], "burst photo n=7 ", 16) && !strncmp(lines[1], "raw photo exp=0.0166667", 23)
          && !strcmp(lines[1], lines[7]), "x7 JPG + DNG: the JPEG burst, then seven raw shots");
    r.bracket = 2; xs.shutter_ns = 2e8;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 6 && !strncmp(lines[0], "jpeg ", 5) && !strncmp(lines[2], "jpeg ", 5) && !strncmp(lines[3], "raw ", 4)
          && strstr(lines[3], "exp=0.0500000") && strstr(lines[5], "exp=0.5000000") && !strstr(lines[3], "q="),
          "+-2 JPG + DNG: three jpeg, then three raw at the same exposures, without the JPEG's fields");
    r.raw = 1; r.bracket = 0; r.bracket_n = 3;
    n = ctl_shoot_lines(&r, &xs, 5e8, 1e6, &fs, lines);
    CHECK(n == 3 && !strncmp(lines[0], "raw ", 4), "a bracket in raw: dng changes nothing");
    r.raw = 0; r.dng = 0; r.bracket = -1;
    printf(fails ? "shoot: FAILED (%d)\n" : "shoot: OK (11 rules)\n", fails);
    return fails ? 1 : 0;
}
