#pragma once
// HUD: blood + water orbs, message log, sun-arc thermometer, day clock.
#include <cstdint>

#include "engine/assets.h"
#include "engine/render.h"

namespace ui {

// R2: 1080p HUD. The bottom panel is ~2x taller so the orbs/log/font (16px) fit.
inline constexpr int kPanelH = 100;
inline constexpr int kPanelY = fb::H - kPanelH;

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
    uint8_t noise_meter;       // 0..100, max noise near the player
    uint8_t worm_state;        // worm::State as int (0 dormant .. 4 surfaced)
    const uint8_t* seismo;     // 64-entry history ring
    uint8_t seismo_head;
    int thumpers;
    bool sandwalk;
};

void draw_hud(DrawCtx& ctx, const HudState& s, const Log& log, int anim_tick,
              const Assets& assets);

}  // namespace ui
