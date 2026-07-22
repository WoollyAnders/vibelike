#pragma once
// Zones: 64x64 tile grids. Generation is stateless from (seed, zone id), so
// zones are never stored — only deltas (M2). Also home to FOV and the
// player-centered Dijkstra map, since both live on the grid.
#include <cstdint>

namespace map {

inline constexpr int W = 64, H = 64;
inline constexpr int N = W * H;

enum ZoneId : uint8_t { kZoneSietch = 0, kZoneDesert = 1, kZoneDeepPan = 2 };
inline constexpr int kZoneCount = 3;

// tile flags
inline constexpr uint8_t TF_BLOCK = 1 << 0;   // impassable
inline constexpr uint8_t TF_INDOOR = 1 << 1;  // no sun, no sky
inline constexpr uint8_t TF_SHADE = 1 << 2;   // static shade (near rock/walls)
inline constexpr uint8_t TF_DOOR = 1 << 3;    // zone exit pad
inline constexpr uint8_t TF_WALL = 1 << 4;    // draw wall block (implies BLOCK)
inline constexpr uint8_t TF_ROCKPROP = 1 << 5;  // draw rock prop (implies BLOCK)
inline constexpr uint8_t TF_CISTERN = 1 << 6;   // sietch cistern (bump = refill)
inline constexpr uint8_t TF_OPAQUE = 1 << 7;    // blocks sight

// vis bits
inline constexpr uint8_t V_VISIBLE = 1 << 0;
inline constexpr uint8_t V_REMEMBERED = 1 << 1;

struct Zone {
    uint8_t floor[N];  // tile sheet frame
    uint8_t flags[N];
    uint8_t vis[N];

    bool walkable(int x, int y) const {
        return x >= 0 && x < W && y >= 0 && y < H && !(flags[y * W + x] & TF_BLOCK);
    }
    bool opaque(int x, int y) const {
        return x < 0 || x >= W || y < 0 || y >= H || (flags[y * W + x] & TF_OPAQUE);
    }
    uint8_t at(int x, int y) const { return flags[y * W + x]; }
};

// Fills zone terrain. Deterministic from (seed, id). Also outputs the spawn
// point (sietch: by the door; desert: at the door back to the sietch).
void generate(Zone& z, uint64_t seed, ZoneId id, int& spawn_x, int& spawn_y);

// Recursive shadowcasting. Sets V_VISIBLE (clearing previous) and accumulates
// V_REMEMBERED within radius around (cx, cy).
void compute_fov(Zone& z, int cx, int cy, int radius);

// Flood the connected indoor room containing (cx, cy) as V_VISIBLE|V_REMEMBERED,
// including its enclosing walls — so an enclosed room is fully lit regardless of
// shape or FOV radius. No-op when (cx, cy) is not an indoor tile. Bounded.
void reveal_room(Zone& z, int cx, int cy);

// Player-centered Dijkstra map (uniform costs, 8-way). dist = -1 unreachable.
void compute_dijkstra(const Zone& z, int cx, int cy, int16_t dist[N]);

}  // namespace map
