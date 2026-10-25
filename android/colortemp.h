/*
 * colortemp.h -- the white balance in force as a correlated colour
 * temperature (kelvin) and a Duv (tint), from the camera's neutral (the
 * camera RGB of a white surface, i.e. 1 / the white-balance gains) and the
 * sensor's two calibration matrices (XYZ -> camera, each for a reference
 * illuminant). Pure C, no Android: included by indraw.c and tested on a PC
 * (android/tests/colortemp_test.c).
 *
 * The method is the DNG SDK's, which darktable uses too: the XYZ -> camera
 * matrix for a temperature T is the two calibration matrices interpolated
 * in 1/T (mired) between their illuminants' temperatures; the white's
 * temperature is the T for which that matrix gives back a white of
 * temperature T -- solved here by bisection. Using one matrix only (0.47.12
 * to 0.47.18) read 1000 K or more too low away from its illuminant (2400 K
 * for a white the method puts at 3700 K, measured on JP2 DNGs).
 *
 * The result is only as good as the HAL's matrices: on the JP2 a clear 40 W
 * filament bulb (~2700 K) reads ~3300 K. Trust differences more than
 * absolute values.
 */
#ifndef INDRAW_COLORTEMP_H
#define INDRAW_COLORTEMP_H

#include <math.h>

/* A DNG / EXIF LightSource code -> its colour temperature (K), 0 if unknown. */
static double ct_illuminant_k(int code)
{
    switch (code) {
    case 17: return 2856;   /* Standard light A */
    case 3:  return 2850;   /* tungsten */
    case 24: return 3200;   /* ISO studio tungsten */
    case 15: return 3450;   /* white fluorescent */
    case 14: return 4150;   /* cool white fluorescent */
    case 18: return 4874;   /* Standard light B */
    case 13: return 5000;   /* day white fluorescent */
    case 23: return 5003;   /* D50 */
    case 1: case 4: case 9: return 5500;  /* daylight, flash, fine weather */
    case 20: return 5503;   /* D55 */
    case 12: return 6430;   /* daylight fluorescent */
    case 10: return 6500;   /* cloudy */
    case 21: return 6504;   /* D65 */
    case 19: return 6774;   /* Standard light C */
    case 11: case 22: return 7500; /* shade, D75 */
    default: return 0;
    }
}

static int ct_invert3(const double m[9], double out[9])
{
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (fabs(det) < 1e-12) return -1;
    out[0] =  (m[4] * m[8] - m[5] * m[7]) / det; out[1] = -(m[1] * m[8] - m[2] * m[7]) / det; out[2] =  (m[1] * m[5] - m[2] * m[4]) / det;
    out[3] = -(m[3] * m[8] - m[5] * m[6]) / det; out[4] =  (m[0] * m[8] - m[2] * m[6]) / det; out[5] = -(m[0] * m[5] - m[2] * m[3]) / det;
    out[6] =  (m[3] * m[7] - m[4] * m[6]) / det; out[7] = -(m[0] * m[7] - m[1] * m[6]) / det; out[8] =  (m[0] * m[4] - m[1] * m[3]) / det;
    return 0;
}

/* xy chromaticity of the white (camera neutral) through an XYZ->camera
 * matrix; 0 on success. */
static int ct_xy(const double xyz_to_cam[9], const double neutral[3], double *x, double *y)
{
    double inv[9], X, Y, Z, s;
    if (ct_invert3(xyz_to_cam, inv)) return -1;
    X = inv[0] * neutral[0] + inv[1] * neutral[1] + inv[2] * neutral[2];
    Y = inv[3] * neutral[0] + inv[4] * neutral[1] + inv[5] * neutral[2];
    Z = inv[6] * neutral[0] + inv[7] * neutral[1] + inv[8] * neutral[2];
    s = X + Y + Z;
    if (!(s > 0) || X < 0 || Y < 0 || Z < 0) return -1;
    *x = X / s; *y = Y / s;
    return 0;
}

