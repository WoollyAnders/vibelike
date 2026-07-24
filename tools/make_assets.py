#!/usr/bin/env python3
"""Procedural stand-in art for the asset pipeline (until genart.py replaces it).

Draws at classic scale, then upsamples 2x nearest so everything matches the
64x32-tile world. Output goes to assets_src/raw/ (+ meta.txt) as if it were
AI-generated art; palette.pal and sfx stay in assets/. The palette written
here is the single source of truth.
"""
import os
import random
from PIL import Image

OUT = os.path.join(os.path.dirname(__file__), "..", "assets")
OUT_RAW = os.path.join(os.path.dirname(__file__), "..", "assets_src", "raw")
SCALE = 4  # R3: native on-screen size. Base art is authored small then scaled up
           # so meta frame boxes are base*SCALE (128x64 tiles, 64x128 chars); the
           # world now blits 1:1 (no 2x nearest upscale) for crisp high-res art.
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
        make_floor(98, 3, edge_dark=2),         # 10 sietch floor variant b
        make_floor(104, 3, edge_dark=2),        # 11 sietch floor variant c
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

def make_ripple():
    """32x16 sand-ripple overlay, 2 frames — the moving wormsign."""
    frames = []
    for i in range(2):
        img = new_indexed(32, 16)
        px = img.load()
        for y, x0, x1 in diamond_rows():
            for x in range(x0, x1):
                d = (abs(x - 16) + abs((y - 8) * 2) + i * 3) % 7
                if d == 0:
                    px[x, y] = 90   # bright sand crest
                elif d == 1:
                    px[x, y] = 44   # trough shadow
        frames.append(img)
    return sheet_from(frames, 32, 16)

def make_mound():
    """32x24 breach mound, 3 frames (growing)."""
    frames = []
    for i in range(3):
        img = new_indexed(32, 24)
        px = img.load()
        h = 5 + i * 5
        for y in range(24 - h, 24):
            t = (y - (24 - h)) / max(1, h)
            half = int(4 + t * 11)
            for x in range(16 - half, 16 + half):
                px[x, y] = 60 + int(t * 16) + ((x * 3 + y) % 3)
        # crest cracks
        for x in range(13, 19):
            px[x, 24 - h] = 40
        frames.append(img)
    return sheet_from(frames, 32, 24)

def make_worm_head():
    """48x56 the maw, 2 frames (open pulse)."""
    frames = []
    for i in range(2):
        img = new_indexed(48, 56)
        px = img.load()
        gape = 2 + i * 3
        for y in range(6, 52):
            for x in range(4, 44):
                dx, dy = (x - 24) / 19.0, (y - 30) / 24.0
                r = dx * dx + dy * dy
                if r < 1.0:
                    shade = 110 + int((1 - r) * 12)
                    px[x, y] = shade
        # segmented rings
        for y in range(10, 52, 7):
            for x in range(6, 42):
                dx = (x - 24) / 19.0
                if dx * dx < 0.9:
                    px[x, y] = 100
        # the maw: dark circle ringed by crystal teeth
        for y in range(12 - gape, 30 + gape):
            for x in range(12, 36):
                dx, dy = (x - 24) / (11.0 + gape), (y - 21) / (9.0 + gape)
                if dx * dx + dy * dy < 1.0:
                    px[x, y] = 2
        import math as _m
        for a in range(0, 360, 24):
            tx = int(24 + _m.cos(_m.radians(a)) * (10 + gape))
            ty = int(21 + _m.sin(_m.radians(a)) * (8 + gape))
            for oy in (0, 1):
                if 0 <= ty + oy < 56:
                    px[tx, ty + oy] = 28
        frames.append(img)
    return sheet_from(frames, 48, 56)

def make_worm_segment():
    """36x36 body arc segment."""
    img = new_indexed(36, 36)
    px = img.load()
    for y in range(8, 34):
        for x in range(2, 34):
            dx, dy = (x - 18) / 15.0, (y - 22) / 12.0
            r = dx * dx + dy * dy
            if r < 1.0:
                px[x, y] = 108 + int((1 - r) * 14) - (2 if (y % 6) < 2 else 0)
    return img

def make_thumper():
    """12x20 deployed thumper post, 2 frames (piston up/down)."""
    frames = []
    for i in range(2):
        img = new_indexed(12, 20)
        px = img.load()
        for y in range(8, 19):
            for x in range(4, 8):
                px[x, y] = 20
        top = 3 if i == 0 else 6
        for y in range(top, top + 3):
            for x in range(3, 9):
                px[x, y] = 232
        frames.append(img)
    return sheet_from(frames, 12, 20)

