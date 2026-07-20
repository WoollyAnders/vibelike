#pragma once
// PCG32 — tiny, fast, deterministic. Gameplay uses separate streams so that
// e.g. visual effects can never perturb world generation (the determinism
// firewall): world/combat/loot are serialized, fx is not.
#include <cstdint>

struct Pcg32 {
    uint64_t state = 0x853c49e6748fea9bULL;
    uint64_t inc = 0xda3e39cb94b95bdbULL;

    void seed(uint64_t init_state, uint64_t stream) {
        state = 0;
        inc = (stream << 1u) | 1u;
        next();
        state += init_state;
        next();
    }

    uint32_t next() {
        const uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        const uint32_t xorshifted = uint32_t(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = uint32_t(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    // Uniform in [0, bound). Bound must be > 0.
    uint32_t below(uint32_t bound) {
        const uint32_t threshold = uint32_t(-int32_t(bound)) % bound;
        for (;;) {
            const uint32_t r = next();
            if (r >= threshold) return r % bound;
        }
    }

    // Uniform in [lo, hi] inclusive.
    int range(int lo, int hi) { return lo + int(below(uint32_t(hi - lo + 1))); }

    bool chance(int num, int den) { return int(below(uint32_t(den))) < num; }
};

// Stateless hash — procgen regenerates zones from (seed, zone, x, y) without
// consuming stream state.
inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline uint32_t hash_combine(uint32_t a, uint32_t b) {
    return hash_u32(a * 0x9E3779B9u + b + (a << 6) + (a >> 2));
}

struct RngSet {
    Pcg32 world, combat, loot, fx;

    void init(uint64_t seed) {
        world.seed(seed, 1);
        combat.seed(seed, 2);
        loot.seed(seed, 3);
        fx.seed(seed, 4);  // never serialized, never affects gameplay
    }
};
