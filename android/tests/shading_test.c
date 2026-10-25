/* Host test of android/shading.h with the JP2's own calibration file
 * (android/data/lens-shading/): parsing, the interpolation in lens position
 * and in mireds, the user's dosing. Run by check-host.sh. */
#include "../shading.h"
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok: "); else { printf("  FAIL: "); ++fails; } printf(__VA_ARGS__); printf("\n"); } while (0)
static double corner(const float *g, int rows, int cols, int ch)
{
    int pts[4] = { 0, cols - 1, (rows - 1) * cols, rows * cols - 1 }, i;
    double s = 0;
    for (i = 0; i < 4; ++i) s += g[pts[i] * 4 + ch];
    return s / 4;
}
static double lcorner(const float *g, int rows, int cols)
{
    return (g[0] + g[cols - 1] + g[(rows - 1) * cols] + g[rows * cols - 1]) / 4;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "android/data/lens-shading/shading_jolla-jp2601_cam0_4096x3072.json";
    FILE *f = fopen(path, "rb");
    char *text, err[128];
    long len;
    shading_cal_t c;
    float out[64 * 64 * 4];
    double inf_lum, near_lum, mid_lum;
    if (!f) { printf("shading: cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    text = malloc((size_t)len + 1);
    if (fread(text, 1, (size_t)len, f) != (size_t)len) return 1;
    text[len] = '\0'; fclose(f);

    CHECK(shading_parse(text, &c, err, sizeof err) == 0, "the JP2 file parses (%s)", err);
    CHECK(c.rows == 12 && c.cols == 16 && c.nlum == 6 && c.ncol == 2 && !strcmp(c.cfa, "BGGR") && c.width == 4096,
          "12 x 16, 6 lens positions, 2 lights, BGGR, 4096 wide");
    inf_lum = lcorner(c.lum[0], c.rows, c.cols);
    near_lum = lcorner(c.lum[c.nlum - 1], c.rows, c.cols);
    CHECK(inf_lum > 4.0 && inf_lum < 4.5 && near_lum > 3.1 && near_lum < 3.6,
          "luminance corners: %.2f at infinity, %.2f at the closest (measured 4.28 / 3.35)", inf_lum, near_lum);

    /* no dosing (balance 0, strength 100, no clamp): the composition itself */
    shading_compose(&c, c.lum_d[0], c.col_k[1], 0, 100, 100, out);           /* infinity, daylight */
    CHECK(fabs(corner(out, c.rows, c.cols, 1) - inf_lum) < 0.01, "at a measured position, in daylight: green = the luminance grid");
    CHECK(fabs(corner(out, c.rows, c.cols, 0) / corner(out, c.rows, c.cols, 1) - 1.13) < 0.03,
          "daylight: red/green in the corners %.3f (measured 1.13)", corner(out, c.rows, c.cols, 0) / corner(out, c.rows, c.cols, 1));
    shading_compose(&c, c.lum_d[0], c.col_k[0], 0, 100, 100, out);           /* infinity, tungsten */
    CHECK(fabs(corner(out, c.rows, c.cols, 0) / inf_lum - 1.26) < 0.03, "tungsten: red factor %.3f (measured 1.26)", corner(out, c.rows, c.cols, 0) / inf_lum);
    /* halfway between two lens positions: the average */
    shading_compose(&c, (c.lum_d[3] + c.lum_d[4]) / 2, c.col_k[1], 0, 100, 100, out);
    mid_lum = (lcorner(c.lum[3], c.rows, c.cols) + lcorner(c.lum[4], c.rows, c.cols)) / 2;
    CHECK(fabs(corner(out, c.rows, c.cols, 1) - mid_lum) < 0.01, "between two lens positions: linear (%.3f)", mid_lum);
    /* beyond the ends: the nearest */
    shading_compose(&c, 40.0, c.col_k[1], 0, 100, 100, out);
    CHECK(fabs(corner(out, c.rows, c.cols, 1) - near_lum) < 0.01, "past the closest position: the closest one");
    /* colour halfway in mireds */
    {
        double k = 2.0 / (1.0 / c.col_k[0] + 1.0 / c.col_k[1]), r0, r1, rm;
        shading_compose(&c, c.lum_d[0], c.col_k[0], 0, 100, 100, out); r0 = corner(out, c.rows, c.cols, 0);
        shading_compose(&c, c.lum_d[0], c.col_k[1], 0, 100, 100, out); r1 = corner(out, c.rows, c.cols, 0);
        shading_compose(&c, c.lum_d[0], k, 0, 100, 100, out); rm = corner(out, c.rows, c.cols, 0);
        CHECK(fabs(rm - (r0 + r1) / 2) < 0.01, "halfway in mireds (%.0f K): the colour halfway", k);
    }
    /* dosing */
    shading_compose(&c, c.lum_d[0], c.col_k[1], 0, 100, 5, out);
    CHECK(corner(out, c.rows, c.cols, 0) <= 5.0001, "max_gain 5 caps the red corners");
    shading_compose(&c, c.lum_d[0], c.col_k[1], 100, 100, 5, out);
    {
        int i; double mx = 0;
        for (i = 0; i < c.rows * c.cols * 4; ++i) if (out[i] > mx) mx = out[i];
        CHECK(fabs(mx - 1.0) < 1e-4, "balance 100: the largest gain is 1 (everything else darkened)");
    }
    shading_compose(&c, c.lum_d[0], c.col_k[1], 50, 0, 5, out);
    {
        int i, flat = 1;
        for (i = 0; i < c.rows * c.cols * 4; ++i) if (fabs(out[i] - 1.0) > 1e-6) flat = 0;
        CHECK(flat, "strength 0: no correction at all");
    }
    shading_free(&c);
    CHECK(shading_parse("{\"grid\":{\"rows\":2,\"cols\":2}}", &c, err, sizeof err) != 0, "a broken file is refused (%s)", err);
    {   /* two luminance entries with the same lens value: refused */
        const char *dup = "{\"width\":4,\"height\":4,\"cfa\":\"BGGR\",\"grid\":{\"rows\":2,\"cols\":2},"
            "\"luminance\":[{\"hal_diopters\":2.0,\"gain\":[1,1,1,1]},{\"hal_diopters\":2.0,\"gain\":[1,2,1,1]}],"
            "\"colour\":[{\"cct\":5000,\"r\":[1,1,1,1],\"g\":[1,1,1,1],\"b\":[1,1,1,1]}]}";
        int rc = shading_parse(dup, &c, err, sizeof err);
        CHECK(rc != 0 && strstr(err, "same hal_diopters"), "two entries with the same hal_diopters are refused (%s)", err);
        if (rc == 0) shading_free(&c);
    }
    {   /* a "_comment" that names a key (escaped quotes) is not the key */
        const char *cm = "{\"_comment\":\"the \\\"width\\\": 9999 here is text\",\"width\":4,\"height\":4,\"cfa\":\"BGGR\",\"grid\":{\"rows\":2,\"cols\":2},"
            "\"luminance\":[{\"hal_diopters\":2.0,\"gain\":[1,1,1,1]}],"
            "\"colour\":[{\"cct\":5000,\"r\":[1,1,1,1],\"g\":[1,1,1,1],\"b\":[1,1,1,1]}]}";
        int rc;
        err[0] = '\0';
        rc = shading_parse(cm, &c, err, sizeof err);
        CHECK(rc == 0 && c.width == 4, "a key named inside a _comment string is skipped (width %d%s%s)", c.width, err[0] ? ": " : "", err);
        if (rc == 0) shading_free(&c);
    }
    printf(fails ? "shading: FAILED (%d)\n" : "shading: OK (15 cases)\n", fails);
    free(text);
    return fails ? 1 : 0;
}
