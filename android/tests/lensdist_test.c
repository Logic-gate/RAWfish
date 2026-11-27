/* Host test of android/lensdist.h: the file, the binding to the active
 * array, the corrected camera (no empty corner), the map and its inverse,
 * and the plane remap (the photos'); and, when given, a shipped
 * file (argv[1]...). Run by check-host.sh. */
#include "../lensdist.h"
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok: "); else { printf("  FAIL: "); ++fails; } printf(__VA_ARGS__); printf("\n"); } while (0)

/* The JP2's wide camera as measured (Nov 2026): strong mustache distortion. */
static const char *k_wide =
    "{\"format\": \"rawfish-ir lens distortion 1\", \"_comment\": \"a \\\"dist\\\" in a comment\",\n"
    " \"width\": 4096, \"height\": 3072, \"fx\": 1738.14, \"fy\": 1735.92, \"cx\": 2050.20, \"cy\": 1515.50,\n"
    " \"dist\": [-0.63654, 0.18024, -0.00057, 0.000059, 0.0, -0.66537, 0.20196, 0.0]}";

static double max_err_roundtrip(const ld_view_t *v)
{
    double worst = 0;
    int i, j;
    for (j = 0; j <= 10; ++j)
        for (i = 0; i <= 10; ++i) {
            double u = (v->w - 1) * i / 10.0, vv = (v->h - 1) * j / 10.0, su, sv, bu, bv, e;
            ld_src(v, u, vv, &su, &sv);
            ld_dst(v, su, sv, &bu, &bv);
            e = hypot(bu - u, bv - vv);
            if (e > worst) worst = e;
        }
    return worst;
}

/* Every border pixel of the corrected frame has picture under it. */
static int covered(const ld_view_t *v)
{
    int i;
    for (i = 0; i < v->w; i += 7) {
        double su, sv;
        ld_src(v, i, 0, &su, &sv); if (su < -0.5 || su > v->w - 0.5 || sv < -0.5 || sv > v->h - 0.5) return 0;
        ld_src(v, i, v->h - 1, &su, &sv); if (su < -0.5 || su > v->w - 0.5 || sv < -0.5 || sv > v->h - 0.5) return 0;
    }
    for (i = 0; i < v->h; i += 7) {
        double su, sv;
        ld_src(v, 0, i, &su, &sv); if (su < -0.5 || su > v->w - 0.5 || sv < -0.5 || sv > v->h - 0.5) return 0;
        ld_src(v, v->w - 1, i, &su, &sv); if (su < -0.5 || su > v->w - 0.5 || sv < -0.5 || sv > v->h - 0.5) return 0;
    }
    return 1;
}

static int check_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char text[16384], err[160];
    size_t n;
    ld_model_t m;
    ld_view_t v43, v169;
    if (!f) { CHECK(0, "%s: cannot open", path); return -1; }
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    err[0] = '\0';
    CHECK(ld_parse(text, &m, err, sizeof err) == 0, "%s parses%s", path, err);
    ld_bind(&m, m.ref_w, m.ref_h);
    CHECK(ld_view_init(&v43, &m, m.ref_w, m.ref_h, NULL) == 0 && covered(&v43),
          "%s: the full frame is corrected with no empty corner (crop %.1f %%)", path, 100 * (1 - m.fx / v43.fo));
    CHECK(ld_view_init(&v169, &m, 1920, 1080, NULL) == 0 && covered(&v169) && max_err_roundtrip(&v169) < 0.01,
          "%s: a 16:9 viewfinder too, and its inverse", path);
    return 0;
}

