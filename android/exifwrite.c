/* exifwrite.c -- see exifwrite.h. A small TIFF/Exif writer that MERGES: the
 * HAL's own APP1 (IFD0, Exif IFD, IFD1 with its thumbnail) is read, every
 * entry it carries is kept, the tags the engine knows better (exposure, ISO,
 * focal length, date, make/model, orientation...) replace the HAL's values
 * or are added, and the whole is rewritten as little-endian TIFF with fresh
 * offsets. Nothing the HAL wrote and we do not know better is lost: its
 * thumbnail (the system gallery shows it without decoding the photo), its
 * flash, its maker note... (0.47.59; up to 0.47.58 the HAL's APP1 was
 * replaced by eleven tags of ours). Without a HAL APP1, ours alone. */
#include "exifwrite.h"
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- little helpers on a growing byte buffer --- */
typedef struct { uint8_t *p; size_t n, cap; int over; } buf_t;
static int put(buf_t *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) { b->over = 1; return 0; }   /* remembered: the APP1 is refused */
    memcpy(b->p + b->n, d, n); b->n += n; return 1;
}
static void put16(buf_t *b, uint16_t v) { uint8_t d[2] = { v & 0xFF, v >> 8 }; put(b, d, 2); }
static void put32(buf_t *b, uint32_t v) { uint8_t d[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24 }; put(b, d, 4); }
static void set32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }

/* One IFD entry, with its data either inline (<= 4 bytes) or appended to the
 * data area after the IFD; `data_off` tracks that area (TIFF offsets). */
typedef struct { uint16_t tag, type; uint32_t count; uint8_t inl[4]; const void *ext; uint32_t extlen; } entry_t;
enum { T_BYTE = 1, T_ASCII = 2, T_SHORT = 3, T_LONG = 4, T_RATIONAL = 5, T_UNDEFINED = 7 };
/* Four undefined bytes inline (ExifVersion, FlashpixVersion). */
static void ent_undef4(entry_t *e, uint16_t tag, const char *four) { e->tag = tag; e->type = T_UNDEFINED; e->count = 4; memcpy(e->inl, four, 4); e->ext = NULL; e->extlen = 0; }

static void ent_short(entry_t *e, uint16_t tag, uint16_t v) { e->tag = tag; e->type = T_SHORT; e->count = 1; memset(e->inl, 0, 4); e->inl[0] = v & 0xFF; e->inl[1] = v >> 8; e->ext = NULL; e->extlen = 0; }
static void ent_long(entry_t *e, uint16_t tag, uint32_t v) { e->tag = tag; e->type = T_LONG; e->count = 1; set32(e->inl, v); e->ext = NULL; e->extlen = 0; }
static void ent_ascii(entry_t *e, uint16_t tag, const char *s) { e->tag = tag; e->type = T_ASCII; e->count = (uint32_t)strlen(s) + 1; memset(e->inl, 0, 4); if (e->count <= 4) { memcpy(e->inl, s, e->count); e->ext = NULL; e->extlen = 0; } else { e->ext = s; e->extlen = e->count; } }
static void ent_rational(entry_t *e, uint16_t tag, uint32_t num, uint32_t den, uint8_t *scratch) { e->tag = tag; e->type = T_RATIONAL; e->count = 1; set32(scratch, num); set32(scratch + 4, den); e->ext = scratch; e->extlen = 8; memset(e->inl, 0, 4); }

/* Writes an IFD at the current position: entries, next-IFD 0, then the
 * external data of the entries; offsets are relative to the TIFF header at
 * `tiff0` in the buffer. */
