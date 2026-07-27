#pragma once
// The per-run world graph (B1). A run is one of Paul Muad'Dib's prescient visions: the
// player (a Fremen, never a seer) enacts a possible future -- a directed march across
// Arrakis toward Arrakeen. The campaign is 5 ACTS (one biome each), each holding 2 AWAKEN
// levels; every Awaken level is a short gauntlet of rooms capped by a named PINNACLE boss.
// 5 acts x 2 = 10 Awaken levels / 10 pinnacles, culminating in Baron Harkonnen at Awaken 10.
//
// The environment is CURATED (fixed per act -- the geography march), never rolled; procedural
// biome variety is an endgame-only feature (the B10 cartography "maps"). The graph is rebuilt
// from the run seed every load, so a save stores only the current node + per-node flags.
//
// Header-only + tiny. B1.3 wires this to drive the single resident zone (enter_node).
#include <cstdint>

namespace world {

enum class NodeType : uint8_t { Sietch, Field, Boss, Pinnacle };

// Stage = the act's biome (the geography march to Arrakeen). Fixed per act, never rolled.
// Maps to map::ZoneId for the terrain generator (see zone_id_of).
enum class Stage : uint8_t { Sietch, DeepDesert, Rimwall, ShieldWall, Outskirts, Arrakeen };

// per-node flags (persisted in the save; the rest of the node rebuilds from seed)
inline constexpr uint8_t NF_VISITED = 1 << 0;
inline constexpr uint8_t NF_DENIED = 1 << 1;   // this room's objective was resolved
inline constexpr uint8_t NF_CLEARED = 1 << 2;  // boss killed / room resolved

struct Node {
    uint64_t seed = 0;
    NodeType type = NodeType::Field;
    Stage stage = Stage::DeepDesert;
    uint8_t act = 0;           // 0 = sietch, 1..kActs = the geography march
    uint8_t awaken = 0;        // 0 = sietch, 1..10 = the Awaken level this node belongs to
    uint8_t links[3] = {0, 0, 0};  // forward node indices (Hades route choice; B2)
    uint8_t nlinks = 0;
    uint8_t flags = 0;
};

inline constexpr int kActs = 5;            // 5 biomes: the geography march to Arrakeen
inline constexpr int kAwakenPerAct = 2;    // 2 Awaken levels (2 pinnacles) per act
inline constexpr int kAwakenLevels = kActs * kAwakenPerAct;  // 10
inline constexpr int kRoomsPerLevel = 3;   // gauntlet rooms before the level's pinnacle
// 1 sietch + 10 levels * (3 rooms + 1 pinnacle) = 41; leave headroom.
inline constexpr int kMaxNodes = 48;

// The act's fixed biome. act is 1-based (1..kActs); act 0 is the sietch hub.
inline Stage stage_for_act(int act) {
    switch (act) {
        case 1: return Stage::DeepDesert;   // open erg (Tanzerouft)
        case 2: return Stage::Rimwall;      // broken rock & sinks
        case 3: return Stage::ShieldWall;   // the great rock barrier
        case 4: return Stage::Outskirts;    // graben-pan & Harkonnen villages
        default: return Stage::Arrakeen;    // act 5: the city & the Residency
    }
}

// Stage -> map::ZoneId (0 = kZoneSietch, 1 = kZoneDesert, 2 = kZoneDeepPan). Kept as a plain
// int so world.h stays decoupled from map.h; mirrors map::ZoneId. ShieldWall/Outskirts/Arrakeen
// use placeholder desert generators until their real biomes land (see ROADMAP B1.3).
inline int zone_id_of(Stage s) {
    switch (s) {
        case Stage::Sietch:     return 0;   // kZoneSietch
        case Stage::DeepDesert: return 2;   // kZoneDeepPan (open erg)
        case Stage::Rimwall:    return 1;   // kZoneDesert (rock country)
        default:                return 1;   // ShieldWall/Outskirts/Arrakeen placeholder
    }
}

// SplitMix64 -- a stateless per-node seed from (run_seed, index). No RNG stream is drawn,
// so the whole graph is reconstructable from the run seed alone.
inline uint64_t node_hash(uint64_t s, uint64_t i) {
    uint64_t x = s + i * 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

struct Graph {
    Node nodes[kMaxNodes];
    int count = 0;

    NodeType type_of(int i) const { return nodes[i].type; }
    Stage stage_of(int i) const { return nodes[i].stage; }
    bool is_sietch(int i) const { return nodes[i].type == NodeType::Sietch; }
    bool is_pinnacle(int i) const { return nodes[i].type == NodeType::Pinnacle; }
    // First forward link (linear spine for B1.3; route choice among links[] comes in B2).
    int forward(int i) const { return nodes[i].nlinks ? int(nodes[i].links[0]) : -1; }

    // Deterministic layout: sietch(0) -> Awaken 1 (kRoomsPerLevel rooms + pinnacle) -> Awaken 2
    // -> ... -> Awaken 10 (Baron). Stage is fixed by the act (geography march). Linear spine;
    // the 2-3 previewed forward doors per room come in B2.
    void build(uint64_t run_seed) {
        count = 0;
        auto add = [&](NodeType t, Stage s, uint8_t act, uint8_t awaken) -> int {
            int idx = count++;
            Node& n = nodes[idx];
            n = Node{};
            n.type = t;
            n.stage = s;
            n.act = act;
            n.awaken = awaken;
            n.seed = node_hash(run_seed, uint64_t(idx));
            return idx;
        };
        auto link = [&](int from, int to) {
            if (nodes[from].nlinks < 3) nodes[from].links[nodes[from].nlinks++] = uint8_t(to);
        };

        add(NodeType::Sietch, Stage::Sietch, 0, 0);  // node 0
        int prev = 0;
        for (int awaken = 1; awaken <= kAwakenLevels; ++awaken) {
            const uint8_t act = uint8_t((awaken + 1) / 2);   // 1,2->1  3,4->2 ... 9,10->5
            const Stage stage = stage_for_act(act);
            if (count + kRoomsPerLevel + 1 > kMaxNodes) break;  // guard
            for (int r = 0; r < kRoomsPerLevel; ++r) {
                int idx = add(NodeType::Field, stage, act, uint8_t(awaken));
                link(prev, idx);
                prev = idx;
            }
            int pin = add(NodeType::Pinnacle, stage, act, uint8_t(awaken));
            link(prev, pin);
            prev = pin;
        }
    }
};

}  // namespace world
