/* exifwrite.h -- rewrites the Exif APP1 of a JPEG the HAL produced: the
 * HAL's entries are KEPT (IFD0, the Exif IFD, IFD1 with its thumbnail --
 * what we do not know better is not touched), and what the engine knows it
 * really applied (exposure, ISO, focal length, date, make/model, software,
 * orientation, white balance) replaces the HAL's value or is added. The
 * Jolla Phone 2's HAL lacks ISO and exposure; the XA2's are its own guesses.
 * The Orientation tag is the HAL's when there is one (it may have rotated
 * the pixels or set the tag; we do not second-guess it), else derived from
 * the sensor mounting angle. */
#ifndef EXIFWRITE_H
#define EXIFWRITE_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *make, *model, *software;   /* ASCII, may be NULL              */
    const char *datetime;                  /* "YYYY:MM:DD HH:MM:SS"           */
    const char *description;               /* ASCII, may be NULL: written as ImageDescription,
                                            * UserComment and an XMP dc:description (the
                                            * fields viewers show; 400 characters at most) */
    int64_t exposure_ns;                   /* <= 0: not written                */
    int iso;                               /* <= 0: not written                */
    float focal_mm;                        /* <= 0: not written                */
    int focal_eq_mm;                       /* 35 mm equivalent; <= 0: not written */
    float f_number;                        /* <= 0: not written                */
    int orientation;                       /* EXIF 1..8; 0: not written        */
    int wb_manual;                         /* WhiteBalance: 1 manual, 0 auto   */
    int exposure_manual;                   /* ExposureMode: 1 manual pair, 0 auto */
    int flash;                             /* Exif Flash value (fired bit, mode bits); <= 0: not written, the HAL's kept */
    float digital_zoom;                    /* DigitalZoomRatio; <= 0: not written */
    int keep_existing;                     /* 1: only the tags given above are written over
                                            * the file's (not the ExposureMode, WhiteBalance,
                                            * versions and colour space written otherwise) */
    const uint8_t *thumb;                  /* a new Exif thumbnail (a JPEG), replacing the
                                            * HAL's; NULL: the HAL's kept (or none)  */
    size_t thumb_len;
    int *hal_dropped;                      /* out, optional: HAL entries left out for
                                            * lack of room (its IFD over 64 entries,
                                            * its data over the pool): 0 normally */
} exif_info_t;

/* Writes `jpeg` (len bytes, starting FF D8) to `path` with one Exif APP1:
 * the HAL's merged with ours (ours wins on the tags it carries), rewritten
 * little-endian with fresh offsets, then, with a description, an XMP APP1
 * carrying it (for our JPEGs from YUV, which have none of their own). Sub-IFDs
 * we do not rewrite (GPS, Interoperability) are dropped rather than left dangling; a maker note is
 * copied as bytes. Returns 0 or an errno value. `info->orientation` 0 lets
 * the HAL's tag through unchanged. */
int exif_write_jpeg(const char *path, const uint8_t *jpeg, size_t len, const exif_info_t *info);

/* The same for a NEW image that replaces a JPEG (`orig`, the file as it
 * was: a lens-corrected re-encoding of it): the Exif merged is orig's, and
 * orig's other metadata segments (ICC profile, XMP, a vendor's own...)
 * are carried over; its JFIF APP0 and MPF APP2 (offsets into the old
 * image) are not. Give info->thumb, or the old thumbnail stays. */
int exif_rewrite_jpeg(const char *path, const uint8_t *jpeg, size_t len,
                      const uint8_t *orig, size_t orig_len, const exif_info_t *info);

/* Reads the Orientation tag (1..8) of a JPEG's Exif APP1, 0 if none. */
int exif_read_orientation(const uint8_t *jpeg, size_t len);

#endif
