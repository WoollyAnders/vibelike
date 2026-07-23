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

STYLE = ("isometric video game asset in the style of Diablo II — highly detailed "
         "90s dark-fantasy pixel art with rich painterly shading, dithered "
         "gradients and many color shades, deep moody chiaroscuro lighting, "
         "gritty ornate detail, muted desert palette of ochre sand and dusty "
         "rock, no text, no watermark")

# Sprites: one generated image fitted into the meta-declared frame box.
SPRITES = {
    "player":  "a lone desert nomad warrior in a worn dark hooded stillsuit robe "
               "with subtle tubing, faint glowing blue eyes under the hood, "
               "standing pose, full body, facing slightly left",
    "player_fedaykin":
               "a fierce desert commando warrior in a fitted dark stillsuit with "
               "chest-crossed straps and a tribal headband, scarred face, "
               "gripping a curved pale crystalline knife low and ready, battle "
               "stance, full body, facing slightly left, isolated figure on a "
               "fully transparent background, no ground, no scene",
    "player_saboteur":
               "a lean desert saboteur in a hooded dark stillsuit hung with "
               "satchels, det-cord coils and small charges across the chest, "
               "wary hunched posture, full body, facing slightly left, isolated "
               "figure on a fully transparent background, no ground, no scene",
    "player_sayyadina":
               "a desert mystic seeress in flowing indigo-shadowed robes over a "
               "stillsuit, veiled face with intensely glowing blue-within-blue "
               "eyes, one hand raised in a prescient gesture, full body, facing "
               "slightly left, isolated figure on a fully transparent "
               "background, no ground, no scene",
    "hound":   "a lean sinewy desert hound predator, mangy tan-grey hide, "
               "ember-orange eyes, prowling stance, side view, full body",
    "corpse":  "a fallen desert warrior's corpse lying flat on its back, "
               "tattered dark robes, viewed from above at an isometric angle, "
               "isolated figure on a fully transparent background, no ground, "
               "no sand, no scene",
    "wall":    "a tall carved sandstone cave wall block segment, rectangular "
               "monolithic slab, hewn stone texture with chisel marks",
    "cistern": "an ancient stone water cistern basin, round carved rock rim, "
               "still dark water surface glinting inside, small pipes",
    "rock":    "a weathered desert rock outcrop boulder cluster, wind-carved "
               "sandstone, rounded forms",
    "wormhead": "the gaping circular maw of a colossal sandworm rearing up, "
                "rings of crystalline teeth around a black gullet, segmented "
                "leathery hide, terrifying, front view, isolated single "
                "creature on a fully transparent background, no ground, no "
                "sand base, no border, no frame",
    "wormseg": "one massive segmented body arc of a colossal sandworm emerging "
               "from sand, leathery ringed hide, arch shape",
    "thumper": "a single narrow metal stake device with a cylindrical piston "
               "hammer head on top, red activation band around the shaft, "
               "utilitarian desert survival tool, upright, isolated object on "
               "a fully transparent background, no ground, no sand, no scene",
    "crawler": "a massive industrial sand-harvester crawler machine on wide "
               "tracks, rusted orange-brown armored hull, dust intakes, "
               "smokestack with beacon, three-quarter isometric view, isolated "
               "vehicle on a fully transparent background, no ground, no sand "
               "base, no border, no frame",
    "trooper": "a brutal soldier in dark segmented armor with a sealed helmet "
               "and narrow red visor slit, holding a short blade, standing, "
               "full body",
    "spotter": "a military observer in dark fatigues and goggles holding up a "
               "long brass telescope, standing on watch, full body",
    "glowglobe": "a small floating luminous orb lamp glowing warm amber inside "
                 "a delicate brass tether cage, hovering, softly radiant, "
                 "isolated object on a fully transparent background, no scene",
    "naib":    "a weathered desert tribe elder chieftain in layered dark robes "
               "with a heavy woven mantle and water rings braided into a grey "
               "beard, stern bearing, arms folded, full body, isolated figure "
               "on a fully transparent background, no scene",
    "smuggler": "a grizzled off-world smuggler in a patched long duster coat "
                "over desert gear, goggles pushed up, sly grin, one hand "
                "resting on a strongbox, full body, isolated figure on a "
                "fully transparent background, no scene",
    "panadder": "a venomous desert viper snake coiled to strike, dusty "
                "sand-colored scales with darker diamond pattern, isolated "
                "creature on a fully transparent background, no scene",
    "hermit":  "a wild-eyed emaciated desert hermit in shredded rags, skin "
               "stained faint cinnamon-orange, clutching a crooked staff, "
               "hunched and twitching, full body, isolated figure on a fully "
               "transparent background, no scene",
    "hunterseeker": "a sinister sliver-thin hovering assassin drone, dark "
                    "needle-like metal body with a single glowing red sensor "
                    "eye, floating vertically, isolated object on a fully "
                    "transparent background, no scene",
    "champion": "a hulking Harkonnen champion duelist in ornate black-and-red "
                "segmented battle armor with a horned helm and twin curved "
                "blades, menacing stance, full body, isolated figure on a "
                "fully transparent background, no scene",
    "baron":    "a corpulent towering baron in gilded black armor and a flowing "
                "orange-lined cape, cruel grin, suspensor harness glints at his "
                "belt, one ringed hand raised, full body, isolated figure on a "
                "fully transparent background, no scene",
    "guildsman": "a gaunt hooded Spacing Guild agent in long grey-orange robes "
                 "with a metallic breathing apparatus at the mouth, holding an "
                 "abacus-like ledger device, full body, isolated figure on a "
                 "fully transparent background, no scene",
    "spicecache": "a small mound of glittering rust-red crystalline spice "
                  "granules heaped on the ground, faintly luminous cinnamon "
                  "crystals, isolated on a fully transparent background, "
                  "no scene",
}

