/* lensfix.h -- lens distortion correction of the photos, in the CLI
 * (android/lensdist.h holds the model and the remap; the engine applies the
 * same to the viewfinder). Two kinds of photo:
 *   - our JPG from the YUV stream: the frame is corrected before it is
 *     compressed (lensfix_yuv), so it is compressed once;
 *   - a JPEG written by the camera driver (HAL) or any JPEG file: decoded,
 *     corrected, compressed again with its metadata carried over and a new
 *     Exif thumbnail (lensfix_jpeg_file). */
#ifndef INDRAW_LENSFIX_H
#define INDRAW_LENSFIX_H
#include <stddef.h>
#include <stdint.h>
#include "../android/yuvframe.h"
#include "../android/lensdist.h"

/* Reads and parses a lens distortion file (not bound: ld_bind). 0, or -1
 * with the reason in err. */
int lensfix_load(const char *path, ld_model_t *m, char *err, size_t errn);

/* The corrected copy of `in` (its planes malloc'd here: free them with
 * free(); its other fields copied). field: what the frame shows in
 * active-array pixels (x, y, w, h), NULL for the frame's whole band (zoom 1).
 * 0, -1 (memory) or -2 (the model cannot correct this frame). */
int lensfix_yuv(const ld_model_t *m, const double *field, const ir_yuv_t *in, ir_yuv_t *out);

/* A small JPEG of the frame (about max_w pixels wide) for the Exif. */
int lensfix_thumbnail(const ir_yuv_t *f, int max_w, uint8_t **jpg, unsigned long *len);

/* A JPEG file into its planar 4:2:0 frame (planes malloc'd). 0 on success. */
int lensfix_decode(const uint8_t *jpg, size_t len, ir_yuv_t *out);

/* What a photo of w x h shows of the active array (x, y, w, h): 0, or -1
 * when unknown (then: its whole band, zoom 1). The engine's
 * indraw_photo_field is one. */
typedef int (*lensfix_field_fn)(int w, int h, int32_t out[4]);

/* Corrects the JPEG file in_path into out_path (the same path: replaced
 * atomically), at JPEG quality q; field_for may be NULL. rot_deg: the
 * rotation (clockwise) the photo was asked to carry: a JPEG with no Exif
 * orientation had its pixels turned by it (the camera driver did), and is
 * turned back to the sensor's frame for the correction, then upright again;
 * -1: unknown (a JPEG whose pixels do not have the sensor's shape is then
 * refused). 0, or -1 with the reason in msg. */
int lensfix_jpeg_file(const ld_model_t *m, lensfix_field_fn field_for, int rot_deg, const char *in_path,
                      const char *out_path, int quality, char *msg, size_t msgn);

/* f turned clockwise by deg (0, 90, 180, 270) into out (planes malloc'd;
 * other fields copied, w and h swapped for 90 and 270). 0 on success. */
int lensfix_rotate(const ir_yuv_t *f, int deg, ir_yuv_t *out);

#endif
