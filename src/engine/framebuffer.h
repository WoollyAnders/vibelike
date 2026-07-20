#pragma once
// The game's render target: a 640x360 8-bit indexed surface. 16:9, so it
// integer-scales exactly to 720p (2x), 1080p (3x), 1440p (4x), and 4K (6x).
#include <cstdint>

namespace fb {
inline constexpr int W = 640;
inline constexpr int H = 360;
}  // namespace fb

struct Framebuffer {
    uint8_t px[fb::W * fb::H];

    void clear(uint8_t index) {
        for (int i = 0; i < fb::W * fb::H; ++i) px[i] = index;
    }
    uint8_t& at(int x, int y) { return px[y * fb::W + x]; }
};
