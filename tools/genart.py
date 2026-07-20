#!/usr/bin/env python3
"""OpenAI art generation — the head of the asset pipeline.

Reads target dimensions from assets_src/raw/meta.txt (make_assets.py remains
the dimension authority), generates each asset with gpt-image-1, fits it into
the declared frame size, synthesizes animation frames, and overwrites the raw
PNGs in place. quantize.py + atlas.py then conform everything to the palette.

Raw API outputs are cached in assets_src/gen_cache/ — delete a file there (or
pass --force name...) to regenerate it.
"""
import base64
import json
import os
import random
import sys
import urllib.request
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(ROOT, "assets_src", "raw")
CACHE = os.path.join(ROOT, "assets_src", "gen_cache")

STYLE = ("isometric video game asset, Diablo 1 aesthetic, dark gritty 1990s "
         "retro pixel-art feel, muted desert palette of ochre sand and dusty "
         "rock, moody, no text, no watermark")

# Sprites: one generated image fitted into the meta-declared frame box.
SPRITES = {
    "player":  "a lone desert nomad warrior in a worn dark hooded stillsuit robe "
               "with subtle tubing, faint glowing blue eyes under the hood, "
               "standing pose, full body, facing slightly left",
    "hound":   "a lean sinewy desert hound predator, mangy tan-grey hide, "
               "ember-orange eyes, prowling stance, side view, full body",
    "corpse":  "a fallen desert warrior's corpse half-buried in sand, lying flat, "
               "viewed from above at an angle, tattered dark robes",
    "wall":    "a tall carved sandstone cave wall block segment, rectangular "
               "monolithic slab, hewn stone texture with chisel marks",
    "cistern": "an ancient stone water cistern basin, round carved rock rim, "
               "still dark water surface glinting inside, small pipes",
    "rock":    "a weathered desert rock outcrop boulder cluster, wind-carved "
               "sandstone, rounded forms",
    "wormhead": "the gaping circular maw of a colossal sandworm rising from sand, "
                "rings of crystalline teeth around a black gullet, segmented "
                "leathery hide, terrifying, front view",
    "wormseg": "one massive segmented body arc of a colossal sandworm emerging "
               "from sand, leathery ringed hide, arch shape",
    "thumper": "a small metal spike device planted in sand with a piston hammer "
               "on top, red activation band, utilitarian survival tech",
    "crawler": "a massive industrial sand-harvester crawler machine on wide "
               "tracks, rusted orange-brown armored hull, dust intakes, "
               "smokestack with beacon, three-quarter isometric view",
    "trooper": "a brutal soldier in dark segmented armor with a sealed helmet "
               "and narrow red visor slit, holding a short blade, standing, "
               "full body",
    "spotter": "a military observer in dark fatigues and goggles holding up a "
               "long brass telescope, standing on watch, full body",
}

# Terrain textures: generated once, then diamonds are cut from random crops.
TEXTURES = {
    "tex_sand":  "seamless top-down texture of windswept desert sand with fine "
                 "ripples and small dunes, ochre and tan",
    "tex_rock":  "seamless top-down texture of cracked desert rock ground, "
                 "grey-brown stone plates with dust in the cracks",
    "tex_spice": "seamless top-down texture of desert sand veined with "
                 "glittering rust-red crystalline cinnamon-colored deposits",
}

def api_key():
    key = os.environ.get("OPENAI_API_KEY")
    if key:
        return key
    env = os.path.join(ROOT, ".env")
    if os.path.isfile(env):
        for line in open(env):
            line = line.strip()
            if line.startswith("OPENAI_API_KEY"):
                return line.split("=", 1)[1].strip()
    return None

