#!/usr/bin/env python3
"""Read noise vs ISO from dark-frame DNGs (finds the dual-conversion-gain threshold).

Usage:
    python3 dcg_dark_analysis.py DOSSIER [--roi 1024] [--pattern 'dark-iso_*-n_*.dng']
    python3 dcg_dark_analysis.py file1.dng file2.dng ...   (or a shell glob: folder/*.dng)

Expects files named dark-iso_<ISO>-n_<N>.dng (take them with the engine, lens covered)
(2 frames per ISO preferably: the standard deviation is taken on the difference of
the two frames / sqrt(2), which removes fixed-pattern noise, hot pixels, etc.
Avec une seule image par ISO, on utilise l'ecart-type de l'image seule.)

Outputs in the folder (or the first file's, or --out): dcg_results.csv and dcg_plot.png

Dependances : pip install rawpy numpy matplotlib
"""
import argparse
import csv
import glob
import math
import os
import re
import sys
from collections import defaultdict

import numpy as np
import rawpy


def load_channels(path, roi):
    """Retourne {nom_canal: vecteur de valeurs RAW} dans une zone centrale."""
    with rawpy.imread(path) as raw:
        img = raw.raw_image_visible
        col = raw.raw_colors_visible
        desc = raw.color_desc.decode()          # ex. "RGBG"
        h, w = img.shape
        r = min(roi, h, w) & ~1                  # even size, aligned on the Bayer pattern
        y0 = ((h - r) // 2) & ~1
        x0 = ((w - r) // 2) & ~1
        sub = img[y0:y0 + r, x0:x0 + r].astype(np.float64)
        subc = col[y0:y0 + r, x0:x0 + r]
        out = {}
        seen = defaultdict(int)
        for idx in range(len(desc)):
            name = desc[idx]
            seen[name] += 1
            label = name if desc.count(name) == 1 else f"{name}{seen[name]}"
            out[label] = sub[subc == idx]
        return out, (h, w, r)


def robust_sigma(x, k=5.0, iters=4):
    """Standard deviation after iterative rejection beyond k sigma (hot pixels).

    The MAD alone is avoided: on integer data (10-bit RAW) it is too coarse
    when the noise is only 1 to 2 DN.
    """
    x = x[np.isfinite(x)]
    m, s = np.median(x), x.std()
    for _ in range(iters):
        keep = np.abs(x - m) <= k * max(s, 0.5)
        if keep.all():
            break
        x = x[keep]
        m, s = x.mean(), x.std()
    return x.std()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+", help="un dossier, ou une liste de fichiers .dng")
    ap.add_argument("--out", help="output folder (default: the images' folder)")
    ap.add_argument("--roi", type=int, default=1024, help="side of the central area in pixels")
    ap.add_argument("--pattern", default="dark-iso_*-n_*.dng")
    args = ap.parse_args()

    if len(args.inputs) == 1 and os.path.isdir(args.inputs[0]):
        folder = args.inputs[0]
        files = sorted(glob.glob(os.path.join(folder, args.pattern)))
        if not files:
            sys.exit(f"No file '{args.pattern}' in {folder}")
    else:
        files = sorted(args.inputs)
        missing = [f for f in files if not os.path.isfile(f)]
        if missing:
            sys.exit(f"Fichier introuvable : {missing[0]}")
        folder = os.path.dirname(os.path.abspath(files[0]))
    outdir = args.out or folder
    os.makedirs(outdir, exist_ok=True)

    by_iso = defaultdict(list)
    for f in files:
        m = re.search(r"iso_(\d+)", os.path.basename(f))
        if m:
            by_iso[int(m.group(1))].append(f)
    if not by_iso:
        sys.exit("No file name contains 'iso_<number>': cannot tell the ISO.")
    isos = sorted(by_iso)
    iso_min = isos[0]

    rows = []
    channels = None
    for iso in isos:
        fl = by_iso[iso]
        data = [load_channels(f, args.roi)[0] for f in fl[:2]]
        channels = list(data[0].keys())
        for ch in channels:
            if len(data) >= 2:
                d = data[0][ch] - data[1][ch]
                s_std = d.std() / math.sqrt(2)
                s_clip = robust_sigma(d) / math.sqrt(2)
                mode = "diff"
            else:
                s_std = data[0][ch].std()
                s_clip = robust_sigma(data[0][ch])
                mode = "single"
            rows.append({
                "iso": iso, "channel": ch, "mean": data[0][ch].mean(),
                "sigma_dn": s_std, "sigma_dn_clipped": s_clip,
                "sigma_dn_norm": s_clip / (iso / iso_min), "mode": mode,
            })

    csv_path = os.path.join(outdir, "dcg_results.csv")
    with open(csv_path, "w", newline="") as fh:
        wr = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        wr.writeheader()
        wr.writerows(rows)

    # Summary: robust sigma, mean over channels, and sigma normalised by the ISO gain.
    print("ISO       : setting tested")
    print("sigma DN  : noise of the dark frames (RAW units)")
    print(f"sigma/gain: sigma DN divided by (ISO/{iso_min}) -> noise brought back to the base gain")
    print("ratio     : sigma/gain divided by the previous ISO's (1.0 = no change)\n")
    print(f"{'ISO':>6} {'sigma DN':>9} {'sigma/gain':>11} {'ratio':>9}")
    agg = []
    for iso in isos:
        s = np.mean([r["sigma_dn_clipped"] for r in rows if r["iso"] == iso])
        agg.append((iso, s, s / (iso / iso_min)))
    prev = None
    worst = None
    for iso, s, sn in agg:
        ratio = "" if prev is None else f"{sn / prev:.3f}"
        flag = "   <== clear drop" if prev is not None and sn / prev < 0.85 else ""
        if prev is not None and (worst is None or sn / prev < worst[2]):
            worst = (agg[[a[0] for a in agg].index(iso) - 1][0], iso, sn / prev)
        print(f"{iso:>6} {s:>9.3f} {sn:>11.3f} {ratio:>9}{flag}")
        prev = sn
    if worst:
        print(f"\nLargest drop of the normalised noise: between ISO {worst[0]} and ISO {worst[1]} "
              f"(x{worst[2]:.2f}).")
        print("A clear step (ratio well below 1, around 0.5-0.8) followed by a plateau "
              "is consistent with a switch to high conversion gain; a smooth drift is not.")
    print("Caution: with 10-bit RAW and ~1-2 DN of noise, the quantisation "
          "(~0.29 DN) slightly biases the lowest ISOs.")
    print(f"CSV: {csv_path}")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib absent : pas de graphique.")
        return
    fig, ax = plt.subplots(1, 2, figsize=(11, 4.2))
    for ch in channels:
        xs = [r["iso"] for r in rows if r["channel"] == ch]
        ax[0].plot(xs, [r["sigma_dn_clipped"] for r in rows if r["channel"] == ch], "o-", label=ch)
        ax[1].plot(xs, [r["sigma_dn_norm"] for r in rows if r["channel"] == ch], "o-", label=ch)
    for a, t, yl in ((ax[0], "Output noise", "sigma (DN)"),
                     (ax[1], f"Noise normalised by the ISO gain (ISO min = {iso_min})", "sigma / (ISO/ISOmin)")):
        a.set_xscale("log")
        a.set_xticks(isos)
        a.set_xticklabels([str(i) for i in isos], rotation=60, fontsize=8)
        a.minorticks_off()
        a.set_xlabel("ISO")
        a.set_ylabel(yl)
        a.set_title(t, fontsize=10)
        a.grid(True, which="both", alpha=0.3)
    ax[0].set_yscale("log")
    ax[0].legend()
    fig.tight_layout()
    png = os.path.join(outdir, "dcg_plot.png")
    fig.savefig(png, dpi=130)
    print(f"Graphique : {png}")


if __name__ == "__main__":
    main()
