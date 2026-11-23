/* Host test of rawcheck.h: a RAW16 frame partly left unwritten by the HAL
 * (the Xperia 10 III's binned image in a full-size buffer) is told from a
 * whole frame, a dark one included. Run by scripts/check-host.sh. */
#include "../rawcheck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

/* A w x h frame with row padding; rows [0, filled) get noise around `level`,
 * the rest stay 0. */
static uint8_t *frame(int w, int h, size_t stride, int filled, int level)
{
    uint8_t *d = calloc(stride, (size_t)h);
    int y, x;
    unsigned s = 12345;
    if (!d) exit(2);
    for (y = 0; y < filled; ++y) {
        uint16_t *row = (uint16_t *)(d + (size_t)y * stride);
        for (x = 0; x < w; ++x) {
            s = s * 1103515245u + 12345u;
            row[x] = (uint16_t)(level + (int)((s >> 16) % 9) - 4);
        }
    }
    return d;
}

int main(void)
{
    const int w = 4032, h = 3016;
    const size_t stride = (size_t)w * 2 + 64;
    uint8_t *d;
    int n;

    /* the 10 III of 23 Nov 2026: 568 rows written, 2448 at 0 */
    d = frame(w, h, stride, 568, 200);
    n = raw_unwritten_rows(d, stride, w, h, 64.0);
    CHECK(n == h - 568, "binned image in a full buffer: 2448 rows unwritten");
    CHECK(raw_damaged(n, h), "binned image in a full buffer: damaged");
    free(d);

    /* a whole frame, bright and dark (noise around black) */
    d = frame(w, h, stride, h, 200);
    n = raw_unwritten_rows(d, stride, w, h, 64.0);
    CHECK(n == 0 && !raw_damaged(n, h), "whole bright frame: sound");
    free(d);
    d = frame(w, h, stride, h, 64);
    n = raw_unwritten_rows(d, stride, w, h, 64.0);
    CHECK(n == 0 && !raw_damaged(n, h), "whole dark frame (at black): sound");
    free(d);

    /* a few rows missing: reported, not damaged (under an eighth) */
    d = frame(w, h, stride, h - 10, 200);
    n = raw_unwritten_rows(d, stride, w, h, 64.0);
    CHECK(n == 10 && !raw_damaged(n, h), "10 rows missing: counted, not damaged");
    free(d);

    /* exactly an eighth: damaged */
    CHECK(raw_damaged(377, 3016) && !raw_damaged(376, 3016), "the threshold is an eighth");

    /* a row with one pixel near black is a written row */
    d = frame(w, h, stride, 0, 0);
    ((uint16_t *)(d + 5 * stride))[w - 1] = 60;
    n = raw_unwritten_rows(d, stride, w, h, 64.0);
    CHECK(n == h - 1, "one pixel near black writes its row");
    free(d);

    /* not judged: black under 8, bad arguments */
    d = frame(w, h, stride, 0, 0);
    CHECK(raw_unwritten_rows(d, stride, w, h, 0.0) == -1, "black 0: not judged");
    CHECK(raw_unwritten_rows(d, (size_t)w, w, h, 64.0) == -1, "stride too small: not judged");
    CHECK(raw_unwritten_rows(NULL, stride, w, h, 64.0) == -1, "no data: not judged");
    CHECK(!raw_damaged(-1, h), "not judged is not damaged");
    free(d);

    if (fails) { printf("rawcheck: %d FAILED\n", fails); return 1; }
    printf("rawcheck: all passed\n");
    return 0;
}
