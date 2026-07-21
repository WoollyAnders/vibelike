#!/usr/bin/env python3
"""Synthesizes walk-cycle strips from base sprites (until G2 brings real
per-frame art): 4 frames of stride shear + bob derived from frame 0 of each
character's sheet. Writes <name>_walk.png beside the raw art and appends
meta.txt entries when missing.
"""
import os
from PIL import Image

RAW = os.path.join(os.path.dirname(__file__), "..", "assets_src", "raw")

WALKERS = ["player_fedaykin", "player_saboteur", "player_sayyadina",
           "trooper", "hound", "hermit"]

def synth_walk(base_frame, fw, fh):
    """4 frames: lean-left, upright+bob, lean-right, upright+bob."""
    frames = []
    for i in range(4):
        img = Image.new("RGBA", (fw, fh), (0, 0, 0, 0))
        shear = (-2, 0, 2, 0)[i]
        bob = (0, -1, 0, -1)[i]
        # shear: shift rows progressively toward the feet
        for y in range(fh):
            t = y / max(1, fh - 1)
            dx = int(shear * (1.0 - t))  # top leans, feet stay planted
            row = base_frame.crop((0, y, fw, y + 1))
            img.paste(row, (dx, y + bob), row)
        frames.append(img)
    sheet = Image.new("RGBA", (fw * 4, fh), (0, 0, 0, 0))
    for i, f in enumerate(frames):
        sheet.paste(f, (i * fw, 0), f)
    return sheet

def main():
    meta_path = os.path.join(RAW, "meta.txt")
    entries = {}
    lines = []
    with open(meta_path) as f:
        for line in f:
            lines.append(line.rstrip("\n"))
            parts = line.split()
            if parts and not parts[0].startswith("#"):
                entries[parts[0]] = parts

    added = 0
    for name in WALKERS:
        if name not in entries:
            continue
        walk_name = f"{name}_walk"
        _, fname, fw, fh, frames, ax, ay = entries[name]
        fw, fh = int(fw), int(fh)
        base = Image.open(os.path.join(RAW, fname)).convert("RGBA").crop((0, 0, fw, fh))
        synth_walk(base, fw, fh).save(os.path.join(RAW, f"{walk_name}.png"))
        if walk_name not in entries:
            lines.append(f"{walk_name} {walk_name}.png {fw} {fh} 4 {ax} {ay}")
            added += 1

    with open(meta_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"animsynth: walk strips written ({added} new meta entries)")

if __name__ == "__main__":
    main()
