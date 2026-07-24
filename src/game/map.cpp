#include "game/map.h"

#include <cstring>
#include <initializer_list>

#include "engine/rng.h"

namespace map {

namespace {

void carve_room(Zone& z, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            uint8_t& f = z.flags[y * W + x];
            f &= uint8_t(~(TF_BLOCK | TF_WALL | TF_OPAQUE));
            f |= TF_INDOOR;
            // Cheap deterministic floor variety (no art, no RNG stream): a stone
            // interior with a worn rock rim and the odd spice-fleck underfoot.
            const bool rim = (x == x0 || x == x1 || y == y0 || y == y1);
            const uint32_t h = hash_combine(x, y) & 255u;
            // Stone cavern floor only — no spice-fleck (frame 5) here; the reddish
            // spice tile belongs in the desert, not the carved sietch interior.
            uint8_t frame = 8;                          // sietch stone
            if (rim)          frame = uint8_t(3 + (h & 1));  // rock ground rim
            else if (h < 48)  frame = uint8_t(3 + (h & 1));  // scattered worn rock
            z.floor[y * W + x] = frame;
        }
}

void generate_sietch(Zone& z, int& spawn_x, int& spawn_y) {
    // Solid rock, carved rooms.
    for (int i = 0; i < N; ++i) {
        z.floor[i] = 8;
        z.flags[i] = TF_BLOCK | TF_WALL | TF_OPAQUE | TF_INDOOR;
    }
    carve_room(z, 28, 24, 38, 34);   // main hall
    carve_room(z, 30, 20, 34, 24);   // cistern alcove
    carve_room(z, 32, 34, 34, 40);   // entry corridor

    z.flags[21 * W + 32] |= TF_CISTERN | TF_BLOCK;  // cistern (bump target)
    z.flags[40 * W + 33] |= TF_DOOR;                 // exit to the desert
    z.floor[40 * W + 33] = 9;

    spawn_x = 33;
    spawn_y = 30;
}

void generate_desert(Zone& z, uint64_t seed, ZoneId id, int& spawn_x, int& spawn_y) {
    const uint32_t s = uint32_t(seed ^ (seed >> 32)) + uint32_t(id) * 7919u;
    // Deep Pan is open erg: far less rock, far more worm country.
    const int rock_threshold = id == kZoneDeepPan ? 225 : 190;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const uint32_t coarse = hash_combine(s, hash_combine(x >> 3, y >> 3)) & 255;
            const uint32_t fine = hash_combine(s + 1, hash_combine(x, y)) & 255;
            const int v = int(coarse * 3 + fine) / 4;
            uint8_t floor;
            uint8_t flags = 0;
            if (v > rock_threshold) {
                floor = uint8_t(3 + (fine & 1));  // rock ground
                if (fine > 215) flags |= TF_ROCKPROP | TF_BLOCK | TF_OPAQUE;
            } else if (v < 40) {
                floor = 5;  // spice-flecked pan
            } else if (fine > 251) {
                floor = 6;  // drum sand
            } else {
                floor = uint8_t(fine % 3);  // open sand
            }
            z.floor[y * W + x] = floor;
            z.flags[y * W + x] = flags;
        }
    }

    // Static shade: any tile bordering a rock prop (M2 makes this sun-tracked).
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (z.flags[y * W + x] & TF_BLOCK) continue;
            for (int dy = -1; dy <= 1 && !(z.flags[y * W + x] & TF_SHADE); ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
                        (z.flags[ny * W + nx] & TF_ROCKPROP)) {
                        z.flags[y * W + x] |= TF_SHADE;
                        break;
                    }
                }
        }

    if (id == kZoneDesert) {
        // Door back to the sietch at the west edge; clear around it.
        const int dx = 4, dy = 32;
        for (int y = dy - 2; y <= dy + 2; ++y)
            for (int x = dx - 2; x <= dx + 2; ++x) {
                z.flags[y * W + x] &= uint8_t(~(TF_BLOCK | TF_ROCKPROP | TF_OPAQUE));
                if (z.floor[y * W + x] > 6) z.floor[y * W + x] = 0;
            }
        z.flags[dy * W + dx] |= TF_DOOR;
        z.floor[dy * W + dx] = 9;
        spawn_x = dx + 1;
        spawn_y = dy;
    } else {
        spawn_x = 2;
        spawn_y = H / 2;
    }

    // Keep the east and west edge columns traversable somewhere: clear a lane
    // so zone-edge travel always has crossings.
    for (int x : {0, 1, W - 2, W - 1})
        for (int y = H / 2 - 3; y <= H / 2 + 3; ++y) {
            z.flags[y * W + x] &= uint8_t(~(TF_BLOCK | TF_ROCKPROP | TF_OPAQUE));
            if (z.floor[y * W + x] > 6) z.floor[y * W + x] = uint8_t((x + y) % 3);
        }
}

}  // namespace

