#pragma once
// The game's render target: a 640x480 8-bit indexed surface — Diablo 1's
// resolution, 4:3. Integer scaling pillarboxes on modern 16:9 displays
// (2x on 1080p, 3x on 1440p), exactly like playing the classics.
#include <cstdint>

namespace fb {
inline constexpr int W = 640;
inline constexpr int H = 480;
}  // namespace fb

struct Framebuffer {
    uint8_t px[fb::W * fb::H];

    void clear(uint8_t index) {
        for (int i = 0; i < fb::W * fb::H; ++i) px[i] = index;
    }
    uint8_t& at(int x, int y) { return px[y * fb::W + x]; }
};
