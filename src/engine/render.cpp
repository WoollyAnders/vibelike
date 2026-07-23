#include "engine/render.h"

#include "font8x8_basic.h"

// R1: RGBA truecolor. Sprites/text/fills are authored in palette-index space;
// we look up the master palette (0xAARRGGBB) and, for sprites, apply lighting as
// a straight RGB multiply by light/31 (replaces the indexed colormap remap).
// Index 0 in sprites is the transparent key.
namespace {
int g_scale = 1;      // world sprites blit at this scale; UI stays 1
int g_text_scale = 1;  // default font scale for the UI pass (see set_text_scale)
inline uint32_t shade(uint32_t argb, int light) {
    if (light >= 31) return 0xff000000u | argb;
    const uint32_t r = (((argb >> 16) & 0xff) * unsigned(light)) / 31;
    const uint32_t g = (((argb >> 8) & 0xff) * unsigned(light)) / 31;
    const uint32_t b = ((argb & 0xff) * unsigned(light)) / 31;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}
inline void plot(Framebuffer* f, int x, int y, uint32_t c) {
    if (x >= 0 && x < fb::W && y >= 0 && y < fb::H) f->px[y * fb::W + x] = c;
}
}  // namespace

void set_draw_scale(int s) { g_scale = s < 1 ? 1 : s; }
void set_text_scale(int s) { g_text_scale = s < 1 ? 1 : s; }
int text_scale() { return g_text_scale; }

void draw_sprite(DrawCtx& ctx, const SpriteSheet& sheet, int frame_index,
                 int x, int y, int light, bool flip_x) {
    if (frame_index < 0 || frame_index >= sheet.frames) return;
    light = light < 0 ? 0 : (light > 31 ? 31 : light);
    const int s = g_scale;
    // (x, y) is the anchor point; the sprite scales about it by s (nearest).
    const int x0 = x - sheet.ax * s;
    const int y0 = y - sheet.ay * s;
    const uint32_t* base = sheet.frame_ptr(frame_index);  // R3: native ARGB8888
    const int pitch = sheet.pitch;
    Framebuffer* fbuf = ctx.frame;

    for (int sy = 0; sy < sheet.fh; ++sy) {
        const uint32_t* row = base + sy * pitch;
        const int py0 = y0 + sy * s;
        if (py0 + s <= 0 || py0 >= fb::H) continue;
        for (int sx = 0; sx < sheet.fw; ++sx) {
            const uint32_t argb = row[flip_x ? sheet.fw - 1 - sx : sx];
            if ((argb >> 24) < 128) continue;  // 1-bit alpha key (transparent)
            const uint32_t c = shade(argb, light);
            const int px0 = x0 + sx * s;
            for (int dy = 0; dy < s; ++dy)
                for (int dx = 0; dx < s; ++dx) plot(fbuf, px0 + dx, py0 + dy, c);
        }
    }
}

static void draw_glyph(DrawCtx& ctx, char c, int x, int y, uint8_t color, int s) {
    if (c < 32 || c > 126) return;
    const uint32_t rgb = 0xff000000u | ctx.pal->color[color];
    const unsigned char* glyph = font8x8_basic[int(c)];
    for (int gy = 0; gy < 8; ++gy)
        for (int gx = 0; gx < 8; ++gx) {
            if (!(glyph[gy] & (1 << gx))) continue;
            for (int dy = 0; dy < s; ++dy)
                for (int dx = 0; dx < s; ++dx) plot(ctx.frame, x + gx * s + dx, y + gy * s + dy, rgb);
        }
}

void draw_text(DrawCtx& ctx, const char* text, int x, int y, uint8_t color, int scale) {
    const int s = scale > 0 ? scale : g_text_scale;
    for (const char* c = text; *c; ++c, x += 8 * s) draw_glyph(ctx, *c, x, y, color, s);
}

void draw_text_shadow(DrawCtx& ctx, const char* text, int x, int y,
                      uint8_t color, uint8_t shadow, int scale) {
    const int s = scale > 0 ? scale : g_text_scale;
    draw_text(ctx, text, x + s, y + s, shadow, s);
    draw_text(ctx, text, x, y, color, s);
}

void fill_rect(DrawCtx& ctx, int x, int y, int w, int h, uint8_t color) {
    const uint32_t rgb = 0xff000000u | ctx.pal->color[color];
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > fb::W ? fb::W : x + w;
    int y1 = y + h > fb::H ? fb::H : y + h;
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) ctx.frame->at(px, py) = rgb;
}
