#include "game/light.h"

namespace light {

namespace {
// Integer sqrt of small values, doubled (result in half-tile units).
int isqrt2x(int v4) {  // v4 = d2 * 4
    int r = 0;
    while ((r + 1) * (r + 1) <= v4) ++r;
    return r;
}
}  // namespace

void compute(const map::Zone& z, const Source* sources, int count,
             int ambient_outdoor, uint8_t out[map::N]) {
    const bool day = ambient_outdoor > kAmbientIndoor + kShadePenalty;
    for (int i = 0; i < map::N; ++i) {
        const uint8_t f = z.flags[i];
        int a = (f & map::TF_INDOOR) ? kAmbientIndoor : ambient_outdoor;
        if (day && !(f & map::TF_INDOOR) && (f & map::TF_SHADE)) a -= kShadePenalty;
        out[i] = uint8_t(a < 0 ? 0 : a);
    }

    for (int s = 0; s < count; ++s) {
        const Source& src = sources[s];
        const int r = src.radius;
        for (int dy = -r; dy <= r; ++dy) {
            const int y = src.y + dy;
            if (y < 0 || y >= map::H) continue;
            for (int dx = -r; dx <= r; ++dx) {
                const int x = src.x + dx;
                if (x < 0 || x >= map::W) continue;
                const int d2 = dx * dx + dy * dy;
                if (d2 > r * r) continue;
                // falloff: 1.5 levels per tile, computed in half-levels with a
                // checkerboard dither on the fractional band
                const int lvl2 = int(src.intensity) * 2 - isqrt2x(d2 * 4) * 3 / 2;
                int lvl = lvl2 >> 1;
                if ((lvl2 & 1) && ((x + y) & 1)) ++lvl;
                if (lvl < 0) continue;
                if (lvl > 31) lvl = 31;
                const int i = y * map::W + x;
                if (uint8_t(lvl) > out[i]) out[i] = uint8_t(lvl);
            }
        }
    }
}

}  // namespace light