/* The size the IFD will take: entries, next pointer, external data. */
static size_t ifd_size(const entry_t *ents, int n)
{
    size_t sz = 2 + (size_t)n * 12 + 4;
    int i;
    for (i = 0; i < n; ++i) if (ents[i].ext) sz += (ents[i].extlen + 1) & ~1u;
    return sz;
}
static void write_ifd(buf_t *b, size_t tiff0, entry_t *ents, int n, uint32_t next_ifd)
{
    size_t data_off = b->n - tiff0 + 2 + (size_t)n * 12 + 4;   /* where external data lands */
    int i;
    put16(b, (uint16_t)n);
    for (i = 0; i < n; ++i) {
        put16(b, ents[i].tag); put16(b, ents[i].type); put32(b, ents[i].count);
        if (ents[i].ext) { put32(b, (uint32_t)data_off); data_off += (ents[i].extlen + 1) & ~1u; }
        else put(b, ents[i].inl, 4);
    }
    put32(b, next_ifd);
    for (i = 0; i < n; ++i)
        if (ents[i].ext) { put(b, ents[i].ext, ents[i].extlen); if (ents[i].extlen & 1) { uint8_t z = 0; put(b, &z, 1); } }
}
/* Entries sorted by tag (TIFF requires it); a few dozen at most. */
static void sort_entries(entry_t *e, int n)
{
    int i, j;
    for (i = 1; i < n; ++i) for (j = i; j > 0 && e[j - 1].tag > e[j].tag; --j) { entry_t t = e[j]; e[j] = e[j - 1]; e[j - 1] = t; }
}
/* Our entry replaces the HAL's of the same tag, or is appended; -1 when the
 * list is full (the lists hold the HAL's maximum plus all of ours, so this
 * is a programming error, not a HAL's doing). */
static int merge_entry(entry_t *list, int n, int cap, const entry_t *ours)
{
    int i;
    if (n < 0) return -1;                                /* an earlier refusal stays one */
    for (i = 0; i < n; ++i) if (list[i].tag == ours->tag) { list[i] = *ours; return n; }
    if (n >= cap) return -1;
    list[n++] = *ours;
    return n;
}

