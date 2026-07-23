#include "game/ui.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ui {

void Log::push(const char* fmt, ...) {
    std::memmove(lines[1], lines[0], sizeof(lines[0]) * (kLines - 1));
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(lines[0], kLen, fmt, ap);
    va_end(ap);
    if (count < kLines) ++count;
}

namespace {

// Filled orb: a circle that fills bottom-up with a color ramp; empty part dark.
void draw_orb(DrawCtx& ctx, int cx, int cy, int r, int fill_permille,
              uint8_t ramp_base, uint8_t rim_color) {
    const int fill_y = cy + r - (2 * r * fill_permille) / 1000;
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            const int d2 = x * x + y * y;
            if (d2 > r * r) continue;
            const int px = cx + x, py = cy + y;
            uint8_t c;
            if (d2 > (r - 1) * (r - 1)) {
                c = rim_color;
            } else if (py >= fill_y) {
                // deeper = darker; small highlight near the surface line
                const int depth = (py - fill_y);
                c = uint8_t(ramp_base + (depth < 2 ? 14 : 10 - (depth * 6) / (2 * r + 1)));
            } else {
                c = 3;  // empty glass
            }
            if (px >= 0 && px < fb::W && py >= 0 && py < fb::H)
                ctx.frame->at(px, py) = 0xff000000u | ctx.pal->color[c];
        }
    }
}

}  // namespace

void draw_hud(DrawCtx& ctx, const HudState& s, const Log& log, int anim_tick,
              const Assets& assets) {
    (void)assets;  // R2: the 640-wide ui_panel sprite no longer spans 1920; use fills.
    fill_rect(ctx, 0, kPanelY, fb::W, kPanelH, 4);
    fill_rect(ctx, 0, kPanelY, fb::W, 2, 12);

    // Blood orb (left) and water orb (right). Bigger (r=42) for the 1080p panel;
    // no socket sprite (it was authored tiny) — the orb rim reads as the socket.
    const int orb_r = 42;
    const int ocy = kPanelY + kPanelH / 2 + 4;
    draw_orb(ctx, 6 + orb_r, ocy, orb_r, s.hp_max ? s.hp * 1000 / s.hp_max : 0, 198, 96);
    draw_orb(ctx, fb::W - 6 - orb_r, ocy, orb_r, s.water_max ? s.water * 1000 / s.water_max : 0,
             172, 96);
    draw_text(ctx, "BLOOD", 6 + orb_r - 40, kPanelY + 4, 14);
    draw_text(ctx, "WATER", fb::W - 6 - orb_r - 40, kPanelY + 4, 14);

    const int col = 6 + 2 * orb_r + 16;  // left content column, clear of the blood orb

    // Gear line: thumpers + sandwalk indicator (top of the content column).
    char buf[48];
    std::snprintf(buf, sizeof buf, "thumpers %d%s", s.thumpers,
                  s.sandwalk ? "  [sandwalk]" : "");
    draw_text(ctx, buf, col, kPanelY + 6, uint8_t(s.sandwalk ? 240 : 14));

    // Message log, newest at the bottom, 16px lines.
    for (int i = 0; i < 3 && i < log.count; ++i) {
        char line[52];
        std::snprintf(line, sizeof line, "%s", log.lines[i]);
        draw_text(ctx, line, col, kPanelY + kPanelH - 26 - i * 22, uint8_t(i == 0 ? 28 : 20));
    }

    // Sun-arc thermometer: a world readout, so it lives in a top strip.
    const int bx = fb::W / 2 - 120, bw = 240, by = 8;
    fill_rect(ctx, bx, by, bw, 10, 2);
    for (int i = 0; i < bw; ++i) {
        const int hour = i * 24 / bw;
        const int amb = (hour >= 6 && hour <= 18) ? 5 + (9 - (hour > 12 ? hour - 12 : 12 - hour)) / 2 : 2;
        const uint32_t c = 0xff000000u | ctx.pal->color[amb >= 8 ? 150 : amb >= 5 ? 88 : 165];
        for (int yy = by + 3; yy < by + 7; ++yy) ctx.frame->at(bx + i, yy) = c;
    }
    const int sun_x = bx + s.hour * bw / 24;
    fill_rect(ctx, sun_x - 2, by - 2, 4, 14, uint8_t(s.ambient >= 8 ? 158 : 30));

    std::snprintf(buf, sizeof buf, "day %d  %02d:00 %s", s.day, s.hour,
                  s.indoors ? "sietch" : (s.ambient >= 8 ? "SCORCHING" : s.ambient <= 2 ? "cold" : ""));
    draw_text(ctx, buf, bx, by + 20, 22);

    // Exposure warning.
    if (s.exposure >= 50 && (anim_tick / 20) % 2 == 0) {
        std::snprintf(buf, sizeof buf, "EXPOSURE %d", s.exposure);
        draw_text_shadow(ctx, buf, bx, by + 42, 210, 1);
    }

    // Seismograph: 64-sample noise trace, colored by worm state. Right-center of
    // the panel, clear of the water orb.
    {
        const int gw = 128, gh = 48, gx = fb::W - 6 - 2 * orb_r - 40 - gw,
                  gy = kPanelY + (kPanelH - gh) / 2;
        fill_rect(ctx, gx - 3, gy - 3, gw + 6, gh + 6, 2);
        uint8_t trace;
        switch (s.worm_state) {
            case 0: trace = 246; break;              // dormant: green
            case 1: trace = 232; break;              // stirring: amber
            default: trace = 208; break;             // hunting+: red
        }
        const uint32_t tc = 0xff000000u | ctx.pal->color[trace];
        for (int i = 0; i < gw; ++i) {
            const uint8_t v = s.seismo[(s.seismo_head + (i / 2)) & 63];
            const int h = 1 + v * (gh - 2) / 100;
            const int x = gx + i;
            ctx.frame->at(x, gy + gh - 1 - h) = tc;
            if (i >= gw - 2)
                for (int yy = gy + gh - h; yy < gy + gh; ++yy) ctx.frame->at(x, yy) = tc;
        }
        draw_text(ctx, "WORMSIGN", gx, gy - 20, uint8_t(s.worm_state >= 2 ? 208 : 14));
    }
}

}  // namespace ui
