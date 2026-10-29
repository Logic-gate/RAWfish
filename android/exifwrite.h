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
    int64_t exposure_ns;                   /* <= 0: not written                */
    int iso;                               /* <= 0: not written                */
    float focal_mm;                        /* <= 0: not written                */
    int focal_eq_mm;                       /* 35 mm equivalent; <= 0: not written */
    float f_number;                        /* <= 0: not written                */
    int orientation;                       /* EXIF 1..8; 0: not written        */
    int wb_manual;                         /* WhiteBalance: 1 manual, 0 auto   */
    int exposure_manual;                   /* ExposureMode: 1 manual pair, 0 auto */
    int *hal_dropped;                      /* out, optional: HAL entries left out for
                                            * lack of room (its IFD over 64 entries,
                                            * its data over the pool): 0 normally */
} exif_info_t;

/* Writes `jpeg` (len bytes, starting FF D8) to `path` with one Exif APP1:
 * the HAL's merged with ours (ours wins on the tags it carries), rewritten
 * little-endian with fresh offsets. Sub-IFDs we do not rewrite (GPS,
 * Interoperability) are dropped rather than left dangling; a maker note is
 * copied as bytes. Returns 0 or an errno value. `info->orientation` 0 lets
 * the HAL's tag through unchanged. */
int exif_write_jpeg(const char *path, const uint8_t *jpeg, size_t len, const exif_info_t *info);

/* Reads the Orientation tag (1..8) of a JPEG's Exif APP1, 0 if none. */
int exif_read_orientation(const uint8_t *jpeg, size_t len);

#endif
