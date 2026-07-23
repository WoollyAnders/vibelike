#pragma once
// The game's render target. R1: 32-bit RGBA (ARGB8888, matching the present
// texture) truecolor — was an 8-bit indexed surface. Game code still authors in
// palette-index space; the draw primitives convert index -> RGB at blit time,
// with lighting as an RGB multiply (see render.cpp). Present uploads the buffer
// directly. Resolution stays 640x480 in R1; R2 raises it to 1080p.
#include <cstdint>

namespace fb {
inline constexpr int W = 1920;  // R2: 1080p internal. Iso tiles stay 64x32 (D2
inline constexpr int H = 1080;  // 2:1 diamonds) -> a wider, sharper tactical view.
}  // namespace fb

struct Framebuffer {
    uint32_t px[fb::W * fb::H];  // 0xAARRGGBB

    void clear(uint32_t color) {
        for (int i = 0; i < fb::W * fb::H; ++i) px[i] = color;
    }
    uint32_t& at(int x, int y) { return px[y * fb::W + x]; }
};
