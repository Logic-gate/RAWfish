#!/usr/bin/env python3
# The launcher icons at 86, 108, 128 and 172 px, filling the Sailfish
# template outline (Sailfish-Apps-icon-template, CC BY 3.0): the art of
# SRC (any size) cropped to its shape, scaled, then cut to the outline.
# Usage: python3 scripts/launcher-icons.py app/icons/rawfish-ir-172.png app/icons
import sys
from PIL import Image, ImageDraw
SRC, OUT = sys.argv[1], sys.argv[2]
S = 8
def shape(n):
    # The Sailfish template's outline (icon-launcher-template.svg, 86 px:
    # 0.3 .. 85.7, arcs of 42.7 at the top left and bottom right, corners of
    # 1.423 at the others), scaled to n.
    k = n / 86.0; m = Image.new('L', (n * S, n * S), 0); d = ImageDraw.Draw(m)
    a = 0.3 * k * S; b = 85.7 * k * S; R = 42.7 * k * S; r = 1.423 * k * S
    d.rectangle([a + R, a, b, b - R], 255); d.rectangle([a, a + R, b - R, b], 255)
    d.pieslice([a, a, a + 2 * R, a + 2 * R], 180, 270, 255); d.pieslice([b - 2 * R, b - 2 * R, b, b], 0, 90, 255)
    d.rectangle([a + R, a + R, b - R, b - R], 255)
    d.rectangle([b - R, a + r, b, b - R], 255); d.rectangle([a + R, a, b - r, a + R], 255); d.pieslice([b - 2 * r, a, b, a + 2 * r], 270, 360, 255)
    d.rectangle([a, a + R, a + r, b - r], 255); d.rectangle([a, a + R, a + R, b], 255); d.rectangle([a + r, b - r, a + R, b], 255); d.pieslice([a, b - 2 * r, a + 2 * r, b], 90, 180, 255)
    return m.resize((n, n), Image.LANCZOS)
src = Image.open(SRC).convert('RGBA')
art = src.crop(src.split()[3].point(lambda v: 255 if v > 200 else 0).getbbox()).convert('RGB')
for n in (86, 108, 128, 172):
    out = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    out.paste(art.resize((n, n), Image.LANCZOS).convert('RGBA'), (0, 0), shape(n))
    out.save('%s/rawfish-ir-%d.png' % (OUT, n), optimize=True)
