/* yuvframe.h -- a full-resolution YUV frame handed from the engine (bionic)
 * to the command line (glibc), which compresses it to a JPEG itself. Both
 * run in one process (libhybris), so the planes are shared, not copied; the
 * engine allocated them, the engine frees them (indraw_yuv_free). Plain C,
 * fixed-size fields: the same layout on both sides. */
#ifndef INDRAW_YUVFRAME_H
#define INDRAW_YUVFRAME_H
#include <stdint.h>

typedef struct {
    uint8_t *y, *u, *v;         /* planar 4:2:0: Y w x h, U and V (w/2) x (h/2) */
    int w, h;                   /* even                                         */
    int orientation_deg;        /* clockwise to view it upright (0/90/180/270)   */
    int64_t exposure_ns;        /* as the camera reported applying them; <= 0: unknown */
    int iso;
    float focal_mm, f_number;
    int focal_eq_mm;
    int exposure_manual;        /* a manual pair was asked                       */
    int wb_manual;
    char make[64], model[64];   /* the phone's, for the Exif                     */
} ir_yuv_t;

#endif