/* McCamy's approximation, xy -> CCT. */
static double ct_mccamy(double x, double y)
{
    double n = (x - 0.3320) / (0.1858 - y);
    return 449.0 * n * n * n + 3525.0 * n * n + 6823.3 * n + 5520.33;
}

/* Ohno's approximation of the distance from the Planckian locus (+ green side). */
static double ct_duv(double x, double y)
{
    static const double k[7] = { -0.471106, 1.925865, -2.4243787, 1.5317403, -0.5179722, 0.0893944, -0.00616793 };
    double d = -2 * x + 12 * y + 3, u = 4 * x / d, v = 6 * y / d;
    double lfp = sqrt((u - 0.292) * (u - 0.292) + (v - 0.24) * (v - 0.24)), a, lbb;
    if (lfp <= 0) return 0;
    a = acos((u - 0.292) / lfp);
    lbb = k[6] * pow(a, 6) + k[5] * pow(a, 5) + k[4] * pow(a, 4) + k[3] * pow(a, 3) + k[2] * a * a + k[1] * a + k[0];
    return lfp - lbb;
}

/* The matrix for temperature t: m1 (for t1) and m2 (for t2) interpolated in
 * 1/T, clamped outside [min(t1,t2), max(t1,t2)]. */
static void ct_matrix_at(const double m1[9], double t1, const double m2[9], double t2, double t, double out[9])
{
    double w, lo = t1 < t2 ? t1 : t2, hi = t1 < t2 ? t2 : t1;
    const double *mlo = t1 < t2 ? m1 : m2, *mhi = t1 < t2 ? m2 : m1;
    int i;
    if (t <= lo) w = 1.0;
    else if (t >= hi) w = 0.0;
    else w = (1.0 / t - 1.0 / hi) / (1.0 / lo - 1.0 / hi);       /* weight of the warmer matrix */
    for (i = 0; i < 9; ++i) out[i] = w * mlo[i] + (1.0 - w) * mhi[i];
}

/* The white's temperature and Duv. m2 may be NULL (one matrix: used alone).
 * neutral: camera RGB of white (any scale). Returns 0, or -1 (unknown: *cct
 * set to 0). */
static int ct_estimate(const double m1[9], double t1, const double m2[9], double t2,
                       const double neutral[3], int *cct, double *duv)
{
    double lo = 1500, hi = 20000, m[9], x, y, flo, fmid, mid;
    int i;
    *cct = 0; *duv = 0;
    if (!m2 || !(t1 > 0) || !(t2 > 0) || fabs(t1 - t2) < 1) {
        if (ct_xy(m1, neutral, &x, &y)) return -1;
        mid = ct_mccamy(x, y);
        if (mid < 1500 || mid > 20000) return -1;
    } else {
        /* f(T) = CCT(white through the matrix at T) - T; bisect its sign change */
        ct_matrix_at(m1, t1, m2, t2, lo, m);
        if (ct_xy(m, neutral, &x, &y)) return -1;
        flo = ct_mccamy(x, y) - lo;
        ct_matrix_at(m1, t1, m2, t2, hi, m);
        if (ct_xy(m, neutral, &x, &y)) return -1;
        if (flo * (ct_mccamy(x, y) - hi) > 0) return -1;      /* no consistent temperature in range */
        for (i = 0; i < 50; ++i) {
            mid = 0.5 * (lo + hi);
            ct_matrix_at(m1, t1, m2, t2, mid, m);
            if (ct_xy(m, neutral, &x, &y)) return -1;
            fmid = ct_mccamy(x, y) - mid;
            if (flo * fmid <= 0) hi = mid; else { lo = mid; flo = fmid; }
        }
        mid = 0.5 * (lo + hi);
        ct_matrix_at(m1, t1, m2, t2, mid, m);
        if (ct_xy(m, neutral, &x, &y)) return -1;
    }
    *cct = (int)lround(mid);
    *duv = ct_duv(x, y);
    return 0;
}

#endif
