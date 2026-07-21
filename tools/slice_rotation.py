#!/usr/bin/env python3
"""Slices a generated character turnaround sheet (5 views of the same figure
in a horizontal row) into an aligned, height-normalized direction strip.

View order expected from the prompt: front (S), 3/4 front-right (SE),
right profile (E), 3/4 back-right (NE), back (N) — matching the game's
frame layout S=0, SE=1, E=2, NE=3, N=4.
"""
from PIL import Image

def find_view_spans(img, want=5):
    """Split by fully-transparent column gaps; fall back to equal fifths."""
    w, h = img.size
    alpha = img.getchannel("A")
    occupied = []
    px = alpha.load()
    for x in range(w):
        col = False
        for y in range(0, h, 4):
            if px[x, y] > 40:
                col = True
                break
        occupied.append(col)

    spans = []
    start = None
    for x, occ in enumerate(occupied + [False]):
        if occ and start is None:
            start = x
        elif not occ and start is not None:
            spans.append((start, x))
            start = None

    # dropped weapons and props form small stray blobs; the five FIGURES are
    # the widest blobs. Center an equal-width window on each figure's center.
    if len(spans) >= want:
        biggest = sorted(spans, key=lambda sp: sp[1] - sp[0], reverse=True)[:want]
        centers = sorted((a + b) // 2 for a, b in biggest)
        half = w // (want * 2)
        return [(max(0, c - half), min(w, c + half)) for c in centers]

    fifth = w // want
    return [(i * fifth, (i + 1) * fifth) for i in range(want)]

def _widest_blob(view):
    """Column span of the widest contiguous opaque blob inside a window."""
    w, h = view.size
    px = view.getchannel("A").load()
    best, cur_start, best_span = 0, None, None
    for x in range(w + 1):
        occ = False
        if x < w:
            for y in range(0, h, 4):
                if px[x, y] > 40:
                    occ = True
                    break
        if occ and cur_start is None:
            cur_start = x
        elif not occ and cur_start is not None:
            if x - cur_start > best:
                best = x - cur_start
                best_span = (cur_start, x)
            cur_start = None
    return best_span

def slice_turnaround(img, fw, fh, views=5):
    """Returns a (fw*views, fh) RGBA strip, each view fit + bottom-anchored."""
    img = img.convert("RGBA")
    spans = find_view_spans(img, views)
    strip = Image.new("RGBA", (fw * views, fh), (0, 0, 0, 0))

    crops = []
    for (x0, x1) in spans:
        view = img.crop((x0, 0, x1, img.height))
        # neighbors can bleed into the window edge: keep only the widest blob
        sub = _widest_blob(view)
        if sub:
            view = view.crop((sub[0], 0, sub[1], view.height))
        bbox = view.getbbox()
        crops.append(view.crop(bbox) if bbox else view)

    # normalize to the tallest view so heads line up across directions
    tallest = max(c.height for c in crops)
    for i, c in enumerate(crops):
        scale = min((fw - 2) / c.width, (fh - 2) / tallest)
        nw, nh = max(1, int(c.width * scale)), max(1, int(c.height * scale))
        c = c.resize((nw, nh), Image.LANCZOS)
        strip.paste(c, (i * fw + (fw - nw) // 2, fh - nh - 1), c)
    return strip
