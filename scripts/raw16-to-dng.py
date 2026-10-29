#!/usr/bin/env python3
"""Wraps an indraw RAW16 frame or a stack into a DNG, on a PC.

    python3 scripts/raw16-to-dng.py FILE [-o OUT.dng] [--no-shading]

FILE is the pixel file from `raw FILE` (uint16) or `stack FILE` (uint32);
FILE.json is its sidecar. A DNG is a TIFF with a few extra tags, so this
writes one directly, no libtiff. It carries everything a raw developer
(darktable, Lightroom, RawTherapee) needs:
  - the sensor pixels (a stack is scaled back down to 16 bits)
  - black / white levels and the Bayer pattern (the sensor's, from the sidecar)
  - both colour calibrations (illuminant, colour matrix, forward matrix)
  - the as-shot white balance (from the neutral colour point)
  - the lens shading map as an OpcodeList2 GainMap, so the vignetting is
    corrected by the developer (the RAW itself is NOT corrected); skip it
    with --no-shading
  - EXIF: exposure, ISO, focal length, aperture, and orientation
Needs numpy. This is a container writer, not a developer: it does no
demosaic, denoise or tone mapping.
"""
import json
import struct
import sys

import numpy as np

# TIFF/DNG type codes and their struct formats + byte sizes.
BYTE, ASCII, SHORT, LONG, RATIONAL, SBYTE, UNDEFINED, SSHORT, SLONG, SRATIONAL, FLOAT, DOUBLE = range(1, 13)
TYPE = {BYTE: ("B", 1), ASCII: ("B", 1), SHORT: ("H", 2), LONG: ("I", 4),
        RATIONAL: ("I", 4), UNDEFINED: ("B", 1), SLONG: ("i", 4),
        SRATIONAL: ("i", 4), FLOAT: ("f", 4), DOUBLE: ("d", 8)}


class IFD:
    """One TIFF image file directory, collecting tags then serialising them."""

    def __init__(self):
        self.entries = {}   # tag -> (type, values tuple)

    def set(self, tag, typ, *values):
        if len(values) == 1 and isinstance(values[0], (list, tuple)):
            values = tuple(values[0])
        self.entries[tag] = (typ, values)

    def _rat(self, x, signed=False):
        """Approximate a float as a fraction. Uses a smaller denominator for
        big magnitudes so numerator x denominator stays in 32-bit range."""
        den = 1000000
        num = int(round(x * den))
        lo, hi = (-(2**31), 2**31 - 1) if signed else (0, 2**32 - 1)
        while (num < lo or num > hi) and den > 1:
            den //= 10
            num = int(round(x * den))
        return (max(lo, min(hi, num)), den)

    def set_rational(self, tag, values, signed=False):
        flat = []
        for v in values:
            n, d = self._rat(v, signed)
            flat += [n, d]
        self.set(tag, SRATIONAL if signed else RATIONAL, flat)

    def serialise(self, start_offset):
        """Returns (ifd_bytes, trailing_bytes, next_offset_placeholder_pos).

        Values that don't fit in 4 bytes go into trailing_bytes, referenced
        by an offset; this follows the TIFF layout rules.
        """
        tags = sorted(self.entries)
        n = len(tags)
        # IFD = 2-byte count + 12 bytes per entry + 4-byte next-IFD offset.
        ifd_size = 2 + 12 * n + 4
        trailing = bytearray()
        trailing_base = start_offset + ifd_size
        body = bytearray()
        body += struct.pack("<H", n)
        for tag in tags:
            typ, values = self.entries[tag]
            single, size = TYPE[typ]
            if typ == ASCII:
                raw = values[0].encode() + b"\0" if isinstance(values[0], str) else bytes(values)
                count = len(raw)
                payload = raw
            else:
                # RATIONAL/SRATIONAL already carry 2 ints per value in `values`;
                # count is the number of rationals, the rest are plain scalars.
                count = len(values) // 2 if typ in (RATIONAL, SRATIONAL) else len(values)
                payload = struct.pack("<" + single * len(values), *values)
            total = len(payload)
            if total <= 4:
                value_field = payload + b"\0" * (4 - total)
            else:
                value_field = struct.pack("<I", trailing_base + len(trailing))
                trailing += payload
                if len(trailing) % 2:
                    trailing += b"\0"
            body += struct.pack("<HHI", tag, typ, count) + value_field
        next_off_pos = len(body)
        body += struct.pack("<I", 0)   # next IFD: patched by caller if needed
        return bytes(body), bytes(trailing), next_off_pos