int main(int argc, char **argv)
{
    ld_model_t m, id;
    ld_view_t v, vz;
    char err[160];
    int i;

    printf("lensdist:\n");
    /* --- the file --- */
    err[0] = '\0';
    CHECK(ld_parse(k_wide, &m, err, sizeof err) == 0, "the wide camera's file parses%s", err);
    CHECK(m.ref_w == 4096 && m.ref_h == 3072 && fabs(m.k[0] + 0.63654) < 1e-9 && fabs(m.k[6] - 0.20196) < 1e-9,
          "its size and coefficients, in OpenCV's order");
    CHECK(m.r_fold > 2.0, "its radial curve rises well past the corners (fold at %.2f)", m.r_fold);
    CHECK(ld_parse("{\"format\": \"something else\"}", &id, err, sizeof err) != 0, "another format refused (%s)", err);
    CHECK(ld_parse("{\"format\": \"rawfish-ir lens distortion 2\", \"width\": 4, \"height\": 3}", &id, err, sizeof err) != 0,
          "a later version refused (%s)", err);
    CHECK(ld_parse("{\"format\": \"rawfish-ir lens distortion 1\", \"width\": 4096, \"height\": 3072, \"fx\": 1, \"fy\": 1, "
                   "\"cx\": 1, \"cy\": 1, \"dist\": [1, 2, 3]}", &id, err, sizeof err) != 0,
          "three coefficients refused (%s)", err);
    CHECK(ld_parse("{\"format\": \"rawfish-ir lens distortion 1\", \"width\": 4096, \"height\": 3072, \"fx\": 1000, \"fy\": 1000, "
                   "\"cx\": 2047.5, \"cy\": 1535.5, \"dist\": [0, 0, 0, 0]}", &id, err, sizeof err) == 0,
          "four coefficients taken");

    /* --- binding: an active array twice the file's frame --- */
    {
        ld_model_t b = m;
        ld_bind(&b, 8192, 6144);
        CHECK(fabs(b.fx - 2 * m.ref_fx) < 1e-9 && fabs(b.cx - 2 * (m.ref_cx + 0.5)) < 1e-9,
              "bound to an array of twice the size: focal and centre doubled (centre %.2f)", b.cx);
        ld_bind(&b, 4096, 3200);                 /* a taller array: the file's frame is its middle */
        CHECK(fabs(b.cy - (64 + m.ref_cy + 0.5)) < 1e-9 && fabs(b.fx - m.ref_fx) < 1e-9,
              "a taller array: the frame in its middle (centre y %.2f)", b.cy);
    }

    /* --- no distortion: the identity --- */
    ld_bind(&id, 0, 0);
    CHECK(ld_view_init(&v, &id, 4096, 3072, NULL) == 0 && fabs(v.fo - 1000) < 0.01, "no distortion: the corrected camera is the lens' (%.4f)", v.fo);
    {
        double su, sv;
        ld_src(&v, 100, 2000, &su, &sv);
        CHECK(fabs(su - 100) < 0.01 && fabs(sv - 2000) < 0.01, "no distortion: every pixel stays (%.6f, %.6f)", su, sv);
    }

    /* --- the wide lens --- */
    ld_bind(&m, 4096, 3072);
    CHECK(ld_view_init(&v, &m, 4096, 3072, NULL) == 0, "the wide lens: a 4:3 photo can be corrected");
    CHECK(covered(&v), "no empty corner (corrected focal %.1f against %.1f)", v.fo, m.fx);
    {   /* and no more cut than needed: somewhere the border touches the frame's */
        double margin = 1e9, su, sv;
        for (i = 0; i < 4096; i += 4) {
            ld_src(&v, i, 0, &su, &sv); if (sv + 0.5 < margin) margin = sv + 0.5;
            ld_src(&v, i, 3071, &su, &sv); if (3071.5 - sv < margin) margin = 3071.5 - sv;
        }
        for (i = 0; i < 3072; i += 4) {
            ld_src(&v, 0, i, &su, &sv); if (su + 0.5 < margin) margin = su + 0.5;
            ld_src(&v, 4095, i, &su, &sv); if (4095.5 - su < margin) margin = 4095.5 - su;
        }
        CHECK(margin > -0.01 && margin < 1.0, "and no more cut than needed: the border's closest point %.3f px inside", margin);
    }
    CHECK(max_err_roundtrip(&v) < 0.01, "the inverse finds every point back (%.5f px)", max_err_roundtrip(&v));
    {   /* the same correction at another size: the 12 MP and the 3 MP of one 4:3 frame */
        ld_view_t s;
        double a, b2, c, d;
        ld_view_init(&s, &m, 2048, 1536, NULL);
        ld_src(&v, 1000, 700, &a, &b2);
        ld_src(&s, 499.75, 349.75, &c, &d);       /* the same point, half the pixels */
        CHECK(fabs((c + 0.5) * 2 - (a + 0.5)) < 1e-6 && fabs((d + 0.5) * 2 - (b2 + 0.5)) < 1e-6,
              "a smaller photo of the same frame: the same correction, scaled");
    }
    {   /* 16:9: the middle band; the photo and its viewfinder agree */
        ld_view_t p, f;
        double a, b2, c, d;
        CHECK(ld_view_init(&p, &m, 4096, 2304, NULL) == 0 && covered(&p) && p.fy0 == 384 && p.fh == 2304,
              "a 16:9 photo: the band 384..2688, no empty corner");
        ld_view_init(&f, &m, 1920, 1080, NULL);
        ld_src(&p, 2047.5 + 1000, 1151.5, &a, &b2);
        ld_src(&f, 959.5 + 1000 * 1920.0 / 4096, 539.5, &c, &d);
        CHECK(fabs((a + 0.5) / 4096 - (c + 0.5) / 1920) < 1e-9 && fabs((b2 + 0.5) / 2304 - (d + 0.5) / 1080) < 1e-9,
              "its viewfinder shows the same corrected picture");
        CHECK(p.fo <= v.fo + 1e-9, "a 16:9 photo needs no more crop than a 4:3 one (%.1f <= %.1f)", p.fo, v.fo);
    }
    {   /* zoomed: the middle quarter, the same corrected camera */
        double fld[4] = { 1024, 768, 2048, 1536 }, a, b2, c, d;
        CHECK(ld_view_init(&vz, &m, 2048, 1536, fld) == 0 && fabs(vz.fo - v.fo) < 1e-9, "zoomed x2: the same corrected camera");
        ld_src(&v, 2047.5, 1535.5, &a, &b2);
        ld_src(&vz, 1023.5, 767.5, &c, &d);
        CHECK(fabs(a - 1024 - c) < 1e-6 && fabs(b2 - 768 - d) < 1e-6, "zoomed x2: the middle is the full frame's middle");
    }

    /* --- a model that folds inside the frame: refused --- */
    {
        ld_model_t bad;
        ld_view_t bv;
        ld_parse("{\"format\": \"rawfish-ir lens distortion 1\", \"width\": 4096, \"height\": 3072, \"fx\": 1700, \"fy\": 1700, "
                 "\"cx\": 2047.5, \"cy\": 1535.5, \"dist\": [-0.9, 0.0, 0, 0, 0]}", &bad, err, sizeof err);
        ld_bind(&bad, 0, 0);
        CHECK(bad.r_fold < 1.0 && ld_view_init(&bv, &bad, 4096, 3072, NULL) != 0,
              "a model folding at r %.2f, inside the frame: refused (it would crop most of it)", bad.r_fold);
    }

    /* --- the plane remap (the photos') --- */
    {
        const int w = 64, h = 48;
        static uint8_t pl[64 * 48], po[64 * 48];
        ld_view_t s;
        ld_grid_t g;
        float rx[64], ry[64];
        int same, k;
        for (i = 0; i < w * h; ++i) pl[i] = (uint8_t)((i % w) * 3 + (i / w) * 2);
        ld_view_init(&s, &id, w, h, NULL);
        CHECK(ld_grid_build(&s, 16, &g) == 0, "the grid builds");
        for (k = 0; k < 2; ++k) {
            same = 1;
            ld_remap_plane(&g, pl, w, h, po, w, 0, h, 0, k, rx, ry);
            for (i = 0; i < w * h; ++i) if (po[i] != pl[i]) same = 0;
            CHECK(same, "no distortion: a plane unchanged (%s)", k ? "cubic" : "bilinear");
        }
        ld_grid_free(&g);
        /* the wide lens: each pixel of a ramp (x in one plane, y in another)
         * shows the lens' point that ld_src gives, through the coarse grid */
        {
            static uint8_t px[200 * 150], py[200 * 150], qx[200 * 150], qy[200 * 150];
            ld_view_t vf;
            float rx2[64], ry2[64];                    /* the grid's columns: 200 / 8 + 2 */
            double su, sv;
            int x, y, ok = 1;
            for (y = 0; y < 150; ++y) for (x = 0; x < 200; ++x) { px[y * 200 + x] = (uint8_t)x; py[y * 200 + x] = (uint8_t)y; }
            ld_view_init(&vf, &m, 200, 150, NULL);
            ld_grid_build(&vf, 8, &g);                 /* 8 of 200 px: about the photos' 16 of 4096 and more */
            ld_remap_plane(&g, px, 200, 150, qx, 200, 0, 150, 0, 0, rx2, ry2);
            ld_remap_plane(&g, py, 200, 150, qy, 200, 0, 150, 0, 0, rx2, ry2);
            for (y = 5; y < 150; y += 37) for (x = 3; x < 200; x += 41) {
                ld_src(&vf, x, y, &su, &sv);
                if (fabs(qx[y * 200 + x] - su) > 0.6 || fabs(qy[y * 200 + x] - sv) > 0.6) ok = 0;
            }
            CHECK(ok, "the wide lens: each pixel shows the lens' point (within the rounding)");
            ld_grid_free(&g);
        }
    }

    for (i = 1; i < argc; ++i) check_file(argv[i]);
    printf(fails ? "lensdist: %d FAILED\n" : "lensdist: all passed\n", fails);
    return fails != 0;
}
