#!/usr/bin/env python3
"""Palette Quantizer: raw RGBA PNGs -> indexed PNGs against the master palette.

Every pixel maps to the nearest palette color (index 0 is reserved for
transparency; alpha < 128 becomes 0). This is the stage that makes any input
— AI-generated art included — conform to the game's 256-color look.
"""
import os
import sys
from PIL import Image

def load_palette(path):
    with open(path) as f:
        header = f.readline().split()
        assert header[0] == "DUSTFALL-PAL" and int(header[1]) == 256
        return [tuple(int(v) for v in f.readline().split()) for _ in range(256)]

def quantize_file(src, dst, pal):
    img = Image.open(src).convert("RGBA")
    alpha = img.getchannel("A")

    # PIL nearest-color quantization against our palette (no dithering:
    # crisp retro pixels; dithering is an artistic choice we can add per-asset)
    pal_img = Image.new("P", (1, 1))
    flat = []
    for r, g, b in pal:
        flat += [r, g, b]
    pal_img.putpalette(flat)
    indexed = img.convert("RGB").quantize(palette=pal_img, dither=Image.Dither.NONE)

    # transparency: alpha < 128 -> index 0
    px = indexed.load()
    ap = alpha.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            if ap[x, y] < 128:
                px[x, y] = 0
            elif px[x, y] == 0:
                px[x, y] = 1  # opaque pixel that landed on the reserved index

    indexed.putpalette(flat)
    indexed.save(dst, transparency=0)

def main(raw_dir, out_dir, palette_path):
    pal = load_palette(palette_path)
    os.makedirs(out_dir, exist_ok=True)
    count = 0
    for name in sorted(os.listdir(raw_dir)):
        if not name.endswith(".png"):
            continue
        quantize_file(os.path.join(raw_dir, name), os.path.join(out_dir, name), pal)
        count += 1
    print(f"quantized {count} images -> {out_dir}")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
