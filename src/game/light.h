#pragma once
// M7: Diablo-style lighting. A per-tile lightmap (0..31 colormap levels) is
// rebuilt every turn from the hour's ambient plus radial light sources with
// dithered half-step falloff. Rendering reads the lightmap instead of flat
// day/night levels — night is genuinely dark beyond your light.
#include <cstdint>

#include "game/map.h"

namespace light {

struct Source {
    int16_t x, y;
    uint8_t radius;     // tiles
    uint8_t intensity;  // colormap level at the source (0..31)
    int8_t ttl;         // transient flash turns remaining (unused by compute)
};

// Outdoor ambient light level by hour (0..23).
inline constexpr uint8_t kAmbientByHour[24] = {
    4, 4, 3, 3, 3, 4, 8, 14, 20, 24, 28, 30,
    31, 31, 31, 30, 28, 24, 18, 12, 8, 6, 5, 4,
};
inline constexpr int kAmbientIndoor = 28;  // the sietch is a lamp-lit home, not a night cave
inline constexpr int kEntityMinLight = 16;  // actors in view are never near-black (see the mobs)
inline constexpr int kShadePenalty = 5;   // daytime shade reads cooler
inline constexpr int kDaylightShadeAt = 12;  // outdoor ambient above this = "day" (shade bites)
inline constexpr int kRemembered = 4;     // out-of-sight memory level

// Composes ambient + sources into out[]. day_shade applies the shade penalty
// (pass true when ambient outshines the shade, i.e. daytime).
void compute(const map::Zone& z, const Source* sources, int count,
             int ambient_outdoor, uint8_t out[map::N]);

}  // namespace light
