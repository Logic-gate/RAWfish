/* yuvproc.h -- our own noise reduction on the engine's YUV frames: colour by
 * a cross bilateral filter, grain by merging several frames (never by
 * smoothing). */
#ifndef INDRAW_YUVPROC_H
#define INDRAW_YUVPROC_H
#include "../android/yuvframe.h"

typedef struct {
    double ms_align, ms_merge, ms_colour, ms_sharpen;   /* time of each step   */
    int ref;                                /* the frame the others were merged onto (0-based): the sharpest */
    float sharp_pct[4];                     /* each frame's sharpness, % of the sharpest (0 for one frame) */
    float merged_pct[4];                    /* share of the image each frame added, by its number (the reference: 100) */
    int shift_dy[4], shift_dx[4];           /* each frame's shift onto the reference (the reference: 0, 0) */
    float sigma;                            /* the luminance noise measured on the first frame */
    float sigma_pair;                       /* the noise measured between frames 1 and 2 (0 for one frame) */
    float sigma_merge;                      /* the noise the merge's threshold used */
} yuv_denoise_report_t;

/* n frames (1..4); the reference is the sharpest of them (see yuv_denoise,
 * and yuv_set_pick_sharpest). Writes the result into out (its
 * planes malloc'd here: free them with free()). colour: the bilateral filter
 * on U and V, 0 off, 1..5 at 1, 2, 3, 5 and 8 times the measured noise;
 * median: 0 none, 1..4 a 3x3 .. 9x9 on U and V before it (false colour).
 * 0 on success. */
int yuv_denoise(const ir_yuv_t *frames, int n, int colour, int median, ir_yuv_t *out, yuv_denoise_report_t *rep);

/* Sharpening of a frame's luminance, in place: level 1..5 (0.5, 1, 1.5, 2.5
 * and 4 times the fine detail added back), sigma the noise the detail must
 * exceed. */
void yuv_sharpen(ir_yuv_t *f, int level, float sigma);
/* The detail scale of the next images: 1 (pixels are the sensor's) or 2
 * (enlarged twice from the sensor's reading, e.g. the JP2's 50 MP). Every
 * filter (noise estimates, colour filter, false-colour median, sharpening)
 * looks that many pixels apart. */
void yuv_set_detail_scale(int s);
/* 1 (the default): the merge's reference is the sharpest frame; 0: always
 * the first. 0 for frames of different exposures (an ev= list), where a
 * change of reference would change the photo's brightness. */
void yuv_set_pick_sharpest(int on);

#endif
