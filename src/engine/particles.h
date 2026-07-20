#pragma once
// Tiny screen-space particle pool for the Particles render pass — dust puffs,
// sand streaks, spark bursts. Purely visual: stepped per frame on the fx RNG,
// never serialized, never touches gameplay.
#include <cstdint>

#include "engine/render.h"
#include "engine/rng.h"

namespace particles {

struct Particle {
    int32_t x, y;    // framebuffer coords, 8.8 fixed point
    int32_t vx, vy;  // per frame, 8.8
    int16_t life;    // frames remaining
    uint8_t color;
};

class Pool {
public:
    void spawn(int fb_x, int fb_y, int vx_fp, int vy_fp, int life, uint8_t color) {
        Particle& p = items_[next_++ & (kMax - 1)];
        p.x = fb_x << 8;
        p.y = fb_y << 8;
        p.vx = vx_fp;
        p.vy = vy_fp;
        p.life = int16_t(life);
        p.color = color;
    }

    // A puff of drifting dust around a point.
    void burst_dust(Pcg32& fx, int fb_x, int fb_y, int count, uint8_t color) {
        for (int i = 0; i < count; ++i)
            spawn(fb_x + int(fx.below(17)) - 8, fb_y + int(fx.below(9)) - 4,
                  int(fx.below(129)) - 64, -32 - int(fx.below(64)),
                  20 + int(fx.below(30)), color);
    }

    void step_and_draw(DrawCtx& ctx) {
        for (Particle& p : items_) {
            if (p.life <= 0) continue;
            --p.life;
            p.x += p.vx;
            p.y += p.vy;
            p.vy += 2;  // faint gravity
            const int px = p.x >> 8, py = p.y >> 8;
            if (px >= 0 && px < fb::W && py >= 0 && py < fb::H)
                ctx.frame->at(px, py) = p.color;
        }
    }

    void clear() {
        for (Particle& p : items_) p.life = 0;
    }

private:
    static constexpr int kMax = 256;  // power of two
    Particle items_[kMax] = {};
    uint32_t next_ = 0;
};

}  // namespace particles
