#include "engine/animator.h"

#include <cstdio>
#include <cstring>

namespace anim {

uint8_t Animator::tick(const Set* set) {
    if (!set) return EV_NONE;
    const Def* d = set->get(State(state));
    if (!d) {
        state = uint8_t(State::Idle);
        d = set->get(State::Idle);
        if (!d) return EV_NONE;
    }
    uint8_t fired = EV_NONE;
    if (++clock >= d->ticks_per_frame) {
        clock = 0;
        const int frames = d->frames_per_dir();
        if (frame + 1 >= frames) {
            if (d->loop) {
                frame = 0;
            } else {
                state = uint8_t(State::Idle);
                frame = 0;
            }
        } else {
            ++frame;
        }
        if (frame == d->event_frame) fired = d->event;
    }
    return fired;
}

const SpriteSheet* Animator::current(const Set* set, int* frame_out) const {
    if (!set) return nullptr;
    const Def* d = set->get(State(state));
    if (!d) d = set->get(State::Idle);
    if (!d) return nullptr;
    // resolve direction view + animation frame into the flat strip index
    int f = frame < d->frames_per_dir() ? frame : 0;
    if (d->dirs == 5) {
        int view;
        bool flip;
        facing_to_view5(facing, view, flip);
        f = view * d->frames_per_dir() + f;
    }
    if (frame_out) *frame_out = f;
    return d->sheet;
}

namespace {
State parse_state(const char* s) {
    if (!std::strcmp(s, "idle")) return State::Idle;
    if (!std::strcmp(s, "walk")) return State::Walk;
    if (!std::strcmp(s, "attack")) return State::Attack;
    if (!std::strcmp(s, "use")) return State::Use;
    if (!std::strcmp(s, "hurt")) return State::Hurt;
    if (!std::strcmp(s, "death")) return State::Death;
    return State::Count;
}
uint8_t parse_event(const char* s) {
    if (!std::strcmp(s, "footstep")) return EV_FOOTSTEP;
    if (!std::strcmp(s, "impact")) return EV_IMPACT;
    return EV_NONE;
}
}  // namespace

bool Registry::load(const char* data_dir, const Assets& assets) {
    char path[512];
    std::snprintf(path, sizeof path, "%s/anims.txt", data_dir);
    FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "anims: cannot open %s\n", path);
        return false;
    }
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char key[24], state_s[12], entry[32], loop_s[8], events[64];
        int tpf = 10, dirs = 1;
        if (std::sscanf(line, "%23s %11s %31s %d %7s %d %63s", key, state_s, entry, &tpf,
                        loop_s, &dirs, events) != 7)
            continue;
        const State st = parse_state(state_s);
        if (st == State::Count) continue;
        const SpriteSheet* sheet = assets.get(entry);
        if (!sheet) {
            std::fprintf(stderr, "anims: unknown atlas entry '%s'\n", entry);
            continue;
        }

        Set* set = nullptr;
        for (int i = 0; i < count_; ++i)
            if (!std::strcmp(sets_[i].key, key)) { set = &sets_[i]; break; }
        if (!set) {
            if (count_ >= kMax) continue;
            set = &sets_[count_++];
            std::snprintf(set->key, sizeof set->key, "%s", key);
        }

        Def& d = set->states[uint8_t(st)];
        d.sheet = sheet;
        d.ticks_per_frame = uint8_t(tpf);
        d.loop = std::strcmp(loop_s, "loop") == 0;
        d.dirs = uint8_t(dirs == 5 ? 5 : 1);
        d.event_frame = 0xFF;
        d.event = EV_NONE;
        if (events[0] != '-') {  // first event only, for now
            int ef = 0;
            char ev_name[24];
            if (std::sscanf(events, "%d:%23[^,]", &ef, ev_name) == 2) {
                d.event_frame = uint8_t(ef);
                d.event = parse_event(ev_name);
            }
        }
    }
    std::fclose(f);
    return count_ > 0;
}

const Set* Registry::get(const char* sprite_key) const {
    for (int i = 0; i < count_; ++i)
        if (!std::strcmp(sets_[i].key, sprite_key)) return &sets_[i];
    return nullptr;
}

}  // namespace anim
