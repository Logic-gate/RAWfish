/*
 * dngwrite.c -- see dngwrite.h (including why we do not use libtiff).
 *
 * A DNG is a TIFF: an 8-byte header, one image file directory (IFD) of tagged
 * fields, and the pixel data. A value that does not fit in a tag's 4-byte slot
 * is stored in a "trailing" blob after the directory and referenced by file
 * offset -- the ordinary TIFF rule. Because those offsets depend on how big the
 * directory and the blob end up, we build each IFD twice: once to learn its
 * size, once with the real offsets filled in.
 *
 * Layout of the file we write:
 *   [0]        8-byte header: "II" 42 <offset of IFD0 = 8>
 *   [8]        IFD0 body, then IFD0 trailing blob (big tag values + GainMap)
 *   [...]      EXIF IFD body, then its trailing blob
 *   [...]      the pixel strip
 */
#include "dngwrite.h"

#include <errno.h>   /* errno, error codes returned to the caller */
#include <stdio.h>   /* FILE, fopen, fwrite, rename               */
#include <stdlib.h>  /* malloc, realloc, free                     */
#include <string.h>  /* memcpy, memset, strlen                    */
#include <unistd.h>  /* unlink                                    */

/* TIFF field type codes, as stored in each directory entry. */
enum {
    T_BYTE = 1,       /* 8-bit unsigned                            */
    T_ASCII = 2,      /* NUL-terminated string                     */
    T_SHORT = 3,      /* 16-bit unsigned                           */
    T_LONG = 4,       /* 32-bit unsigned                           */
    T_RATIONAL = 5,   /* two 32-bit unsigned: numerator, denom     */
    T_UNDEFINED = 7,  /* raw bytes (used for the opcode list)       */
    T_SRATIONAL = 10, /* two 32-bit signed                         */
    T_DOUBLE = 12     /* 64-bit IEEE-754                           */
};

/* One IFD under construction: its entries, plus a growing blob for the values
 * that are too big to sit inline in an entry. */
typedef struct {
    uint8_t entries[64 * 12];  /* up to 64 entries, 12 bytes each            */
    int n;                     /* number of entries filled so far            */
    uint8_t *trail;            /* heap blob for oversized values             */
    size_t trail_len;          /* bytes used in the blob                     */
    size_t trail_cap;          /* bytes allocated for the blob               */
    uint32_t trail_base;       /* file offset where the blob will be written */
    int oom;                   /* set if a blob allocation failed            */
} ifd_t;

/* Store a 16-bit value little-endian into b. */
static void put16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;            /* low byte first                           */
    b[1] = (uint8_t)(v >> 8);     /* then high byte                           */
}

/* Store a 32-bit value little-endian into b. */
static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v;            /* bytes from least to most significant     */
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

/* Append len bytes to the trailing blob and return the file offset where they
 * will land (trail_base + position). Returns 0 and sets oom on failure; 0 is
 * never a valid offset here because the header occupies the first 8 bytes. */
static uint32_t trail_put(ifd_t *d, const void *data, size_t len)
{
    uint32_t off;                                    /* result offset          */
    if (d->trail_len + len + 1 > d->trail_cap) {     /* room for the value AND */
        size_t cap = d->trail_cap ? d->trail_cap * 2 : 4096;  /* the padding   */
        uint8_t *n;                                   /* byte below (explicit, */
        while (cap < d->trail_len + len + 1) cap *= 2; /* not by parity)       */
        n = realloc(d->trail, cap);                   /* enlarge the blob      */
        if (!n) { d->oom = 1; return 0; }             /* allocation failed     */
        d->trail = n;
        d->trail_cap = cap;
    }
    memcpy(d->trail + d->trail_len, data, len);       /* copy the value        */
    off = d->trail_base + (uint32_t)d->trail_len;     /* its file offset       */
    d->trail_len += len;                              /* advance               */
    if (d->trail_len & 1) d->trail[d->trail_len++] = 0; /* keep length even    */
    return off;
}

/* Write one directory entry: tag, type, count, then either the value inline
 * (when payload_len <= 4) or an offset into the trailing blob. */
