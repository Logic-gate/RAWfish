/* yuvsize_test.c -- the YUV stream choice (android/yuvsize.h) on the real
 * sizes of the Jolla Phone 2's cameras. Plain host test, no camera. */
#include <stdio.h>
#include "../../android/yuvsize.h"

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } else printf("  ok: %s\n", msg); } while (0)

int main(void)
{
    /* Camera 1 (selfie): photo 3264x2448, largest stream 6560x4928 -> halved: 3280x2464. */
    CHECK(yuv_binned_k(3264, 2448, 6560, 4928) == 2, "camera 1, 4:3 photo: the full stream, averaged by 2");
    /* Camera 1, the whole sensor as the photo: the stream is its own size, no factor. */
    CHECK(yuv_binned_k(6560, 4928, 6560, 4928) == 0, "camera 1, full size asked: no factor (exact stream)");
    /* Camera 1, 16:9: another shape than the stream's, never cropped here. */
    CHECK(yuv_binned_k(3840, 2160, 6560, 4928) == 0, "camera 1, 16:9 photo: refused, the exact stream is kept");
    /* Camera 0: 4096x3072 from 8192x6144 (the exact multiple, found before this); same answer anyway. */
    CHECK(yuv_binned_k(4096, 3072, 8192, 6144) == 2, "camera 0, 12.5 MP: the same factor 2");
    CHECK(yuv_binned_k(2048, 1536, 8192, 6144) == 4, "camera 0, 3 MP: factor 4");
    /* Camera 2: its largest stream is the photo's own size: no factor. */
    CHECK(yuv_binned_k(4096, 3072, 4096, 3072) == 0, "camera 2: the largest stream is the photo: no factor");
    /* A smaller photo that no whole factor reaches within 8 %. */
    CHECK(yuv_binned_k(2900, 2176, 6560, 4928) == 0, "no factor within 8 %: refused (6560/2 = 3280 is 13 % over 2900)");
    /* Of two factors that qualify the closer one: 6000x4500 to 3000x2250. */
    CHECK(yuv_binned_k(3000, 2250, 6000, 4500) == 2, "factor 2 exactly: 3000x2250");
    CHECK(yuv_binned_k(2050, 1537, 6560, 4928) == 3, "closest factor: 6560/3 = 2186 is 6.6 % over, 6560/4 = 1640 is 20 % under");
    /* Garbage in. */
    CHECK(yuv_binned_k(0, 2448, 6560, 4928) == 0 && yuv_binned_k(3264, 2448, 0, 0) == 0 && yuv_binned_k(-1, -1, 6560, 4928) == 0,
          "zero or negative sizes: no factor");
    printf("%s\n", fails ? "yuvsize: FAILED" : "yuvsize: OK (10 rules)");
    return fails ? 1 : 0;
}
