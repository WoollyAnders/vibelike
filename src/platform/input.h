#pragma once
// Logical input state, decoupled from SDL scancodes via a rebindable mapping
// table. The platform layer fills this once per frame; edge flags (pressed/
// released) are consumed by the first game tick of that frame, and per-key
// held-tick counters drive roguelike key repeat at tick granularity.
#include <cstdint>

enum class Key : uint8_t {
    Up, Down, Left, Right,
    UpLeft, UpRight, DownLeft, DownRight,   // diagonal moves (numpad)
    Wait,
    Confirm, Cancel,
    Inventory, Look, Powers, Sandwalk, Map, Harvest, Thumper, Plant, Poison,
    Save, Load,
    Count
};
inline constexpr int kKeyCount = int(Key::Count);

struct InputState {
    bool down[kKeyCount] = {};
    bool pressed[kKeyCount] = {};   // went down since last tick consumed
    bool released[kKeyCount] = {};  // went up since last tick consumed
    uint16_t held_ticks[kKeyCount] = {};

    int mouse_x = -1, mouse_y = -1;  // framebuffer coords; -1 when outside
    bool mouse_down[3] = {};
    bool mouse_pressed[3] = {};

    bool is_down(Key k) const { return down[int(k)]; }
    bool was_pressed(Key k) const { return pressed[int(k)]; }

    // True on the initial press, then repeats after `delay` ticks every
    // `interval` ticks while held. The roguelike movement primitive.
    bool pressed_or_repeat(Key k, int delay = 18, int interval = 6) const {
        if (pressed[int(k)]) return true;
        const int held = held_ticks[int(k)];
        return down[int(k)] && held >= delay && (held - delay) % interval == 0;
    }

    // Called once per game tick after the game consumed this tick's input.
    void end_tick() {
        for (int i = 0; i < kKeyCount; ++i) {
            if (down[i]) ++held_ticks[i]; else held_ticks[i] = 0;
            pressed[i] = false;
            released[i] = false;
        }
        for (bool& b : mouse_pressed) b = false;
    }
};