# DNG-specific tag numbers.
T = dict(NewSubfileType=254, ImageWidth=256, ImageLength=257, BitsPerSample=258,
         Compression=259, PhotometricInterpretation=262, Make=271, Model=272,
         StripOffsets=273, Orientation=274, SamplesPerPixel=277, RowsPerStrip=278,
         StripByteCounts=279, PlanarConfiguration=284, Software=305,
         CFARepeatPatternDim=33421, CFAPattern=33422,
         DNGVersion=50706, DNGBackwardVersion=50707, UniqueCameraModel=50708,
         CFAPlaneColor=50710, CFALayout=50711, BlackLevel=50714, WhiteLevel=50717,
         DefaultScale=50718, DefaultCropOrigin=50719, DefaultCropSize=50720,
         ColorMatrix1=50721, ColorMatrix2=50722, CameraCalibration1=50723,
         CameraCalibration2=50724, AnalogBalance=50727, AsShotNeutral=50728,
         BaselineExposure=50730, CalibrationIlluminant1=50778,
         CalibrationIlluminant2=50779, ForwardMatrix1=50964, ForwardMatrix2=50965,
         OpcodeList2=51009, NoiseProfile=51041,
         ExposureTime=33434, ISOSpeedRatings=34855, FocalLength=37386,
         FNumber=33437, ExifTag=34665)


def flat_matrix(m):
    """[[num, den], ...] (9 entries) -> list of 9 floats."""
    return [a / b for a, b in m]


# The sensor's colour filter arrangement (SENSOR_INFO_COLOR_FILTER_ARRANGEMENT,
# 0 RGGB 1 GRBG 2 GBRG 3 BGGR) as the DNG CFAPattern colours of its 2x2 cell
# (0 red, 1 green, 2 blue), row by row. Until 0.1.1 this script wrote BGGR for
# every phone (October 2026 audit).
CFA_COLOURS = {0: (0, 1, 1, 2), 1: (1, 0, 2, 1), 2: (1, 2, 0, 1), 3: (2, 1, 1, 0)}
# The photo's rotation (degrees clockwise to view it upright) as a TIFF Orientation.
ORIENTATION_TAG = {0: 1, 90: 6, 180: 3, 270: 8}


def gainmap_opcode(shading_map, cols, rows, width, height, cfa=3):
    """Builds an OpcodeList2 blob with four GainMap opcodes (one per CFA
    plane), so the developer multiplies the RAW by these per-position gains
    and removes the vignetting. Big-endian, as the DNG opcode spec requires."""
    import struct as _s
    m = np.array(shading_map, dtype=np.float32).reshape(rows, cols, 4)
    # DNG GainMap planes map to CFA positions; the AOSP map order is
    # (R, G on even rows, G on odd rows, B): each Bayer position takes the
    # channel of its colour (and, for a green, of its row's parity).
    colours = CFA_COLOURS.get(cfa, CFA_COLOURS[3])
    planes = []   # (row0, col0, channel in the shading map)
    for k, (row0, col0) in enumerate(((0, 0), (0, 1), (1, 0), (1, 1))):
        c = colours[k]
        planes.append((row0, col0, 0 if c == 0 else 3 if c == 2 else (1 if row0 == 0 else 2)))
    blob = _s.pack(">I", len(planes))           # opcode count
    for row0, col0, ch in planes:
        gains = m[:, :, ch].astype(">f4")
        # GainMap opcode payload, big-endian. This matches, field type by
        # field type, the encoder in RAWfish's dnglensshading.cpp (its
        # writeGainMapOpcode), which is the layout darktable actually reads on
        # this phone. The mix of int and double is deliberate and was taken
        # from that source, not guessed:
        #   Top, Left, Bottom, Right : uint32 (PIXELS; Bottom/Right = H/W)
        #   Plane, Planes, RowPitch, ColPitch : uint32  (pitch 2 = one CFA phase)
        #   MapPointsV, MapPointsH : uint32
        #   MapSpacingV, MapSpacingH : DOUBLE (normalised, 1/(points-1))
        #   MapOriginV, MapOriginH : DOUBLE (0)
        #   MapPlanes : uint32
        #   gains : float32
        payload  = _s.pack(">IIII", row0, col0, height, width)
        payload += _s.pack(">IIII", 0, 1, 2, 2)
        payload += _s.pack(">II", rows, cols)
        payload += _s.pack(">dd", 1.0 / (rows - 1), 1.0 / (cols - 1))
        payload += _s.pack(">dd", 0.0, 0.0)
        payload += _s.pack(">I", 1)
        payload += gains.tobytes()
        # opcode header: id=9 (GainMap), DNG version 1.3.0.0, flags (1 = optional/
        # skippable by an older reader), payload size.
        blob += _s.pack(">IIII", 9, 0x01030000, 1, len(payload)) + payload
    return blob