static void entry(ifd_t *d, uint16_t tag, uint16_t type, uint32_t count,
                  const uint8_t *payload, size_t payload_len)
{
    uint8_t *e;
    if (d->n >= 64) { d->oom = 1; return; } /* never overrun the entries array  */
    e = d->entries + d->n * 12;           /* the 12-byte slot for this entry   */
    put16(e, tag);                        /* bytes 0-1: tag number             */
    put16(e + 2, type);                   /* bytes 2-3: field type             */
    put32(e + 4, count);                  /* bytes 4-7: value count            */
    if (payload_len <= 4) {               /* value fits inline                 */
        memset(e + 8, 0, 4);              /* clear the 4-byte value slot       */
        if (payload) memcpy(e + 8, payload, payload_len); /* copy it in        */
    } else {                              /* value goes to the trailing blob   */
        put32(e + 8, trail_put(d, payload, payload_len)); /* store its offset  */
    }
    d->n++;                               /* one more entry                    */
}

/* --- Typed entry helpers: each packs its value(s) then calls entry(). --- */

/* A single SHORT. */
static void e_short(ifd_t *d, uint16_t tag, uint16_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), 0, 0 }; /* little-endian   */
    entry(d, tag, T_SHORT, 1, b, 2);
}

/* A single LONG. */
static void e_long(ifd_t *d, uint16_t tag, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    entry(d, tag, T_LONG, 1, b, 4);
}

/* An array of SHORTs. */
static void e_shorts(ifd_t *d, uint16_t tag, const uint16_t *v, int count)
{
    uint8_t tmp[32];                      /* enough for our small arrays       */
    int i;
    if (count < 0 || count > 16) { d->oom = 1; return; } /* 16 shorts = 32 bytes */
    for (i = 0; i < count; ++i) put16(tmp + i * 2, v[i]); /* pack each         */
    entry(d, tag, T_SHORT, count, tmp, (size_t)count * 2);
}

/* An array of LONGs. */
static void e_longs(ifd_t *d, uint16_t tag, const uint32_t *v, int count)
{
    uint8_t tmp[32];
    int i;
    if (count < 0 || count > 8) { d->oom = 1; return; } /* 8 longs = 32 bytes   */
    for (i = 0; i < count; ++i) put32(tmp + i * 4, v[i]);
    entry(d, tag, T_LONG, count, tmp, (size_t)count * 4);
}

/* An array of BYTEs. */
static void e_bytes(ifd_t *d, uint16_t tag, const uint8_t *v, int count)
{
    entry(d, tag, T_BYTE, count, v, (size_t)count);
}

/* A NUL-terminated ASCII string. */
static void e_ascii(ifd_t *d, uint16_t tag, const char *s)
{
    size_t n = strlen(s) + 1;             /* include the terminating NUL       */
    entry(d, tag, T_ASCII, (uint32_t)n, (const uint8_t *)s, n);
}

/* Nine signed rationals (a 3x3 colour matrix). */
static void e_srational9(ifd_t *d, uint16_t tag, const int32_t *num, const int32_t *den)
{
    uint8_t tmp[9 * 8];                   /* 9 pairs of two 32-bit values      */
    int i;
    for (i = 0; i < 9; ++i) {
        put32(tmp + i * 8, (uint32_t)num[i]);     /* numerator                 */
        put32(tmp + i * 8 + 4, (uint32_t)den[i]); /* denominator               */
    }
    entry(d, tag, T_SRATIONAL, 9, tmp, sizeof tmp);
}

/* A single unsigned rational. */
static void e_rational(ifd_t *d, uint16_t tag, uint32_t num, uint32_t den)
{
    uint8_t tmp[8];
    put32(tmp, num);
    put32(tmp + 4, den);
    entry(d, tag, T_RATIONAL, 1, tmp, 8);
}

/* Three unsigned rationals (AnalogBalance, AsShotNeutral). */
static void e_rationals3(ifd_t *d, uint16_t tag, const int32_t *num, const int32_t *den)
{
    uint8_t tmp[3 * 8];
    int i;
    for (i = 0; i < 3; ++i) {
        put32(tmp + i * 8, (uint32_t)num[i]);
        put32(tmp + i * 8 + 4, (uint32_t)den[i]);
    }
    entry(d, tag, T_RATIONAL, 3, tmp, sizeof tmp);
}

