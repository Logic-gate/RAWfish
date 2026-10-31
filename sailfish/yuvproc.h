/* yuvproc.h -- our own noise reduction on the engine's YUV frames: colour by
 * a guided filter, grain by merging several frames (never by smoothing). */
#ifndef INDRAW_YUVPROC_H
#define INDRAW_YUVPROC_H
#include "../android/yuvframe.h"

typedef struct {
    double ms_align, ms_merge, ms_colour;   /* time of each step               */
    float merged_pct[4];                    /* share of the image each later frame added (frame 1 = 100) */
    int shift_dy[4], shift_dx[4];           /* each frame's shift onto the first */
    float sigma;                            /* the luminance noise measured on the first frame */
} yuv_denoise_report_t;

/* n frames (1..4), the first the reference. Writes the result into out (its
 * planes malloc'd here: free them with free()). colour: the guided filter on
 * U and V. 0 on success. */
int yuv_denoise(const ir_yuv_t *frames, int n, int colour, ir_yuv_t *out, yuv_denoise_report_t *rep);

#endif