# UI elements: stretched to fill their exact frame (no aspect-fit).
UI = {
    "ui_panel":  "long horizontal fantasy game HUD panel frame, dark carved "
                 "sandstone and aged bronze, desert-tribal etched motifs, "
                 "rivets and worn edges, mostly flat empty center band for "
                 "widgets, wide thin strip composition",
    "ui_socket": "ornate circular orb socket ring for a fantasy game HUD, aged "
                 "bronze and carved stone ring with desert etchings, fully "
                 "open transparent hole in the center, front view",
    "ui_plaque": "ornate horizontal metal banner plaque for a fantasy game HUD, "
                 "dark aged bronze with rivets and etched border, empty center "
                 "for text, wide thin strip",
}

# Terrain textures: generated once, then diamonds are cut from random crops.
TEXTURES = {
    "tex_sand":  "seamless top-down texture of windswept desert sand, finely "
                 "detailed wind-carved ripples, grains and soft dune ridges, "
                 "layered ochre, tan and shadowed umber, rich painterly 90s "
                 "pixel-art shading with dithered gradients",
    "tex_rock":  "seamless top-down texture of cracked desert rock ground, "
                 "detailed grey-brown stone plates with deep fissures, pitting "
                 "and rubble, dust settled in the cracks, rich painterly 90s "
                 "pixel-art shading",
    "tex_spice": "seamless top-down texture of desert sand veined with glittering "
                 "rust-red crystalline cinnamon spice, sparkling flecks and "
                 "layered deposits, rich detailed 90s pixel-art shading",
    "tex_ripple": "seamless top-down texture of strong concentric sand ripple "
                  "rings radiating across desert sand, deep wave shadows, "
                  "agitated disturbed sand, detailed 90s pixel-art shading",
}