/* An array of little-endian DOUBLEs (NoiseProfile). */
static void e_doubles(ifd_t *d, uint16_t tag, const double *v, int count)
{
    uint8_t tmp[16];                      /* we only ever write two            */
    int i;
    for (i = 0; i < count; ++i) {
        uint64_t bits;
        memcpy(&bits, &v[i], 8);          /* reinterpret the double's bits     */
        put32(tmp + i * 8, (uint32_t)bits);        /* low 32 bits              */
        put32(tmp + i * 8 + 4, (uint32_t)(bits >> 32)); /* high 32 bits        */
    }
    entry(d, tag, T_DOUBLE, count, tmp, (size_t)count * 8);
}

/* Approximate a double as a rational num/den. Starts with a 1e6 denominator and
 * reduces it until num fits the signed or unsigned 32-bit range. */
static void to_rational(double x, int32_t *num, int32_t *den, int sign)
{
    double d = 1000000.0;                 /* starting denominator              */
    double n = x * d;                     /* corresponding numerator           */
    double lo = sign ? -2147483648.0 : 0.0;        /* lower bound by signedness */
    double hi = sign ? 2147483647.0 : 4294967295.0;/* upper bound              */
    while ((n < lo || n > hi) && d > 1.0) {        /* shrink until it fits      */
        d /= 10.0;
        n = x * d;
    }
    if (n < lo) n = lo;                   /* clamp as a last resort            */
    if (n > hi) n = hi;
    *num = (int32_t)(n < 0 ? n - 0.5 : n + 0.5);   /* round to nearest          */
    *den = (int32_t)d;
}

/* --- Big-endian stores: the DNG opcode list is big-endian, unlike the rest. */

/* A big-endian float32. */
static void be_f32(uint8_t *b, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, 4);                 /* the float's raw bits              */
    b[0] = (uint8_t)(bits >> 24);         /* most significant byte first       */
    b[1] = (uint8_t)(bits >> 16);
    b[2] = (uint8_t)(bits >> 8);
    b[3] = (uint8_t)bits;
}

/* A big-endian uint32. */
static void be_u32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

/* A big-endian float64. */
static void be_f64(uint8_t *b, double f)
{
    uint64_t bits;
    int i;
    memcpy(&bits, &f, 8);                 /* the double's raw bits             */
    for (i = 0; i < 8; ++i) b[i] = (uint8_t)(bits >> (56 - i * 8)); /* MSB first */
}

/* Build the OpcodeList2 blob: four GainMap opcodes, one per CFA phase, so a raw
 * developer removes the lens vignetting. The field types below are the exact
 * mix phone darktable reads (verified against a working RAWfish (ric9k patched) capture, which
 * matches Adobe's dng_gain_map): region Top/Left/Bottom/Right and the integer
 * header fields are uint32, MapSpacing/MapOrigin are float64, gains are float32.
 * Returns a malloc'd blob (caller frees) and its length in *out_len, or NULL on
 * allocation failure. */
