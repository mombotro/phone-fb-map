#!/usr/bin/env python3
"""Draws the FB Tools icon (the same four colors as the picker tiles). usage: make_icon.py OUTDIR"""
import os, sys
from PIL import Image, ImageDraw

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
for size, name in ((120, "Icon-60@2x.png"), (60, "Icon-60.png")):
    img = Image.new("RGB", (size, size), (16, 24, 32))
    d = ImageDraw.Draw(img)
    m, g = size // 8, size // 14                      # margin, gap
    tile = (size - 2 * m - g) // 2
    colors = [(58, 110, 165), (58, 165, 110), (165, 58, 78), (165, 138, 58)]
    for i, c in enumerate(colors):
        x = m + (i % 2) * (tile + g)
        y = m + (i // 2) * (tile + g)
        d.rectangle([x, y, x + tile, y + tile], fill=c)
    img.save(os.path.join(out, name))
print("icons written to", out)