# Turnaround sheets: 5 views of the same figure, sliced into direction strips.
TURN_STYLE = ("video game character turnaround sheet, exactly five full-body "
              "views of the SAME character standing in one horizontal row: "
              "front view, three-quarter front-right view, right side profile "
              "view, three-quarter back-right view, back view. identical "
              "costume, colors and proportions in every view, evenly spaced, "
              "feet on one baseline, isolated on a fully transparent "
              "background, no labels, no grid, no frame. "
              "Bold high-contrast readable silhouette, strong single-source rim "
              "lighting, clean crisp edges, chunky clear forms, low visual noise "
              "so the figure stays legible when scaled down to a tiny sprite")

TURNAROUNDS = {
    "player_fedaykin": "a fierce desert commando warrior in a fitted dark "
                       "stillsuit with chest-crossed straps and a tribal "
                       "headband, gripping a curved pale crystalline knife",
    "player_saboteur": "a lean desert saboteur in a hooded dark stillsuit hung "
                       "with satchels, det-cord coils and small charges",
    "player_sayyadina": "a desert mystic seeress in flowing indigo-shadowed "
                        "robes over a stillsuit, veiled face with glowing "
                        "blue-within-blue eyes",
    "trooper": "a brutal soldier in dark segmented armor with a sealed helmet "
               "and narrow red visor slit, blade sheathed at the hip, arms at "
               "sides, nothing on the ground",
    "hound": "a lean sinewy desert hound predator, mangy tan-grey hide, "
             "ember-orange eyes, standing alert",
}

# Sprites whose subject colour is too close to a sandy backdrop for corner-flood
# keying (they came back invisible). Regenerate these on a solid chroma-key GREEN
# screen; strip_background() then keys the green cleanly. Requested opaque (not
# transparent) so the model actually paints the flat green field.
CHROMA = {"hound", "spotter", "carryall"}
CHROMA_BG = (" , the ENTIRE background a single flat uniform chroma-key green "
             "(rgb 0 200 0), even studio lighting, no ground, no shadow, no other "
             "green anywhere on the subject, the subject fully inside the frame")

# Specials: generated once, frames synthesized in post.
SPECIALS = {
    "mound":    "a swelling mound of disturbed desert sand bulging upward with "
                "cracks radiating from its crest, dust sifting off, isolated on "
                "a fully transparent background, no scene, no border",
    "carryall": "a detailed top-down view of a massive heavy-lift ornithopter "
                "cargo aircraft with dragonfly wings and twin hulls, weathered "
                "metallic bronze and grey plating, seen from directly above",
}

def build_ripple(tex, fw, fh, frames):
    """Animate by rolling the ripple texture between frames."""
    sheet = Image.new("RGBA", (fw * frames, fh), (0, 0, 0, 0))
    mask = diamond_mask(fw, fh)
    for i in range(frames):
        ox, oy = 128 + i * 9, 128 + i * 5
        crop = tex.crop((ox, oy, ox + fw, oy + fh)).convert("RGBA")
        tile = Image.new("RGBA", (fw, fh), (0, 0, 0, 0))
        tile.paste(crop, (0, 0), mask)
        sheet.paste(tile, (i * fw, 0), tile)
    return sheet