static uint8_t *build_gainmap(const dng_shading_t *s, int width, int height, size_t *out_len)
{
    int cols = s->cols, rows = s->rows;  /* grid dimensions                   */
    int ng = cols * rows;                /* gains per opcode (one per point)  */
    /* One opcode's parameter block: 11 uint32 + 4 float64 + ng float32. */
    size_t op_param = (size_t)(10 + 1) * 4 + 4 * 8 + (size_t)ng * 4;
    size_t op_total = 16 + op_param;     /* + the 16-byte opcode header        */
    size_t total = 4 + 4 * op_total;     /* + the 4-byte opcode count          */
    uint8_t *blob = malloc(total);       /* the whole OpcodeList2 value        */
    uint8_t *q;                          /* write cursor                       */
    /* The four phases are the 2x2 mosaic positions (0,0),(0,1),(1,0),(1,1).
     * Camera2's shading gains are ALWAYS ordered [R, Geven, Godd, B]; which of
     * those feeds a phase depends on the CFA colour at that position. ph_chan
     * per arrangement maps phase -> gain channel so this is right on any sensor,
     * not just BGGR. Greens: Geven (ch1) for the top-row green, Godd (ch2) for
     * the bottom-row green. */
    static const int ph_row[4] = { 0, 0, 1, 1 };   /* Top per phase            */
    static const int ph_col[4] = { 0, 1, 0, 1 };   /* Left per phase           */
    static const int ph_chan_cfa[4][4] = {
        { 0, 1, 2, 3 },  /* RGGB: R, Ge, Go, B                                 */
        { 1, 0, 3, 2 },  /* GRBG: Ge, R, B, Go                                 */
        { 2, 3, 0, 1 },  /* GBRG: Go, B, R, Ge                                 */
        { 3, 1, 2, 0 },  /* BGGR: B, Ge, Go, R                                 */
    };
    const int *ph_chan = ph_chan_cfa[s->cfa & 3];
    int phase, i, j;
    if (!blob) return NULL;              /* out of memory                      */
    q = blob;
    be_u32(q, 4); q += 4;                /* opcode count = 4                   */
    for (phase = 0; phase < 4; ++phase) {
        int ch = ph_chan[phase];         /* which gain channel for this phase  */
        be_u32(q, 9); q += 4;            /* opcode id 9 = GainMap              */
        be_u32(q, 0x01030000); q += 4;   /* min DNG version 1.3.0.0            */
        be_u32(q, 1); q += 4;            /* flags: bit 0 = optional            */
        be_u32(q, (uint32_t)op_param); q += 4; /* parameter block size         */
        be_u32(q, (uint32_t)ph_row[phase]); q += 4; /* region Top (pixels)     */
        be_u32(q, (uint32_t)ph_col[phase]); q += 4; /* region Left             */
        be_u32(q, (uint32_t)height); q += 4;         /* region Bottom = height */
        be_u32(q, (uint32_t)width); q += 4;          /* region Right = width   */
        be_u32(q, 0); q += 4;            /* Plane (first affected plane)       */
        be_u32(q, 1); q += 4;            /* Planes (count)                     */
        be_u32(q, 2); q += 4;            /* RowPitch 2: one CFA phase          */
        be_u32(q, 2); q += 4;            /* ColPitch 2                         */
        be_u32(q, (uint32_t)rows); q += 4; /* MapPointsV                       */
        be_u32(q, (uint32_t)cols); q += 4; /* MapPointsH                       */
        be_f64(q, rows > 1 ? 1.0 / (rows - 1) : 1.0); q += 8; /* MapSpacingV   */
        be_f64(q, cols > 1 ? 1.0 / (cols - 1) : 1.0); q += 8; /* MapSpacingH   */
        be_f64(q, 0.0); q += 8;          /* MapOriginV                         */
        be_f64(q, 0.0); q += 8;          /* MapOriginH                         */
        be_u32(q, 1); q += 4;            /* MapPlanes                          */
        for (i = 0; i < rows; ++i)       /* the gains, row-major               */
            for (j = 0; j < cols; ++j) {
                be_f32(q, s->gains[(i * cols + j) * 4 + ch]); /* this channel   */
                q += 4;
            }
    }
    *out_len = total;                    /* report the blob length             */
    return blob;
}

