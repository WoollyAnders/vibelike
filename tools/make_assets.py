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
        make_floor(100, 3, edge_dark=2),        # 8 sietch stone floor
        make_floor(103, 2, edge_dark=2),        # 9 sietch door pad
    ]
    # drum sand: concentric arcs
    px = tiles[6].load()
    for y, x0, x1 in diamond_rows():
        for x in range(x0, x1):
            d = abs(x - 16) + abs((y - 8) * 2)
            if d in (6, 12):
                px[x, y] = 52
    # door pad: gold inlay border
    px = tiles[9].load()
    for y, x0, x1 in diamond_rows():
        px[x0, y] = 228
        px[x1 - 1, y] = 228
    return sheet_from(tiles, 32, 16)

def make_hound():
    """16x16 dust hound, 2 frames (lope)."""
    frames = []
    for i in range(2):
        img = new_indexed(16, 16)
        px = img.load()
        lift = 1 if i == 1 else 0
        # lean body
        for y in range(6, 12):
            for x in range(2, 13):
                if (x + y) % 7 != 0:
                    px[x, y - lift] = 100 + ((x * 3 + y) % 5)
        # head
        for y in range(4, 8):
            for x in range(11, 15):
                px[x, y - lift] = 104
        px[14, 5 - lift] = 210  # eye ember
        # legs
        for x in (3, 6, 9, 12):
            px[x, 12 + (i if x % 2 else -i) % 2] = 98
            px[x, 13] = 98
        frames.append(img)
    return sheet_from(frames, 16, 16)

def make_corpse():
    """16x10 harvestable corpse mound."""
    img = new_indexed(16, 10)
    px = img.load()
    for y in range(3, 9):
        for x in range(2, 14):
            dx, dy = (x - 8) / 6.5, (y - 6) / 3.2
            if dx * dx + dy * dy < 1.0:
                px[x, y] = 96 + int((1 - dy) * 4)
    px[6, 4] = 200
    px[9, 5] = 198
    return img

def make_wall():
    """32x40 sietch wall block (fills a diamond footprint, tall)."""
    img = new_indexed(32, 40)
    px = img.load()
    for y, x0, x1 in diamond_rows():
        for x in range(x0, x1):
            # extrude the diamond upward 24px
            for h in range(25):
                yy = y + 24 - h
                if 0 <= yy < 40:
                    shade = 108 if h > 0 else 116
                    if x < x0 + 3 or x > x1 - 4:
                        shade -= 6
                    px[x, yy] = shade + ((x * 7 + yy * 3) % 3)
    # top face brighter
    for y, x0, x1 in diamond_rows():
        for x in range(x0, x1):
            if y < 14:
                px[x, y] = 118 + ((x + y) % 3)
    return img

def make_cistern():
    """24x24 water cistern."""
    img = new_indexed(24, 24)
    px = img.load()
    for y in range(8, 22):
        for x in range(3, 21):
            dx, dy = (x - 12) / 9.5, (y - 15) / 7.5
            if dx * dx + dy * dy < 1.0:
                px[x, y] = 102
    for y in range(9, 14):
        for x in range(6, 18):
            dx, dy = (x - 12) / 6.5, (y - 11.5) / 2.8
            if dx * dx + dy * dy < 1.0:
                px[x, y] = 168 + int(2 * math.sin(x))  # water shimmer blues
    return img

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

# ---- sfx: 8-bit 22050Hz mono WAVs --------------------------------------------

import math
import wave
import struct

SFX_RATE = 22050

def write_wav8(path, samples):
    """samples: floats -1..1 -> 8-bit unsigned mono WAV."""
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(SFX_RATE)
        data = bytes(max(0, min(255, int((s * 0.95 + 1.0) * 127.5))) for s in samples)
        w.writeframes(data)

def make_worm_roar():
    """1.8s descending growl: pitch-swept saw + heavy noise, crunchy."""
    n = int(1.8 * SFX_RATE)
    out = []
    phase = 0.0
    lp = 0.0
    rng = random.Random(7)
    for i in range(n):
        t = i / n
        freq = 90.0 * (1.0 - t * 0.7) + 18.0 * math.sin(t * 21.0)
        phase += freq / SFX_RATE
        saw = 2.0 * (phase - math.floor(phase)) - 1.0
        noise = rng.uniform(-1, 1)
        lp += (noise - lp) * 0.12
        env = math.sin(min(t * 4.0, 1.0) * math.pi / 2) * (1.0 - t) ** 0.6
        s = (saw * 0.6 + lp * 0.9) * env
        s = max(-1.0, min(1.0, s * 2.2))  # drive into clipping for grit
        out.append(s)
    return out

def make_blip():
    n = int(0.09 * SFX_RATE)
    out = []
    for i in range(n):
        t = i / n
        f = 880.0 if t < 0.5 else 1174.0
        s = 1.0 if (i * f / SFX_RATE) % 1.0 < 0.5 else -1.0
        out.append(s * 0.5 * (1.0 - t))
    return out

def main():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(os.path.join(OUT, "sfx"), exist_ok=True)
    write_pal(os.path.join(OUT, "palette.pal"))
    make_tiles().save(os.path.join(OUT, "tiles.png"), transparency=0)
    make_player().save(os.path.join(OUT, "player.png"), transparency=0)
    make_rock_prop().save(os.path.join(OUT, "rock.png"), transparency=0)
    write_wav8(os.path.join(OUT, "sfx", "worm_roar.wav"), make_worm_roar())
    write_wav8(os.path.join(OUT, "sfx", "blip.wav"), make_blip())
    make_hound().save(os.path.join(OUT, "hound.png"), transparency=0)
    make_corpse().save(os.path.join(OUT, "corpse.png"), transparency=0)
    make_wall().save(os.path.join(OUT, "wall.png"), transparency=0)
    make_cistern().save(os.path.join(OUT, "cistern.png"), transparency=0)
    with open(os.path.join(OUT, "manifest.txt"), "w") as f:
        f.write("# name  file  frame_w frame_h frames  anchor_x anchor_y\n")
        f.write("tiles   tiles.png   32 16 10  16 8\n")
        f.write("player  player.png  16 32 4   8 30\n")
        f.write("rock    rock.png    32 32 1   16 26\n")
        f.write("hound   hound.png   16 16 2   8 13\n")
        f.write("corpse  corpse.png  16 10 1   8 7\n")
        f.write("wall    wall.png    32 40 1   16 32\n")
        f.write("cistern cistern.png 24 24 1   12 20\n")
    print("assets written to", os.path.abspath(OUT))

if __name__ == "__main__":
    main()
