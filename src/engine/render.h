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

// R2: world sprites blit at this integer scale (set to iso::kView for the world
// passes, 1 for UI). Kept as global state so the ~20 world draw sites are unchanged.
void set_draw_scale(int s);

void draw_sprite(DrawCtx& ctx, const SpriteSheet& sheet, int frame_index,
                 int x, int y, int light = 31, bool flip_x = false);

// R2: default font scale (set once for the whole UI pass at 1080p) so the ~40
// text sites don't each need a scale arg. draw_text `scale=0` uses this global.
void set_text_scale(int s);
int text_scale();

// 8x8 bitmap font (public domain font8x8). Draws ASCII 32..126. scale 0 = use the
// global; >0 overrides. Advances x by 8*scale per glyph.
void draw_text(DrawCtx& ctx, const char* text, int x, int y, uint8_t color, int scale = 0);
void draw_text_shadow(DrawCtx& ctx, const char* text, int x, int y,
                      uint8_t color, uint8_t shadow, int scale = 0);

void fill_rect(DrawCtx& ctx, int x, int y, int w, int h, uint8_t color);
