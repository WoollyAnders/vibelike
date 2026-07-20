#include "engine/render.h"

#include "font8x8_basic.h"

void draw_sprite(DrawCtx& ctx, const SpriteSheet& sheet, int frame_index,
                 int x, int y, int light, bool flip_x) {
    if (frame_index < 0 || frame_index >= sheet.frames) return;
    light = light < 0 ? 0 : (light > 31 ? 31 : light);
    const uint8_t* remap = ctx.cmaps->map[light];

    const int x0 = x - sheet.ax;
    const int y0 = y - sheet.ay;
    const uint8_t* src = sheet.frame_ptr(frame_index);
    const int pitch = sheet.pitch();

    int sy0 = 0, sy1 = sheet.fh;
    if (y0 < 0) sy0 = -y0;
    if (y0 + sy1 > fb::H) sy1 = fb::H - y0;
    int sx0 = 0, sx1 = sheet.fw;
    if (x0 < 0) sx0 = -x0;
    if (x0 + sx1 > fb::W) sx1 = fb::W - x0;

    for (int sy = sy0; sy < sy1; ++sy) {
        const uint8_t* row = src + sy * pitch;
        uint8_t* dst = ctx.frame->px + (y0 + sy) * fb::W + x0;
        for (int sx = sx0; sx < sx1; ++sx) {
            const uint8_t p = row[flip_x ? sheet.fw - 1 - sx : sx];
            if (p) dst[sx] = remap[p];
        }
    }
}

static void draw_glyph(Framebuffer& f, char c, int x, int y, uint8_t color) {
    if (c < 32 || c > 126) return;
    const unsigned char* glyph = font8x8_basic[int(c)];
    for (int gy = 0; gy < 8; ++gy) {
        const int py = y + gy;
        if (py < 0 || py >= fb::H) continue;
        const uint8_t bits = glyph[gy];
        for (int gx = 0; gx < 8; ++gx) {
            if (!(bits & (1 << gx))) continue;
            const int px = x + gx;
            if (px >= 0 && px < fb::W) f.at(px, py) = color;
        }
    }
}

void draw_text(DrawCtx& ctx, const char* text, int x, int y, uint8_t color) {
    for (const char* c = text; *c; ++c, x += 8) draw_glyph(*ctx.frame, *c, x, y, color);
}

void draw_text_shadow(DrawCtx& ctx, const char* text, int x, int y,
                      uint8_t color, uint8_t shadow) {
    draw_text(ctx, text, x + 1, y + 1, shadow);
    draw_text(ctx, text, x, y, color);
}

void fill_rect(DrawCtx& ctx, int x, int y, int w, int h, uint8_t color) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > fb::W ? fb::W : x + w;
    int y1 = y + h > fb::H ? fb::H : y + h;
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) ctx.frame->at(px, py) = color;
}