/* --- the HAL's APP1, read into entries whose data we own (little-endian) --- */
#define HAL_MAX 64                        /* entries per IFD we keep */
#define OURS_IFD0 8                       /* our IFD0 tags at most   */
#define OURS_EXIF 16                      /* our Exif tags at most   */
typedef struct {
    int le;                               /* its byte order            */
    const uint8_t *t; size_t tl;          /* its TIFF                  */
    entry_t ifd0[HAL_MAX], exif[HAL_MAX], ifd1[HAL_MAX];
    int n0, ne, n1;
    const uint8_t *thumb; size_t thumb_len;   /* IFD1's JPEG thumbnail, in t  */
    uint8_t *pool; size_t pool_n, pool_cap;   /* converted external data      */
    int dropped;                              /* entries left out for lack of room */
} hal_t;
static uint32_t rd16(const hal_t *h, const uint8_t *p) { return h->le ? (uint32_t)(p[0] | (p[1] << 8)) : (uint32_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const hal_t *h, const uint8_t *p)
{
    return h->le ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
                 : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}
static size_t type_size(uint16_t type)
{
    switch (type) { case 1: case 2: case 6: case 7: return 1; case 3: case 8: return 2;
                    case 4: case 9: case 11: return 4; case 5: case 10: case 12: return 8; default: return 0; }
}
/* Copies a value to the pool, swapped to little-endian when the HAL is
 * big-endian (RATIONAL = two LONGs; DOUBLE = one 8-byte word). */
static const uint8_t *pool_put(hal_t *h, const uint8_t *src, size_t len, uint16_t type)
{
    uint8_t *d;
    size_t i, w;
    if (h->pool_n + len > h->pool_cap) return NULL;
    d = h->pool + h->pool_n; h->pool_n += (len + 1) & ~(size_t)1;
    memcpy(d, src, len);
    if (!h->le) {
        w = type == 3 || type == 8 ? 2 : type == 4 || type == 9 || type == 11 || type == 5 || type == 10 ? 4 : type == 12 ? 8 : 1;
        if (w > 1) for (i = 0; i + w <= len; i += w) { size_t a = 0, b2 = w - 1; while (a < b2) { uint8_t x = d[i + a]; d[i + a] = d[i + b2]; d[i + b2] = x; ++a; --b2; } }
    }
    return d;
}
/* Reads one IFD of the HAL into `out`; returns the next-IFD offset (0 none).
 * Pointer tags to sub-IFDs we do not rewrite (GPS 0x8825, Interop 0xA005)
 * are dropped rather than left dangling; 0x8769 is dropped (we add ours). */
static uint32_t hal_read_ifd(hal_t *h, size_t off, entry_t *out, int *n, uint32_t *exif_ptr)
{
    size_t cnt, k;
    *n = 0;
    /* Subtractive bounds: on a 32-bit phone (the XA2) size_t is 32 bits and
     * an offset read from the APP1 near 2^32 plus a small length wraps to a
     * small number that an additive test lets through (0.47.64). */
    if (off > h->tl || h->tl - off < 2) return 0;
    cnt = rd16(h, h->t + off);
    if (h->tl - off - 2 < cnt * 12 + 4) return 0;
    if (cnt > HAL_MAX) h->dropped += (int)(cnt - HAL_MAX);     /* over our room: counted, not silent */
    for (k = 0; k < cnt && *n < HAL_MAX; ++k) {
        const uint8_t *e = h->t + off + 2 + k * 12;
        entry_t x;
        size_t ts, vl;
        x.tag = (uint16_t)rd16(h, e); x.type = (uint16_t)rd16(h, e + 2); x.count = rd32(h, e + 4);
        ts = type_size(x.type);
        if (!ts || x.count > 65535) continue;                     /* unknown type: not copied */
        vl = ts * x.count;
        if (x.tag == 0x8769) { if (exif_ptr) *exif_ptr = rd32(h, e + 8); continue; }
        if (x.tag == 0x8825 || x.tag == 0xA005) continue;
        if (vl <= 4) {
            const uint8_t *v = pool_put(h, e + 8, vl, x.type);
            if (!v) { ++h->dropped; continue; }
            memset(x.inl, 0, 4); memcpy(x.inl, v, vl); x.ext = NULL; x.extlen = 0;
        } else {
            uint32_t vo = rd32(h, e + 8);
            if (vo > h->tl || vl > h->tl - vo) continue;           /* points outside: dropped */
            x.ext = pool_put(h, h->t + vo, vl, x.type);
            if (!x.ext) { ++h->dropped; continue; }
            x.extlen = (uint32_t)vl; memset(x.inl, 0, 4);
        }
        out[(*n)++] = x;
    }
    return rd32(h, h->t + off + 2 + cnt * 12);
}
/* Reads the HAL's APP1 TIFF: IFD0, its Exif IFD, IFD1 and the thumbnail. */
static int hal_read(hal_t *h, const uint8_t *t, size_t tl)
{
    uint32_t ifd, exif_ptr = 0, next;
    int k;
    memset(h, 0, sizeof *h);
    h->t = t; h->tl = tl;
    if (tl < 8) return -1;
    if (!memcmp(t, "II", 2)) h->le = 1; else if (!memcmp(t, "MM", 2)) h->le = 0; else return -1;
    if (rd16(h, t + 2) != 42) return -1;
    h->pool_cap = tl + 64; h->pool = malloc(h->pool_cap);
    if (!h->pool) return -1;
    ifd = rd32(h, t + 4);
    next = hal_read_ifd(h, ifd, h->ifd0, &h->n0, &exif_ptr);
    if (exif_ptr) hal_read_ifd(h, exif_ptr, h->exif, &h->ne, NULL);
    if (next && next <= tl && tl - next >= 2) {
        uint32_t to = 0, tn = 0;
        hal_read_ifd(h, next, h->ifd1, &h->n1, NULL);
        for (k = 0; k < h->n1; ++k) {
            if (h->ifd1[k].tag == 0x0201 && !h->ifd1[k].ext) to = (uint32_t)(h->ifd1[k].inl[0] | (h->ifd1[k].inl[1] << 8) | (h->ifd1[k].inl[2] << 16) | ((uint32_t)h->ifd1[k].inl[3] << 24));
            if (h->ifd1[k].tag == 0x0202 && !h->ifd1[k].ext) tn = (uint32_t)(h->ifd1[k].inl[0] | (h->ifd1[k].inl[1] << 8) | (h->ifd1[k].inl[2] << 16) | ((uint32_t)h->ifd1[k].inl[3] << 24));
        }
        /* a JPEG thumbnail: at least SOI + EOI (4 bytes), inside the TIFF
         * (subtractive test), starting with SOI */
        if (to && tn >= 4 && to <= tl && tn <= tl - to && t[to] == 0xFF && t[to + 1] == 0xD8) { h->thumb = t + to; h->thumb_len = tn; }
        else h->n1 = 0;                                  /* no usable thumbnail: no IFD1 */
    }
    return 0;
}
static void hal_free(hal_t *h) { free(h->pool); h->pool = NULL; }

/* A signed rational for an APEX value (x 1000). */
static void ent_srational(entry_t *e, uint16_t tag, double v, uint8_t *scratch)
{
    int32_t num = (int32_t)(v * 1000.0 + (v >= 0 ? 0.5 : -0.5));
    e->tag = tag; e->type = 10; e->count = 1; set32(scratch, (uint32_t)num); set32(scratch + 4, 1000); e->ext = scratch; e->extlen = 8; memset(e->inl, 0, 4);
}
/* Drops entry `tag` from a list (the HAL's invalid Orientation 0). */
static int drop_entry(entry_t *list, int n, uint16_t tag)
{
    int i, j = 0;
    for (i = 0; i < n; ++i) if (list[i].tag != tag) list[j++] = list[i];
    return j;
}

/* Exposure as a rational: 1/N for short times, else X/1000 s. */
static void exposure_rational(int64_t ns, uint32_t *num, uint32_t *den)
{
    if (ns < 250000000) { *num = 1; *den = (uint32_t)((1000000000.0 / ns) + 0.5); }
    else { *num = (uint32_t)(ns / 1000000); *den = 1000; }
}

int exif_read_orientation(const uint8_t *j, size_t len)
{
    size_t i = 2;
    while (i + 4 <= len && j[i] == 0xFF) {
        uint8_t m = j[i + 1];
        size_t seg = ((size_t)j[i + 2] << 8) | j[i + 3];
        if (m == 0xDA || seg < 2 || i + 2 + seg > len) break;     /* SOS, or a lying length */
        if (m == 0xE1 && seg >= 16 && !memcmp(j + i + 4, "Exif\0\0", 6)) {
            const uint8_t *t = j + i + 10;                        /* TIFF header */
            size_t tl = seg - 8, ifd, n, k;
            int le;
            if (tl < 8) return 0;
            if (!memcmp(t, "II", 2)) le = 1; else if (!memcmp(t, "MM", 2)) le = 0; else return 0;
#define RD16(p) (le ? ((p)[0] | ((p)[1] << 8)) : (((p)[0] << 8) | (p)[1]))
#define RD32(p) (le ? ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24)) \
                    : (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) | ((uint32_t)(p)[2] << 8) | (uint32_t)(p)[3]))
            ifd = RD32(t + 4);
            if (ifd > tl || tl - ifd < 2) return 0;                   /* subtractive: no 32-bit wrap */
            n = RD16(t + ifd);
            if (n > 512) n = 512;                                     /* sane entry count */
            for (k = 0; k < n && tl - ifd - 2 >= (size_t)(k + 1) * 12; ++k) {
                const uint8_t *e = t + ifd + 2 + k * 12;
                if (RD16(e) == 0x0112) { int v = RD16(e + 8); return v >= 1 && v <= 8 ? v : 0; }
            }
#undef RD16
#undef RD32
            return 0;
        }
        i += 2 + seg;
    }
    return 0;
}