void generate(Zone& z, uint64_t seed, ZoneId id, int& spawn_x, int& spawn_y) {
    std::memset(z.vis, 0, sizeof z.vis);
    if (id == kZoneSietch)
        generate_sietch(z, spawn_x, spawn_y);
    else
        generate_desert(z, seed, id, spawn_x, spawn_y);
}

// ---- FOV: recursive shadowcasting (8 octants) ---------------------------------

namespace {

struct Octant { int xx, xy, yx, yy; };
constexpr Octant kOctants[8] = {
    {1, 0, 0, 1},  {0, 1, 1, 0},  {0, -1, 1, 0},  {-1, 0, 0, 1},
    {-1, 0, 0, -1}, {0, -1, -1, 0}, {0, 1, -1, 0}, {1, 0, 0, -1},
};

void cast_light(Zone& z, int cx, int cy, int radius, int row,
                float start, float end, const Octant& o) {
    if (start < end) return;
    float new_start = 0.0f;
    bool blocked = false;
    for (int dist = row; dist <= radius && !blocked; ++dist) {
        const int dy = -dist;
        for (int dx = -dist; dx <= 0; ++dx) {
            const int X = cx + dx * o.xx + dy * o.xy;
            const int Y = cy + dx * o.yx + dy * o.yy;
            const float l_slope = (dx - 0.5f) / (dy + 0.5f);
            const float r_slope = (dx + 0.5f) / (dy - 0.5f);
            if (start < r_slope) continue;
            if (end > l_slope) break;
            if (X < 0 || X >= W || Y < 0 || Y >= H) continue;

            if (dx * dx + dy * dy <= radius * radius)
                z.vis[Y * W + X] |= V_VISIBLE | V_REMEMBERED;

            if (blocked) {
                if (z.opaque(X, Y)) { new_start = r_slope; continue; }
                blocked = false;
                start = new_start;
            } else if (z.opaque(X, Y) && dist < radius) {
                blocked = true;
                cast_light(z, cx, cy, radius, dist + 1, start, l_slope, o);
                new_start = r_slope;
            }
        }
    }
}

}  // namespace

void compute_fov(Zone& z, int cx, int cy, int radius) {
    for (int i = 0; i < N; ++i) z.vis[i] &= uint8_t(~V_VISIBLE);
    z.vis[cy * W + cx] |= V_VISIBLE | V_REMEMBERED;
    for (const Octant& o : kOctants) cast_light(z, cx, cy, radius, 1, 1.0f, 0.0f, o);
}

void reveal_room(Zone& z, int cx, int cy) {
    if (cx < 0 || cx >= W || cy < 0 || cy >= H) return;
    if (!(z.flags[cy * W + cx] & TF_INDOOR)) return;  // interiors only
    static int16_t queue[N];
    static uint8_t seen[N];
    std::memset(seen, 0, sizeof seen);
    int head = 0, count = 0;
    const int start = cy * W + cx;
    seen[start] = 1;
    queue[count++] = int16_t(start);
    while (head < count) {
        const int idx = queue[head++];
        const int x = idx % W, y = idx / W;
        // Reveal this interior tile and its 8 neighbours, so the enclosing walls
        // draw too (an unremembered wall renders as black).
        for (int ny = y - 1; ny <= y + 1; ++ny)
            for (int nx = x - 1; nx <= x + 1; ++nx)
                if (nx >= 0 && nx < W && ny >= 0 && ny < H)
                    z.vis[ny * W + nx] |= V_VISIBLE | V_REMEMBERED;
        // Flood 4-connected through open (non-opaque) indoor tiles only, so the
        // room stops at its walls and never leaks diagonally into the next region.
        constexpr int dxs[4] = {1, -1, 0, 0}, dys[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
            const int nx = x + dxs[k], ny = y + dys[k];
            if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
            const int nidx = ny * W + nx;
            if (seen[nidx]) continue;
            const uint8_t f = z.flags[nidx];
            if (!(f & TF_INDOOR) || (f & TF_OPAQUE)) continue;
            seen[nidx] = 1;
            if (count < N) queue[count++] = int16_t(nidx);
        }
    }
}

// ---- Dijkstra (uniform BFS, 8-way) --------------------------------------------

void compute_dijkstra(const Zone& z, int cx, int cy, int16_t dist[N]) {
    for (int i = 0; i < N; ++i) dist[i] = -1;
    static int16_t queue[N];
    int head = 0, count = 0;
    dist[cy * W + cx] = 0;
    queue[count++] = int16_t(cy * W + cx);
    while (head < count) {
        const int idx = queue[head++];
        const int x = idx % W, y = idx / W;
        const int16_t d = dist[idx];
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = x + dx, ny = y + dy;
                if (!z.walkable(nx, ny)) continue;
                const int nidx = ny * W + nx;
                if (dist[nidx] >= 0) continue;
                dist[nidx] = int16_t(d + 1);
                if (count < N) queue[count++] = int16_t(nidx);
            }
    }
}

}  // namespace map