/* DNG / TIFF tag numbers used below. */
enum {
    TAG_SubfileType = 254, TAG_ImageWidth = 256, TAG_ImageLength = 257,
    TAG_BitsPerSample = 258, TAG_Compression = 259, TAG_Photometric = 262,
    TAG_Make = 271, TAG_Model = 272, TAG_StripOffsets = 273,
    TAG_Orientation = 274, TAG_SamplesPerPixel = 277, TAG_RowsPerStrip = 278,
    TAG_StripByteCounts = 279, TAG_PlanarConfig = 284, TAG_Software = 305,
    TAG_CFARepeatDim = 33421, TAG_CFAPattern = 33422,
    TAG_DateTime = 306,
    TAG_ExposureTime = 33434, TAG_FNumber = 33437, TAG_ExifIFD = 34665,
    TAG_DateTimeOriginal = 36867, TAG_DateTimeDigitized = 36868,
    TAG_ISO = 34855, TAG_FocalLength = 37386, TAG_WhiteBalance = 41987,
    TAG_DNGVersion = 50706, TAG_DNGBackVersion = 50707, TAG_UniqueModel = 50708,
    TAG_CFAPlaneColor = 50710, TAG_CFALayout = 50711, TAG_BlackLevel = 50714,
    TAG_WhiteLevel = 50717, TAG_DefaultCropOrigin = 50719, TAG_DefaultCropSize = 50720,
    TAG_ColorMatrix1 = 50721, TAG_ColorMatrix2 = 50722, TAG_CameraCalib1 = 50723,
    TAG_CameraCalib2 = 50724, TAG_AnalogBalance = 50727, TAG_AsShotNeutral = 50728,
    TAG_CalIlluminant1 = 50778, TAG_CalIlluminant2 = 50779,
    TAG_ForwardMatrix1 = 50964, TAG_ForwardMatrix2 = 50965,
    TAG_OpcodeList2 = 51009, TAG_NoiseProfile = 51041
};

/* Serialise d into out: the IFD body (2-byte entry count, 12*n entries, 4-byte
 * next-IFD offset = 0) followed by its trailing blob. Entries are sorted by tag
 * first, as TIFF requires. Returns the number of bytes written. */
static size_t emit_ifd(ifd_t *d, uint8_t *out)
{
    int i, j;
    size_t body;
    for (i = 1; i < d->n; ++i)            /* insertion sort entries by tag      */
        for (j = i; j > 0; --j) {
            uint8_t *a = d->entries + (j - 1) * 12; /* previous entry           */
            uint8_t *b = d->entries + j * 12;       /* this entry               */
            uint8_t t[12];
            if (((a[1] << 8) | a[0]) <= ((b[1] << 8) | b[0])) break; /* in order */
            memcpy(t, a, 12); memcpy(a, b, 12); memcpy(b, t, 12);    /* swap     */
        }
    put16(out, (uint16_t)d->n);           /* entry count                        */
    memcpy(out + 2, d->entries, (size_t)d->n * 12); /* the entries              */
    put32(out + 2 + d->n * 12, 0);        /* next-IFD offset: none              */
    body = 2 + (size_t)d->n * 12 + 4;     /* size of the directory body         */
    if (d->trail_len) memcpy(out + body, d->trail, d->trail_len); /* append blob */
    return body + d->trail_len;
}

/* Add every IFD0 tag whose value is known up front (that is, all but the three
 * offset-bearing tags, which the caller adds once their targets are placed). */
