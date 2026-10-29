/* exif_test.c -- exif_write_jpeg merges our tags into the HAL's APP1:
 * everything the HAL wrote and we do not know better stays (its Flash, its
 * IFD1 thumbnail, its byte order converted), ours replace or add, the
 * rewritten TIFF is little-endian with valid, sorted entries. Host build:
 *   gcc -std=c11 -Wall -Wextra -Werror -o exif_test android/tests/exif_test.c android/exifwrite.c
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* memmem */
#endif
#include "../exifwrite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (cond) { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } \
                              else { ++fails; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* --- a synthetic HAL APP1: IFD0 {Make, Orientation, ExifIFD}, Exif {ExposureTime, ISO, Flash}, IFD1 + thumbnail --- */
static size_t w16(uint8_t *p, int be, unsigned v) { if (be) { p[0] = v >> 8; p[1] = v & 255; } else { p[0] = v & 255; p[1] = v >> 8; } return 2; }
static size_t w32(uint8_t *p, int be, unsigned long v)
{
    if (be) { p[0] = (v >> 24) & 255; p[1] = (v >> 16) & 255; p[2] = (v >> 8) & 255; p[3] = v & 255; }
    else { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255; }
    return 4;
}
static const uint8_t k_thumb[24] = { 0xFF, 0xD8, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 0xFF, 0xD9 };

/* Returns the JPEG length; `jpeg` must hold 2 KB. With `be` the TIFF is MM. */
static size_t make_hal_jpeg(uint8_t *jpeg, int be)
{
    uint8_t *t = jpeg + 2 + 2 + 2 + 6;              /* SOI, APP1 marker, length, "Exif\0\0" */
    uint8_t *p = t;
    size_t ifd0 = 8, exif_at, ifd1_at, make_at, exp_at, thumb_at;
    /* layout (TIFF offsets): header 8; IFD0 at 8: 3 entries (2+36+4 = 42) -> ends 50;
       Make "HALMAKE\0" (8) at 50 -> 58; Exif IFD at 58: 3 entries (42) -> 100;
       ExposureTime rational (8) at 100 -> 108; IFD1 at 108: 3 entries (42) -> 150; thumbnail at 150 */
    make_at = 50; exif_at = 58; exp_at = 100; ifd1_at = 108; thumb_at = 150;
    memcpy(p, be ? "MM" : "II", 2); p += 2; p += w16(p, be, 42); p += w32(p, be, ifd0);
    /* IFD0 */
    p += w16(p, be, 3);
    p += w16(p, be, 0x010F); p += w16(p, be, 2); p += w32(p, be, 8); p += w32(p, be, make_at);
    p += w16(p, be, 0x0112); p += w16(p, be, 3); p += w32(p, be, 1); p += w16(p, be, 6); p += w16(p, be, 0);
    p += w16(p, be, 0x8769); p += w16(p, be, 4); p += w32(p, be, 1); p += w32(p, be, exif_at);
    p += w32(p, be, ifd1_at);                                  /* next IFD: IFD1 */
    memcpy(p, "HALMAKE", 8); p += 8;
    /* Exif IFD */
    p += w16(p, be, 3);
    p += w16(p, be, 0x829A); p += w16(p, be, 5); p += w32(p, be, 1); p += w32(p, be, exp_at);
    p += w16(p, be, 0x8827); p += w16(p, be, 3); p += w32(p, be, 1); p += w16(p, be, 100); p += w16(p, be, 0);
    p += w16(p, be, 0x9209); p += w16(p, be, 3); p += w32(p, be, 1); p += w16(p, be, 1); p += w16(p, be, 0);
    p += w32(p, be, 0);
    p += w32(p, be, 1); p += w32(p, be, 50);                   /* 1/50 s */
    /* IFD1 */
    p += w16(p, be, 3);
    p += w16(p, be, 0x0103); p += w16(p, be, 3); p += w32(p, be, 1); p += w16(p, be, 6); p += w16(p, be, 0);
    p += w16(p, be, 0x0201); p += w16(p, be, 4); p += w32(p, be, 1); p += w32(p, be, thumb_at);
    p += w16(p, be, 0x0202); p += w16(p, be, 4); p += w32(p, be, 1); p += w32(p, be, sizeof k_thumb);
    p += w32(p, be, 0);
    memcpy(p, k_thumb, sizeof k_thumb); p += sizeof k_thumb;
    {
        size_t tl = (size_t)(p - t), seg = 2 + 6 + tl;
        jpeg[0] = 0xFF; jpeg[1] = 0xD8; jpeg[2] = 0xFF; jpeg[3] = 0xE1;
        jpeg[4] = (uint8_t)(seg >> 8); jpeg[5] = (uint8_t)(seg & 255);
        memcpy(jpeg + 6, "Exif\0\0", 6);
    }
    /* a fake SOS and EOI */
    p[0] = 0xFF; p[1] = 0xDA; p[2] = 0; p[3] = 4; p[4] = 1; p[5] = 2; p[6] = 0xFF; p[7] = 0xD9;
    return (size_t)(p - jpeg) + 8;
}

/* --- a little reader of the OUTPUT (little-endian expected) --- */
typedef struct { uint16_t tag, type; uint32_t count, val; } rent_t;   /* val: inline 32 bits or the offset */
static const uint8_t *g_t; static size_t g_tl;
static unsigned r16(size_t o) { return g_t[o] | (g_t[o + 1] << 8); }
static unsigned long r32(size_t o) { return (unsigned long)g_t[o] | ((unsigned long)g_t[o + 1] << 8) | ((unsigned long)g_t[o + 2] << 16) | ((unsigned long)g_t[o + 3] << 24); }
static int read_ifd(size_t off, rent_t *out, int cap, unsigned long *next)
{
    int n = (int)r16(off), i;
    if (n > cap) n = cap;
    for (i = 0; i < n; ++i) {
        size_t e = off + 2 + (size_t)i * 12;
        out[i].tag = (uint16_t)r16(e); out[i].type = (uint16_t)r16(e + 2); out[i].count = (uint32_t)r32(e + 4); out[i].val = (uint32_t)r32(e + 8);
    }
    *next = r32(off + 2 + (size_t)n * 12);
    return n;
}
static const rent_t *find(const rent_t *l, int n, unsigned tag) { int i; for (i = 0; i < n; ++i) if (l[i].tag == tag) return &l[i]; return NULL; }
static int sorted(const rent_t *l, int n) { int i; for (i = 1; i < n; ++i) if (l[i - 1].tag >= l[i].tag) return 0; return 1; }

static int check_output(const char *path, int had_hal, const char *label)
{
    static uint8_t out[4096];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(out, 1, sizeof out, f) : 0, seg;
    rent_t ifd0[32], exif[32], ifd1[32];
    int n0, ne, n1 = 0;
    unsigned long next, exif_off;
    const rent_t *e;
    if (f) fclose(f);
    CHECK(n > 20 && out[0] == 0xFF && out[1] == 0xD8 && out[2] == 0xFF && out[3] == 0xE1, "%s: SOI then our APP1", label);
    if (n <= 20) return 0;
    seg = ((size_t)out[4] << 8) | out[5];
    g_t = out + 12; g_tl = seg - 8;                 /* marker(2) len(2) "Exif\0\0"(6) */
    CHECK(!memcmp(g_t, "II", 2) && r16(2) == 42 && r32(4) == 8, "%s: little-endian TIFF, IFD0 at 8", label);
    n0 = read_ifd(8, ifd0, 32, &next);
    CHECK(sorted(ifd0, n0), "%s: IFD0 tags sorted (%d entries)", label, n0);
    e = find(ifd0, n0, 0x010F);
    CHECK(e && e->type == 2 && e->val + e->count <= g_tl && !memcmp(g_t + e->val, "indRAW-make", 12), "%s: Make is ours (replaced the HAL's)", label);
    e = find(ifd0, n0, 0x0112);
    CHECK(e && (e->val & 0xFFFF) == (had_hal ? 6u : 1u), "%s: Orientation %s", label, had_hal ? "kept from the HAL (6)" : "ours (1)");
    e = find(ifd0, n0, 0x8769);
    CHECK(e != NULL, "%s: ExifIFD pointer present", label);
    exif_off = e ? e->val : 0;
    ne = read_ifd(exif_off, exif, 32, &next);
    CHECK(sorted(exif, ne), "%s: Exif tags sorted (%d entries)", label, ne);
    e = find(exif, ne, 0x8827);
    CHECK(e && (e->val & 0xFFFF) == 400, "%s: ISO is ours (400, the HAL said 100)", label);
    e = find(exif, ne, 0x829A);
    CHECK(e && e->type == 5 && r32(e->val) == 1 && r32(e->val + 4) == 30, "%s: ExposureTime is ours (1/30, the HAL said 1/50)", label);
    e = find(exif, ne, 0x9209);
    CHECK((e != NULL) == had_hal && (!had_hal || (e->val & 0xFFFF) == 1), "%s: Flash %s", label, had_hal ? "kept from the HAL" : "absent (none to keep)");
    e = find(exif, ne, 0x920A);
    CHECK(e && e->type == 5 && r32(e->val) == 595 && r32(e->val + 4) == 100, "%s: FocalLength ours (5.95)", label);
    e = find(exif, ne, 0x9201);
    CHECK(e && e->type == 10 && (long)r32(e->val) == 4907 && r32(e->val + 4) == 1000, "%s: ShutterSpeedValue ours (APEX 4.907 for 1/30)", label);
    e = find(exif, ne, 0x9205);
    CHECK(e && e->type == 10 && (long)r32(e->val) == 1696, "%s: MaxApertureValue ours (APEX 1.696 for f/1.8)", label);
    e = find(exif, ne, 0x9004);
    CHECK(e && e->type == 2 && !memcmp(g_t + e->val, "2026:10:25 10:00:00", 20), "%s: DateTimeDigitized ours", label);
    e = find(exif, ne, 0xA402);
    CHECK(e && (e->val & 0xFFFF) == 1, "%s: ExposureMode manual (ours)", label);
    /* IFD1 + thumbnail */
    read_ifd(8, ifd0, 32, &next);
    if (had_hal) {
        n1 = next ? read_ifd(next, ifd1, 32, &next) : 0;
        e = find(ifd1, n1, 0x0201);
        {
            const rent_t *l = find(ifd1, n1, 0x0202);
            CHECK(e && l && l->val == sizeof k_thumb && e->val + l->val <= g_tl && !memcmp(g_t + e->val, k_thumb, sizeof k_thumb),
                  "%s: the HAL's thumbnail kept, byte for byte, at its new offset", label);
        }
    } else CHECK(next == 0, "%s: no IFD1 (the HAL had none)", label);
    /* the fake image data follows untouched */
    CHECK(out[n - 2] == 0xFF && out[n - 1] == 0xD9 && out[n - 8] == 0xFF && out[n - 7] == 0xDA, "%s: the image segments follow", label);
    return 0;
}

int main(void)
{
    static uint8_t jpeg[2048];
    exif_info_t info;
    size_t len;
    int rc, pass;
    memset(&info, 0, sizeof info);
    info.make = "indRAW-make"; info.model = "test"; info.software = "indraw test";
    info.datetime = "2026:10:25 10:00:00"; info.exposure_ns = 33333333; info.iso = 400;
    info.focal_mm = 5.95f; info.focal_eq_mm = 26; info.f_number = 1.8f; info.exposure_manual = 1;
    for (pass = 0; pass < 3; ++pass) {
        const char *label = pass == 0 ? "big-endian HAL" : pass == 1 ? "little-endian HAL" : "no HAL APP1";
        if (pass < 2) len = make_hal_jpeg(jpeg, pass == 0);
        else { memcpy(jpeg, "\xFF\xD8\xFF\xDA\x00\x04\x01\x02\xFF\xD9", 10); len = 10; }
        info.orientation = pass == 2 ? 1 : 0;              /* 0: take the HAL's */
        rc = exif_write_jpeg("/tmp/indraw_exif_test.jpg", jpeg, len, &info);
        CHECK(rc == 0, "%s: written (rc %d)", label, rc);
        if (rc == 0) check_output("/tmp/indraw_exif_test.jpg", pass < 2, label);
    }
    {   /* a HAL Orientation of 0 (invalid, as the JP2 writes) is dropped, not kept */
        static uint8_t out[4096];
        rent_t ifd0[32];
        unsigned long next;
        FILE *f;
        size_t n;
        len = make_hal_jpeg(jpeg, 0);                            /* little-endian */
        jpeg[12 + 8 + 2 + 12 + 8] = 0;                           /* IFD0 entry 2 (Orientation), inline value -> 0 */
        info.orientation = 0;
        rc = exif_write_jpeg("/tmp/indraw_exif_test.jpg", jpeg, len, &info);
        f = fopen("/tmp/indraw_exif_test.jpg", "rb"); n = f ? fread(out, 1, sizeof out, f) : 0; if (f) fclose(f);
        g_t = out + 12; g_tl = n > 12 ? n - 12 : 0;
        read_ifd(8, ifd0, 32, &next);
        {
            int cnt = read_ifd(8, ifd0, 32, &next);
            const rent_t *o = find(ifd0, cnt, 0x0112);
            CHECK(rc == 0 && o == NULL, "a HAL Orientation of 0 is dropped (invalid), not kept (rc %d, %d entries%s)", rc, cnt, o ? ", orientation present" : "");
        }
    }
    {   /* hostile offsets: a value offset near 2^32 (wraps on 32-bit size_t),
         * a thumbnail of 1 byte (t[to + 1] would be t[tl]), an IFD0 pointer
         * past the end -- all refused or skipped, never read out of bounds
         * (run under ASan) */
        static uint8_t out[4096];
        FILE *f;
        size_t n;
        len = make_hal_jpeg(jpeg, 0);
        /* Make's value offset (IFD0 entry 0, LE, at file 12+8+2+8) -> 0xFFFFFFFC */
        jpeg[30] = 0xFC; jpeg[31] = 0xFF; jpeg[32] = 0xFF; jpeg[33] = 0xFF;
        rc = exif_write_jpeg("/tmp/indraw_exif_test.jpg", jpeg, len, &info);
        CHECK(rc == 0, "a value offset near 2^32 is dropped, the file still written (rc %d)", rc);
        len = make_hal_jpeg(jpeg, 0);
        /* thumbnail length (IFD1 entry 2 value, LE): IFD1 at TIFF 108 -> file 120; entry 2 at 120+2+24 = 146, value at 154 */
        jpeg[154] = 1; jpeg[155] = 0; jpeg[156] = 0; jpeg[157] = 0;
        rc = exif_write_jpeg("/tmp/indraw_exif_test.jpg", jpeg, len, &info);
        f = fopen("/tmp/indraw_exif_test.jpg", "rb"); n = f ? fread(out, 1, sizeof out, f) : 0; if (f) fclose(f);
        CHECK(rc == 0 && n > 20 && !memmem(out, n, k_thumb + 2, 20), "a 1-byte thumbnail is not kept (no IFD1, no over-read)");
        len = make_hal_jpeg(jpeg, 0);
        jpeg[16] = 0xF0; jpeg[17] = 0xFF; jpeg[18] = 0xFF; jpeg[19] = 0xFF;   /* IFD0 offset near 2^32 */
        rc = exif_write_jpeg("/tmp/indraw_exif_test.jpg", jpeg, len, &info);
        CHECK(rc == 0 && exif_read_orientation(jpeg, len) == 0, "an IFD0 offset near 2^32: HAL APP1 unusable, ours alone written (rc %d)", rc);
    }
    {   /* a HAL Exif IFD full (64 entries, none of ours): every one of our
         * 15 tags still fits (the lists hold HAL_MAX + ours) */
        static uint8_t big[8192];
        uint8_t *t = big + 12, *p = t;
        size_t exif_at = 8 + 2 + 12 + 4, i;             /* IFD0: one entry (ExifIFD) */
        memcpy(p, "II", 2); p += 2; p += w16(p, 0, 42); p += w32(p, 0, 8);
        p += w16(p, 0, 1); p += w16(p, 0, 0x8769); p += w16(p, 0, 4); p += w32(p, 0, 1); p += w32(p, 0, exif_at); p += w32(p, 0, 0);
        p += w16(p, 0, 64);
        for (i = 0; i < 64; ++i) { p += w16(p, 0, (unsigned)(0xC000 + i)); p += w16(p, 0, 3); p += w32(p, 0, 1); p += w16(p, 0, 7); p += w16(p, 0, 0); }
        p += w32(p, 0, 0);
        {
            size_t tl = (size_t)(p - t), seg = 2 + 6 + tl;
            big[0] = 0xFF; big[1] = 0xD8; big[2] = 0xFF; big[3] = 0xE1; big[4] = (uint8_t)(seg >> 8); big[5] = (uint8_t)(seg & 255);
            memcpy(big + 6, "Exif\0\0", 6);
            p[0] = 0xFF; p[1] = 0xDA; p[2] = 0; p[3] = 4; p[4] = 1; p[5] = 2; p[6] = 0xFF; p[7] = 0xD9;
            len = (size_t)(p - big) + 8;
        }
        info.orientation = 1;
        {
            int dropped = -1;
            info.hal_dropped = &dropped;
            rc = exif_write_jpeg("/tmp/indraw_exif_test.jpg", big, len, &info);
            CHECK(rc == 0 && dropped == 0, "a HAL IFD of exactly 64 entries: nothing dropped (count %d)", dropped);
            info.hal_dropped = NULL;
        }
        {
            static uint8_t out[8192];
            rent_t ifd0[16], exif[96];
            unsigned long next; int ne; const rent_t *e;
            FILE *f = fopen("/tmp/indraw_exif_test.jpg", "rb"); size_t n = f ? fread(out, 1, sizeof out, f) : 0; if (f) fclose(f);
            g_t = out + 12; g_tl = n > 12 ? n - 12 : 0;
            read_ifd(8, ifd0, 16, &next);
            e = find(ifd0, read_ifd(8, ifd0, 16, &next), 0x8769);
            ne = e ? read_ifd(e->val, exif, 96, &next) : 0;
            CHECK(rc == 0 && ne == 64 + 15 && find(exif, ne, 0xA405) && find(exif, ne, 0xA001) && find(exif, ne, 0xA403),
                  "a HAL Exif IFD of 64 entries keeps all of ours too (%d entries, rc %d)", ne, rc);
        }
    }
    unlink("/tmp/indraw_exif_test.jpg");
    printf(fails ? "exif: FAILED (%d)\n" : "exif: OK (6 passes)\n", fails);
    return fails ? 1 : 0;
}
