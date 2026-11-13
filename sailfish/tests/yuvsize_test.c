/* yuvsize_test.c -- the YUV stream choice (android/yuvsize.h): unit cases,
 * then the JP2's real size lists (its generated-hal.json, 13 Nov 2026), on
 * which only camera 1's 3264x2448 may change. Plain host test, no camera. */
#include <stdio.h>
#include "../../android/yuvsize.h"

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } else printf("  ok: %s\n", msg); } while (0)

/* The JP2's lists, as its probe wrote them: YUV and JPEG output sizes. */
static const int yuv0[][2] = { {8192,6144}, {4096,3072}, {4096,2304}, {4032,3024}, {3840,2160}, {3264,2448}, {3072,1728}, {2560,1920}, {2560,1440}, {1920,1440}, {1920,1088}, {1920,1080}, {1600,1200}, {1600,720}, {1560,720}, {1520,720}, {1440,1088}, {1440,1080}, {1440,720}, {1280,960}, {1280,720}, {1080,720}, {960,720}, {720,720}, {720,480}, {640,480}, {640,360}, {480,320}, {352,288}, {320,240}, {192,144}, {192,108}, {176,144}, {160,96} };
static const int jpeg0[][2] = { {8192,6144}, {4096,3072}, {4096,2304}, {4032,3024}, {3840,2160}, {3264,2448}, {3072,1728}, {2560,1920}, {1920,1440}, {1920,1088}, {1920,1080}, {1600,1200}, {1600,720}, {1560,720}, {1520,720}, {1440,1088}, {1440,720}, {1280,960}, {1280,720}, {1080,720}, {960,720}, {720,720}, {640,480}, {640,360}, {480,320}, {320,240} };
static const int yuv1[][2] = { {6560,4928}, {3840,2160}, {3264,2448}, {3168,1584}, {3072,1728}, {2560,1920}, {2560,1440}, {2560,1280}, {2304,1296}, {2176,1088}, {2048,1536}, {1920,1440}, {1920,1088}, {1920,1080}, {1600,1200}, {1600,900}, {1600,720}, {1560,720}, {1520,720}, {1440,1088}, {1440,1080}, {1440,720}, {1280,960}, {1280,720}, {1080,720}, {960,720}, {960,540}, {960,480}, {800,600}, {720,720}, {720,480}, {640,480}, {640,360}, {480,320}, {352,288}, {320,240}, {256,192}, {192,144}, {192,108}, {176,144}, {160,96} };
static const int jpeg1[][2] = { {6560,4928}, {3840,2160}, {3264,2448}, {3168,1584}, {3072,1728}, {2560,1920}, {2560,1280}, {2304,1296}, {2176,1088}, {2048,1536}, {1920,1440}, {1920,1088}, {1920,1080}, {1600,1200}, {1600,900}, {1600,720}, {1560,720}, {1520,720}, {1440,1088}, {1440,1080}, {1440,720}, {1280,960}, {1280,720}, {1080,720}, {960,720}, {960,480}, {800,600}, {720,720}, {640,480}, {640,360}, {480,320}, {320,240} };
static const int yuv2[][2] = { {4096,3072}, {4096,2304}, {4032,3024}, {3840,2160}, {3264,2448}, {3264,1836}, {3264,1456}, {3072,1728}, {2560,1920}, {2560,1440}, {2448,2448}, {2304,1296}, {2048,1536}, {1920,1440}, {1920,1088}, {1920,1080}, {1616,912}, {1600,1200}, {1600,720}, {1560,720}, {1520,720}, {1440,1088}, {1440,1080}, {1440,720}, {1280,960}, {1280,720}, {1080,720}, {960,960}, {960,720}, {960,540}, {800,600}, {720,720}, {720,480}, {640,480}, {640,360}, {480,320}, {352,288}, {320,240}, {256,192}, {192,144}, {192,108}, {176,144}, {160,96} };
static const int jpeg2[][2] = { {4096,3072}, {4096,2304}, {4032,3024}, {3264,2448}, {3264,1836}, {3264,1456}, {2560,1920}, {2448,2448}, {2304,1296}, {2048,1536}, {1920,1440}, {1920,1088}, {1920,1080}, {1616,912}, {1600,1200}, {1600,720}, {1560,720}, {1520,720}, {1440,1088}, {1440,1080}, {1440,720}, {1280,960}, {1280,720}, {1080,720}, {960,960}, {960,720}, {800,600}, {720,720}, {640,480}, {640,360}, {480,320}, {320,240} };