def generate(key, name, prompt, transparent):
    cache_file = os.path.join(CACHE, f"{name}.png")
    if os.path.isfile(cache_file):
        print(f"  {name}: cached")
        return Image.open(cache_file).convert("RGBA")
    body = {
        "model": "gpt-image-1",
        "prompt": f"{prompt}, {STYLE}",
        "size": "1024x1024",
        "quality": "medium",
        "output_format": "png",
        "n": 1,
    }
    if transparent:
        body["background"] = "transparent"
    req = urllib.request.Request(
        "https://api.openai.com/v1/images/generations",
        data=json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    print(f"  {name}: generating...")
    with urllib.request.urlopen(req, timeout=240) as resp:
        data = json.load(resp)
    png = base64.b64decode(data["data"][0]["b64_json"])
    os.makedirs(CACHE, exist_ok=True)
    with open(cache_file, "wb") as f:
        f.write(png)
    return Image.open(cache_file).convert("RGBA")

def read_meta():
    meta = {}
    with open(os.path.join(RAW, "meta.txt")) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, fname, fw, fh, frames, ax, ay = line.split()
            meta[name] = (fname, int(fw), int(fh), int(frames), int(ax), int(ay))
    return meta

def fit_sprite(img, fw, fh):
    """Alpha-crop, scale to fit the frame, anchor at bottom-center."""
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    scale = min((fw - 2) / img.width, (fh - 2) / img.height)
    img = img.resize((max(1, int(img.width * scale)), max(1, int(img.height * scale))),
                     Image.LANCZOS)
    frame = Image.new("RGBA", (fw, fh), (0, 0, 0, 0))
    frame.paste(img, ((fw - img.width) // 2, fh - img.height - 1), img)
    return frame

def make_sheet(frame_img, fw, fh, frames):
    """Frame 0 = base; alternate frames get a 1px bob (until real anim art)."""
    sheet = Image.new("RGBA", (fw * frames, fh), (0, 0, 0, 0))
    for i in range(frames):
        bob = -1 if i % 2 == 1 else 0
        sheet.paste(frame_img, (i * fw, bob), frame_img)
    return sheet

def diamond_mask(w, h):
    mask = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(mask)
    d.polygon([(w // 2, 0), (w - 1, h // 2), (w // 2, h - 1), (0, h // 2)], fill=255)
    return mask

def cut_diamond(tex, w, h, rng, brightness=1.0):
    x = rng.randint(0, tex.width - w - 1)
    y = rng.randint(0, tex.height - h - 1)
    crop = tex.crop((x, y, x + w, y + h)).convert("RGBA")
    if brightness != 1.0:
        crop = Image.eval(crop, lambda v: min(255, int(v * brightness)))
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(crop, (0, 0), diamond_mask(w, h))
    return out

def build_tiles(textures, fw, fh, frames):
    """Compose the 10-frame floor sheet from generated textures."""
    rng = random.Random(7)
    sand, rock, spice = textures["tex_sand"], textures["tex_rock"], textures["tex_spice"]
    tiles = [
        cut_diamond(sand, fw, fh, rng),            # 0 sand a
        cut_diamond(sand, fw, fh, rng, 0.92),      # 1 sand b
        cut_diamond(sand, fw, fh, rng, 1.08),      # 2 sand c
        cut_diamond(rock, fw, fh, rng),            # 3 rock a
        cut_diamond(rock, fw, fh, rng, 0.9),       # 4 rock b
        cut_diamond(spice, fw, fh, rng),           # 5 spice-flecked
        cut_diamond(sand, fw, fh, rng, 0.8),       # 6 drum sand (ringed below)
        None,                                       # 7 highlight (procedural)
        cut_diamond(rock, fw, fh, rng, 0.55),      # 8 sietch floor
        cut_diamond(rock, fw, fh, rng, 0.7),       # 9 door pad (gold rim below)
    ]
    # drum sand rings
    d = ImageDraw.Draw(tiles[6])
    for r in (0.35, 0.65):
        d.ellipse([fw * (0.5 - r / 2), fh * (0.5 - r / 2),
                   fw * (0.5 + r / 2), fh * (0.5 + r / 2)], outline=(120, 96, 52, 255))
    # highlight outline
    hl = Image.new("RGBA", (fw, fh), (0, 0, 0, 0))
    d = ImageDraw.Draw(hl)
    d.polygon([(fw // 2, 0), (fw - 1, fh // 2), (fw // 2, fh - 1), (0, fh // 2)],
              outline=(255, 255, 255, 255), width=3)
    tiles[7] = hl
    # door pad gold rim
    d = ImageDraw.Draw(tiles[9])
    d.polygon([(fw // 2, 0), (fw - 1, fh // 2), (fw // 2, fh - 1), (0, fh // 2)],
              outline=(228, 190, 80, 255), width=2)

    sheet = Image.new("RGBA", (fw * frames, fh), (0, 0, 0, 0))
    for i, t in enumerate(tiles):
        sheet.paste(t, (i * fw, 0), t)
    return sheet

def main():
    key = api_key()
    if not key:
        print("genart: no OPENAI_API_KEY (env or .env) — stand-ins stay in place.")
        return 1
    force = set(sys.argv[1:])
    if "--force" in force:
        force.discard("--force")
        for name in (force or list(SPRITES) + list(TEXTURES) + ["tiles"]):
            p = os.path.join(CACHE, f"{name}.png")
            if os.path.isfile(p):
                os.remove(p)

    meta = read_meta()

    textures = {name: generate(key, name, prompt, transparent=False)
                for name, prompt in TEXTURES.items()}
    _, fw, fh, frames, _, _ = meta["tiles"]
    build_tiles(textures, fw, fh, frames).save(os.path.join(RAW, "tiles.png"))
    print(f"  tiles: composed {frames} diamonds at {fw}x{fh}")

    for name, prompt in SPRITES.items():
        if name not in meta:
            print(f"  {name}: not in meta.txt, skipped")
            continue
        img = generate(key, name, prompt, transparent=True)
        _, fw, fh, frames, _, _ = meta[name]
        sheet = make_sheet(fit_sprite(img, fw, fh), fw, fh, frames)
        sheet.save(os.path.join(RAW, f"{name}.png"))
        print(f"  {name}: {fw}x{fh} x{frames}")

    print("genart: raw art updated — build to run quantize+atlas.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