static void fill_ifd0(ifd_t *d, const dng_params_t *p)
{
    uint16_t cfa_dim[2] = { 2, 2 };       /* CFA repeat pattern is 2x2          */
    uint8_t cfa_pattern[4];               /* the four phase colours             */
    uint8_t cfa_plane[3] = { 0, 1, 2 };   /* plane colours: R, G, B             */
    uint8_t dngver[4] = { 1, 4, 0, 0 };   /* DNGVersion 1.4.0.0                 */
    uint8_t dngback[4] = { 1, 3, 0, 0 };  /* readable back to 1.3.0.0           */
    uint32_t crop_origin[2] = { 0, 0 };   /* no default crop offset             */
    uint32_t crop_size[2] = { (uint32_t)p->width, (uint32_t)p->height }; /* full */
    int32_t ab_num[3] = { 1, 1, 1 };      /* AnalogBalance = 1,1,1              */
    int32_t ab_den[3] = { 1, 1, 1 };
    /* CFAPattern uses 0=R 1=G 2=B. Fill it for the sensor's arrangement. */
    if (p->cfa == 0) {        /* RGGB */
        cfa_pattern[0] = 0; cfa_pattern[1] = 1; cfa_pattern[2] = 1; cfa_pattern[3] = 2;
    } else if (p->cfa == 1) { /* GRBG */
        cfa_pattern[0] = 1; cfa_pattern[1] = 0; cfa_pattern[2] = 2; cfa_pattern[3] = 1;
    } else if (p->cfa == 2) { /* GBRG */
        cfa_pattern[0] = 1; cfa_pattern[1] = 2; cfa_pattern[2] = 0; cfa_pattern[3] = 1;
    } else {                  /* BGGR (our sensor) */
        cfa_pattern[0] = 2; cfa_pattern[1] = 1; cfa_pattern[2] = 1; cfa_pattern[3] = 0;
    }

    e_long(d, TAG_SubfileType, 0);                 /* full-resolution image     */
    e_long(d, TAG_ImageWidth, (uint32_t)p->width); /* width                     */
    e_long(d, TAG_ImageLength, (uint32_t)p->height);/* height                   */
    e_short(d, TAG_BitsPerSample, 16);             /* 16 bits per sample        */
    e_short(d, TAG_Compression, 1);                /* uncompressed              */
    e_short(d, TAG_Photometric, 32803);            /* CFA (colour filter array) */
    e_ascii(d, TAG_Make, p->make && p->make[0] ? p->make : "unknown");   /* maker  */
    e_ascii(d, TAG_Model, p->model && p->model[0] ? p->model : "unknown"); /* model */
    e_short(d, TAG_Orientation, (uint16_t)(p->orientation ? p->orientation : 1));
    e_short(d, TAG_SamplesPerPixel, 1);            /* one sample (mosaiced)     */
    e_long(d, TAG_RowsPerStrip, (uint32_t)p->height); /* whole image in 1 strip */
    e_short(d, TAG_PlanarConfig, 1);               /* contiguous                */
    e_ascii(d, TAG_Software, p->software ? p->software : "rawfish-ir");
    if (p->datetime && p->datetime[0])             /* when the file was written  */
        e_ascii(d, TAG_DateTime, p->datetime);
    e_shorts(d, TAG_CFARepeatDim, cfa_dim, 2);     /* 2x2 CFA tile              */
    e_bytes(d, TAG_CFAPattern, cfa_pattern, 4);    /* the mosaic order          */
    e_bytes(d, TAG_DNGVersion, dngver, 4);         /* DNG version               */
    e_bytes(d, TAG_DNGBackVersion, dngback, 4);    /* backward-compatible to    */
    {                                              /* UniqueCameraModel = maker + model */
        char uc[132];
        snprintf(uc, sizeof uc, "%s %s", p->make && p->make[0] ? p->make : "unknown",
                 p->model && p->model[0] ? p->model : "unknown");
        e_ascii(d, TAG_UniqueModel, uc);           /* identifies the sensor     */
    }
    e_bytes(d, TAG_CFAPlaneColor, cfa_plane, 3);   /* plane colour order R,G,B  */
    e_short(d, TAG_CFALayout, 1);                  /* rectangular layout        */
    if (p->have_black4) {                          /* per-phase 2x2 black level  */
        uint16_t bl[4];
        uint16_t bdim[2] = { 2, 2 };
        int i;
        for (i = 0; i < 4; ++i) bl[i] = (uint16_t)p->black4[i];
        e_shorts(d, 50713, bdim, 2);               /* BlackLevelRepeatDim        */
        e_shorts(d, TAG_BlackLevel, bl, 4);        /* the four phase levels      */
    } else {
        e_short(d, TAG_BlackLevel, (uint16_t)p->black); /* one level for all     */
    }
    e_long(d, TAG_WhiteLevel, (uint32_t)p->white); /* white (saturation) level  */
    e_longs(d, TAG_DefaultCropOrigin, crop_origin, 2); /* crop origin           */
    e_longs(d, TAG_DefaultCropSize, crop_size, 2); /* crop size = full image    */
    e_short(d, TAG_CalIlluminant1, (uint16_t)p->illuminant1); /* illuminant 1   */
    e_short(d, TAG_CalIlluminant2, (uint16_t)p->illuminant2); /* illuminant 2   */
    if (p->color1.have) e_srational9(d, TAG_ColorMatrix1, p->color1.num, p->color1.den);
    if (p->color2.have) e_srational9(d, TAG_ColorMatrix2, p->color2.num, p->color2.den);
    if (p->calibration1.have) e_srational9(d, TAG_CameraCalib1, p->calibration1.num, p->calibration1.den);
    if (p->calibration2.have) e_srational9(d, TAG_CameraCalib2, p->calibration2.num, p->calibration2.den);
    if (p->forward1.have) e_srational9(d, TAG_ForwardMatrix1, p->forward1.num, p->forward1.den);
    if (p->forward2.have) e_srational9(d, TAG_ForwardMatrix2, p->forward2.num, p->forward2.den);
    e_rationals3(d, TAG_AnalogBalance, ab_num, ab_den); /* unity analog balance */
    if (p->have_neutral) e_rationals3(d, TAG_AsShotNeutral, p->neutral_num, p->neutral_den);
    if (p->have_noise) e_doubles(d, TAG_NoiseProfile, p->noise, 2);
}

