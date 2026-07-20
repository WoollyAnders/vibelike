#pragma once
// The sandworm: a map-scale creature whose only sense is the noise heatmap.
// Burrowed states track a head position telegraphed by ripples; SURFACED puts
// a real segment chain on the map that crushes anything on sand it passes
// over. Armored except the maw; hurt enough, it submerges and goes quiet.
#include <cstdint>

#include "engine/rng.h"
#include "game/map.h"

namespace worm {

enum class State : uint8_t { Dormant = 0, Stirring, Hunting, Breaching, Surfaced };

inline constexpr int kSegments = 7;  // + head

struct Worm {
    State state = State::Dormant;
    int16_t x = 0, y = 0;          // head
    int16_t seg_x[kSegments], seg_y[kSegments];
    int16_t hp = 60;
    uint8_t timer = 0;             // breach windup / gorge countdown
    uint8_t quiet_turns = 0;       // stirring -> dormant hysteresis
};

struct NoiseField {
    int16_t at[map::N] = {};

    void add(int x, int y, int amount) {
        if (x < 0 || x >= map::W || y < 0 || y >= map::H) return;
        int v = at[y * map::W + x] + amount;
        at[y * map::W + x] = int16_t(v > 400 ? 400 : v);
    }
    void decay() {
        for (int i = 0; i < map::N; ++i) at[i] -= at[i] >> 2;  // -25%
    }
    void clear() {
        for (int i = 0; i < map::N; ++i) at[i] = 0;
    }
    // Sharply cut noise around a point (engine shutdown, dampeners).
    void dampen(int cx, int cy, int r, int shift) {
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x) {
                if (x < 0 || x >= map::W || y < 0 || y >= map::H) continue;
                at[y * map::W + x] >>= shift;
            }
    }
    // loudest cell (returns value; xy out)
    int hottest(int& hx, int& hy) const {
        int best = 0, bi = 0;
        for (int i = 0; i < map::N; ++i)
            if (at[i] > best) { best = at[i]; bi = i; }
        hx = bi % map::W;
        hy = bi / map::W;
        return best;
    }
    // max noise within radius r of (cx, cy) — the HUD meter
    int near(int cx, int cy, int r) const {
        int best = 0;
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x) {
                if (x < 0 || x >= map::W || y < 0 || y >= map::H) continue;
                if (at[y * map::W + x] > best) best = at[y * map::W + x];
            }
        return best;
    }
};

struct Tuning {
    int stir_threshold = 40;
    int hunt_threshold = 70;
    bool active = true;  // false in the sietch
};

// Callbacks into game.cpp so the worm module stays decoupled.
struct Callbacks {
    void* user;
    // Something stood at (x,y) on unsafe ground as the worm passed: kill it.
    void (*crush)(void* user, int x, int y);
    void (*message)(void* user, const char* text);
    void (*churn)(void* user, int x, int y);  // terrain becomes churned sand
};

// One worm turn. `wormsafe(x,y)` = rock/indoor tiles the worm cannot take.
void turn(Worm& w, NoiseField& noise, const map::Zone& z, Pcg32& rng,
          const Tuning& tune, const Callbacks& cb);

// Damage the worm at a body position. Returns damage actually dealt (0 if the
// hit lands on armor). Weak point: the head itself.
int hit(Worm& w, int x, int y, int dmg, const Callbacks& cb);

}  // namespace worm
