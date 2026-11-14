/* viewfield.h -- what part of the sensor a viewfinder frame really shows
 *
 * Camera2 crops every output stream further to the stream's own proportions:
 * of the crop region asked (SCALER_CROP_REGION), a stream shows the largest
 * centred rectangle of its shape, and the results report only the crop
 * region. On the Jolla Phone 2's 4:3 sensors a 16:9 photo format opens a
 * 16:9 viewfinder stream: the frames show the middle 2304 lines of camera
 * 0's 3072, while the crop region says all 3072. Until 0.3.8 the engine took
 * the frame for the crop region: a touch near the frame's long edges put
 * the focus zone up to 12.5 % of the sensor's height off the finger, partly
 * outside the picture (log of 14 Nov 2026: a touch at 0.11 of the frame got
 * a zone over lines 118-578, the point touched at line ~645).
 *
 * The convention since: everything the controller and the interface express
 * in fractions of the sensor (the display window, the loupe's centre, the
 * frame's crop "fcrop") is in fractions of the BAND, the part of the whole
 * sensor the viewfinder's shape shows (vf_band); the engine converts to the
 * sensor at its edges. With a stream of the sensor's own shape the band is
 * the whole sensor and nothing changes.
 *
 * Plain C, no camera needed: host test android/tests/viewfield_test.c.
 */
#ifndef INDRAW_VIEWFIELD_H
#define INDRAW_VIEWFIELD_H

typedef struct { double x, y, w, h; } vf_rect_t;

/* The part of r (pixels, or any units the same in x and y) that a stream of
 * sw x sh pixels shows: the largest rectangle of the stream's proportions,
 * centred in r. sw or sh <= 0, or an empty r: r itself. */
static inline vf_rect_t vf_field(vf_rect_t r, int sw, int sh)
{
    double s, ra;
    if (sw <= 0 || sh <= 0 || !(r.w > 0) || !(r.h > 0)) return r;
    s = (double)sw / sh;
    ra = r.w / r.h;
    if (ra > s * 1.0005) {                     /* r wider than the stream: its sides go */
        double nw = r.h * s;
        r.x += (r.w - nw) / 2; r.w = nw;
    } else if (ra < s / 1.0005) {              /* r taller: its top and bottom go */
        double nh = r.w / s;
        r.y += (r.h - nh) / 2; r.h = nh;
    }
    return r;
}

/* The band: what a stream of sw x sh shows of the whole sensor (aw x ah
 * pixels), as fractions of the sensor (0..1). */
static inline vf_rect_t vf_band(int aw, int ah, int sw, int sh)
{
    vf_rect_t a = { 0, 0, (double)aw, (double)ah }, f, b = { 0, 0, 1, 1 };
    if (aw <= 0 || ah <= 0) return b;
    f = vf_field(a, sw, sh);
    b.x = f.x / aw; b.y = f.y / ah; b.w = f.w / aw; b.h = f.h / ah;
    return b;
}

/* Band fractions -> sensor fractions (a point or a rectangle). */
static inline vf_rect_t vf_to_sensor(vf_rect_t band, vf_rect_t v)
{
    vf_rect_t s;
    s.x = band.x + v.x * band.w; s.y = band.y + v.y * band.h;
    s.w = v.w * band.w;          s.h = v.h * band.h;
    return s;
}

/* Sensor fractions -> band fractions. */
static inline vf_rect_t vf_to_band(vf_rect_t band, vf_rect_t s)
{
    vf_rect_t v = s;
    if (band.w > 0 && band.h > 0) {
        v.x = (s.x - band.x) / band.w; v.y = (s.y - band.y) / band.h;
        v.w = s.w / band.w;            v.h = s.h / band.h;
    }
    return v;
}

#endif
