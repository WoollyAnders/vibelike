#include "game/worm.h"

namespace worm {

namespace {

bool worm_passable(const map::Zone& z, int x, int y) {
    if (x < 0 || x >= map::W || y < 0 || y >= map::H) return false;
    const uint8_t f = z.at(x, y);
    if (f & map::TF_INDOOR) return false;
    // Sand and even rock-prop tiles get plowed through when surfaced; only
    // true rock ground (floor 3/4) is safe. Check by floor type.
    const uint8_t floor = z.floor[y * map::W + x];
    return floor <= 2 || floor == 5 || floor == 6;  // sand variants, spice, drum
}

void step_head_toward(Worm& w, const map::Zone& z, int tx, int ty, Pcg32& rng) {
    // Greedy step (allowing slight wobble) constrained to worm-passable sand.
    int best_x = w.x, best_y = w.y;
    int best_d = INT32_MAX;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            const int nx = w.x + dx, ny = w.y + dy;
            if (!worm_passable(z, nx, ny)) continue;
            const int d = (nx - tx) * (nx - tx) + (ny - ty) * (ny - ty);
            const int jitter = int(rng.below(3));
            if (d + jitter < best_d) { best_d = d + jitter; best_x = nx; best_y = ny; }
        }
    // segments follow
    for (int i = kSegments - 1; i > 0; --i) {
        w.seg_x[i] = w.seg_x[i - 1];
        w.seg_y[i] = w.seg_y[i - 1];
    }
    w.seg_x[0] = w.x;
    w.seg_y[0] = w.y;
    w.x = int16_t(best_x);
    w.y = int16_t(best_y);
}

void enter_edge_near(Worm& w, const map::Zone& z, int tx, int ty, Pcg32& rng) {
    // Materialize at the map edge closest to the target, on passable sand.
    int best_x = 0, best_y = 0, best_d = INT32_MAX;
    for (int i = 0; i < map::W; ++i) {
        const int cands[4][2] = {{i, 0}, {i, map::H - 1}, {0, i}, {map::W - 1, i}};
        for (const auto& c : cands) {
            if (!worm_passable(z, c[0], c[1])) continue;
            const int d = (c[0] - tx) * (c[0] - tx) + (c[1] - ty) * (c[1] - ty) +
                          int(rng.below(16));
            if (d < best_d) { best_d = d; best_x = c[0]; best_y = c[1]; }
        }
    }
    w.x = int16_t(best_x);
    w.y = int16_t(best_y);
    for (int i = 0; i < kSegments; ++i) { w.seg_x[i] = w.x; w.seg_y[i] = w.y; }
}

void crush_at(const Callbacks& cb, const map::Zone& z, int x, int y) {
    if (x < 0 || x >= map::W || y < 0 || y >= map::H) return;
    const uint8_t floor = z.floor[y * map::W + x];
    const bool safe = !(floor <= 2 || floor == 5 || floor == 6);
    if (!safe) cb.crush(cb.user, x, y);
}

}  // namespace

void turn(Worm& w, NoiseField& noise, const map::Zone& z, Pcg32& rng,
          const Tuning& tune, const Callbacks& cb) {
    noise.decay();
    if (!tune.active) { w.state = State::Dormant; return; }

    int hx = 0, hy = 0;
    const int loudest = noise.hottest(hx, hy);

    switch (w.state) {
        case State::Dormant:
            if (loudest > tune.stir_threshold) {
                w.state = State::Stirring;
                w.quiet_turns = 0;
                cb.message(cb.user, "The sand hums.");
            }
            break;

        case State::Stirring:
            if (loudest > tune.hunt_threshold) {
                w.state = State::Hunting;
                enter_edge_near(w, z, hx, hy, rng);
                cb.message(cb.user, "Wormsign. It is coming.");
            } else if (loudest < tune.stir_threshold) {
                if (++w.quiet_turns > 10) w.state = State::Dormant;
            } else {
                w.quiet_turns = 0;
            }
            break;

        case State::Hunting: {
            // burrowed: 3 tiles per turn toward the hottest cell, re-targeting
            for (int s = 0; s < 3; ++s) step_head_toward(w, z, hx, hy, rng);
            const int dx = w.x - hx, dy = w.y - hy;
            if (dx * dx + dy * dy <= 4) {
                w.state = State::Breaching;
                w.timer = 3;
                cb.message(cb.user, "The ground swells beneath the sand!");
            } else if (loudest < tune.stir_threshold / 2) {
                w.state = State::Stirring;  // lost the scent
                w.quiet_turns = 0;
            }
            break;
        }

        case State::Breaching:
            if (--w.timer == 0) {
                w.state = State::Surfaced;
                w.timer = 12;  // gorge duration
                // the breach: kill radius 2 around the head on unsafe ground
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        crush_at(cb, z, w.x + dx, w.y + dy);
                        if (dx * dx + dy * dy <= 2) cb.churn(cb.user, w.x + dx, w.y + dy);
                    }
                for (int i = 0; i < kSegments; ++i) { w.seg_x[i] = w.x; w.seg_y[i] = w.y; }
                cb.message(cb.user, "The sandworm breaches! The desert screams.");
            }
            break;

        case State::Surfaced:
            step_head_toward(w, z, hx, hy, rng);
            crush_at(cb, z, w.x, w.y);
            cb.churn(cb.user, w.x, w.y);
            if (--w.timer == 0 || loudest < 4) {
                w.state = State::Dormant;
                noise.clear();
                cb.message(cb.user, "The worm sinks beneath the dunes. Silence.");
            }
            break;
    }
}

int hit(Worm& w, int x, int y, int dmg, const Callbacks& cb) {
    if (w.state != State::Surfaced) return 0;
    if (x == w.x && y == w.y) {
        w.hp -= int16_t(dmg);
        if (w.hp <= 0) {
            w.state = State::Dormant;
            cb.message(cb.user, "The impossible: the maker is slain.");
        } else if (w.hp < 25) {
            w.state = State::Dormant;
            cb.message(cb.user, "Wounded, the worm plunges into the deep sand.");
        }
        return dmg;
    }
    for (int i = 0; i < kSegments; ++i)
        if (x == w.seg_x[i] && y == w.seg_y[i]) {
            cb.message(cb.user, "Your blow glances off ring-armor plate.");
            return 0;
        }
    return 0;
}

}  // namespace worm