def build_mound(img, fw, fh, frames):
    """Growth cycle: progressive scale of one generated mound."""
    img = strip_background(img)
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    sheet = Image.new("RGBA", (fw * frames, fh), (0, 0, 0, 0))
    for i in range(frames):
        t = (i + 1) / frames
        s = min((fw - 2) * t / img.width, (fh - 2) * t / img.height)
        scaled = img.resize((max(1, int(img.width * s)), max(1, int(img.height * s))),
                            Image.LANCZOS)
        sheet.paste(scaled, (i * fw + (fw - scaled.width) // 2, fh - scaled.height - 1),
                    scaled)
    return sheet

def build_carryall(img, fw, fh, frames):
    """Aircraft silhouette flattened to a dithered ground shadow."""
    img = strip_background(img)
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    img = img.resize((fw - 2, fh - 2), Image.LANCZOS)
    shadow = Image.new("RGBA", (fw * frames, fh), (0, 0, 0, 0))
    px_src = img.load()
    px_dst = shadow.load()
    for y in range(img.height):
        for x in range(img.width):
            if px_src[x, y][3] > 100 and (x + y) % 2 == 0:  # 50% dither
                px_dst[x + 1, y + 1] = (16, 12, 10, 255)
    return shadow

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

# Canonical on-screen figure heights (px). fit_sprite scales to these instead
# of filling the frame, so a hound no longer looms as large as a man.
# R3: heights scale with make_assets SCALE (2->4, i.e. x2) to stay proportional
# inside the doubled native frame boxes.
FIG_SCALE = 2
HEIGHTS = {k: v * FIG_SCALE for k, v in {
    "player": 52, "player_fedaykin": 52, "player_saboteur": 52,
    "player_sayyadina": 52, "trooper": 54, "spotter": 52, "naib": 52,
    "smuggler": 52, "hermit": 48, "hound": 22, "panadder": 12,
    "hunterseeker": 26, "corpse": 14, "spicecache": 12, "thumper": 26,
    "glowglobe": 20, "rock": 52, "wall": 76, "cistern": 40,
    "wormhead": 104, "wormseg": 60, "crawler": 88,
    "champion": 58, "baron": 60, "guildsman": 54,
}.items()}

def _green_dominant(c):
    """A chroma-key green pixel. Two tiers: (a) strong pure green; (b) the
    desaturated green *fringe* left by antialiasing at the subject edge — green
    is the top channel and clearly above blue. Sandy/brown subjects have red as
    the top channel (r > g), so neither tier touches them."""
    if c[3] < 128:
        return False
    r, g, b = c[0], c[1], c[2]
    if g > 80 and g - r > 45 and g - b > 45:   # (a) pure green screen
        return True
    if g >= r and g > b + 18 and g > 50:       # (b) green fringe (not sand: g>=r)
        return True
    return False

def strip_background(img):
    """If the model painted a backdrop despite background=transparent, remove
    it: flood from every edge pixel, clearing anything close to the corner
    colors. No-op when the image already has real transparency.

    R3: some masters (a sand-coloured creature on a sandy backdrop) are impossible
    to key by corner colour — they are regenerated on a solid chroma-key GREEN
    screen instead. When the corners read green, drop every green-dominant pixel
    globally (not just edge-connected), which also clears green seen between limbs."""
    w, h = img.size
    px = img.load()
    corners = [px[0, 0], px[w - 1, 0], px[0, h - 1], px[w - 1, h - 1]]

    if sum(_green_dominant(c) for c in corners) >= 3:  # green screen
        for y in range(h):
            for x in range(w):
                if _green_dominant(px[x, y]):
                    px[x, y] = (0, 0, 0, 0)
        return img

    if any(c[3] < 128 for c in corners):
        return img  # already transparent

    def near(c, ref, tol=42):
        return (abs(c[0] - ref[0]) + abs(c[1] - ref[1]) + abs(c[2] - ref[2])) <= tol * 3

    from collections import deque
    seen = bytearray(w * h)
    q = deque()
    for x in range(w):
        q.append((x, 0)); q.append((x, h - 1))
    for y in range(h):
        q.append((0, y)); q.append((w - 1, y))
    while q:
        x, y = q.popleft()
        i = y * w + x
        if seen[i]:
            continue
        seen[i] = 1
        c = px[x, y]
        if c[3] < 128 or any(near(c, ref) for ref in corners):
            px[x, y] = (0, 0, 0, 0)
            if x > 0: q.append((x - 1, y))
            if x < w - 1: q.append((x + 1, y))
            if y > 0: q.append((x, y - 1))
            if y < h - 1: q.append((x, y + 1))
    return img

def fit_sprite(img, fw, fh, target_h=None):
    """Alpha-crop, scale to the canonical figure height (or fit), anchor at
    bottom-center on the shared baseline."""
    img = strip_background(img)
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    if target_h:
        # Scale to the canonical stature, but cap so a wide pose (weapon/cape/arms)
        # also fits the frame width — wide figures render a touch shorter rather
        # than getting guillotined at the frame edge.
        scale = min(target_h / img.height, (fw - 2) / img.width)
    else:
        scale = min((fw - 2) / img.width, (fh - 2) / img.height)
    img = img.resize((max(1, int(img.width * scale)), max(1, int(img.height * scale))),
                     Image.LANCZOS)
    if img.width > fw:  # center-crop weapon tips rather than shrink the figure
        x0 = (img.width - fw) // 2
        img = img.crop((x0, 0, x0 + fw, img.height))
    if img.height > fh:
        img = img.crop((0, img.height - fh, img.width, img.height))
    frame = Image.new("RGBA", (fw, fh), (0, 0, 0, 0))
    frame.paste(img, ((fw - img.width) // 2, fh - img.height - 1), img)
    return frame

def fit_stretch(img, fw, fh):
    """Alpha-crop, then stretch to exactly fill the frame (UI strips)."""
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    return img.resize((fw, fh), Image.LANCZOS)

def make_sheet(frame_img, fw, fh, frames):
    """Frame 0 = base; alternate frames get a 1px bob (until real anim art)."""
    sheet = Image.new("RGBA", (fw * frames, fh), (0, 0, 0, 0))
    for i in range(frames):
        bob = -1 if i % 2 == 1 else 0
        sheet.paste(frame_img, (i * fw, bob), frame_img)
    return sheet

def diamond_mask(w, h):
    # Rasterize oversized at 4x then downsample: full coverage to the very
    # edge so adjacent floor diamonds tessellate without seams.
    big = Image.new("L", (w * 4, h * 4), 0)
    d = ImageDraw.Draw(big)
    d.polygon([(w * 2, -4), (w * 4 + 4, h * 2), (w * 2, h * 4 + 4), (-4, h * 2)],
              fill=255)
    return big.resize((w, h), Image.BILINEAR).point(lambda v: 255 if v > 32 else 0)

def cut_diamond(tex, w, h, rng, brightness=1.0, anchor=None):
    if anchor:
        # sample near a shared anchor so sibling tiles blend at their seams
        x = max(0, min(tex.width - w - 1, anchor[0] + rng.randint(-24, 24)))
        y = max(0, min(tex.height - h - 1, anchor[1] + rng.randint(-12, 12)))
    else:
        x = rng.randint(0, tex.width - w - 1)
        y = rng.randint(0, tex.height - h - 1)
    crop = tex.crop((x, y, x + w, y + h)).convert("RGBA")
    if brightness != 1.0:
        # Dim RGB only — NOT alpha. (Image.eval dimmed all bands, dropping alpha
        # below draw_sprite's 128 key so dark tiles like the sietch floor became
        # fully transparent -> black.) Preserve the alpha channel.
        r, g, b, a = crop.split()
        lut = lambda v: min(255, int(v * brightness))
        crop = Image.merge("RGBA", (r.point(lut), g.point(lut), b.point(lut), a))
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(crop, (0, 0), diamond_mask(w, h))
    return out

def build_tiles(textures, fw, fh, frames):
    """Compose the 10-frame floor sheet from generated textures."""
    rng = random.Random(7)
    sand, rock, spice = textures["tex_sand"], textures["tex_rock"], textures["tex_spice"]
    sand_a = (sand.width // 2, sand.height // 2)
    rock_a = (rock.width // 2, rock.height // 2)
    tiles = [
        cut_diamond(sand, fw, fh, rng, 1.0, sand_a),    # 0 sand a
        cut_diamond(sand, fw, fh, rng, 0.985, sand_a),  # 1 sand b (near-identical)
        cut_diamond(sand, fw, fh, rng, 1.015, sand_a),  # 2 sand c
        cut_diamond(rock, fw, fh, rng, 1.0, rock_a),    # 3 rock a
        cut_diamond(rock, fw, fh, rng, 0.96, rock_a),   # 4 rock b
        cut_diamond(spice, fw, fh, rng),                # 5 spice-flecked
        cut_diamond(sand, fw, fh, rng, 0.9, sand_a),    # 6 drum sand (ringed below)
        None,                                            # 7 highlight (procedural)
        cut_diamond(rock, fw, fh, rng, 0.85, rock_a),   # 8 sietch floor (lit cavern stone)
        cut_diamond(rock, fw, fh, rng, 0.95, rock_a),   # 9 door pad (gold rim below)
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

    _, fw, fh, frames, _, _ = meta["ripple"]
    build_ripple(textures["tex_ripple"], fw, fh, frames).save(os.path.join(RAW, "ripple.png"))
    print(f"  ripple: {frames} rolled frames at {fw}x{fh}")

    for name, builder in (("mound", build_mound), ("carryall", build_carryall)):
        if name not in meta:
            continue
        chroma = name in CHROMA
        img = generate(key, name, SPECIALS[name] + (CHROMA_BG if chroma else ""),
                       transparent=not chroma)
        _, fw, fh, frames, _, _ = meta[name]
        builder(img, fw, fh, frames).save(os.path.join(RAW, f"{name}.png"))
        print(f"  {name}: {frames} synthesized frames at {fw}x{fh}")

    for name, prompt in SPRITES.items():
        if name not in meta:
            print(f"  {name}: not in meta.txt, skipped")
            continue
        chroma = name in CHROMA
        img = generate(key, name, prompt + (CHROMA_BG if chroma else ""),
                       transparent=not chroma)
        _, fw, fh, frames, _, _ = meta[name]
        sheet = make_sheet(fit_sprite(img, fw, fh, HEIGHTS.get(name)), fw, fh, frames)
        sheet.save(os.path.join(RAW, f"{name}.png"))
        print(f"  {name}: {fw}x{fh} x{frames}")

    # turnaround sheets -> aligned 5-view direction strips (+ meta entries)
    from slice_rotation import slice_turnaround
    meta_appends = []
    for name, prompt in TURNAROUNDS.items():
        if name not in meta:
            continue
        img = generate(key, f"{name}_turn", f"{prompt}, {TURN_STYLE}", transparent=True)
        _, fw, fh, _, ax, ay = meta[name]
        strip = slice_turnaround(img, fw, fh, 5, HEIGHTS.get(name))
        dir_name = f"{name}_dir"
        strip.save(os.path.join(RAW, f"{dir_name}.png"))
        if dir_name not in meta:
            meta_appends.append(f"{dir_name} {dir_name}.png {fw} {fh} 5 {ax} {ay}")
        print(f"  {dir_name}: 5 views at {fw}x{fh}")
    if meta_appends:
        with open(os.path.join(RAW, "meta.txt"), "a") as f:
            f.write("\n".join(meta_appends) + "\n")

    for name, prompt in UI.items():
        if name not in meta:
            print(f"  {name}: not in meta.txt, skipped")
            continue
        img = generate(key, name, prompt, transparent=True)
        _, fw, fh, frames, _, _ = meta[name]
        fitted = fit_stretch(img, fw, fh)
        sheet = Image.new("RGBA", (fw * frames, fh), (0, 0, 0, 0))
        for i in range(frames):
            sheet.paste(fitted, (i * fw, 0), fitted)
        sheet.save(os.path.join(RAW, f"{name}.png"))
        print(f"  {name}: {fw}x{fh} x{frames} (ui)")

    print("genart: raw art updated — build to run quantize+atlas.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