def make_crawler():
    """64x48 harvester crawler, 2 frames (intake churn)."""
    frames = []
    for i in range(2):
        img = new_indexed(64, 48)
        px = img.load()
        # tracked hull: big slab
        for y in range(18, 42):
            for x in range(6, 58):
                dx = abs(x - 32) / 27.0
                dy = abs(y - 30) / 13.0
                if dx + dy < 1.25:
                    shade = 14 + int((1 - dy) * 6)
                    px[x, y] = shade
        # deck plating lines
        for y in range(20, 40, 4):
            for x in range(10, 54):
                if abs(x - 32) / 27.0 + abs(y - 30) / 13.0 < 1.15:
                    px[x, y] = 11
        # intake maw at front-right, churning
        for y in range(30, 42):
            for x in range(48, 60):
                if (x + y + i * 2) % 4 < 2:
                    px[min(63, x), y] = 8
        # spice hopper glow on top
        for y in range(14, 20):
            for x in range(22, 42):
                if (x + y) % 3 != 0:
                    px[x, y] = 140 + ((x * y) % 6)
        # stack + beacon
        for y in range(6, 16):
            for x in range(14, 18):
                px[x, y] = 12
        px[15, 5] = 210 if i == 0 else 200
        frames.append(img)
    return sheet_from(frames, 64, 48)

def make_trooper():
    """16x32 Harkonnen trooper, 2 frames."""
    frames = []
    for i in range(2):
        img = new_indexed(16, 32)
        px = img.load()
        bob = i
        # armored bulk
        for y in range(10 + bob, 27):
            half = 4 if y < 20 else 3
            for x in range(8 - half, 8 + half):
                px[x, y] = 100 + (y % 3)
        # helmet
        for y in range(4 + bob, 10 + bob):
            for x in range(5, 11):
                px[x, y] = 13
        px[7, 7 + bob] = 210  # visor slit
        px[8, 7 + bob] = 210
        # weapon slab
        for y in range(14 + bob, 17 + bob):
            for x in range(11, 15):
                px[x, y] = 9
        for x in (4, 11):
            px[x, 28] = 10
            px[x, 29] = 10
        frames.append(img)
    return sheet_from(frames, 16, 32)

def make_spotter():
    """16x32 spotter with scope glint, 2 frames."""
    frames = []
    for i in range(2):
        img = new_indexed(16, 32)
        px = img.load()
        for y in range(12, 28):
            half = 3
            for x in range(8 - half, 8 + half):
                px[x, y] = 98 + (y % 2)
        for y in range(5, 12):
            for x in range(5, 11):
                px[x, y] = 12
        # raised scope
        for x in range(10, 15):
            px[x, 8] = 9
        px[14, 8] = 30 if i == 0 else 224  # lens flash
        for x in (5, 10):
            px[x, 29] = 10
        frames.append(img)
    return sheet_from(frames, 16, 32)

def make_carryall_shadow():
    """56x24 looming carryall shadow."""
    img = new_indexed(56, 24)
    px = img.load()
    for y in range(4, 20):
        for x in range(4, 52):
            dx, dy = (x - 28) / 25.0, (y - 12) / 9.0
            if dx * dx + dy * dy < 1.0:
                if (x + y) % 2 == 0:
                    px[x, y] = 2
    return img

# ---- UI stand-ins (replaced by generated art) ---------------------------------

def make_ui_panel():
    """320x22 HUD panel strip (drawn 2x by SCALE -> 640x44)."""
    img = new_indexed(320, 22)
    px = img.load()
    for y in range(22):
        for x in range(320):
            shade = 4 + (1 if y < 2 else 0) + ((x * 7 + y * 13) % 3 == 0)
            px[x, y] = shade
    return img

def make_ui_socket():
    """22x22 orb socket ring."""
    img = new_indexed(22, 22)
    px = img.load()
    for y in range(22):
        for x in range(22):
            d2 = (x - 11) ** 2 + (y - 11) ** 2
            if 81 <= d2 <= 121:
                px[x, y] = 230
    return img

def make_ui_plaque():
    """120x12 banner plaque."""
    img = new_indexed(120, 12)
    px = img.load()
    for y in range(12):
        for x in range(120):
            if x < 2 or x > 117 or y < 1 or y > 10:
                px[x, y] = 228
            else:
                px[x, y] = 3
    return img

def make_box(w, h, frames=1, base=100):
    """Generic placeholder box for props/decor — genart.py overwrites it with the
    real generated art; it only needs to match the declared frame size."""
    img = new_indexed(w * frames, h)
    px = img.load()
    for fi in range(frames):
        for y in range(h):
            for x in range(w):
                edge = x == 0 or x == w - 1 or y == 0 or y == h - 1
                px[fi * w + x, y] = 3 if edge else uint8_box(base, x, y)
    return img

def uint8_box(base, x, y):
    v = base + ((x * 5 + y * 3) % 7)
    return max(1, min(255, v))

