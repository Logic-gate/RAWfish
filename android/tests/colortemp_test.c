/* Host test of android/colortemp.h with the JP2's own calibration (the two
 * ColorMatrix of its DNGs: D65 and Standard A) and the white-balance neutrals
 * of five JP2 DNGs of 20 October 2026, whose temperatures were computed
 * independently (Python, the same DNG-SDK method). Run by check-host.sh. */
#include "../colortemp.h"
#include <stdio.h>

static const double CM_D65[9] = { 43699/65536.0, -10413/65536.0, -5619/65536.0, -37614/65536.0, 91081/65536.0, 9373/65536.0, -9036/65536.0, 17377/65536.0, 39559/65536.0 };
static const double CM_A[9]   = { 100366/65536.0, -30776/65536.0, -14094/65536.0, -31210/65536.0, 94721/65536.0, 439/65536.0, -4702/65536.0, 15645/65536.0, 15267/65536.0 };

int main(void)
{
    static const struct { const char *what; double n[3]; int want; } c[] = {
        { "bulb, Lee 216, AWB",          { 0.801, 1, 0.365 },  3706 },
        { "bulb, polycarbonate, AWB",    { 0.788, 1, 0.368 },  3740 },
        { "sky, AWB",                    { 0.613, 1, 0.493 },  4463 },
        { "overcast through PC, AWB",    { 0.5523, 1, 0.5872 }, 4911 },
        { "overcast through PC, target", { 0.5300, 1, 0.6684 }, 5227 },
    };
    int i, fails = 0, cct;
    double duv;
    for (i = 0; i < (int)(sizeof c / sizeof c[0]); ++i) {
        int ok = ct_estimate(CM_D65, ct_illuminant_k(21), CM_A, ct_illuminant_k(17), c[i].n, &cct, &duv) == 0
                 && cct > c[i].want - 40 && cct < c[i].want + 40;
        printf("  %s: %s %d K (expected %d), Duv %+.4f\n", ok ? "ok" : "FAIL", c[i].what, cct, c[i].want, duv);
        fails += !ok;
    }
    /* one matrix only (a HAL with a single calibration): still an answer */
    {
        double n[3] = { 0.5523, 1, 0.5872 };
        int ok = ct_estimate(CM_D65, 6504, NULL, 0, n, &cct, &duv) == 0 && cct > 3500 && cct < 4000;
        printf("  %s: one matrix only gives %d K (the old reading, ~3745)\n", ok ? "ok" : "FAIL", cct);
        fails += !ok;
    }
    printf(fails ? "colortemp: FAILED (%d)\n" : "colortemp: OK (6 cases)\n", fails);
    return fails ? 1 : 0;
}
