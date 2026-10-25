/*
 * dngwrite.h -- minimal DNG (TIFF) writer, no libtiff.
 *
 * Why not libtiff:
 *   1. libtiff does not know the DNG-private tags (the GainMap opcode list
 *      above all). A libtiff writer must register each one by hand
 *      (TIFFMergeFieldInfo); RAWfish hit several of its bugs exactly there --
 *      tags silently dropped, floats promoted to doubles by Qt. Emitting the
 *      bytes ourselves means we control the exact layout phone darktable
 *      reads, the one validated against a working capture.
 *   2. This library is loaded on the Android side through libhybris. A libtiff
 *      dependency would have to be shipped as a bionic build or linked
 *      statically; our writer has no dependency at all.
 *   A DNG is a simple TIFF (header, one directory of tagged fields, pixels),
 *   so writing it directly is little code.
 *
 * Writes a one-image, uncompressed, little-endian DNG from a 16-bit CFA buffer
 * plus the Camera2 metadata indraw already has. The caller fills a dng_params_t
 * and calls dng_write(). Everything is copied or serialised immediately, so
 * the caller may free its inputs right after the call returns.
 */
#ifndef INDRAW_DNGWRITE_H
#define INDRAW_DNGWRITE_H

#include <stdint.h>  /* fixed-width integer types for the tag values */
#include <stddef.h>  /* size_t                                       */

/* A 3x3 colour matrix as 9 signed rationals (numerator / denominator pairs). */
typedef struct {
    int have;           /* 0 = not provided by the sensor: skip the tag      */
    int32_t num[9];     /* the 9 numerators, row-major                       */
    int32_t den[9];     /* the 9 denominators                                */
} dng_matrix_t;

/* The lens shading map: a cols x rows grid, 4 gains per point (R, Ge, Go, B). */
typedef struct {
    int have;             /* 0 = no shading map: omit the GainMap opcode      */
    int cols, rows;       /* grid size (Camera2 LENS_INFO_SHADING_MAP_SIZE)   */
    int cfa;              /* CFA arrangement (0 RGGB 1 GRBG 2 GBRG 3 BGGR)    */
    const float *gains;   /* cols*rows*4 floats [R,Ge,Go,B] per point         */
} dng_shading_t;

/* Everything dng_write() needs, gathered by the caller from Camera2 metadata. */
typedef struct {
    /* --- pixels --- */
    const uint16_t *pixels;   /* width*height samples, row-major, no padding  */
    int width, height;        /* image size in pixels                         */
    int black;                /* mean black level (fallback for black4)       */
    int black4[4];            /* per-phase black levels (BlackLevelRepeatDim 2x2) */
    int have_black4;          /* 1 = write the 2x2 black pattern              */
    int white;                /* white (saturation) level                     */
    int cfa;                  /* CFA arrangement: 0 RGGB 1 GRBG 2 GBRG 3 BGGR */

    /* --- colour calibration (illuminants are EXIF light-source codes) --- */
    int illuminant1, illuminant2;   /* e.g. 21 = D65, 17 = Standard A         */
    dng_matrix_t color1, color2;          /* ColorMatrix 1 / 2                 */
    dng_matrix_t forward1, forward2;      /* ForwardMatrix 1 / 2               */
    dng_matrix_t calibration1, calibration2;  /* CameraCalibration 1 / 2       */
    int have_neutral;               /* 0 = no AsShotNeutral                    */
    int32_t neutral_num[3];         /* AsShotNeutral R,G,B numerators          */
    int32_t neutral_den[3];         /* ... and denominators                    */
    int have_noise;                 /* 0 = no NoiseProfile                     */
    double noise[2];                /* NoiseProfile: scale, offset             */

    /* --- EXIF (each 0 = omit the tag) --- */
    int64_t exposure_ns;      /* shutter time in nanoseconds                  */
    int iso;                  /* ISO sensitivity                              */
    int wb_manual;            /* EXIF WhiteBalance: 1 manual (a measured or
                               * shifted white), 0 auto                       */
    double focal_mm;          /* focal length in millimetres                  */
    double aperture;          /* f-number                                     */

    dng_shading_t shading;    /* optional lens-shading GainMap                */

    const char *software;     /* Software tag, e.g. "indraw 0.8.0"             */
    const char *make;         /* camera maker (ro.product.manufacturer)        */
    const char *model;        /* camera model (ro.product.model)               */
    int orientation;          /* TIFF Orientation value (1..8); 0 -> 1          */
    const char *datetime;     /* EXIF "YYYY:MM:DD HH:MM:SS", or NULL to omit  */
} dng_params_t;

/* Writes the DNG to `path` (atomically, via "<path>.part" then rename).
 * Returns 0 on success, an errno value on an I/O or allocation error, or -1
 * on a bad argument. */
int dng_write(const char *path, const dng_params_t *p);

#endif /* INDRAW_DNGWRITE_H */
