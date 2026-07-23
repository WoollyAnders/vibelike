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

GRID_ROWS, GRID_COLS = 2, 4   # 8 keyframes, read left-to-right then top-to-bottom

def prompt_for(desc):
    return (
        f"A {GRID_ROWS}-row by {GRID_COLS}-column grid of {GRID_ROWS*GRID_COLS} "
        "animation keyframes on a fully transparent background, exactly ONE figure "
        "centered in each grid cell, read left-to-right then top-to-bottom as the "
        "frames of one smooth walk cycle. Every cell shows the SAME character in "
        "strict right-facing side profile at the next point of the stride: contact "
        "with the left foot forward, passing, contact with the right foot forward, "
        "passing, and so on — legs swing (one forward, one back) and arms swing "
        "opposite the legs, so the cells in order animate a walking loop. "
        f"Character: {desc}. CRITICAL: keep the character's costume, colors, "
        "proportions and overall SIZE IDENTICAL in every cell; only the limb pose "
        "changes between cells. Center each figure in its cell with generous "
        "transparent margins; figures never touch or cross cell boundaries; feet "
        "level within each cell. " + STYLE + ". Evenly sized cells, no visible grid "
        "lines, no panel borders, no ground, no shadow, no text, no numbers.")

def generate(key, name, desc):
    cache = os.path.join(CACHE, f"{name}_walkcycle.png")
    if os.path.isfile(cache):
        print(f"  {name}: cached")
        return Image.open(cache).convert("RGBA")
    body = {"model": "gpt-image-1", "prompt": prompt_for(desc), "size": "1024x1024",
            "quality": "high", "output_format": "png", "background": "transparent", "n": 1}
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

def _largest_component(cell):
    """Keep only the largest connected opaque blob in a cell — drops the stray
    detached marks the model sometimes paints (a floating blade, a stray box)
    while keeping the figure and anything it holds/touches."""
    from collections import deque
    w, h = cell.size
    a = cell.getchannel("A").load()
    seen = bytearray(w * h)
    best, best_n = None, 0
    for sy in range(h):
        for sx in range(w):
            if seen[sy * w + sx] or a[sx, sy] <= 60:
                continue
            comp, q = [], deque([(sx, sy)])
            seen[sy * w + sx] = 1
            while q:
                x, y = q.popleft(); comp.append((x, y))
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx] and a[nx, ny] > 60:
                        seen[ny * w + nx] = 1; q.append((nx, ny))
            if len(comp) > best_n:
                best_n, best = len(comp), comp
    out = Image.new("RGBA", cell.size, (0, 0, 0, 0))
    if best:
        src, dst = cell.load(), out.load()
        for (x, y) in best:
            dst[x, y] = src[x, y]
    return out

def grid_cells(img):
    """Divide the image into the GRID_ROWS x GRID_COLS grid and return the
    cleaned, bbox-cropped figure from each non-empty cell, in reading order
    (the frame sequence). A fixed grid slices far more reliably than gaps."""
    w, h = img.size
    cw, ch = w // GRID_COLS, h // GRID_ROWS
    cells = []
    for r in range(GRID_ROWS):
        for c in range(GRID_COLS):
            cell = _largest_component(img.crop((c * cw, r * ch, (c + 1) * cw, (r + 1) * ch)))
            bb = cell.getbbox()
            if not bb:
                continue
            fig = cell.crop(bb)
            if fig.width >= 20 and fig.height >= 36:   # skip specks / stray marks
                cells.append(fig)
    return cells

def _alpha_area(c):
    a = c.getchannel("A").load()
    w, h = c.size
    return sum(1 for y in range(h) for x in range(w) if a[x, y] > 60) or 1

def _head_ref(c):
    """Opaque mass in the top ~20% (head + hood). Nearly pose-invariant across a
    walk cycle, so it's a stable proxy for the character's true scale — unlike
    full-body mass or bbox height, which change as the legs spread."""
    a = c.getchannel("A").load()
    w, h = c.size
    band = max(2, int(h * 0.20))
    return sum(1 for y in range(band) for x in range(w) if a[x, y] > 60) or 1

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
    crops = grid_cells(img)
    if not crops:
        return None, 0
    # Drop mis-sliced fragments (a detached blade/box that crossed a cell edge):
    # they carry far less opaque mass than a real figure.
    a_all = [_alpha_area(c) for c in crops]
    med = sorted(a_all)[len(a_all) // 2]
    crops = [c for c, a in zip(crops, a_all) if a >= 0.55 * med]
    n = len(crops)
    # gpt-image-1 draws each frame at a slightly different scale — the source of
    # the frame-to-frame "size change". Normalize every frame by its HEAD-band size
    # (pose-invariant: the head/hood doesn't change as the legs spread, unlike full
    # mass or bbox height), then one global scale so the median head sits at HEIGHT.
    # Feet bottom-anchored; body centred by centroid so limb swing doesn't slide it.
    refs = [_head_ref(c) for c in crops]
    target_ref = sorted(refs)[len(refs) // 2]
    normed = []
    for c, rf in zip(crops, refs):
        s = (target_ref / rf) ** 0.5
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
