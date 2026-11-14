/* Host test of viewfield.h: what a viewfinder frame shows of its crop, on
 * the Jolla Phone 2's real sizes. Run by scripts/check-host.sh. */
#include "../viewfield.h"
#include <math.h>
#include <stdio.h>

static int fails = 0;
#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)
#define CHECK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

int main(void)
{
    /* camera 0: 4096x3072 (4:3); a 16:9 viewfinder (4096x2304 stream) */
    vf_rect_t b = vf_band(4096, 3072, 4096, 2304), full = vf_band(4096, 3072, 1280, 960);
    CHECK(NEAR(b.x, 0) && NEAR(b.w, 1) && NEAR(b.y, 384.0 / 3072) && NEAR(b.h, 2304.0 / 3072),
          "camera 0, 16:9: the middle 2304 lines (384..2688)");
    CHECK(NEAR(full.x, 0) && NEAR(full.y, 0) && NEAR(full.w, 1) && NEAR(full.h, 1),
          "camera 0, 4:3: the whole sensor");
    /* camera 2: 4208x3120 (a little wider than 4:3), a 4096x3072 stream */
    {
        vf_rect_t b2 = vf_band(4208, 3120, 4096, 3072);
        CHECK(NEAR(b2.y, 0) && NEAR(b2.h, 1) && fabs(b2.w * 4208 - 4160) < 0.01 && fabs(b2.x * 4208 - 24) < 0.01,
              "camera 2, 4:3 stream: 24 columns off each side");
    }
    /* the touch of 14 Nov: 0.1134 of a 16:9 frame is line ~645, not 348 */
    {
        vf_rect_t p = { 0.3657, 0.1134, 0, 0 }, s = vf_to_sensor(b, p);
        CHECK(fabs(s.y * 3072 - (384 + 0.1134 * 2304)) < 0.01 && NEAR(s.x, 0.3657),
              "the touch at 0.1134: line 645 of the sensor");
    }
    /* a loupe x2 in the middle: its crop 2048x1536, the frame shows 2048x1152 */
    {
        vf_rect_t c = { 1024, 768, 2048, 1536 }, f = vf_field(c, 4096, 2304);
        CHECK(NEAR(f.x, 1024) && NEAR(f.w, 2048) && NEAR(f.y, 768 + 192) && NEAR(f.h, 1152),
              "loupe x2, 16:9: the middle band of its crop");
        /* in band fractions: half the band each way, as the controller
         * reckons a x2 loupe (its sensor fractions are the band's) */
        {
            vf_rect_t fs = { f.x / 4096, f.y / 3072, f.w / 4096, f.h / 3072 }, fb = vf_to_band(b, fs);
            CHECK(NEAR(fb.w, 0.5) && NEAR(fb.h, 0.5) && NEAR(fb.x, 0.25) && NEAR(fb.y, 0.25),
                  "loupe x2, 16:9: 0.25 0.25 0.5 0.5 of the band");
        }
    }
    /* no loupe: the frame is the band, i.e. 0 0 1 1 in band fractions */
    {
        vf_rect_t a = { 0, 0, 4096, 3072 }, f = vf_field(a, 4096, 2304);
        vf_rect_t fs = { f.x / 4096, f.y / 3072, f.w / 4096, f.h / 3072 }, fb = vf_to_band(b, fs);
        CHECK(NEAR(fb.x, 0) && NEAR(fb.y, 0) && NEAR(fb.w, 1) && NEAR(fb.h, 1), "no loupe: fcrop 0 0 1 1");
    }
    /* round trip, and unknown sizes leave everything as it is */
    {
        vf_rect_t v = { 0.2, 0.7, 0.3, 0.1 }, r = vf_to_band(b, vf_to_sensor(b, v));
        vf_rect_t c = { 10, 20, 300, 400 }, u = vf_field(c, 0, 0);
        CHECK(NEAR(r.x, v.x) && NEAR(r.y, v.y) && NEAR(r.w, v.w) && NEAR(r.h, v.h), "band <-> sensor: a round trip");
        CHECK(NEAR(u.x, 10) && NEAR(u.y, 20) && NEAR(u.w, 300) && NEAR(u.h, 400), "no stream size: the crop itself");
    }
    printf(fails ? "viewfield: FAILED (%d)\n" : "viewfield: OK (8 cases)\n", fails);
    return fails ? 1 : 0;
}
