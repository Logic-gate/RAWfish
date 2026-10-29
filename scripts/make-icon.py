#!/usr/bin/env python3
"""Generates the RAWfish-ir app icon at every size Sailfish OS uses.

The icon is a dark Sailfish-style tile (rounded, bottom-left and top-right
corners sharp) filled with coarse sensor noise and crossed by three analog
interference lines. The noise lives on a fixed 43x43 grid of blocks and is
scaled up with nearest-neighbour, so every size shows the same pattern; the
lines sit at fixed fractions of the height. Deterministic (fixed seed).

    python3 scripts/make-icon.py        # writes app/icons/rawfish-ir-<N>.png

Sizes: 86, 108, 128, 172 (the launcher picks the one for the screen's
scale). Requires numpy and Pillow.
"""
import os
import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = (86, 108, 128, 172)
GRID = 43                                   # noise blocks per side

rng = np.random.default_rng(7)
LUM = rng.normal(0, 1, (GRID, GRID))
CHROMA = rng.normal(0, 1, (GRID, GRID, 3))
STREAK = [np.convolve(rng.normal(0, 1, GRID * 4), np.ones(9) / 9, 'same') for _ in range(3)]
# the interference lines: (position as a fraction of the height, thickness
# as a fraction, brightness, sideways shift as a fraction of the width)
LINES = ((0.27, 0.025, 150, 0.06), (0.59, 0.012, 125, -0.05), (0.77, 0.03, 110, 0.035))


def bezier(p0, p1, p2, p3, n=80):
    t = np.linspace(0, 1, n)[:, None]
    return ((1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * p1 + 3 * (1 - t) * t * t * p2 + t ** 3 * p3).tolist()


def tile_mask(size):
    """The tile's shape, anti-aliased (drawn 4x, reduced)."""
    big = size * 4
    s = big / 172.0
    p = lambda x, y: np.array([x, y], float)
    pts = bezier(p(4, 86), p(4, 40), p(40, 4), p(86, 4)) + [[168, 4], [168, 86]] \
        + bezier(p(168, 86), p(168, 132), p(132, 168), p(86, 168)) + [[4, 168]]
    m = Image.new('L', (big, big), 0)
    ImageDraw.Draw(m).polygon([(x * s, y * s) for x, y in pts], fill=255)
    return np.asarray(m.resize((size, size), Image.LANCZOS)) / 255.0


def icon(size):
    idx = (np.arange(size) * GRID // size)               # nearest-neighbour block index
    lum = LUM[np.ix_(idx, idx)]
    chroma = CHROMA[idx][:, idx]
    img = np.array([18, 20, 27], float) + lum[..., None] * 22 + chroma * 6
    for (fy, fh, amp, fshift), streak in zip(LINES, STREAK):
        y0 = int(fy * size)
        h = max(1, int(round(fh * size)))
        shift = int(round(fshift * size))
        sx = streak[(np.arange(size) * len(streak)) // size]
        for y in range(y0, min(size, y0 + h)):
            img[y] = np.roll(img[y], shift, axis=0)
            img[y] += amp * (0.6 + 0.4 * sx)[:, None]
            img[y, :, 0] += 12                            # a warm line ...
        if y0 > 0:
            img[y0 - 1, :, 2] += amp * 0.35               # ... with a blue fringe above
        if y0 + h < size:
            img[y0 + h] += amp * 0.25                     # and a faint echo below
    img = np.clip(img, 0, 255)
    return Image.fromarray(np.dstack([img, tile_mask(size) * 255]).astype(np.uint8), 'RGBA')


if __name__ == '__main__':
    for n in SIZES:
        path = os.path.join(ROOT, 'app', 'icons', 'rawfish-ir-%d.png' % n)
        icon(n).save(path)
        print('wrote', path)