/* Build the EXIF sub-IFD into e from the params (each tag omitted if absent). */
static void fill_exif(ifd_t *e, const dng_params_t *p)
{
    int32_t n, dd;                         /* scratch for rational conversions  */
    if (p->exposure_ns > 0) {              /* shutter time, in seconds          */
        to_rational(p->exposure_ns / 1e9, &n, &dd, 0);
        e_rational(e, TAG_ExposureTime, (uint32_t)n, (uint32_t)dd);
    }
    if (p->iso > 0) e_short(e, TAG_ISO, (uint16_t)p->iso); /* ISO               */
    if (p->focal_mm > 0) {                 /* focal length, mm                  */
        to_rational(p->focal_mm, &n, &dd, 0);
        e_rational(e, TAG_FocalLength, (uint32_t)n, (uint32_t)dd);
    }
    if (p->aperture > 0) {                 /* f-number                          */
        to_rational(p->aperture, &n, &dd, 0);
        e_rational(e, TAG_FNumber, (uint32_t)n, (uint32_t)dd);
    }
    if (p->datetime && p->datetime[0]) {   /* capture time (same clock)         */
        e_ascii(e, TAG_DateTimeOriginal, p->datetime);
        e_ascii(e, TAG_DateTimeDigitized, p->datetime);
    }
    e_short(e, TAG_WhiteBalance, (uint16_t)(p->wb_manual ? 1 : 0));   /* 0 auto, 1 manual */
}