int exif_write_jpeg(const char *path, const uint8_t *jpeg, size_t len, const exif_info_t *info)
{
    enum { APP1_MAX = 65533 };            /* a JPEG segment's payload limit  */
    uint8_t *app1, scratch[64];
    buf_t b;
    entry_t ifd0[HAL_MAX + OURS_IFD0], exif[HAL_MAX + OURS_EXIF], ours;
    int n0 = 0, ne = 0, orient, rc = 0, have_hal = 0, k;
    size_t tiff0, ifd0_at, exif_at, ifd1_at = 0, thumb_at = 0, skip_from, skip_len = 0, insert_at = 2, i;
    uint32_t num, den;
    char tmp[64];
    hal_t hal;
    FILE *f;

    if (!path || !jpeg || len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 || !info) return EINVAL;
    skip_from = len;                                 /* "no HAL Exif" until found  */

    /* --- find the HAL's Exif APP1 and a leading JFIF APP0 --- */
    {
        size_t app0_end = 2;                                       /* insert point   */
        i = 2;
        while (i + 4 <= len && jpeg[i] == 0xFF) {
            uint8_t m = jpeg[i + 1];
            size_t seg = ((size_t)jpeg[i + 2] << 8) | jpeg[i + 3];
            if (m == 0xDA || seg < 2 || i + 2 + seg > len) break;  /* SOS, or a lying length */
            if (m == 0xE0 && i == 2) app0_end = i + 2 + seg;       /* JFIF first: keep it first */
            if (m == 0xE1 && seg >= 16 && !memcmp(jpeg + i + 4, "Exif\0\0", 6)) { skip_from = i; skip_len = 2 + seg; break; }
            i += 2 + seg;
        }
        insert_at = app0_end;
        if (skip_from < insert_at) insert_at = skip_from;          /* HAL Exif before APP0: take its place */
    }
    memset(&hal, 0, sizeof hal);
    if (skip_len) have_hal = hal_read(&hal, jpeg + skip_from + 10, skip_len - 10) == 0;
    orient = info->orientation > 0 ? info->orientation : exif_read_orientation(jpeg, len);

    app1 = malloc(APP1_MAX);
    if (!app1) { hal_free(&hal); return ENOMEM; }
    b.p = app1; b.n = 0; b.cap = APP1_MAX; b.over = 0;

    /* --- the entries: the HAL's, then ours replacing or adding --- */
    if (have_hal) { memcpy(ifd0, hal.ifd0, sizeof(entry_t) * (size_t)hal.n0); n0 = hal.n0;
                    memcpy(exif, hal.exif, sizeof(entry_t) * (size_t)hal.ne); ne = hal.ne; }
    if (info->make)     { ent_ascii(&ours, 0x010F, info->make);     n0 = merge_entry(ifd0, n0, HAL_MAX + OURS_IFD0, &ours); }
    if (info->model)    { ent_ascii(&ours, 0x0110, info->model);    n0 = merge_entry(ifd0, n0, HAL_MAX + OURS_IFD0, &ours); }
    if (orient)         { ent_short(&ours, 0x0112, (uint16_t)orient); n0 = merge_entry(ifd0, n0, HAL_MAX + OURS_IFD0, &ours); }
    else n0 = drop_entry(ifd0, n0, 0x0112);          /* the HAL's tag is 0 or absent: an invalid 0 is not kept */
    if (info->software) { ent_ascii(&ours, 0x0131, info->software); n0 = merge_entry(ifd0, n0, HAL_MAX + OURS_IFD0, &ours); }
    if (info->datetime) { ent_ascii(&ours, 0x0132, info->datetime); n0 = merge_entry(ifd0, n0, HAL_MAX + OURS_IFD0, &ours); }
    ent_long(&ours, 0x8769, 0); n0 = merge_entry(ifd0, n0, HAL_MAX + OURS_IFD0, &ours);   /* patched below */
    /* The APEX values (ShutterSpeedValue, ApertureValue, MaxApertureValue)
     * and the ExposureMode are ours too: the HAL computes them from its own
     * figures (a JP2 wrote 1/24 for our 1/25 and a maximum aperture of 1.0
     * for f/1.8); DateTimeDigitized is the capture's time, as the original
     * (the HAL wrote it in UTC next to our local DateTimeOriginal). */
    if (info->exposure_ns > 0) {
        exposure_rational(info->exposure_ns, &num, &den); ent_rational(&ours, 0x829A, num, den, scratch); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);
        ent_srational(&ours, 0x9201, -log2((double)info->exposure_ns / 1e9), scratch + 24); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);
    }
    if (info->f_number > 0) {
        ent_rational(&ours, 0x829D, (uint32_t)(info->f_number * 100 + 0.5), 100, scratch + 8); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);
        ent_srational(&ours, 0x9202, 2.0 * log2((double)info->f_number), scratch + 32); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);
        ent_srational(&ours, 0x9205, 2.0 * log2((double)info->f_number), scratch + 40); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);
    }
    ent_short(&ours, 0xA402, (uint16_t)(info->exposure_manual ? 1 : 0)); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);   /* ExposureMode */
    if (info->iso > 0) { ent_short(&ours, 0x8827, (uint16_t)(info->iso > 65535 ? 65535 : info->iso)); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours); }
    ent_undef4(&ours, 0x9000, "0231"); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);            /* ExifVersion 2.31 */
    if (info->datetime) {
        snprintf(tmp, sizeof tmp, "%s", info->datetime);
        ent_ascii(&ours, 0x9003, tmp); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);   /* DateTimeOriginal  */
        ent_ascii(&ours, 0x9004, tmp); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);   /* DateTimeDigitized */
    }
    if (info->focal_mm > 0) { ent_rational(&ours, 0x920A, (uint32_t)(info->focal_mm * 100 + 0.5), 100, scratch + 16); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours); }
    ent_undef4(&ours, 0xA000, "0100"); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);            /* FlashpixVersion  */
    ent_short(&ours, 0xA001, 1); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);                  /* ColorSpace sRGB  */
    ent_short(&ours, 0xA403, (uint16_t)(info->wb_manual ? 1 : 0)); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours);   /* WhiteBalance */
    if (info->focal_eq_mm > 0) { ent_short(&ours, 0xA405, (uint16_t)info->focal_eq_mm); ne = merge_entry(exif, ne, HAL_MAX + OURS_EXIF, &ours); }
    if (n0 < 0 || ne < 0) { rc = EOVERFLOW; goto out; }          /* a list full: never with these sizes */
    sort_entries(ifd0, n0); sort_entries(exif, ne);

    /* --- TIFF: IFD0, Exif IFD, [IFD1 + thumbnail] --- */
    put(&b, "Exif\0\0", 6);
    tiff0 = b.n;
    put(&b, "II", 2); put16(&b, 42); put32(&b, 8);
    ifd0_at = b.n;
    exif_at = ifd0_at + ifd_size(ifd0, n0);
    if (have_hal && hal.n1) ifd1_at = exif_at + ifd_size(exif, ne);
    write_ifd(&b, tiff0, ifd0, n0, ifd1_at ? (uint32_t)(ifd1_at - tiff0) : 0);
    for (k = 0; k < n0; ++k) if (ifd0[k].tag == 0x8769)      /* ExifIFDPointer -> the Exif IFD */
        set32(b.p + ifd0_at + 2 + (size_t)k * 12 + 8, (uint32_t)(exif_at - tiff0));
    write_ifd(&b, tiff0, exif, ne, 0);
    if (ifd1_at) {
        thumb_at = ifd1_at + ifd_size(hal.ifd1, hal.n1);
        for (k = 0; k < hal.n1; ++k) {                           /* the thumbnail's new place */
            if (hal.ifd1[k].tag == 0x0201) set32(hal.ifd1[k].inl, (uint32_t)(thumb_at - tiff0));
            if (hal.ifd1[k].tag == 0x0202) set32(hal.ifd1[k].inl, (uint32_t)hal.thumb_len);
        }
        write_ifd(&b, tiff0, hal.ifd1, hal.n1, 0);
        put(&b, hal.thumb, hal.thumb_len);
    }
    if (b.over || b.n + 4 > APP1_MAX) { rc = EOVERFLOW; goto out; }   /* any put() that did not fit */

    /* --- write: [SOI (+ APP0)], our APP1, the rest minus the HAL's APP1 --- */
    {
        char part[1200];
        uint8_t hdr[4] = { 0xFF, 0xE1, (uint8_t)(((b.n + 2) >> 8) & 0xFF), (uint8_t)((b.n + 2) & 0xFF) };
        size_t a = insert_at, s1 = skip_from, s2 = skip_from + skip_len;   /* [0,a) ours [a,s1) [s2,len) */
        if (s1 < a) s1 = a;
        if (s2 < s1) s2 = s1;
        if (snprintf(part, sizeof part, "%s.part", path) >= (int)sizeof part)  /* as dng_write: */
            { rc = ENAMETOOLONG; goto out; }                                   /* never a cut name */
        f = fopen(part, "wb");
        if (!f) { rc = errno ? errno : EIO; goto out; }
        if (fwrite(jpeg, 1, a, f) != a || fwrite(hdr, 1, 4, f) != 4 || fwrite(b.p, 1, b.n, f) != b.n ||
            (s1 > a && fwrite(jpeg + a, 1, s1 - a, f) != s1 - a) ||
            fwrite(jpeg + s2, 1, len - s2, f) != len - s2) {
            rc = errno ? errno : EIO; fclose(f); unlink(part); goto out;
        }
        if (fclose(f) != 0) { rc = errno ? errno : EIO; unlink(part); goto out; }
        if (rename(part, path) != 0) { rc = errno; unlink(part); goto out; }
    }
out:
    if (info->hal_dropped) *info->hal_dropped = hal.dropped;
    free(app1);
    hal_free(&hal);
    return rc;
}
