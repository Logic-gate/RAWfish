#!/usr/bin/env python3
"""Checks an indraw RAW16 capture on a PC and renders a quick-look PNG.

    python3 scripts/raw16-check.py FILE [--shading]

FILE is the pixel file written by `raw FILE`; FILE.json is its sidecar.
Prints the levels, the white balance and a summary of the lens shading map,
then writes FILE.png: a half-size rendering (each 2x2 Bayer cell -> 1 RGB
pixel), black-subtracted, white-balanced, simple gamma. --shading also
applies the lens shading map, to show the vignetting correction.
This is a sanity check, not a raw developer (no colour matrix, no denoise).
Needs numpy and Pillow.
"""
import json
import sys

import numpy as np
from PIL import Image


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    use_shading = "--shading" in sys.argv[2:]
    meta = json.load(open(path + ".json"))
    lay, st, res = meta["layout"], meta["static"], meta["result"]
    w, h = lay["width"], lay["height"]

    fmt = lay.get("format", "uint16le")
    dtype = "<u4" if fmt == "uint32le" else "<u2"
    raw = np.fromfile(path, dtype=dtype)
    if raw.size != w * h:
        sys.exit(f"size mismatch: {raw.size} values, expected {w}x{h}")
    raw = raw.reshape(h, w).astype(np.float32)
    # A brute stack sums N frames; scale black/white by the net frame-equivalent
    # (base = frames / stk_denoise) so the levels match the summed pixels.
    stack = meta.get("stack", {})
    nframes = stack.get("frames", 1)
    stk_denoise = stack.get("stk_denoise", 1)
    nscale = max(nframes // max(stk_denoise, 1), 1)

    # Levels: the per-frame (dynamic) values win over the static ones.
    black = res.get("SENSOR_DYNAMIC_BLACK_LEVEL", st.get("SENSOR_BLACK_LEVEL_PATTERN"))
    white = res.get("SENSOR_DYNAMIC_WHITE_LEVEL", st.get("SENSOR_INFO_WHITE_LEVEL"))
    if nframes > 1:
        black = [b * nscale for b in black] if isinstance(black, list) else black * nscale
        white = white * nscale
        print(f"stacked: {nframes} frames (stk_denoise {stk_denoise}), film exposure "
              f"{stack.get('film_exposure_ns', 0) / 1e9:.2f} s @ ISO {stack.get('iso')}")
    cfa = st.get("SENSOR_INFO_COLOR_FILTER_ARRANGEMENT")  # 0 RGGB 1 GRBG 2 GBRG 3 BGGR
    print(f"{w}x{h}  min {raw.min():.0f}  max {raw.max():.0f}  mean {raw.mean():.1f}")
    print(f"black {black}  white {white}  cfa {cfa}  matches_result {meta['matches_result']}")
    print(f"exposure {res.get('SENSOR_EXPOSURE_TIME')} ns  iso {res.get('SENSOR_SENSITIVITY')}"
          f"  wb gains {res.get('COLOR_CORRECTION_GAINS')}")
    clipped = (raw >= white).mean() * 100
    print(f"clipped pixels: {clipped:.2f} %")

    # Split the mosaic into its four 2x2 positions, in reading order.
    planes = [raw[0::2, 0::2], raw[0::2, 1::2], raw[1::2, 0::2], raw[1::2, 1::2]]
    planes = [np.clip((p - b) / (white - b), 0, None) for p, b in zip(planes, black)]
    # Name each position: which colour sits at (0,0), (0,1), (1,0), (1,1).
    order = {0: "RGGB", 1: "GRBG", 2: "GBRG", 3: "BGGR"}.get(cfa, "RGGB")

    # Lens shading map: 4 gains (R, Geven, Godd, B) per grid point, rows x cols.
    lsm = res.get("STATISTICS_LENS_SHADING_MAP")
    size = st.get("LENS_INFO_SHADING_MAP_SIZE")          # [columns, rows]
    if lsm and size:
        cols, rows = size
        m = np.array(lsm, dtype=np.float32).reshape(rows, cols, 4)
        corner = m[[0, 0, -1, -1], [0, -1, 0, -1]].mean(axis=0)
        centre = m[rows // 2, cols // 2]
        print(f"shading map {cols}x{rows}: gain at centre {np.round(centre, 2)}"
              f"  mean corner {np.round(corner, 2)}  (R, Ge, Go, B)")
        if use_shading:
            # Upsample the gain grid to each plane and multiply.
            ph, pw = planes[0].shape
            ys = np.linspace(0, rows - 1, ph)
            xs = np.linspace(0, cols - 1, pw)
            # Map each Bayer position to its gain channel (R=0, Ge=1, Go=2, B=3).
            chan = {"R": 0, "B": 3}
            greens = iter([1, 2])
            for i, c in enumerate(order):
                k = chan[c] if c in chan else next(greens)
                g = np.array([np.interp(xs, np.arange(cols), m[int(round(y)), :, k]) for y in ys])
                planes[i] = planes[i] * g
    else:
        print("no lens shading map in this capture")

    # Rebuild R, G, B planes from the mosaic positions.
    rgb = {"R": [], "G": [], "B": []}
    for i, c in enumerate(order):
        rgb[c].append(planes[i])
    r, g, b = rgb["R"][0], (rgb["G"][0] + rgb["G"][1]) / 2, rgb["B"][0]
    gains = res.get("COLOR_CORRECTION_GAINS", [1, 1, 1, 1])  # R, Geven, Godd, B
    img = np.dstack([r * gains[0], g * (gains[1] + gains[2]) / 2, b * gains[3]])
    img = img / max(np.percentile(img, 99.5), 1e-6)      # simple exposure
    img = np.clip(img, 0, 1) ** (1 / 2.2)                # simple gamma
    out = path + (".shading.png" if use_shading else ".png")
    Image.fromarray((img * 255 + 0.5).astype(np.uint8)).save(out)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