#define N(a) ((int)(sizeof a / sizeof a[0]))
static int listed(const int (*l)[2], int n, int w, int h) { int i; for (i = 0; i < n; ++i) if (l[i][0] == w && l[i][1] == h) return 1; return 0; }

/* The engine's order (indraw_open, yuv=jpeg+largest): an exact multiple
 * (4, 3, 2) first; only without one, the largest-stream rule. Returns the
 * rule's factor, 0 when the rule does not apply to this photo size. */
static int rule_applies(const int (*yuv)[2], int ny, int jw, int jh)
{
    int k, i, lw = 0, lh = 0;
    for (k = 4; k >= 2; --k) if (listed(yuv, ny, jw * k, jh * k)) return 0;
    for (i = 0; i < ny; ++i) if ((long)yuv[i][0] * yuv[i][1] > (long)lw * lh) { lw = yuv[i][0]; lh = yuv[i][1]; }
    return yuv_binned_k(jw, jh, lw, lh);
}
static int count_changed(const int (*yuv)[2], int ny, const int (*jpeg)[2], int nj, int *last_w, int *last_h)
{
    int i, n = 0;
    for (i = 0; i < nj; ++i) if (rule_applies(yuv, ny, jpeg[i][0], jpeg[i][1])) { n++; *last_w = jpeg[i][0]; *last_h = jpeg[i][1]; }
    return n;
}

int main(void)
{
    int w = 0, h = 0;
    /* Unit cases. */
    CHECK(yuv_binned_k(3264, 2448, 6560, 4928) == 2, "camera 1, 4:3 photo: the largest stream halved (3280x2464)");
    CHECK(yuv_binned_k(6560, 4928, 6560, 4928) == 0, "the photo at the stream's own size: no factor");
    CHECK(yuv_binned_k(3840, 2160, 6560, 4928) == 0, "16:9 from a 4:3 stream: refused");
    CHECK(yuv_binned_k(4096, 3072, 8192, 6144) == 2 && yuv_binned_k(2048, 1536, 8192, 6144) == 4, "exact halves and quarters: 2 and 4");
    CHECK(yuv_binned_k(4032, 3024, 8192, 6144) == 0, "an odd size (8192/4032 = 2.03, 1.6 % off): refused, as the menu does");
    CHECK(yuv_binned_k(2186, 1642, 6560, 4928) == 0, "a third (6560/2186 = 3.0): refused, the menu offers no thirds");
    CHECK(yuv_binned_k(0, 2448, 6560, 4928) == 0 && yuv_binned_k(3264, 2448, 0, 0) == 0 && yuv_binned_k(-1, -1, 6560, 4928) == 0,
          "zero or negative sizes: no factor");
    /* The phone's real lists: every JPEG size of each camera. */
    CHECK(count_changed(yuv0, N(yuv0), jpeg0, N(jpeg0), &w, &h) == 0, "camera 0 (real lists): no JPEG size changes");
    CHECK(count_changed(yuv1, N(yuv1), jpeg1, N(jpeg1), &w, &h) == 1 && w == 3264 && h == 2448,
          "camera 1 (real lists): only 3264x2448 changes");
    CHECK(count_changed(yuv2, N(yuv2), jpeg2, N(jpeg2), &w, &h) == 0, "camera 2 (real lists): no JPEG size changes");
    printf("%s\n", fails ? "yuvsize: FAILED" : "yuvsize: OK (10 rules)");
    return fails ? 1 : 0;
}
