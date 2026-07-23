#pragma once
// Indexed-space blitters. Everything draws palette indices into the
// Framebuffer; lighting is a colormap remap chosen per draw call
// (light 0 = black, 31 = full bright). Index 0 in sprites = transparent.
#include <cstdint>

#include "engine/assets.h"
#include "engine/framebuffer.h"
#include "engine/palette.h"
// (SpriteSheet pixels now live in atlas pages; draw code is unchanged beyond
// pitch being a field.)

struct DrawCtx {
    Framebuffer* frame;
    const Colormaps* cmaps;      // (unused since R1's RGB lighting; kept for now)
    const Palette* pal = nullptr;  // index -> RGB source for the RGBA primitives
};

void draw_sprite(DrawCtx& ctx, const SpriteSheet& sheet, int frame_index,
                 int x, int y, int light = 31, bool flip_x = false);

// 8x8 bitmap font (public domain font8x8). Draws ASCII 32..126.
void draw_text(DrawCtx& ctx, const char* text, int x, int y, uint8_t color);
void draw_text_shadow(DrawCtx& ctx, const char* text, int x, int y,
                      uint8_t color, uint8_t shadow);

void fill_rect(DrawCtx& ctx, int x, int y, int w, int h, uint8_t color);