/* Write the whole DNG. See the file header for the layout. */
int dng_write(const char *path, const dng_params_t *p)
{
    ifd_t ifd0, exif;                      /* the two directories               */
    uint8_t *gm = NULL;                    /* the GainMap blob, if any          */
    uint8_t *filebuf = NULL;               /* the assembled header + IFDs       */
    size_t gm_len = 0;                     /* GainMap blob length               */
    size_t pixel_bytes;                    /* size of the pixel strip           */
    char tmp[1024];                        /* "<path>.part"                     */
    FILE *f = NULL;                        /* output stream                     */
    int err = 0;                           /* errno-style result                */
    uint32_t ifd0_pos = 8;                 /* IFD0 starts right after header    */
    size_t ifd0_body, exif_body;           /* directory body sizes              */
    uint32_t exif_pos, strip_pos;          /* file offsets we compute           */
    int final_n0;                          /* IFD0 entry count incl. offsets    */

    if (!path || !p || !p->pixels || p->width <= 0 || p->height <= 0) return -1;
    pixel_bytes = (size_t)p->width * p->height * 2; /* 16 bits per sample       */
    if (snprintf(tmp, sizeof tmp, "%s.part", path) >= (int)sizeof tmp) return -1;

    /* Build the GainMap blob once; it is referenced from IFD0's trailing blob. */
    if (p->shading.have && p->shading.gains && p->shading.cols > 0 && p->shading.rows > 0) {
        gm = build_gainmap(&p->shading, p->width, p->height, &gm_len);
        if (!gm) return ENOMEM;            /* out of memory                     */
    }

    /* Pass 1: build EXIF (it holds no offsets) to learn its body size. */
    memset(&exif, 0, sizeof exif);
    exif.trail_base = 0;                    /* fixed up before the real build    */
    fill_exif(&exif, p);
    exif_body = 2 + (size_t)exif.n * 12 + 4;

    /* Pass 1: build IFD0's known tags to count them and size its body. The
     * three offset-bearing tags are not added yet but are reserved here. */
    memset(&ifd0, 0, sizeof ifd0);
    ifd0.trail_base = 0;
    fill_ifd0(&ifd0, p);
    if (gm) entry(&ifd0, TAG_OpcodeList2, T_UNDEFINED, (uint32_t)gm_len, gm, gm_len);
    final_n0 = ifd0.n + 3;                 /* + ExifIFD, StripOffsets, StripByteCounts */
    ifd0_body = 2 + (size_t)final_n0 * 12 + 4;

    /* Pass 2: rebuild IFD0 with the correct trailing-blob base now known. The
     * pass-1 build may have allocated a trailing blob; free it before reusing. */
    free(ifd0.trail);
    memset(&ifd0, 0, sizeof ifd0);
    ifd0.trail_base = ifd0_pos + (uint32_t)ifd0_body; /* blob follows the body  */
    fill_ifd0(&ifd0, p);
    if (gm) entry(&ifd0, TAG_OpcodeList2, T_UNDEFINED, (uint32_t)gm_len, gm, gm_len);

    /* EXIF comes after IFD0 (body + its blob); place it and rebuild it. */
    exif_pos = ifd0.trail_base + (uint32_t)ifd0.trail_len;
    free(exif.trail);                      /* free the pass-1 EXIF blob          */
    memset(&exif, 0, sizeof exif);
    exif.trail_base = exif_pos + (uint32_t)exif_body; /* EXIF blob follows it    */
    fill_exif(&exif, p);

    /* The pixel strip comes after EXIF (body + its blob). */
    strip_pos = exif.trail_base + (uint32_t)exif.trail_len;

    /* Now add IFD0's three offset-bearing tags, pointing at what we placed. */
    e_long(&ifd0, TAG_ExifIFD, exif_pos);           /* link to the EXIF IFD      */
    e_long(&ifd0, TAG_StripOffsets, strip_pos);     /* where the pixels start    */
    e_long(&ifd0, TAG_StripByteCounts, (uint32_t)pixel_bytes); /* their size     */

    if (ifd0.oom || exif.oom) { err = ENOMEM; goto done; } /* any blob OOM       */

    /* Assemble the header and the two directories in one buffer, then write
     * it and the pixels straight from the caller's buffer: no second copy of
     * the image (100 MB at 8192x6144x16 bit; the caller already holds one),
     * which mattered on a 32-bit phone (0.47.58). The .part file is renamed
     * only once everything is written. */
    {
        filebuf = malloc(strip_pos);                /* header + IFDs + blobs     */
        if (!filebuf) { err = ENOMEM; goto done; }
        filebuf[0] = 'I'; filebuf[1] = 'I';         /* little-endian byte order  */
        put16(filebuf + 2, 42);                     /* TIFF magic number         */
        put32(filebuf + 4, ifd0_pos);               /* offset of IFD0            */
        emit_ifd(&ifd0, filebuf + ifd0_pos);        /* IFD0 body + blob          */
        emit_ifd(&exif, filebuf + exif_pos);        /* EXIF body + blob          */

        f = fopen(tmp, "wb");                        /* write to the .part file   */
        if (!f) { err = errno; goto done; }
        if (fwrite(filebuf, 1, strip_pos, f) != strip_pos ||          /* head  */
            fwrite(p->pixels, 1, pixel_bytes, f) != pixel_bytes) {    /* pixels */
            err = errno ? errno : EIO;
            fclose(f);
            f = NULL;
            goto done;
        }
        if (fclose(f) != 0) { err = errno; f = NULL; goto done; } /* flush        */
        f = NULL;
        if (rename(tmp, path) != 0) { err = errno; goto done; }  /* atomic swap   */
    }

done:
    if (f) fclose(f);                      /* close on an error path            */
    if (err) unlink(tmp);                  /* leave no partial file behind      */
    free(ifd0.trail);                      /* free the directory blobs          */
    free(exif.trail);
    free(gm);                              /* free the GainMap blob             */
    free(filebuf);                         /* free the file buffer              */
    return err;
}
