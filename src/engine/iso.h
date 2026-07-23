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

// R2: the logical grid is 64x32 diamonds, rendered at 2x (128x64 on screen) for
// a Diablo-II zoom at 1080p. kHalfW/kHalfH are the ON-SCREEN half-tile pixels.
inline constexpr int kView = 2;                 // world render scale
inline constexpr int kHalfW = 32 * kView;       // 64
inline constexpr int kHalfH = 16 * kView;       // 32

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
    // wx = (tx-ty)*kHalfW, wy = (tx+ty)*kHalfH
    // => tx = wx/(2*kHalfW) + wy/(2*kHalfH), ty likewise with -wx.
    const int a = wx + 2 * wy;       // (2*kHalfW) * tx, since kHalfW == 2*kHalfH
    const int b = 2 * wy - wx;       // (2*kHalfW) * ty
    constexpr int d = 2 * kHalfW;
    tx = (a + (a >= 0 ? d / 2 : -d / 2)) / d;
    ty = (b + (b >= 0 ? d / 2 : -d / 2)) / d;
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
