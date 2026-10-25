/* yuv_test.c -- yuv420_to_rgb (android/yuv.h) is pixel-exact with the plain
 * per-pixel conversion it replaced (bounds checked per pixel), over random
 * planes with every stride layout Camera2 uses (planar: pixel stride 1;
 * semi-planar NV12/NV21: pixel stride 2, interleaved U/V; padded rows), odd
 * widths included; and a truncated plane leaves rows black.
 * Host build: gcc -std=c11 -Wall -Wextra -Werror -o yuv_test android/tests/yuv_test.c */
#include "../yuv.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(cond, ...) do { if (cond) { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } \
                              else { ++fails; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static inline uint8_t clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }
/* The reference: the per-pixel conversion of indraw.c up to 0.47.60. */
static void reference(const uint8_t *py, int ly, int ry, int xy, const uint8_t *pu, int lu, int ru, int xu,
                      const uint8_t *pv, int lv, int rv, int xv, int w, int h, uint8_t *rgb)
{
    int x, y;
    for (y = 0; y < h; ++y) {
        uint8_t *row = rgb + (size_t)y * w * 3;
        for (x = 0; x < w; ++x) {
            int yi = y * ry + x * xy, ci = (y / 2) * ru + (x / 2) * xu, vi = (y / 2) * rv + (x / 2) * xv, Y, U, V;
            if (yi < 0 || yi >= ly || ci < 0 || ci >= lu || vi < 0 || vi >= lv) { row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = 0; continue; }
            Y = py[yi]; U = pu[ci] - 128; V = pv[vi] - 128;
            row[x * 3] = clip8(Y + ((91881 * V) >> 16));
            row[x * 3 + 1] = clip8(Y - ((22554 * U + 46802 * V) >> 16));
            row[x * 3 + 2] = clip8(Y + ((116130 * U) >> 16));
        }
    }
}

static uint32_t rng = 12345;
static int rnd(int n) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 8) % (uint32_t)n); }

int main(void)
{
    int t;
    for (t = 0; t < 40; ++t) {
        int w = 2 + rnd(64), h = 2 + rnd(48);
        int semi = rnd(2);                                /* NV12/NV21-like: U and V interleaved */
        int xy = 1, xu = semi ? 2 : 1, xv = xu;
        int ry = w + rnd(16), ru = (semi ? w : (w + 1) / 2) + rnd(16), rv = semi ? ru : (w + 1) / 2 + rnd(16);
        int ly = ry * h, lu = ru * ((h + 1) / 2), lv = rv * ((h + 1) / 2);
        uint8_t *py = malloc((size_t)ly), *pu = malloc((size_t)lu + 1), *pv, *a = malloc((size_t)w * h * 3), *b = malloc((size_t)w * h * 3);
        int i;
        for (i = 0; i < ly; ++i) py[i] = (uint8_t)rnd(256);
        for (i = 0; i < lu + 1; ++i) pu[i] = (uint8_t)rnd(256);
        pv = semi ? pu + 1 : malloc((size_t)lv);          /* semi-planar: V one byte after U, same buffer */
        if (!semi) for (i = 0; i < lv; ++i) pv[i] = (uint8_t)rnd(256);
        if (semi) lv = lu;                                /* V's plane: the same length, from its start */
        reference(py, ly, ry, xy, pu, lu, ru, xu, pv, lv, rv, xv, w, h, a);
        yuv420_to_rgb(py, ly, ry, xy, pu, lu, ru, xu, pv, lv, rv, xv, w, h, b);
        CHECK(memcmp(a, b, (size_t)w * h * 3) == 0, "%dx%d %s ry %d ru %d: pixel-exact", w, h, semi ? "semi-planar" : "planar", ry, ru);
        /* a luma plane too short by a few rows: those rows black in both */
        {
            int short_ly = ly - ry * 2;
            reference(py, short_ly, ry, xy, pu, lu, ru, xu, pv, lv, rv, xv, w, h, a);
            yuv420_to_rgb(py, short_ly, ry, xy, pu, lu, ru, xu, pv, lv, rv, xv, w, h, b);
            CHECK(memcmp(a, b, (size_t)w * h * 3) == 0, "   luma plane 2 rows short: the last rows black, both alike");
        }
        free(py); free(pu); if (!semi) free(pv); free(a); free(b);
    }
    printf(fails ? "yuv: FAILED (%d)\n" : "yuv: OK (80 cases)\n", fails);
    return fails ? 1 : 0;
}
