/* yuv.h -- Camera2 YUV_420_888 planes to RGB888, as a pure function so the
 * host can test it against a plain per-pixel reference. BT.601 full-range
 * coefficients as fixed point (x65536), the same ones RAWfish uses; chroma
 * at half resolution (nearest), fine for a viewfinder.
 *
 * Runs on the camera thread for every preview frame (1.2 Mpx at 30 fps):
 * the bounds are checked ONCE PER ROW (a row whose last sample would fall
 * outside its plane is left black, as the per-pixel version did for each
 * such pixel), and the three chroma terms are computed once per pair of
 * pixels, which share the chroma sample. Pixel-exact with the per-pixel
 * version (0.47.61; android/tests/yuv_test.c). */
#ifndef INDRAW_YUV_H
#define INDRAW_YUV_H
#include <stdint.h>
#include <string.h>

static inline uint8_t yuv_clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

/* py/pu/pv: the planes; ly/lu/lv: their lengths in bytes; ry/ru/rv: row
 * strides; xy/xu/xv: pixel strides. rgb: w*h*3 bytes, written in full. */
static inline void yuv420_to_rgb(const uint8_t *py, int ly, int ry, int xy,
                                 const uint8_t *pu, int lu, int ru, int xu,
                                 const uint8_t *pv, int lv, int rv, int xv,
                                 int w, int h, uint8_t *rgb)
{
    int x, y;
    if (w <= 0 || h <= 0) return;
    for (y = 0; y < h; ++y) {
        uint8_t *row = rgb + (size_t)y * w * 3;
        const int cy = y / 2, cw = (w - 1) / 2;        /* chroma row, last chroma column */
        const long y_first = (long)y * ry, y_last = y_first + (long)(w - 1) * xy;
        const long u_first = (long)cy * ru, u_last = u_first + (long)cw * xu;
        const long v_first = (long)cy * rv, v_last = v_first + (long)cw * xv;
        const uint8_t *yp, *up, *vp;
        /* The whole row inside its three planes, else black (strides are
         * positive, so the first and last samples bound every one between). */
        if (xy <= 0 || xu <= 0 || xv <= 0 || ry < 0 || ru < 0 || rv < 0 ||
            y_first < 0 || y_last >= ly || u_first < 0 || u_last >= lu || v_first < 0 || v_last >= lv) {
            memset(row, 0, (size_t)w * 3);
            continue;
        }
        yp = py + y_first; up = pu + u_first; vp = pv + v_first;
        for (x = 0; x + 1 < w; x += 2) {              /* two pixels, one chroma sample */
            const int U = *up - 128, V = *vp - 128;
            const int tr = (91881 * V) >> 16, tg = (22554 * U + 46802 * V) >> 16, tb = (116130 * U) >> 16;
            int Y = yp[0];
            row[0] = yuv_clip8(Y + tr); row[1] = yuv_clip8(Y - tg); row[2] = yuv_clip8(Y + tb);
            Y = yp[xy];
            row[3] = yuv_clip8(Y + tr); row[4] = yuv_clip8(Y - tg); row[5] = yuv_clip8(Y + tb);
            row += 6; yp += 2 * xy; up += xu; vp += xv;
        }
        if (x < w) {                                   /* an odd last pixel */
            const int U = *up - 128, V = *vp - 128, Y = yp[0];
            row[0] = yuv_clip8(Y + ((91881 * V) >> 16));
            row[1] = yuv_clip8(Y - ((22554 * U + 46802 * V) >> 16));
            row[2] = yuv_clip8(Y + ((116130 * U) >> 16));
        }
    }
}
#endif
