#pragma once
// Master 256-color palette + runtime effects (fades, rotation) baked into a
// 32-bit lookup table used by the present path, plus Doom-style colormaps:
// 32 light levels remapping palette indices for distance/night falloff.
// All game rendering happens in indexed space; present is the only place
// indices become RGB.
#include <cstdint>

struct Palette {
    uint32_t color[256];  // 0xAARRGGBB
};

// A global fade toward a solid color. amount: 0 = master palette, 256 = fully faded.
struct PaletteFade {
    uint8_t r = 0, g = 0, b = 0;
    uint16_t amount = 0;
};

// 32 light levels. map[31] is identity; lower levels remap each index to the
// nearest palette color of the darkened original. Index 0 (transparent key)
// always maps to 0.
struct Colormaps {
    uint8_t map[32][256];
};

// Loads "DUSTFALL-PAL 256" text format (R G B per line). Returns false on
// parse failure. Index 0's stored color is irrelevant (transparent key);
// it is forced to black.
bool palette_load(const char* path, Palette& out);

void colormaps_build(const Palette& master, Colormaps& out);

// Builds the present-time LUT: master palette, optionally rotated within
// [rotate_lo, rotate_hi] (for cycling effects), then faded toward fade color.
void palette_build_lut(const Palette& master, const PaletteFade& fade,
                       int rotate_offset, int rotate_lo, int rotate_hi,
                       uint32_t out[256]);

// Procedural fallback palette (used if data/palette.pal is missing).
Palette palette_make_default();
