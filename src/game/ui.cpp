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
                ctx.frame->at(px, py) = c;
        }
    }
}

}  // namespace

void draw_hud(DrawCtx& ctx, const HudState& s, const Log& log, int anim_tick) {
    fill_rect(ctx, 0, kPanelY, fb::W, kPanelH, 4);
    fill_rect(ctx, 0, kPanelY, fb::W, 1, 12);

    // Blood orb (left) and water orb (right).
    const int oy = kPanelY + kPanelH / 2 + 2;
    draw_orb(ctx, 26, oy, 17, s.hp_max ? s.hp * 1000 / s.hp_max : 0, 198, 96);
    draw_orb(ctx, fb::W - 26, oy, 17, s.water_max ? s.water * 1000 / s.water_max : 0, 172, 96);
    draw_text(ctx, "BLOOD", 6, kPanelY + 2, 14);
    draw_text(ctx, "WATER", fb::W - 46, kPanelY + 2, 14);

    // Message log, newest at the bottom.
    for (int i = 0; i < 3 && i < log.count; ++i)
        draw_text(ctx, log.lines[i], 56, kPanelY + 30 - i * 10, uint8_t(i == 0 ? 28 : 20));

    // Sun-arc thermometer: a bar with the sun marker; color by ambient heat.
    const int bx = fb::W - 190, bw = 120, by = kPanelY + 6;
    fill_rect(ctx, bx, by, bw, 6, 2);
    for (int i = 0; i < bw; ++i) {
        const int hour = i * 24 / bw;
        const int amb = (hour >= 6 && hour <= 18) ? 5 + (9 - (hour > 12 ? hour - 12 : 12 - hour)) / 2 : 2;
        ctx.frame->at(bx + i, by + 2) = uint8_t(amb >= 8 ? 150 : amb >= 5 ? 88 : 165);
    }
    const int sun_x = bx + s.hour * bw / 24;
    fill_rect(ctx, sun_x - 1, by - 1, 3, 8, uint8_t(s.ambient >= 8 ? 158 : 30));

    char buf[40];
    std::snprintf(buf, sizeof buf, "day %d  %02d:00 %s", s.day, s.hour,
                  s.indoors ? "sietch" : (s.ambient >= 8 ? "SCORCHING" : s.ambient <= 2 ? "cold" : ""));
    draw_text(ctx, buf, bx, by + 10, 22);

    // Exposure warning.
    if (s.exposure >= 50 && (anim_tick / 20) % 2 == 0) {
        std::snprintf(buf, sizeof buf, "EXPOSURE %d", s.exposure);
        draw_text_shadow(ctx, buf, bx, by + 22, 210, 1);
    }

    // Seismograph: 64-sample noise trace, colored by worm state.
    {
        const int gx = 452, gw = 64, gy = kPanelY + 8, gh = 24;
        fill_rect(ctx, gx - 2, gy - 2, gw + 4, gh + 4, 2);
        uint8_t trace;
        switch (s.worm_state) {
            case 0: trace = 246; break;              // dormant: green
            case 1: trace = 232; break;              // stirring: amber
            default: trace = 208; break;             // hunting+: red
        }
        for (int i = 0; i < gw; ++i) {
            const uint8_t v = s.seismo[(s.seismo_head + i) & 63];
            const int h = 1 + v * (gh - 2) / 100;
            const int x = gx + i;
            ctx.frame->at(x, gy + gh - 1 - h) = trace;
            if (i == gw - 1)
                for (int yy = gy + gh - h; yy < gy + gh; ++yy)
                    ctx.frame->at(x, yy) = trace;
        }
        draw_text(ctx, "WORMSIGN", gx - 2, gy + gh + 3, uint8_t(s.worm_state >= 2 ? 208 : 14));
    }

    // Gear line: thumpers + sandwalk indicator.
    std::snprintf(buf, sizeof buf, "thumpers %d%s", s.thumpers,
                  s.sandwalk ? "  [sandwalk]" : "");
    draw_text(ctx, buf, 56, kPanelY + 2, uint8_t(s.sandwalk ? 240 : 14));
}

}  // namespace ui
