/* focal35.h -- the 35 mm-equivalent focal length of a phone camera.
 *
 * By the diagonal, as the standard has it (CIPA DC-008, the Exif tag
 * FocalLengthIn35mmFilm): f x 43.27 / the sensor's diagonal, 43.27 mm being
 * the 36 x 24 mm frame's. Up to 0.3.9-500 RAWfish-ir divided by the width
 * (f x 36 / w): about 4 % long on a 4:3 sensor (the Xperia 10 III's main
 * camera: 28.9 mm where Sony says 27; by the diagonal 27.8) and 5 % short on
 * a 16:9 one. The HAL's own figure cannot stand in: the 10 III's telephoto
 * camera writes 6 for its 54 mm (23 Nov 2026).
 *
 * From ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS[0] and
 * ACAMERA_SENSOR_INFO_PHYSICAL_SIZE (w, h in mm). A height of 0 falls back
 * to the width. 0 when the figures are missing.
 *
 * Plain C, no camera needed: host test android/tests/focal35_test.c.
 */
#ifndef INDRAW_FOCAL35_H
#define INDRAW_FOCAL35_H

#include <math.h>

static inline float focal_35mm(float focal_mm, float sensor_w_mm, float sensor_h_mm)
{
    if (!(focal_mm > 0.0f) || !(sensor_w_mm > 0.0f)) return 0.0f;
    if (!(sensor_h_mm > 0.0f)) return focal_mm * 36.0f / sensor_w_mm;
    return focal_mm * 43.2666f / sqrtf(sensor_w_mm * sensor_w_mm + sensor_h_mm * sensor_h_mm);
}

#endif
