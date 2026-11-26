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

/* ---- I420: the viewfinder frame as it goes to the interface (SF2Y) ----
 *
 * Since 0.3.11 the viewfinder can leave the engine as YUV instead of RGB:
 * the interface's GPU converts it (a shader), so the camera thread only
 * packs the planes (a copy, no arithmetic) and the pipe carries half the
 * bytes (1.5 per pixel instead of 3). I420 is planar: Y (w x h), then U
 * and V at half resolution ((w+1)/2 x (h+1)/2 each), rows unpadded.
 * Black where a row falls outside its plane: Y 0, U and V 128 (yuv420_to_rgb
 * blacks the whole RGB row when ANY of its planes is short; here each plane
 * is checked alone -- only a malformed buffer can tell them apart). */
static inline int i420_cw(int w) { return (w + 1) / 2; }
static inline int i420_ch(int h) { return (h + 1) / 2; }
static inline size_t i420_size(int w, int h)
{
    return (size_t)w * h + 2u * (size_t)i420_cw(w) * i420_ch(h);
}
static inline void yuv420_to_i420(const uint8_t *py, int ly, int ry, int xy,
                                  const uint8_t *pu, int lu, int ru, int xu,
                                  const uint8_t *pv, int lv, int rv, int xv,
                                  int w, int h, uint8_t *out)
{
    int x, y;
    const int cw = i420_cw(w), ch = i420_ch(h);
    uint8_t *oy = out, *ou = out + (size_t)w * h, *ov = ou + (size_t)cw * ch;
    if (w <= 0 || h <= 0) return;
    for (y = 0; y < h; ++y) {
        uint8_t *row = oy + (size_t)y * w;
        const long first = (long)y * ry, last = first + (long)(w - 1) * xy;
        if (xy <= 0 || ry < 0 || first < 0 || last >= ly) { memset(row, 0, (size_t)w); continue; }
        if (xy == 1) memcpy(row, py + first, (size_t)w);
        else { const uint8_t *s = py + first; for (x = 0; x < w; ++x, s += xy) row[x] = *s; }
    }
    for (y = 0; y < ch; ++y) {
        uint8_t *ru8 = ou + (size_t)y * cw, *rv8 = ov + (size_t)y * cw;
        const long uf = (long)y * ru, ul = uf + (long)(cw - 1) * xu;
        const long vf = (long)y * rv, vl = vf + (long)(cw - 1) * xv;
        if (xu <= 0 || ru < 0 || uf < 0 || ul >= lu) memset(ru8, 128, (size_t)cw);
        else if (xu == 1) memcpy(ru8, pu + uf, (size_t)cw);
        else { const uint8_t *s = pu + uf; for (x = 0; x < cw; ++x, s += xu) ru8[x] = *s; }
        if (xv <= 0 || rv < 0 || vf < 0 || vl >= lv) memset(rv8, 128, (size_t)cw);
        else if (xv == 1) memcpy(rv8, pv + vf, (size_t)cw);
        else { const uint8_t *s = pv + vf; for (x = 0; x < cw; ++x, s += xv) rv8[x] = *s; }
    }
}
/* One pixel of an I420 frame in RGB, with yuv420_to_rgb's arithmetic (the
 * viewfinder meter samples the frame this way: ~80 000 pixels at most every
 * 100 ms, instead of converting all of it). Pixel-exact with yuv420_to_rgb
 * on in-bounds planes (android/tests/yuv_test.c). */
static inline void i420_px(const uint8_t *i420, int w, int h, int x, int y, uint8_t rgb[3])
{
    const int cw = i420_cw(w), ch = i420_ch(h);
    const int Y = i420[(size_t)y * w + x];
    const size_t ci = (size_t)(y / 2) * cw + (size_t)(x / 2);
    const int U = i420[(size_t)w * h + ci] - 128, V = i420[(size_t)w * h + (size_t)cw * ch + ci] - 128;
    rgb[0] = yuv_clip8(Y + ((91881 * V) >> 16));
    rgb[1] = yuv_clip8(Y - ((22554 * U + 46802 * V) >> 16));
    rgb[2] = yuv_clip8(Y + ((116130 * U) >> 16));
}
#endif
