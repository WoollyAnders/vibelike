#!/usr/bin/env python3
"""OpenAI art generation — the head of the asset pipeline.

Reads per-asset prompts from tools/art_prompts.py, calls the OpenAI image API,
and writes raw PNGs + meta.txt into assets_src/raw/ for quantize/atlas to
consume. Requires OPENAI_API_KEY in the environment.

Until a key is available, make_assets.py provides procedural stand-ins in the
same format, so the rest of the pipeline runs unchanged.
"""
import os
import sys

def main():
    key = os.environ.get("OPENAI_API_KEY")
    if not key:
        print("genart: OPENAI_API_KEY not set — run tools/make_assets.py for "
              "procedural stand-ins, or export a key to generate real art.")
        return 1
    # Implementation lands with M6 phase 2: per-asset prompt table, gpt-image-1
    # calls, background removal, downscale to native sprite sizes, frame
    # alignment for animation strips.
    print("genart: not yet implemented (M6 phase 2)")
    return 1

if __name__ == "__main__":
    sys.exit(main())
