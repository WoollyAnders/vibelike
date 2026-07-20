#pragma once
// Isometric projection and the y-sorted sprite draw list. Game logic lives on
// a square grid; this is projection only. 2:1 diamonds: a tile is 32x16 px,
// tile (tx,ty)'s diamond CENTER projects to ((tx-ty)*16, (tx+ty)*8) in world
// pixels. The camera holds the world-pixel coords shown at screen center.
#include <cstdint>
#include <vector>

#include "engine/framebuffer.h"
#include "engine/render.h"

namespace iso {

inline constexpr int kHalfW = 16;  // half diamond width
inline constexpr int kHalfH = 8;   // half diamond height

struct Camera {
    int x = 0, y = 0;  // world pixels at screen center
};

inline void tile_to_screen(int tx, int ty, const Camera& cam, int& sx, int& sy) {
    sx = (tx - ty) * kHalfW - cam.x + fb::W / 2;
    sy = (tx + ty) * kHalfH - cam.y + fb::H / 2;
}

// Inverse mapping; screen point -> containing tile (rounded to nearest center).
inline void screen_to_tile(int sx, int sy, const Camera& cam, int& tx, int& ty) {
    const int wx = sx + cam.x - fb::W / 2;
    const int wy = sy + cam.y - fb::H / 2;
    // tx = wx/32 + wy/16, ty = wy/16 - wx/32 — computed in 1/32 units with
    // rounding toward nearest.
    const int a = wx * 1 + wy * 2;   // 32*tx  (since wx=16(tx-ty), wy=8(tx+ty))
    const int b = wy * 2 - wx * 1;   // 32*ty
    tx = (a + (a >= 0 ? 16 : -16)) >> 5;
    ty = (b + (b >= 0 ? 16 : -16)) >> 5;
}

// Deferred sprite draw, sorted by screen y (then an explicit layer bias) so
// southern objects draw over northern ones.
struct DrawList {
    struct Item {
        int32_t key;  // (sy << 4) | layer
        const SpriteSheet* sheet;
        int16_t frame;
        int16_t x, y;
        int8_t light;
        bool flip;
    };
    std::vector<Item> items;

    void clear() { items.clear(); }
    void add(const SpriteSheet* sheet, int frame, int x, int y, int light,
             int layer = 0, bool flip = false) {
        items.push_back(Item{(int32_t(y) << 4) | (layer & 15), sheet,
                             int16_t(frame), int16_t(x), int16_t(y),
                             int8_t(light), flip});
    }
    void flush(DrawCtx& ctx);
};

}  // namespace iso
