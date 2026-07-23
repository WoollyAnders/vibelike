#!/usr/bin/env python3
"""Real walk-cycle art via gpt-image-1. For each character it generates ONE
side-profile walk-cycle sheet (the model keeps the figure consistent within a
single image), slices the frames by transparent column gaps, height-normalizes
them onto the shared baseline, and writes <name>_walkcycle.png — a dirs=1 strip
the game plays as the Walk state (mirrored left/right at draw time).

Raw API sheets cache in assets_src/gen_cache/<name>_walkcycle.png; delete one
(or pass the name as an arg) to regenerate it. No arg = all, cached ones reused.
"""
import base64, json, os, sys, urllib.request
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(ROOT, "assets_src", "raw")
CACHE = os.path.join(ROOT, "assets_src", "gen_cache")

STYLE = ("bold high-contrast cel-shaded pixel-art, clean crisp edges, Diablo 1 "
         "dark desert palette of ochre and dusty rock, no text, no watermark")

# One walk-cycle sheet per character, strict side profile so the stride reads.
CHARS = {
    "player_fedaykin":
        "a lean desert commando in a dark hooded stillsuit with crossed chest "
        "straps and a headband, a pale crystalline knife held low in one hand",
    "player_saboteur":
        "a wiry desert saboteur in a hooded dark stillsuit hung with satchels, "
        "det-cord coils and small charges, a snub maula pistol in one hand",
    "player_sayyadina":
        "a desert mystic in flowing indigo-shadowed robes over a stillsuit, "
        "veiled face with glowing blue eyes, hands empty and poised",
}

HEIGHT = 52   # canonical on-screen figure height (matches genart HEIGHTS)
FW, FH, AX, AY = 32, 64, 16, 60

def api_key():
    for line in open(os.path.join(ROOT, ".env")):
        if line.strip().startswith("OPENAI_API_KEY"):
            return line.split("=", 1)[1].strip()
    return None

def prompt_for(desc):
    return (
        "2D game character walk-cycle sprite sheet on a fully transparent "
        "background. A single horizontal row of SIX frames, left to right. "
        "CRITICAL LAYOUT: each frame contains EXACTLY ONE small figure; the "
        "figures are widely separated with a large empty transparent gap between "
        "them (each gap at least as wide as a figure); figures must NEVER touch, "
        "overlap, or share a frame. Every frame shows the SAME character in "
        "strict right-facing side profile at a different point of one smooth walk "
        "cycle: the legs swing (one forward, one back, then passing, then the "
        "opposite) and the arms swing opposite the legs, so the six frames played "
        "in order animate a walking loop. "
        f"Character: {desc}. Identical costume, colors, proportions and height in "
        "every frame; all feet aligned to one common ground baseline. " + STYLE +
        ". No ground, no shadow, no grid, no boxes, no text, no numbers.")

def generate(key, name, desc):
    cache = os.path.join(CACHE, f"{name}_walkcycle.png")
    if os.path.isfile(cache):
        print(f"  {name}: cached")
        return Image.open(cache).convert("RGBA")
    body = {"model": "gpt-image-1", "prompt": prompt_for(desc), "size": "1536x1024",
            "quality": "medium", "output_format": "png", "background": "transparent", "n": 1}
    req = urllib.request.Request(
        "https://api.openai.com/v1/images/generations",
        data=json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    print(f"  {name}: generating walk cycle...")
    with urllib.request.urlopen(req, timeout=300) as r:
        data = json.load(r)
    png = base64.b64decode(data["data"][0]["b64_json"])
    os.makedirs(CACHE, exist_ok=True)
    open(cache, "wb").write(png)
    return Image.open(cache).convert("RGBA")

def slice_frames(img):
    w, h = img.size
    a = img.getchannel("A").load()
    occ = [any(a[x, y] > 40 for y in range(0, h, 3)) for x in range(w)]
    spans, start = [], None
    for x, o in enumerate(occ + [False]):
        if o and start is None: start = x
        elif not o and start is not None:
            spans.append((start, x)); start = None
    # keep only real figures: wide spans (drop specks / edge fragments)
    return [s for s in spans if s[1] - s[0] >= 120]

def _alpha_area(c):
    a = c.getchannel("A").load()
    w, h = c.size
    return sum(1 for y in range(h) for x in range(w) if a[x, y] > 60) or 1

def _centroid_x(c):
    a = c.getchannel("A").load()
    w, h = c.size
    sx = n = 0
    for y in range(0, h, 2):
        for x in range(w):
            if a[x, y] > 60:
                sx += x; n += 1
    return sx / n if n else w / 2

def build_strip(img):
    crops = []
    for (x0, x1) in slice_frames(img):
        c = img.crop((x0, 0, x1, img.height))
        bb = c.getbbox()
        if bb: c = c.crop(bb)
        crops.append(c)
    if not crops:
        return None, 0
    n = len(crops)
    # gpt-image-1 draws each frame at a slightly different scale — the source of
    # the frame-to-frame "size change". Normalize every frame to the same opaque
    # pixel-MASS (pose-stable, unlike bbox height), then a single global scale so
    # the median frame stands ~HEIGHT tall. Feet bottom-anchored, body centred by
    # centroid so arm/leg swing doesn't slide the torso sideways.
    areas = sorted(_alpha_area(c) for c in crops)
    target_area = areas[len(areas) // 2]
    normed = []
    for c in crops:
        s = (target_area / _alpha_area(c)) ** 0.5
        normed.append(c.resize((max(1, round(c.width * s)), max(1, round(c.height * s))),
                               Image.LANCZOS))
    hs = sorted(c.height for c in normed)
    g = HEIGHT / hs[len(hs) // 2]
    strip = Image.new("RGBA", (FW * n, FH), (0, 0, 0, 0))
    for i, c in enumerate(normed):
        nw, nh = max(1, round(c.width * g)), max(1, round(c.height * g))
        c = c.resize((nw, nh), Image.LANCZOS)
        cx = _centroid_x(c)
        ox = i * FW + FW // 2 - int(round(cx))       # centroid to frame centre
        oy = FH - nh - 1                             # feet on the baseline
        if nh > FH: c = c.crop((0, nh - FH, nw, nh)); oy = 0
        strip.paste(c, (ox, oy), c)
    return strip, n

def update_meta(name, frames):
    path = os.path.join(RAW, "meta.txt")
    lines = [l.rstrip("\n") for l in open(path)]
    entry = f"{name}_walkcycle {name}_walkcycle.png {FW} {FH} {frames} {AX} {AY}"
    for i, l in enumerate(lines):
        if l.split() and l.split()[0] == f"{name}_walkcycle":
            lines[i] = entry; break
    else:
        lines.append(entry)
    open(path, "w").write("\n".join(lines) + "\n")

def main():
    key = api_key()
    if not key:
        print("walkgen: no OPENAI_API_KEY"); return 1
    want = [a for a in sys.argv[1:]] or list(CHARS)
    for name in want:
        if name not in CHARS: continue
        img = generate(key, name, CHARS[name])
        strip, n = build_strip(img)
        if not strip:
            print(f"  {name}: NO FRAMES sliced"); continue
        strip.save(os.path.join(RAW, f"{name}_walkcycle.png"))
        update_meta(name, n)
        print(f"  {name}_walkcycle: {n} frames at {FW}x{FH}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
