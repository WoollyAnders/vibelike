#!/usr/bin/env python3
"""Asset pipeline orchestrator.

  (OpenAI genart.py | make_assets.py stand-ins)
        -> assets_src/raw/*.png + meta.txt      generated RGBA art
        -> atlas.py    -> <out>/atlas.bin       packed RGBA (v3) for the game
  plus palette.pal and sfx/ copied into <out>.

R3: palette-quantization (quantize.py) was removed from the pipeline; the atlas
now packs the raw RGBA straight (transparency via the alpha channel). palette.pal
is still shipped because text/fills (draw_glyph/fill_rect) draw palette indices.

Usage: build_assets.py <out_data_dir>
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
RAW = os.path.join(ROOT, "assets_src", "raw")
INDEXED = os.path.join(ROOT, "assets_src", "indexed")
ASSETS = os.path.join(ROOT, "assets")

def run(script, *args):
    subprocess.check_call([sys.executable, os.path.join(TOOLS, script), *args])

def main(out_dir):
    # 1. ensure raw art exists (procedural stand-ins until genart replaces them)
    if not os.path.isfile(os.path.join(RAW, "meta.txt")):
        run("make_assets.py")

    # 1b. derive walk-cycle strips from the base sprites
    run("animsynth.py")

    # 2. pack the RGBA atlas straight into the game's data dir (no quantize step)
    os.makedirs(out_dir, exist_ok=True)
    run("atlas.py", RAW, os.path.join(RAW, "meta.txt"),
        os.path.join(out_dir, "atlas.bin"))

    # 4. non-image data ships as-is
    shutil.copy(os.path.join(ASSETS, "palette.pal"), out_dir)
    shutil.copy(os.path.join(ASSETS, "anims.txt"), out_dir)
    sfx_out = os.path.join(out_dir, "sfx")
    shutil.rmtree(sfx_out, ignore_errors=True)
    shutil.copytree(os.path.join(ASSETS, "sfx"), sfx_out)
    print(f"data ready -> {out_dir}")

if __name__ == "__main__":
    main(sys.argv[1])
