/* Host test of android/shading_build.h: write synthetic flat-field DNGs with
 * the real dng_write(), read them back, build a calibration, and check the
 * grids match the vignetting we put in -- so it also exercises the DNG
 * writer (pixels, CFA, levels) and shading.h's compose. Run by check-host.sh. */
/* shading.h ships several static helpers; a test TU need not call each one. */
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../shading_build.h"
#include "../dngwrite.h"
#include <stdio.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok: "); else { printf("  FAIL: "); ++fails; } printf(__VA_ARGS__); printf("\n"); } while (0)

/* A BGGR flat field W x H with a radial falloff: the centre at `centre` DN,
 * the corners at `corner_frac` of it, red a touch stronger-falling by `redk`. */
static uint16_t *make_flat(int W, int H, double centre, double corner_frac, double redk)
{
    uint16_t *px = malloc((size_t)W * H * 2);
    double cx = (W - 1) / 2.0, cy = (H - 1) / 2.0, rmax = sqrt(cx * cx + cy * cy);
    int y, x;
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x) {
            double r = sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / rmax;
            double fall = 1.0 - (1.0 - corner_frac) * r * r;        /* 1 at centre -> corner_frac */
            int isR = (y & 1) && (x & 1);                           /* BGGR: R at odd,odd */
            double v = 64 + centre * fall * (isR ? pow(fall, redk) : 1.0);
            px[(size_t)y * W + x] = (uint16_t)(v < 0 ? 0 : v > 1023 ? 1023 : v);
        }
    return px;
}

static void mat_identity(dng_matrix_t *m) { int i; m->have = 1; for (i = 0; i < 9; ++i) { m->num[i] = (i % 4 == 0) ? 1 : 0; m->den[i] = 1; } }

static int write_dng(const char *path, int W, int H, uint16_t *px)
{
    dng_params_t p;
    memset(&p, 0, sizeof p);
    p.pixels = px; p.width = W; p.height = H; p.black = 64; p.white = 1023; p.cfa = 3; /* BGGR */
    /* As the engine writes it: BlackLevel as FOUR per-phase SHORTs, which no
     * longer fit the tag's inline field and are stored at an offset. A reader
     * that takes the field as the value reads the offset (570) as the black
     * and empties every corner -- the bug the real phone exposed. */
    p.have_black4 = 1; p.black4[0] = p.black4[1] = p.black4[2] = p.black4[3] = 64;
    p.illuminant1 = 21; p.illuminant2 = 17;
    mat_identity(&p.color1); mat_identity(&p.color2);
    p.have_neutral = 1; p.neutral_num[0] = 600; p.neutral_den[0] = 1000;
    p.neutral_num[1] = 1000; p.neutral_den[1] = 1000; p.neutral_num[2] = 700; p.neutral_den[2] = 1000;
    p.software = "indraw test"; p.make = "Test"; p.model = "Flat"; p.orientation = 1;
    return dng_write(path, &p);
}

static double lcorner(const float *g, int rows, int cols)
{
    return (g[0] + g[cols - 1] + g[(rows - 1) * cols] + g[rows * cols - 1]) / 4;
}

