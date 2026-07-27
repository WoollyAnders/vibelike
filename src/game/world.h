#pragma once
// The per-run world graph (B1). A run is a Hades-style climb: the sietch hub,
// then 4 TIERS of 3 field sections (each carrying a harvester), every tier capped
// by a boss node -- 12 regular areas + 4 bosses. The 4th (summit) boss is the run's
// PINNACLE. Its concrete identity is chosen by the run's AWAKEN level (B5 meta):
// the campaign climbs Awaken 1..10 (Hades "Heat"), each level fielding a dedicated
// pinnacle boss; Baron Harkonnen is the Awaken-10 pinnacle (the true ending), with a
// mid-campaign Baron confrontation at Awaken 5 (he withdraws undefeated). The graph
// is rebuilt from the run seed every load, so a save stores only the current node +
// per-node flags (the Awaken level lives in meta, not the graph).
//
// B1 lands this as a deterministic data structure + builder; it does not yet
// drive the resident zone (that wiring is B1.2 / B2). Kept header-only + tiny.
#include <cstdint>

namespace world {

// Pinnacle = the summit boss slot; the Awaken level (B5) selects the concrete
// encounter that fills it (Baron only at Awaken 10).
enum class NodeType : uint8_t { Sietch, Field, Boss, Pinnacle };
// Biome selects the terrain generator's parameters (see map::Biome). Sietch is
// the carved hub; Rimwall is broken rock country; the Deep Pan is open worm erg.
enum class Biome : uint8_t { Sietch, Rimwall, DeepPan };

// per-node flags (persisted in the save; the rest of the node rebuilds from seed)
inline constexpr uint8_t NF_VISITED = 1 << 0;
inline constexpr uint8_t NF_DENIED = 1 << 1;   // this section's harvester was denied
inline constexpr uint8_t NF_CLEARED = 1 << 2;  // boss killed / section resolved

struct Node {
    uint64_t seed = 0;
    NodeType type = NodeType::Field;
    Biome biome = Biome::Rimwall;
    uint8_t tier = 0;          // 0 = sietch, 1.. = field tiers up to the summit
    uint8_t links[3] = {0, 0, 0};  // forward node indices (Hades route choice)
    uint8_t nlinks = 0;
    uint8_t flags = 0;
};

inline constexpr int kMaxNodes = 32;
inline constexpr int kTiers = 4;             // 4 tiers, each capped by a boss (4 bosses)
inline constexpr int kSectionsPerTier = 3;   // 3 harvester sections/tier -> 12 regular areas

// SplitMix64 — a stateless per-node seed from (run_seed, index). No RNG stream is
// drawn, so the whole graph is reconstructable from the run seed alone.
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
    Biome biome_of(int i) const { return nodes[i].biome; }
    bool is_sietch(int i) const { return nodes[i].type == NodeType::Sietch; }

    // Deterministic layout: sietch(0) -> tier 1 (kSectionsPerTier fields) -> boss
    // -> tier 2 -> boss -> ... -> tier 4 -> Pinnacle summit. B1 builds a linear
    // spine; the 2-3 previewed forward doors per section come in B2 (route choice).
    void build(uint64_t run_seed) {
        count = 0;
        auto add = [&](NodeType t, Biome b, uint8_t tier) -> int {
            int idx = count++;
            Node& n = nodes[idx];
            n = Node{};
            n.type = t;
            n.biome = b;
            n.tier = tier;
            n.seed = node_hash(run_seed, uint64_t(idx));
            return idx;
        };
        auto link = [&](int from, int to) {
            if (nodes[from].nlinks < 3) nodes[from].links[nodes[from].nlinks++] = uint8_t(to);
        };

        add(NodeType::Sietch, Biome::Sietch, 0);  // node 0
        int prev = 0;
        for (int tier = 1; tier <= kTiers && count + kSectionsPerTier + 1 <= kMaxNodes; ++tier) {
            const Biome biome = (tier % 2) ? Biome::Rimwall : Biome::DeepPan;
            for (int s = 0; s < kSectionsPerTier; ++s) {
                int idx = add(NodeType::Field, biome, uint8_t(tier));
                link(prev, idx);
                prev = idx;
            }
            const NodeType bt = (tier == kTiers) ? NodeType::Pinnacle : NodeType::Boss;
            int boss = add(bt, biome, uint8_t(tier));
            link(prev, boss);
            prev = boss;
        }
    }
};

}  // namespace world