def main():
    args = sys.argv[1:]
    out = None
    shading = True
    files = []
    i = 0
    while i < len(args):
        if args[i] == "-o":
            out = args[i + 1]; i += 2
        elif args[i] == "--no-shading":
            shading = False; i += 1
        else:
            files.append(args[i]); i += 1
    if not files:
        sys.exit(__doc__)
    path = files[0]
    out = out or path.rsplit(".", 1)[0] + ".dng"
    meta = json.load(open(path + ".json"))
    lay, st, res = meta["layout"], meta["static"], meta["result"]
    w, h = lay["width"], lay["height"]
    cfa = int(st.get("SENSOR_INFO_COLOR_FILTER_ARRANGEMENT", 3))   # 3 = BGGR (sidecars before 0.1.1: the JP2)
    make = meta.get("make") or "Jolla"         # sidecars before 0.1.1 had no make/model: the JP2's
    model = meta.get("model") or "JP2601"
    if cfa not in CFA_COLOURS:
        sys.exit("unsupported colour filter arrangement %d (not a 2x2 Bayer RGB)" % cfa)

    stack = meta.get("stack", {})
    nframes = stack.get("frames", 1)
    dtype = "<u4" if lay.get("format") == "uint32le" else "<u2"
    px = np.fromfile(path, dtype=dtype).reshape(h, w).astype(np.float64)

    black = st.get("SENSOR_BLACK_LEVEL_PATTERN", [64, 64, 64, 64])
    white = st.get("SENSOR_INFO_WHITE_LEVEL", 1023)
    # A brute stack sums N frames: divide by stk_denoise (and by any power of two
    # needed to fit 16 bits) so the pixels carry the film-equivalent brightness,
    # and multiply the levels by that same net frame-equivalent -- lossless, and
    # the exposure stays truthful (same logic as the on-phone writer).
    if nframes > 1:
        stk_denoise = max(int(stack.get("stk_denoise", 1)), 1)
        # never below one frame-equivalent: a stack shorter than stk_denoise
        # (an abort, or 8 frames at denoise 16) made base 0, so black and
        # white came out 0 and the DNG unusable (October 2026 audit)
        base = max(nframes // stk_denoise, 1)
        shift = 0
        level_mul = base
        while 1023 * level_mul > 65535:
            shift += 1
            level_mul >>= 1
        div = max(stk_denoise << shift, 1)
        px = np.round(px / div)
        black = [b * level_mul for b in black] if isinstance(black, list) else black * level_mul
        white = white * level_mul
    px = np.clip(px, 0, 65535).astype("<u2")

    ifd = IFD()
    ifd.set(T["NewSubfileType"], LONG, 0)
    ifd.set(T["ImageWidth"], LONG, w)
    ifd.set(T["ImageLength"], LONG, h)
    ifd.set(T["BitsPerSample"], SHORT, 16)
    ifd.set(T["Compression"], SHORT, 1)             # uncompressed
    ifd.set(T["PhotometricInterpretation"], SHORT, 32803)  # CFA
    ifd.set(T["Make"], ASCII, make)
    ifd.set(T["Model"], ASCII, model)
    ifd.set(T["Orientation"], SHORT, ORIENTATION_TAG.get(int(meta.get("photo_orientation", 0)) % 360, 1))
    ifd.set(T["SamplesPerPixel"], SHORT, 1)
    ifd.set(T["RowsPerStrip"], LONG, h)
    ifd.set(T["PlanarConfiguration"], SHORT, 1)
    ifd.set(T["Software"], ASCII, "rawfish-ir")
    ifd.set(T["CFARepeatPatternDim"], SHORT, 2, 2)
    ifd.set(T["CFAPattern"], BYTE, *CFA_COLOURS.get(cfa, CFA_COLOURS[3]))   # the sensor's own
    ifd.set(T["DNGVersion"], BYTE, 1, 4, 0, 0)
    ifd.set(T["DNGBackwardVersion"], BYTE, 1, 3, 0, 0)
    ifd.set(T["UniqueCameraModel"], ASCII, "%s %s" % (make, model))
    ifd.set(T["CFAPlaneColor"], BYTE, 0, 1, 2)      # R,G,B
    ifd.set(T["CFALayout"], SHORT, 1)
    ifd.set(T["BlackLevel"], SHORT, int(round(sum(black) / len(black))))
    ifd.set(T["WhiteLevel"], LONG, int(white))
    ifd.set(T["DefaultCropOrigin"], LONG, 0, 0)
    ifd.set(T["DefaultCropSize"], LONG, w, h)
    # Colour calibration: two illuminants, as the sensor reports them.
    ifd.set(T["CalibrationIlluminant1"], SHORT, st.get("SENSOR_REFERENCE_ILLUMINANT1", 21))
    ifd.set(T["CalibrationIlluminant2"], SHORT, st.get("SENSOR_REFERENCE_ILLUMINANT2", 17))
    ifd.set_rational(T["ColorMatrix1"], flat_matrix(st["SENSOR_COLOR_TRANSFORM1"]), signed=True)
    ifd.set_rational(T["ColorMatrix2"], flat_matrix(st["SENSOR_COLOR_TRANSFORM2"]), signed=True)
    ifd.set_rational(T["CameraCalibration1"], flat_matrix(st["SENSOR_CALIBRATION_TRANSFORM1"]), signed=True)
    ifd.set_rational(T["CameraCalibration2"], flat_matrix(st["SENSOR_CALIBRATION_TRANSFORM2"]), signed=True)
    ifd.set_rational(T["ForwardMatrix1"], flat_matrix(st["SENSOR_FORWARD_MATRIX1"]), signed=True)
    ifd.set_rational(T["ForwardMatrix2"], flat_matrix(st["SENSOR_FORWARD_MATRIX2"]), signed=True)
    ifd.set_rational(T["AnalogBalance"], [1.0, 1.0, 1.0])
    # As-shot white balance from the neutral colour point (R,G,B rationals).
    ncp = res.get("SENSOR_NEUTRAL_COLOR_POINT")
    if ncp:
        neutral = [a / b for a, b in ncp]
        ifd.set_rational(T["AsShotNeutral"], neutral)
    # Noise profile: pairs of (scale, offset) doubles, as DNG expects.
    noise = res.get("SENSOR_NOISE_PROFILE")
    if noise:
        ifd.set(T["NoiseProfile"], DOUBLE, noise[:2])

    # Lens shading -> GainMap opcode (unless disabled or absent).
    smap = res.get("STATISTICS_LENS_SHADING_MAP")
    smsize = st.get("LENS_INFO_SHADING_MAP_SIZE")
    if shading and smap and smsize:
        cols, rows = smsize
        blob = gainmap_opcode(smap, cols, rows, w, h, cfa)
        ifd.set(T["OpcodeList2"], UNDEFINED, *blob)

    # EXIF sub-IFD.
    exif = IFD()
    et = res.get("SENSOR_EXPOSURE_TIME")
    if et:
        exif.set_rational(T["ExposureTime"], [et / 1e9])
    iso = meta.get("stack", {}).get("iso") or res.get("SENSOR_SENSITIVITY")
    if iso:
        exif.set(T["ISOSpeedRatings"], SHORT, int(iso))
    fl = res.get("LENS_FOCAL_LENGTH")
    if fl:
        exif.set_rational(T["FocalLength"], [fl])
    fn = res.get("LENS_APERTURE")
    if fn:
        exif.set_rational(T["FNumber"], [fn])

    # --- Assemble the file: header, IFD0, EXIF IFD, pixel strip. ---
    # Header is 8 bytes; IFD0 starts at 8.
    pixels = px.tobytes()
    # First pass with placeholder offsets to learn sizes.
    ifd0_bytes, ifd0_trail, ifd0_next_pos = ifd.serialise(8)
    exif_offset = 8 + len(ifd0_bytes) + len(ifd0_trail)
    exif_bytes, exif_trail, _ = exif.serialise(exif_offset)
    strip_offset = exif_offset + len(exif_bytes) + len(exif_trail)

    # Now set the offsets that depend on the layout and re-serialise.
    ifd.set(T["ExifTag"], LONG, exif_offset)
    ifd.set(T["StripOffsets"], LONG, strip_offset)
    ifd.set(T["StripByteCounts"], LONG, len(pixels))
    ifd0_bytes, ifd0_trail, _ = ifd.serialise(8)
    exif_offset = 8 + len(ifd0_bytes) + len(ifd0_trail)
    exif_bytes, exif_trail, _ = exif.serialise(exif_offset)
    strip_offset = exif_offset + len(exif_bytes) + len(exif_trail)
    # ExifTag / StripOffsets may have shifted by a few bytes; fix and redo once.
    ifd.set(T["ExifTag"], LONG, exif_offset)
    ifd.set(T["StripOffsets"], LONG, strip_offset)
    ifd0_bytes, ifd0_trail, _ = ifd.serialise(8)

    with open(out, "wb") as f:
        f.write(b"II" + struct.pack("<HI", 42, 8))     # little-endian TIFF, IFD0 at 8
        f.write(ifd0_bytes)
        f.write(ifd0_trail)
        f.write(exif_bytes)
        f.write(exif_trail)
        f.write(pixels)
    print(f"wrote {out}  ({w}x{h}, {nframes} frame(s), "
          f"shading {'on' if shading and smap else 'off'})")


if __name__ == "__main__":
    main()
