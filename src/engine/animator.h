#pragma once
// G1 animation core. data/anims.txt maps (sprite_key, state) to an atlas
// strip with frame timing, loop mode, and named event frames. Entities carry
// a tiny POD Animator that the render/update layer ticks per frame — the sim
// never waits on an animation.
#include <cstdint>

#include "engine/assets.h"

namespace anim {

enum class State : uint8_t { Idle = 0, Walk, Attack, Use, Hurt, Death, Count };

enum Event : uint8_t { EV_NONE = 0, EV_FOOTSTEP, EV_IMPACT };

struct Def {
    const SpriteSheet* sheet = nullptr;
    uint8_t ticks_per_frame = 10;
    bool loop = true;
    uint8_t event_frame = 0xFF;   // frame index that fires...
    uint8_t event = EV_NONE;      // ...this event (one per state for now)
};

// One sprite's full state table.
struct Set {
    char key[24] = {};
    Def states[uint8_t(State::Count)];

    const Def* get(State s) const {
        const Def& d = states[uint8_t(s)];
        return d.sheet ? &d : nullptr;
    }
};

// POD, embeddable in Entity (and serialized with it harmlessly).
struct Animator {
    uint8_t state = 0;   // anim::State
    uint8_t frame = 0;
    uint8_t clock = 0;
    uint8_t facing = 0;  // 0..7, E=0 counter-clockwise; G2 uses fully, G1 mirrors

    void set(State s) {
        if (State(state) == s) return;
        state = uint8_t(s);
        frame = 0;
        clock = 0;
    }

    // Advances one render tick. Returns the event fired this tick (EV_NONE
    // usually). One-shot states fall back to Idle when they finish.
    uint8_t tick(const Set* set);

    // Sheet + frame to draw right now (falls back to Idle's sheet).
    const SpriteSheet* current(const Set* set, int* frame_out) const;
};

// Registry: parses data/anims.txt against loaded assets.
class Registry {
public:
    bool load(const char* data_dir, const Assets& assets);
    const Set* get(const char* sprite_key) const;

private:
    static constexpr int kMax = 32;
    Set sets_[kMax];
    int count_ = 0;
};

}  // namespace anim