int main(void)
{
    const int W = 512, H = 384, R = 12, C = 16;   /* small, even dimensions */
    shading_capture_t caps[4];
    shading_cal_t cal, reloaded;
    char err[128], path[64];
    int ncap = 0, i;
    uint16_t *px;

    /* two lens positions (corners 0.40 and 0.55 of centre), one light */
    struct { double hal_d, corner; } pos[2] = { { 2.3, 0.40 }, { 15.0, 0.55 } };
    for (i = 0; i < 2; ++i) {
        FILE *f;
        long n;
        uint8_t *bytes;
        px = make_flat(W, H, 500, pos[i].corner, 0.3);
        snprintf(path, sizeof path, "/tmp/flat%d.dng", i);
        CHECK(write_dng(path, W, H, px) == 0, "dng_write a flat field (lens %.1f)", pos[i].hal_d);
        free(px);
        f = fopen(path, "rb"); fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        bytes = malloc((size_t)n); if (fread(bytes, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
        CHECK(shading_read_dng(bytes, (size_t)n, R, C, pos[i].hal_d, 5000, &caps[ncap], err, sizeof err) == 0,
              "read it back (%s), cfa %s", err, caps[ncap].cfa);
        free(bytes); unlink(path);
        ++ncap;
    }
    /* the corner gain should be ~ 1/corner_frac */
    CHECK(fabs((caps[0].ge[0] + caps[0].ge[C - 1]) / 2 - 1.0 / 0.40) < 0.2,
          "lens 2.3: green corner gain %.2f (put 1/0.40 = 2.50)", (caps[0].ge[0] + caps[0].ge[C - 1]) / 2);
    CHECK(fabs((caps[1].ge[0] + caps[1].ge[C - 1]) / 2 - 1.0 / 0.55) < 0.2,
          "lens 15: green corner gain %.2f (put 1/0.55 = 1.82)", (caps[1].ge[0] + caps[1].ge[C - 1]) / 2);
    CHECK(caps[0].r[0] > caps[0].ge[0], "red falls faster than green (its corner gain is higher)");
    CHECK(caps[0].clip_fraction < 0.01 && caps[0].centre_level > 400, "no clipping, centre ~500 DN (%.0f)", caps[0].centre_level);

    CHECK(shading_build_assemble(caps, ncap, &cal, err, sizeof err) == 0, "assemble (%s): %d lens positions, %d light", err, cal.nlum, cal.ncol);
    CHECK(cal.nlum == 2 && cal.ncol == 1, "two lens positions, one light");
    CHECK(lcorner(cal.lum[0], R, C) > lcorner(cal.lum[1], R, C), "the closer-falling position has the larger corner gain");
    CHECK(cal.col_r[0][0] > 1.0, "the light's red corner factor is above 1 (red falls faster)");
    {
        int ci = (R / 2) * C + C / 2;
        CHECK(fabs(cal.col_r[0][ci] - 1.0) < 0.01 && fabs(cal.col_b[0][ci] - 1.0) < 0.01,
              "the colour factors are 1 at the centre (%.3f, %.3f): shading only, no white balance", cal.col_r[0][ci], cal.col_b[0][ci]);
    }

    /* write the file and read it with shading.h: a full round trip */
    CHECK(shading_write_file("/tmp/cal.json", &cal, "Test", "Flat", "0") > 0, "write the calibration file");
    {
        FILE *f = fopen("/tmp/cal.json", "rb");
        long n; char *text;
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        text = malloc((size_t)n + 1); if (fread(text, 1, (size_t)n, f) != (size_t)n) return 1; text[n] = 0; fclose(f);
        CHECK(shading_parse(text, &reloaded, err, sizeof err) == 0, "shading.h parses it back (%s)", err);
        CHECK(reloaded.nlum == 2 && reloaded.ncol == 1 && reloaded.rows == R, "same grid, positions and light");
        CHECK(fabs(lcorner(reloaded.lum[0], R, C) - lcorner(cal.lum[0], R, C)) < 0.01, "the luminance grid survives the round trip");
        free(text); shading_free(&reloaded); unlink("/tmp/cal.json");
    }
    /* add-a-light merge (the calibrate command's logic): a second light's
     * colour grids go into a copy of the calibration, keeping the luminance. */
    {
        shading_cal_t base, add;
        char e2[128];
        /* rebuild a fresh base and a one-position "light" capture at a warmer cct */
        shading_build_assemble(caps, ncap, &base, e2, sizeof e2);
        {
            shading_capture_t one = caps[0];           /* same grids, another light */
            one.cct = 3000;
            shading_build_assemble(&one, 1, &add, e2, sizeof e2);
        }
        {
            int ci = base.ncol;
            CHECK(ci == 1, "base has one light before the merge");
            base.col_k[ci] = add.col_k[0];
            base.col_r[ci] = malloc(sizeof(float) * R * C); base.col_g[ci] = malloc(sizeof(float) * R * C); base.col_b[ci] = malloc(sizeof(float) * R * C);
            memcpy(base.col_r[ci], add.col_r[0], sizeof(float) * R * C);
            memcpy(base.col_g[ci], add.col_g[0], sizeof(float) * R * C);
            memcpy(base.col_b[ci], add.col_b[0], sizeof(float) * R * C);
            base.ncol++;
            CHECK(base.ncol == 2 && base.nlum == 2, "after the merge: two lights, the two lens positions kept");
        }
        shading_free(&base); shading_free(&add);
    }

    /* the light check, with the levels of the JP2's real run (24 Oct):
     * per position 823/819/820, 800/800/802, 763/762/763, 710/710/710,
     * 664/663/663 -- steps of 2.8-7 % between positions */
    {
        char e3[128];
        static const double firsts[5] = { 823.3, 799.8, 763.5, 710.3, 663.9 };
        int bad = 0, p2;
        for (p2 = 1; p2 < 5; ++p2)
            bad |= shading_level_check(firsts[p2], -1, firsts[p2 - 1], e3, sizeof e3);
        bad |= shading_level_check(819.1, 823.3, -1, e3, sizeof e3);
        CHECK(!bad, "the real run passes: frames within a position, steps between positions");
        CHECK(shading_level_check(780, 823.3, -1, e3, sizeof e3) == 1, "a 5 %%+ change within a position is refused (%s)", e3);
        CHECK(shading_level_check(560, -1, 663.9, e3, sizeof e3) == 2, "a 15 %%+ jump between positions is refused (%s)", e3);
        CHECK(shading_level_check(600, -1, -1, e3, sizeof e3) == 0, "the very first frame is its own reference");
    }

    shading_free(&cal);
    for (i = 0; i < ncap; ++i) shading_capture_free(&caps[i]);
    printf(fails ? "shading_build: FAILED (%d)\n" : "shading_build: OK (23 cases)\n", fails);
    return fails ? 1 : 0;
}
