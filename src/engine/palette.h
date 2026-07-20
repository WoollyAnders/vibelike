#pragma once
// Master 256-color palette + runtime effects (fades, rotation) baked into a
// 32-bit lookup table used by the present path. All game rendering happens in
// indexed space; this is the only place indices become RGB.
#include <cstdint>

struct Palette {
    uint32_t color[256];  // 0xAARRGGBB
};

// A global fade toward a solid color. amount: 0 = master palette, 256 = fully faded.
struct PaletteFade {
    uint8_t r = 0, g = 0, b = 0;
    uint16_t amount = 0;
};

// Builds the present-time LUT: master palette, optionally rotated within
// [rotate_lo, rotate_hi] (for cycling effects), then faded toward fade color.
void palette_build_lut(const Palette& master, const PaletteFade& fade,
                       int rotate_offset, int rotate_lo, int rotate_hi,
                       uint32_t out[256]);

// E1 placeholder: a procedurally generated desert-toned palette, replaced by
// the authored data/palette.pal in E3.
Palette palette_make_default();