# name -> (builder, fw, fh, frames, ax, ay) at authoring (half) scale
SHEETS = {
    "tiles":    (lambda: make_tiles(), 32, 16, 12, 16, 8),
    "player":   (lambda: make_player(), 16, 32, 4, 8, 30),
    "rock":     (lambda: make_rock_prop(), 32, 32, 1, 16, 26),
    "hound":    (lambda: make_hound(), 16, 16, 2, 8, 13),
    "corpse":   (lambda: make_corpse(), 16, 10, 1, 8, 7),
    "wall":     (lambda: make_box(32, 24), 32, 24, 1, 16, 20),  # procedural iso block (low rim)
    "cistern":  (lambda: make_cistern(), 24, 24, 1, 12, 20),
    "ripple":   (lambda: make_ripple(), 32, 16, 2, 16, 8),
    "mound":    (lambda: make_mound(), 32, 24, 3, 16, 16),
    "wormhead": (lambda: make_worm_head(), 48, 56, 2, 24, 48),
    "wormseg":  (lambda: make_worm_segment(), 36, 36, 1, 18, 28),
    "thumper":  (lambda: make_thumper(), 12, 20, 2, 6, 17),
    "crawler":  (lambda: make_crawler(), 64, 48, 2, 32, 40),
    "trooper":  (lambda: make_trooper(), 16, 32, 2, 8, 30),
    "spotter":  (lambda: make_spotter(), 16, 32, 2, 8, 30),
    "carryall": (lambda: make_carryall_shadow(), 56, 24, 1, 28, 12),
    "player_fedaykin": (lambda: make_player(), 16, 32, 4, 8, 30),
    "player_saboteur": (lambda: make_player(), 16, 32, 4, 8, 30),
    "player_sayyadina": (lambda: make_player(), 16, 32, 4, 8, 30),
    "glowglobe": (lambda: make_thumper(), 12, 18, 1, 6, 14),
    "naib": (lambda: make_player(), 16, 32, 1, 8, 30),
    "champion": (lambda: make_trooper(), 16, 32, 2, 8, 30),
    "baron": (lambda: make_trooper(), 16, 32, 2, 8, 30),
    "guildsman": (lambda: make_player(), 16, 32, 1, 8, 30),
    "smuggler": (lambda: make_player(), 16, 32, 1, 8, 30),
    "panadder": (lambda: make_hound(), 16, 16, 2, 8, 13),
    "hermit": (lambda: make_player(), 16, 32, 2, 8, 30),
    "hunterseeker": (lambda: make_thumper(), 12, 18, 2, 6, 14),
    "spicecache": (lambda: make_corpse(), 16, 10, 1, 8, 7),
    "ui_panel": (lambda: make_ui_panel(), 320, 22, 1, 0, 0),
    "ui_socket": (lambda: make_ui_socket(), 22, 22, 1, 11, 11),
    "ui_plaque": (lambda: make_ui_plaque(), 120, 12, 1, 0, 0),
    # --- sietch hub decor / furniture (genart replaces the placeholder boxes) ---
    # Sized up so props read at the iso scale (tiles are 128x64, chars 64x128).
    "prop_rug":        (lambda: make_box(26, 15), 26, 15, 1, 13, 13),
    "prop_brazier":    (lambda: make_box(16, 22), 16, 22, 1, 8, 20),
    "prop_weaponrack": (lambda: make_box(18, 26), 18, 26, 1, 9, 24),
    "prop_suitrack":   (lambda: make_box(18, 26), 18, 26, 1, 9, 24),
    "prop_urn":        (lambda: make_box(12, 18), 12, 18, 1, 6, 16),
    "prop_banner":     (lambda: make_box(14, 28), 14, 28, 1, 7, 26),
    "prop_baskets":    (lambda: make_box(16, 14), 16, 14, 1, 8, 12),
    "prop_table":      (lambda: make_box(22, 16), 22, 16, 1, 11, 14),
    "prop_spicesacks": (lambda: make_box(18, 14), 18, 14, 1, 9, 12),
    "door_seal":       (lambda: make_box(22, 28), 22, 28, 1, 11, 26),
    # --- dialog-only Fremen tribesfolk ---
    "fremen":          (lambda: make_box(16, 32), 16, 32, 1, 8, 30),
    "fremen2":         (lambda: make_box(16, 32), 16, 32, 1, 8, 30),
}

def main():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(OUT_RAW, exist_ok=True)
    os.makedirs(os.path.join(OUT, "sfx"), exist_ok=True)
    write_pal(os.path.join(OUT, "palette.pal"))
    write_wav8(os.path.join(OUT, "sfx", "worm_roar.wav"), make_worm_roar())
    write_wav8(os.path.join(OUT, "sfx", "blip.wav"), make_blip())

    with open(os.path.join(OUT_RAW, "meta.txt"), "w") as meta:
        meta.write("# name  file  frame_w frame_h frames  anchor_x anchor_y\n")
        for name, (build, fw, fh, frames, ax, ay) in SHEETS.items():
            path = os.path.join(OUT_RAW, f"{name}.png")
            if not os.path.exists(path):  # never clobber generated art
                img = build()
                img = img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST)
                img.save(path, transparency=0)
            meta.write(f"{name} {name}.png {fw*SCALE} {fh*SCALE} {frames} "
                       f"{ax*SCALE} {ay*SCALE}\n")
    print("raw stand-in art updated in", os.path.abspath(OUT_RAW))

if __name__ == "__main__":
    main()
