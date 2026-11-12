/* yuvsize.h -- which YUV stream a photo of a given size is taken through
 *
 * Our JPG is made from a YUV stream (the engine's "yuv=jpeg"): the photo is
 * the stream averaged down by a whole factor k (2x2 blocks for k = 2), so the
 * photo's size is the stream's divided by k. Which stream to open matters on
 * the Jolla Phone 2: a stream of an in-between size makes the camera HAL's
 * viewfinder lag by a second or so, while a stream at the sensor's full size
 * does not (seen on camera 0 in October, and on the selfie camera 1 in
 * November 2026, whose 3264x2448 photo had no stream to be averaged from).
 *
 * The first choice is exact: a listed stream that is exactly k times the
 * photo's size (camera 0: 8192x6144 for 4096x3072). This file is the second:
 * when none exists, the LARGEST listed stream, averaged down by the factor
 * that brings it closest to the size asked, if it comes close enough. The
 * engine uses it only when asked to (yuv=jpeg+largest, set by the interface
 * from rawfish-ir.conf's jpgYuvFromLargestStream, off by default).
 *
 * Camera 1 as the example: its largest stream is 6560x4928, the photo asked
 * 3264x2448. Divided by 2 the stream gives 3280x2464: 0.5 % wider and
 * 0.65 % taller than asked, and the same shape (6560/4928 = 3280/2464). That
 * is accepted: the photo is 3280x2464 instead of 3264x2448, and the viewfinder
 * is smooth. A 16:9 photo (3840x2160) from the same stream would have to be
 * cropped, which the averaging does not do, so it is refused (0) and the
 * caller keeps its exact stream.
 *
 * Plain C, no camera needed: the host tests (sailfish/tests/yuvsize_test.c)
 * run it on the real sizes of the phone.
 */
#ifndef INDRAW_YUVSIZE_H
#define INDRAW_YUVSIZE_H

#include <math.h>

/* jw, jh: the photo's size asked. lw, lh: the largest YUV stream the camera
 * lists. Returns the factor k (2..4) to average that stream down by, or 0
 * when no factor gives a photo within 8 % of the size asked in each
 * direction and of the same shape (within 1 %). Of several factors that
 * qualify, the closest to the size asked. */
static inline int yuv_binned_k(int jw, int jh, int lw, int lh)
{
    int k, best = 0;
    double best_dev = 1e9;
    if (jw <= 0 || jh <= 0 || lw <= 0 || lh <= 0) return 0;
    {   /* the shape: the stream's and the photo's width-to-height ratios */
        double shape = ((double)lw / lh) / ((double)jw / jh);
        if (shape < 0.99 || shape > 1.01) return 0;
    }
    for (k = 2; k <= 4; ++k) {
        double fw = (double)(lw / k) / jw, fh = (double)(lh / k) / jh;      /* the photo's size over the size asked */
        double dev = fmax(fabs(fw - 1.0), fabs(fh - 1.0));
        if (dev <= 0.08 && dev < best_dev) { best = k; best_dev = dev; }
    }
    return best;
}

#endif
