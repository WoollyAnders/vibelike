#!/usr/bin/env python3
"""Atlas Builder: indexed PNGs + meta.txt -> data/atlas.bin.

Packs every sprite sheet (frames stay as one horizontal strip so the engine
can address frame N as px + N*fw) into 1024x1024 indexed pages with a simple
shelf packer, and writes a single binary the game loads at startup.

Format (little-endian):
  u32 magic 'DATL'   u16 version=1
  u16 page_count     u16 entry_count
  u16 page_w         u16 page_h
  pages: page_count * page_w*page_h bytes (indexed pixels)
  entries: entry_count * {
      char name[24]  u16 fw  u16 fh  u16 frames
      i16 ax  i16 ay  u16 page  u16 x  u16 y
  }
"""
import os
import struct
import sys
from PIL import Image

PAGE_W = PAGE_H = 1024

def main(indexed_dir, meta_path, out_path):
    entries = []
    with open(meta_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, fname, fw, fh, frames, ax, ay = line.split()
            entries.append((name, fname, int(fw), int(fh), int(frames), int(ax), int(ay)))

    # shelf packer, tallest-first
    entries.sort(key=lambda e: -e[3])
    pages = [bytearray(PAGE_W * PAGE_H)]
    shelves = [[]]  # per page: list of (shelf_y, shelf_h, cursor_x)
    placed = []

    def place(w, h):
        for pi in range(len(pages)):
            y = 0
            for si, (sy, sh, cx) in enumerate(shelves[pi]):
                if h <= sh and cx + w <= PAGE_W:
                    shelves[pi][si] = (sy, sh, cx + w)
                    return pi, cx, sy
                y = max(y, sy + sh)
            if y + h <= PAGE_H:
                shelves[pi].append((y, h, w))
                return pi, 0, y
        pages.append(bytearray(PAGE_W * PAGE_H))
        shelves.append([(0, h, w)])
        return len(pages) - 1, 0, 0

    for name, fname, fw, fh, frames, ax, ay in entries:
        img = Image.open(os.path.join(indexed_dir, fname))
        assert img.mode == "P", f"{fname} is not indexed"
        w, h = img.size
        assert w == fw * frames and h == fh, \
            f"{fname}: {w}x{h} != {fw}x{fh} x{frames}"
        pi, x, y = place(w, h)
        data = img.tobytes()
        for row in range(h):
            off = (y + row) * PAGE_W + x
            pages[pi][off:off + w] = data[row * w:(row + 1) * w]
        placed.append((name, fw, fh, frames, ax, ay, pi, x, y))

    with open(out_path, "wb") as f:
        f.write(struct.pack("<IHHHHH", 0x4C544144, 1, len(pages), len(placed),
                            PAGE_W, PAGE_H))
        for p in pages:
            f.write(p)
        for name, fw, fh, frames, ax, ay, pi, x, y in placed:
            f.write(struct.pack("<24sHHHhhHHH", name.encode()[:24].ljust(24, b"\0"),
                                fw, fh, frames, ax, ay, pi, x, y))
    total = os.path.getsize(out_path)
    print(f"atlas: {len(placed)} entries on {len(pages)} page(s), {total//1024}KB -> {out_path}")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
