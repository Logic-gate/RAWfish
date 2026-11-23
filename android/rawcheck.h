/* rawcheck.h -- does a RAW16 frame hold a whole picture?
 *
 * Some HALs fill only part of the RAW16 buffer they declare. The Xperia 10
 * III, with its AE range reaching 120 fps, ran the sensor in a binned
 * 2016x1136 mode and wrote that small image into the first 568 rows of its
 * 4032x3016 buffer, the rest left at 0 (23 Nov 2026; the device profile's
 * "fps_max" now keeps the full-size mode). Saved as is, that DNG shows two
 * squashed copies of the scene on black, and nothing said so: the only hint
 * was a central mean below black.
 *
 * A row the sensor wrote never lies entirely far below the black level: the
 * read noise around black keeps some pixel of every row near it, even in
 * the dark. A row whose every pixel is under half the black level is memory
 * the HAL never wrote. A frame with an eighth of its rows like that is
 * damaged. A camera that reports a black level under 8 cannot be told from
 * a dark scene: it is not judged. (ProCam reached the same rule on the same
 * phone.)
 *
 * The check only reports: the photo is written as before, with the verdict
 * in its reply and the log.
 *
 * Plain C, no camera needed: host test android/tests/rawcheck_test.c.
 */
#ifndef INDRAW_RAWCHECK_H
#define INDRAW_RAWCHECK_H

#include <stddef.h>
#include <stdint.h>

/* Rows of a w x h RAW16 frame (rows `stride` bytes apart) whose every pixel
 * is under black / 2. -1 if the frame cannot be judged (black under 8, or
 * bad arguments). Most rows end at their first pixel, so this costs little. */
static inline int raw_unwritten_rows(const uint8_t *data, size_t stride, int w, int h, double black)
{
    int y, x, n = 0;
    uint16_t floor_v;
    if (!data || w <= 0 || h <= 0 || stride < (size_t)w * 2 || !(black >= 8.0)) return -1;
    floor_v = (uint16_t)(black / 2.0);
    for (y = 0; y < h; ++y) {
        const uint16_t *row = (const uint16_t *)(data + (size_t)y * stride);
        for (x = 0; x < w && row[x] < floor_v; ++x) ;
        if (x == w) ++n;
    }
    return n;
}

/* Whether that many unwritten rows of h make the frame damaged: an eighth. */
static inline int raw_damaged(int unwritten, int h)
{
    return unwritten > 0 && h > 0 && (long)unwritten * 8 >= (long)h;
}

#endif
