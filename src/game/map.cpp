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
            // Warm sandstone cavern floor: frames 8 and 10/11 are three different
            // crops of the sietch texture (no grey desert rock, no spice here), so
            // the floor has variety without cold tones.
            (void)rim;
            uint8_t frame = 8;
            if (h < 40)      frame = 10;
            else if (h < 80) frame = 11;
            z.floor[y * W + x] = frame;
        }
}

void generate_sietch(Zone& z, int& spawn_x, int& spawn_y) {
    // A sprawling carved warren: chambers connected by corridors, filling most of
    // the 64x64 map. Everything starts solid rock; carve_room hollows each space.
    for (int i = 0; i < N; ++i) {
        z.floor[i] = 8;
        z.flags[i] = TF_BLOCK | TF_WALL | TF_OPAQUE | TF_INDOOR;
    }
    carve_room(z, 22, 22, 42, 38);   // great hall (central)
    carve_room(z, 27, 10, 37, 20);   // cistern chamber (north)
    carve_room(z, 31, 19, 33, 23);   //   corridor -> hall
    carve_room(z, 6, 24, 20, 38);    // living quarters (west)
    carve_room(z, 19, 29, 23, 32);   //   corridor -> hall
    carve_room(z, 44, 22, 57, 36);   // storeroom (east)
    carve_room(z, 41, 28, 45, 31);   //   corridor -> hall
    carve_room(z, 14, 41, 27, 53);   // armory (south-west)
    carve_room(z, 22, 37, 25, 42);   //   corridor -> hall
    carve_room(z, 40, 40, 52, 52);   // sleeping chamber (south-east)
    carve_room(z, 43, 37, 46, 41);   //   corridor -> hall
    carve_room(z, 31, 37, 34, 56);   // entry corridor -> door (south)

    z.flags[13 * W + 32] |= TF_CISTERN | TF_BLOCK;  // cistern basin
    // B2: no walk-out door in the hub -- you enter the vision at Paul's Water of Life.

    spawn_x = 32;
    spawn_y = 36;  // great hall, by the entry corridor
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

    // B2: every field node is a leg of the march to Arrakeen -- enter at the west, cross to a
    // FORWARD door on the east that advances to the next node. Both spots are cleared walkable.
    {
        const int sx0 = 3, sy0 = H / 2;          // entry (west)
        for (int y = sy0 - 2; y <= sy0 + 2; ++y)
            for (int x = sx0 - 2; x <= sx0 + 2; ++x) {
                z.flags[y * W + x] &= uint8_t(~(TF_BLOCK | TF_ROCKPROP | TF_OPAQUE));
                if (z.floor[y * W + x] > 6) z.floor[y * W + x] = 0;
            }
        spawn_x = sx0;
        spawn_y = sy0;

        const int fx = W - 4, fy = H / 2;        // forward door (east)
        for (int y = fy - 2; y <= fy + 2; ++y)
            for (int x = fx - 2; x <= fx + 2; ++x) {
                z.flags[y * W + x] &= uint8_t(~(TF_BLOCK | TF_ROCKPROP | TF_OPAQUE));
                if (z.floor[y * W + x] > 6) z.floor[y * W + x] = 0;
            }
        z.flags[fy * W + fx] |= TF_DOOR;
        z.floor[fy * W + fx] = 9;
    }

    // Keep the east and west edge columns traversable somewhere: clear a lane
    // so zone-edge travel always has crossings.
    for (int x : {0, 1, W - 2, W - 1})
        for (int y = H / 2 - 3; y <= H / 2 + 3; ++y) {
            z.flags[y * W + x] &= uint8_t(~(TF_BLOCK | TF_ROCKPROP | TF_OPAQUE));
            if (z.floor[y * W + x] > 6) z.floor[y * W + x] = uint8_t((x + y) % 3);
        }
}

// B9: a rare hidden cavern — a small enclosed pocket carved off a deep-desert field node. Solid
// sandstone with a central chamber and a few seed-chosen offshoots joined by short corridors; the
// player enters (and leaves) by a door at the south rim. Deterministic from the seed alone (only
// hash_combine, no RNG stream), so save/load and re-entry reproduce the identical layout.
void generate_cave(Zone& z, uint64_t seed, int& spawn_x, int& spawn_y) {
    const uint32_t s = uint32_t(seed ^ (seed >> 32));
    for (int i = 0; i < N; ++i) {
        z.floor[i] = 8;
        z.flags[i] = TF_BLOCK | TF_WALL | TF_OPAQUE | TF_INDOOR;
    }
    // central chamber, roughly centred with a seed-jittered size
    const int cw = 8 + int(hash_combine(s, 1) % 5);   // 8..12
    const int ch = 7 + int(hash_combine(s, 2) % 5);   // 7..11
    const int cx = 32, cy = 30;
    carve_room(z, cx - cw, cy - ch, cx + cw, cy + ch);
    // three offshoot pockets, each linked back to the chamber by a thin corridor
    for (int k = 0; k < 3; ++k) {
        const uint32_t h = hash_combine(s, uint32_t(10 + k));
        const int ang = int(h % 4);                         // N/E/S/W offshoot
        const int dist = 12 + int((h >> 3) % 6);            // 12..17 from centre
        const int rw = 3 + int((h >> 6) % 3), rh = 3 + int((h >> 9) % 3);
        int ox = cx, oy = cy;
        if (ang == 0) oy = cy - dist;
        else if (ang == 1) ox = cx + dist;
        else if (ang == 2) oy = cy + dist;
        else ox = cx - dist;
        if (ox < 6 || ox > W - 7 || oy < 6 || oy > H - 7) continue;
        carve_room(z, ox - rw, oy - rh, ox + rw, oy + rh);
        // corridor from the chamber edge to the pocket (carve a 3-wide lane)
        int lx = cx, ly = cy;
        while (lx != ox || ly != oy) {
            for (int w = -1; w <= 1; ++w) {
                carve_room(z, lx + (ang % 2 ? 0 : w), ly + (ang % 2 ? w : 0),
                           lx + (ang % 2 ? 0 : w), ly + (ang % 2 ? w : 0));
            }
            if (lx != ox) lx += ox > lx ? 1 : -1;
            else if (ly != oy) ly += oy > ly ? 1 : -1;
        }
    }
    // the way out: a door at the south rim of the central chamber
    const int dx0 = cx, dy0 = cy + ch;
    z.flags[dy0 * W + dx0] |= TF_DOOR;
    z.floor[dy0 * W + dx0] = 9;
    spawn_x = cx;
    spawn_y = cy + ch - 1;  // just inside the door
}

}  // namespace

void generate(Zone& z, uint64_t seed, ZoneId id, int& spawn_x, int& spawn_y) {
    std::memset(z.vis, 0, sizeof z.vis);
    if (id == kZoneSietch)
        generate_sietch(z, spawn_x, spawn_y);
    else if (id == kZoneCave)
        generate_cave(z, seed, spawn_x, spawn_y);
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
        // Reveal this interior tile and a thick band of surrounding rock, so the
        // carved chambers sit in a solid drawn sandstone mass with no black voids
        // between them (the sietch is a known, lit home, not fog-of-war).
        constexpr int kBand = 3;
        for (int ny = y - kBand; ny <= y + kBand; ++ny)
            for (int nx = x - kBand; nx <= x + kBand; ++nx)
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
