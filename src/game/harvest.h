#pragma once
// A harvest operation: a crawler working a spice blow, escorted, watched by a
// spotter crew, on the clock until the carryall extracts the load. Deny it by
// any of five routes: worm bait, demolition, spotter-kill (no wormsign
// warning), hold contamination, or theft + disablement.
#include <cstdint>

namespace harvest {

enum class OpState : uint8_t {
    None = 0,     // no op in this zone
    Working,      // hold filling, +noise per turn
    Paused,       // spotters saw wormsign; engines cold, waiting it out
    Evac,         // carryall inbound (hold full or danger), eta ticking
    Shipped,      // carryall got the load out (bad for you)
    Destroyed,    // crawler dead (good for you)
};

struct Op {
    OpState state = OpState::None;
    int16_t x = 0, y = 0;       // crawler anchor (occupies x-1..x, y-1..y)
    int16_t hull = 30;
    int16_t hold = 0;           // 0..100 spice load
    int16_t eta = 0;            // carryall countdown when Evac
    uint8_t zone = 0;
    bool poisoned = false;      // hold contaminated (silent sabotage)
    bool stolen = false;        // grains taken
    uint8_t plant_progress = 0; // demolition planting turns (needs 3)
    bool denied_reported = false;

    bool tile_on(int tx, int ty) const {
        return state != OpState::None && state != OpState::Shipped &&
               state != OpState::Destroyed &&
               tx >= x - 1 && tx <= x && ty >= y - 1 && ty <= y;
    }
    bool adjacent(int tx, int ty) const {
        return tx >= x - 2 && tx <= x + 1 && ty >= y - 2 && ty <= y + 1 &&
               !tile_on(tx, ty);
    }
    bool live() const {
        return state == OpState::Working || state == OpState::Paused ||
               state == OpState::Evac;
    }
};

inline constexpr int kCrawlerNoise = 25;   // per working turn
inline constexpr int kHoldPerTurn = 1;     // fill rate
inline constexpr int kEvacTurns = 14;      // carryall arrival time
inline constexpr int kPlantTurns = 3;      // demolition channel

}  // namespace harvest
