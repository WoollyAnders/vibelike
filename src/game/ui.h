#pragma once
// HUD: blood + water orbs, message log, sun-arc thermometer, day clock.
#include <cstdint>

#include "engine/render.h"

namespace ui {

inline constexpr int kPanelH = 44;
inline constexpr int kPanelY = 360 - kPanelH;

struct Log {
    static constexpr int kLines = 8;
    static constexpr int kLen = 52;
    char lines[kLines][kLen];
    int count = 0;

    void push(const char* fmt, ...);
};

struct HudState {
    int hp, hp_max;
    int water, water_max;
    int exposure;
    int hour;      // 0..23
    int ambient;   // 1..9
    int day;
    bool indoors;
};

void draw_hud(DrawCtx& ctx, const HudState& s, const Log& log, int anim_tick);

}  // namespace ui
