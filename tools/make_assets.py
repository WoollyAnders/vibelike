#!/usr/bin/env python3
"""Generates DUSTFALL's placeholder art: palette.pal, tile and sprite sheets.

The palette written here is the single source of truth — the engine loads
data/palette.pal, and every PNG in assets/ must use only these colors
(index 0 = transparent for sprites).
"""
import os
import random
from PIL import Image

OUT = os.path.join(os.path.dirname(__file__), "..", "assets")
random.seed(1965)  # deterministic art

# ---- palette (mirrors the ramps designed in E1) ------------------------------

def build_palette():
    pal = [(0, 0, 0)] * 256
    def set_(i, r, g, b):
        pal[i] = (max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)))
    # 0: transparent key
    set_(0, 255, 0, 255)  # magenta in the .pal for visibility; engine treats 0 as transparent
    # 1..31 grayscale
    for i in range(1, 32):
        v = i * 255 // 31
        set_(i, v, v, v)
    # 32..95 sand
    for i in range(64):
        t = i * 255 // 63
        set_(32 + i, 60 + t * 195 // 255, 40 + t * 175 // 255, 20 + t * 120 // 255)
    # 96..127 rock
    for i in range(32):
        t = i * 255 // 31
        set_(96 + i, 40 + t * 120 // 255, 35 + t * 110 // 255, 32 + t * 105 // 255)
    # 128..159 spice
    for i in range(32):
        t = i * 255 // 31
        set_(128 + i, 70 + t * 185 // 255, 25 + t * 105 // 255, 10 + t * 30 // 255)
    # 160..191 night blues
    for i in range(32):
        t = i * 255 // 31
        set_(160 + i, 8 + t * 90 // 255, 12 + t * 120 // 255, 30 + t * 190 // 255)
    # 192..223 blood reds
    for i in range(32):
        t = i * 255 // 31
        set_(192 + i, 40 + t * 215 // 255, 5 + t * 40 // 255, 5 + t * 35 // 255)
    # 224..239 UI gold, 240..255 UI green
    for i in range(16):
        t = i * 255 // 15
        set_(224 + i, 90 + t * 165 // 255, 75 + t * 150 // 255, 20 + t * 60 // 255)
    for i in range(16):
        t = i * 255 // 15
        set_(240 + i, 15 + t * 85 // 255, 50 + t * 170 // 255, 25 + t * 95 // 255)
    return pal

PAL = build_palette()

def write_pal(path):
    with open(path, "w") as f:
        f.write("DUSTFALL-PAL 256\n")
        for r, g, b in PAL:
            f.write(f"{r} {g} {b}\n")

def new_indexed(w, h):
    img = Image.new("P", (w, h), 0)
    flat = []
    for r, g, b in PAL:
        flat += [r, g, b]
    img.putpalette(flat)
    return img

# ---- tiles: 32x16 diamonds ---------------------------------------------------

def diamond_rows(w=32, h=16):
    """Yield (y, x0, x1) spans covering an iso diamond of w x h."""
    for y in range(h):
        half = (y + 1) * 2 if y < h // 2 else (h - y) * 2
        x0 = w // 2 - half
        x1 = w // 2 + half
        yield y, x0, x1

def make_floor(base, spread, speckle=None, speckle_p=0.0, edge_dark=2):
    img = new_indexed(32, 16)
    px = img.load()
    for y, x0, x1 in diamond_rows():
        for x in range(x0, x1):
            idx = base + random.randint(-spread, spread)
            if speckle and random.random() < speckle_p:
                idx = speckle + random.randint(-2, 2)
            if x < x0 + 2 or x > x1 - 3:
                idx -= edge_dark
            px[x, y] = max(1, min(255, idx))
    return img

def make_highlight():
    img = new_indexed(32, 16)
    px = img.load()
    for y, x0, x1 in diamond_rows():
        px[x0, y] = 31
        px[x0 + 1, y] = 31
        px[x1 - 1, y] = 31
        px[x1 - 2, y] = 31
        if y in (0, 15):
            for x in range(x0, x1):
                px[x, y] = 31
    return img

def sheet_from(frames, fw, fh):
    sheet = new_indexed(fw * len(frames), fh)
    for i, f in enumerate(frames):
        sheet.paste(f, (i * fw, 0))
    return sheet

def make_tiles():
    tiles = [
        make_floor(70, 4),                      # 0 sand a
        make_floor(66, 5),                      # 1 sand b
        make_floor(74, 3),                      # 2 sand c (brighter)
        make_floor(112, 5, edge_dark=3),        # 3 rock a
        make_floor(106, 6, edge_dark=3),        # 4 rock b
        make_floor(68, 4, speckle=146, speckle_p=0.22),  # 5 spice-flecked sand
        make_floor(60, 3, edge_dark=1),         # 6 drum sand (dimmer, ringed)
        make_highlight(),                        # 7 selection highlight
    ]
    # drum sand: concentric arcs
    px = tiles[6].load()
    for y, x0, x1 in diamond_rows():
        for x in range(x0, x1):
            d = abs(x - 16) + abs((y - 8) * 2)
            if d in (6, 12):
                px[x, y] = 52
    return sheet_from(tiles, 32, 16)

# ---- sprites -----------------------------------------------------------------

def make_player():
    """16x32 robed figure, 4 walk frames (simple bob + feet swap)."""
    frames = []
    for i in range(4):
        img = new_indexed(16, 32)
        px = img.load()
        bob = 1 if i % 2 == 1 else 0
        # robe: trapezoid, rock-light color
        for y in range(12 + bob, 28):
            half = 3 + (y - 12) // 4
            for x in range(8 - half, 8 + half):
                px[x, y] = 122 - (y - 12) // 3
        # head wrap
        for y in range(4 + bob, 12 + bob):
            for x in range(5, 11):
                px[x, y] = 88  # dusky sand tone
        # face slit
        for x in range(6, 10):
            px[x, 8 + bob] = 8
        # eyes: blue-within-blue, naturally
        px[6, 8 + bob] = 180
        px[9, 8 + bob] = 180
        # feet alternate
        if i in (0, 2):
            for x in range(4, 7): px[x, 29] = 10
            for x in range(9, 12): px[x, 30] = 10
        else:
            for x in range(4, 7): px[x, 30] = 10
            for x in range(9, 12): px[x, 29] = 10
        frames.append(img)
    return sheet_from(frames, 16, 32)

def make_rock_prop():
    """32x32 rock outcrop, 1 frame."""
    img = new_indexed(32, 32)
    px = img.load()
    import math
    for y in range(10, 30):
        for x in range(3, 29):
            dx, dy = (x - 16) / 13.0, (y - 22) / 9.0
            if dx * dx + dy * dy < 1.0:
                shade = 118 - int(dy * 6) - int(dx * 4)
                px[x, y] = max(97, min(127, shade + random.randint(-2, 2)))
    return img

def main():
    os.makedirs(OUT, exist_ok=True)
    write_pal(os.path.join(OUT, "palette.pal"))
    make_tiles().save(os.path.join(OUT, "tiles.png"), transparency=0)
    make_player().save(os.path.join(OUT, "player.png"), transparency=0)
    make_rock_prop().save(os.path.join(OUT, "rock.png"), transparency=0)
    with open(os.path.join(OUT, "manifest.txt"), "w") as f:
        f.write("# name  file  frame_w frame_h frames  anchor_x anchor_y\n")
        f.write("tiles   tiles.png   32 16 8   16 8\n")
        f.write("player  player.png  16 32 4   8 30\n")
        f.write("rock    rock.png    32 32 1   16 26\n")
    print("assets written to", os.path.abspath(OUT))

if __name__ == "__main__":
    main()
