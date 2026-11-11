/* Host test of yuvproc: synthetic frames (a gradient with colour edges,
 * Gaussian noise, one moving square), four frames. Checks: the alignment
 * finds the shift; merging four frames lowers the noise about as the
 * square root of four says and leaves the moving square to the first
 * frame; the guided filter lowers the colour noise and keeps a colour edge. */
#include "../yuvproc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("ok: " __VA_ARGS__); else { printf("FAIL: " __VA_ARGS__); fails++; } printf("\n"); } while (0)
static float gauss(unsigned *s) { double u = (rand_r(s) + 1.0) / ((double)RAND_MAX + 2.0), v = (rand_r(s) + 1.0) / ((double)RAND_MAX + 2.0); return (float)(sqrt(-2 * log(u)) * cos(6.2831853 * v)); }
static ir_yuv_t make(int w, int h, int dy, int dx, int sqx, float sigma, unsigned seed)
{
    ir_yuv_t f; memset(&f, 0, sizeof f); f.w = w; f.h = h;
    f.y = malloc((size_t)w * h); f.u = malloc((size_t)w / 2 * (h / 2)); f.v = malloc((size_t)w / 2 * (h / 2));
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        int sy = y - dy, sx = x - dx;                   /* the scene shifted by (dy,dx) */
        unsigned tex = (unsigned)((sx >> 3) * 2654435761u) ^ (unsigned)((sy >> 3) * 40503u);   /* a fixed texture of the scene, 8-px cells */
        float base = 60 + 120.0f * sx / w + 10 * sinf(sy * 0.05f) + (float)((tex >> 8) % 41) - 20;
        if (sx > sqx && sx < sqx + 120 && sy > 300 && sy < 420) base = 200;   /* the moving square */
        float v = base + sigma * gauss(&seed);
        f.y[(size_t)y * w + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    for (int y = 0; y < h / 2; ++y) for (int x = 0; x < w / 2; ++x) {
        int sx = 2 * x - dx;
        float cu = sx < w / 2 ? 100 : 160, cv = 128;      /* a vertical colour edge at mid-width */
        float nu = cu + sigma * 0.7f * gauss(&seed), nv = cv + sigma * 0.7f * gauss(&seed);
        f.u[(size_t)y * (w / 2) + x] = (uint8_t)(nu < 0 ? 0 : nu > 255 ? 255 : nu);
        f.v[(size_t)y * (w / 2) + x] = (uint8_t)(nv < 0 ? 0 : nv > 255 ? 255 : nv);
    }
    return f;
}
static float noise_in(const uint8_t *p, int w, int x0, int y0, int x1, int y1)   /* high-pass std in a box */
{
    double s = 0, s2 = 0; long n = 0;
    for (int y = y0 + 1; y < y1 - 1; ++y) for (int x = x0 + 1; x < x1 - 1; ++x) {
        float d = p[(size_t)y * w + x] - (p[(size_t)y * w + x - 1] + p[(size_t)y * w + x + 1] + p[(size_t)(y - 1) * w + x] + p[(size_t)(y + 1) * w + x]) * 0.25f;
        s += d; s2 += d * d; n++;
    }
    return (float)sqrt(s2 / n - (s / n) * (s / n));
}
int main(void)
{
    const int W = 1024, H = 768; const float SIG = 6.0f;
    ir_yuv_t fr[4], out; yuv_denoise_report_t rep;
    fr[0] = make(W, H, 0, 0, 500, SIG, 1); fr[1] = make(W, H, 3, -5, 560, SIG, 2);
    fr[2] = make(W, H, -2, 4, 620, SIG, 3); fr[3] = make(W, H, 6, 2, 680, SIG, 4);
    float n1 = noise_in(fr[0].y, W, 100, 100, 400, 250);
    CHECK(yuv_denoise(fr, 4, 0, 0, &out, &rep) == 0, "merge of 4 frames runs (%.0f ms merge, %.0f ms align)", rep.ms_merge, rep.ms_align);
    /* the reported shift is the one to APPLY to the frame (minus the scene's motion) */
    CHECK(rep.shift_dy[1] == -3 && rep.shift_dx[1] == 5 && rep.shift_dy[3] == -6 && rep.shift_dx[3] == -2, "shifts found: (%d,%d) (%d,%d) (%d,%d)", rep.shift_dy[1], rep.shift_dx[1], rep.shift_dy[2], rep.shift_dx[2], rep.shift_dy[3], rep.shift_dx[3]);
    CHECK(fabsf(rep.sigma - SIG) < 1.5f, "noise measured %.1f (true %.1f)", rep.sigma, SIG);
    float n4 = noise_in(out.y, W, 100, 100, 400, 250);
    CHECK(n4 < n1 * 0.62f, "flat area noise %.2f -> %.2f (4 frames: expected about /2)", n1, n4);
    CHECK(rep.merged_pct[1] > 90 && rep.merged_pct[3] > 85, "merged share %.0f%% %.0f%% %.0f%%", rep.merged_pct[1], rep.merged_pct[2], rep.merged_pct[3]);
    {   /* Enlarged grain: frames whose grain spans 2x2 pixels (as the JP2's 50
         * MP night frames, a 12.5 MP image upscaled). The one-frame estimate
         * reads low; the merge must still take the frames. (On real JP2 frames
         * 0.3.3 kept a third to a half; on this milder synthetic grain, 86 %.) */
        ir_yuv_t big[4], o4; yuv_denoise_report_t rb; int k2, x2, y2;
        for (k2 = 0; k2 < 4; ++k2) {
            ir_yuv_t sm = make(W / 2, H / 2, 0, 0, -1, SIG, 77u + (unsigned)k2);
            big[k2] = sm; big[k2].w = W; big[k2].h = H;
            big[k2].y = malloc((size_t)W * H); big[k2].u = malloc((size_t)W * H / 4); big[k2].v = malloc((size_t)W * H / 4);
            for (y2 = 0; y2 < H; ++y2) for (x2 = 0; x2 < W; ++x2) {   /* bilinear 2x, as an ISP enlarges */
                float sy = (y2 + 0.5f) / 2 - 0.5f, sx = (x2 + 0.5f) / 2 - 0.5f, fy, fx;
                int y0 = sy < 0 ? 0 : (int)sy, x0 = sx < 0 ? 0 : (int)sx, y1 = y0 + 1 < H / 2 ? y0 + 1 : H / 2 - 1, x1 = x0 + 1 < W / 2 ? x0 + 1 : W / 2 - 1;
                const uint8_t *q = sm.y; int w2 = W / 2;
                fy = sy - y0; fx = sx - x0; if (fy < 0) fy = 0; if (fx < 0) fx = 0;
                big[k2].y[(size_t)y2 * W + x2] = (uint8_t)((q[y0 * w2 + x0] * (1 - fx) + q[y0 * w2 + x1] * fx) * (1 - fy)
                                                          + (q[y1 * w2 + x0] * (1 - fx) + q[y1 * w2 + x1] * fx) * fy + 0.5f);
            }
            for (y2 = 0; y2 < H / 2; ++y2) for (x2 = 0; x2 < W / 2; ++x2) {
                big[k2].u[(size_t)y2 * (W / 2) + x2] = sm.u[(size_t)(y2 / 2) * (W / 4) + x2 / 2];
                big[k2].v[(size_t)y2 * (W / 2) + x2] = sm.v[(size_t)(y2 / 2) * (W / 4) + x2 / 2];
            }
            free(sm.y); free(sm.u); free(sm.v);
        }
        CHECK(yuv_denoise(big, 4, 0, 0, &o4, &rb) == 0 && rb.merged_pct[1] > 95 && rb.merged_pct[3] > 95,
              "enlarged grain: merged %.0f%% %.0f%% %.0f%% (one-frame noise %.2f, between frames %.2f, used %.2f)",
              rb.merged_pct[1], rb.merged_pct[2], rb.merged_pct[3], rb.sigma, rb.sigma_pair, rb.sigma_merge);
        free(o4.y); free(o4.u); free(o4.v);
        {   /* The detail scale: on the same enlarged image, sharpening at scale 2
             * (filters 2 pixels apart) must bring out more of the image's 2-pixel
             * detail than at scale 1, which looks too finely. Measured as the
             * mean |pixel - mean of the 3x3 two pixels apart| on the result. */
            ir_yuv_t a1 = big[0], a2 = big[0]; double e1 = 0, e2 = 0; int sc;
            for (sc = 1; sc <= 2; ++sc) {
                ir_yuv_t *t = sc == 1 ? &a1 : &a2; double *e = sc == 1 ? &e1 : &e2;
                t->y = malloc((size_t)W * H); memcpy(t->y, big[0].y, (size_t)W * H);
                yuv_set_detail_scale(sc);
                yuv_sharpen(t, 3, 1.0f);
                for (y2 = 4; y2 < H - 4; y2 += 3) for (x2 = 4; x2 < W - 4; x2 += 3) {
                    const uint8_t *q = t->y + (size_t)y2 * W + x2; int sum = 0, dy, dx;
                    for (dy = -2; dy <= 2; dy += 2) for (dx = -2; dx <= 2; dx += 2) sum += q[dy * W + dx];
                    *e += fabs(q[0] - sum / 9.0);
                }
                free(t->y);
            }
            yuv_set_detail_scale(1);
            CHECK(e2 > e1 * 1.15, "detail scale 2 sharpens an enlarged image more than scale 1 (%.0f against %.0f)", e2, e1);
        }
        for (k2 = 0; k2 < 4; ++k2) { free(big[k2].y); free(big[k2].u); free(big[k2].v); }
    }
    {   /* the moving square: the output keeps frame 1's square (at 500..620), not a ghost of the others */
        double inside = 0, ghost = 0; int c1 = 0, c2 = 0;
        for (int y = 320; y < 400; ++y) { for (int x = 520; x < 600; ++x) { inside += out.y[(size_t)y * W + x]; c1++; } for (int x = 700; x < 780; ++x) { ghost += out.y[(size_t)y * W + x]; c2++; } }
        CHECK(inside / c1 > 190 && ghost / c2 < 160, "moving square: frame 1's kept (%.0f), no ghost where the others had it (%.0f)", inside / c1, ghost / c2);
    }
    free(out.y); free(out.u); free(out.v);
    float cu1 = noise_in(fr[0].u, W / 2, 20, 20, 200, 300);
    CHECK(yuv_denoise(fr, 1, 2, 0, &out, &rep) == 0, "colour filter on one frame runs (%.0f ms)", rep.ms_colour);
    float cu2 = noise_in(out.u, W / 2, 20, 20, 200, 300);
    CHECK(cu2 < cu1 * 0.5f, "colour noise %.2f -> %.2f", cu1, cu2);
    {   /* the colour edge at x = W/4 (chroma): still a step of ~60 within a few pixels */
        int row = 200, cw = W / 2; float left = 0, right = 0;
        for (int x = cw / 2 - 12; x < cw / 2 - 2; ++x) left += out.u[(size_t)row * cw + x];
        for (int x = cw / 2 + 2; x < cw / 2 + 12; ++x) right += out.u[(size_t)row * cw + x];
        CHECK(right / 10 - left / 10 > 50, "colour edge kept: %.0f -> %.0f", left / 10, right / 10);
    }
    free(out.y); free(out.u); free(out.v);
    {   /* sharpening: the square's edge gets steeper, a flat area's noise barely moves */
        ir_yuv_t sh; yuv_denoise_report_t r2;
        CHECK(yuv_denoise(fr, 4, 0, 0, &sh, &r2) == 0, "a merged frame to sharpen");
        float flat0 = noise_in(sh.y, W, 100, 100, 400, 250);
        int edge0 = abs((int)sh.y[(size_t)360 * W + 502] - (int)sh.y[(size_t)360 * W + 500]);   /* across the square's edge at x = 500/501 */
        yuv_sharpen(&sh, 2, r2.sigma);
        float flat1 = noise_in(sh.y, W, 100, 100, 400, 250);
        int edge1 = abs((int)sh.y[(size_t)360 * W + 502] - (int)sh.y[(size_t)360 * W + 500]);
        CHECK(edge1 > edge0 && flat1 < flat0 * 1.6f, "sharpen 2: edge %d -> %d, flat noise %.2f -> %.2f", edge0, edge1, flat0, flat1);
        free(sh.y); free(sh.u); free(sh.v);
    }
    {   /* a flat, clipped frame (a white sky at ISO 100): no noise to measure;
         * the merge must not divide by it (it gave 0/0, black-green holes) */
        ir_yuv_t flat[2], o2; yuv_denoise_report_t r3; int bad = 0; size_t i, ny = (size_t)W * H, nc = ny / 4;
        for (int k = 0; k < 2; ++k) {
            memset(&flat[k], 0, sizeof flat[k]); flat[k].w = W; flat[k].h = H;
            flat[k].y = malloc(ny); flat[k].u = malloc(nc); flat[k].v = malloc(nc);
            memset(flat[k].y, 255, ny); memset(flat[k].u, 128, nc); memset(flat[k].v, 128, nc);
        }
        CHECK(yuv_denoise(flat, 2, 1, 1, &o2, &r3) == 0, "a flat white frame merges (noise floor %.2f)", r3.sigma);
        for (i = 0; i < ny && !bad; ++i) if (o2.y[i] != 255) bad = 1;
        for (i = 0; i < nc && !bad; ++i) if (o2.u[i] != 128 || o2.v[i] != 128) bad = 2;
        CHECK(bad == 0, "a flat white frame stays white (no 0/0 holes), %d", bad);
        for (int k = 0; k < 2; ++k) { free(flat[k].y); free(flat[k].u); free(flat[k].v); }
        free(o2.y); free(o2.u); free(o2.v);
    }
    for (int k = 0; k < 4; ++k) { free(fr[k].y); free(fr[k].u); free(fr[k].v); }
    printf(fails ? "yuvproc: FAILED (%d)\n" : "yuvproc: OK\n", fails);
    return fails ? 1 : 0;
}
