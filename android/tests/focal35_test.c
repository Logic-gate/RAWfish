/* Host test of focal35.h: the 35 mm equivalent by the diagonal, checked
 * against the Xperia 10 III's cameras (Sony: 16, 27, 54 mm; front 78 deg).
 * Run by scripts/check-host.sh. */
#include "../focal35.h"
#include <math.h>
#include <stdio.h>

static int fails = 0;
static void near(float got, float want, float tol, const char *what)
{
    if (fabsf(got - want) > tol) { fails++; printf("  FAIL: %s: %.2f, wanted %.2f\n", what, got, want); }
}

int main(void)
{
    /* main: 4.05 mm, 4032x3016 at 1.25 um (the width the old formula implied: 28.9 mm) */
    float w = 4.05f * 36.0f / 28.9f, h = w * 3016.0f / 4032.0f;
    near(focal_35mm(4.05f, w, h), 27.8f, 0.2f, "10 III main");
    /* ultra-wide 1.657 mm and telephoto 5.52 mm, 3264x2448 at 1.12 um */
    w = 3264 * 0.00112f; h = 2448 * 0.00112f;
    near(focal_35mm(1.657f, w, h), 15.7f, 0.2f, "10 III ultra-wide");
    near(focal_35mm(5.52f, w, h), 52.2f, 0.3f, "10 III telephoto");
    /* front: 2.79 mm; 78 deg diagonal is 26.7 mm */
    near(focal_35mm(2.79f, w, h), 26.4f, 0.3f, "10 III front");
    /* a 3:2 sensor: the diagonal and the width agree */
    near(focal_35mm(10.0f, 36.0f, 24.0f), 10.0f, 0.01f, "full frame");
    /* a 16:9 sensor: longer than by the width */
    near(focal_35mm(4.0f, 16.0f, 9.0f), 4.0f * 43.2666f / sqrtf(256 + 81), 0.01f, "16:9");
    /* missing figures */
    near(focal_35mm(4.0f, 5.0f, 0.0f), 4.0f * 36.0f / 5.0f, 0.01f, "no height: by the width");
    near(focal_35mm(0.0f, 5.0f, 4.0f), 0.0f, 0.0f, "no focal");
    near(focal_35mm(4.0f, 0.0f, 4.0f), 0.0f, 0.0f, "no width");
    if (fails) { printf("focal35: %d FAILED\n", fails); return 1; }
    printf("focal35: all passed\n");
    return 0;
}
