// M1 vertical slice: sietch + one desert region, energy turns, FOV, bump
// combat vs dust hounds, water/heat/exposure survival, HUD, permadeath.
#include "game/game.h"

#include <cstdio>
#include <cstring>

#include "engine/render.h"
#include "engine/rng.h"
#include "game/map.h"
#include "game/harvest.h"
#include "game/tunables.h"
#include "game/ui.h"
#include "game/worm.h"

namespace {

constexpr uint32_t kChunkGame = 0x454D4147;  // 'GAME'
constexpr int kMaxEnts = 64;
constexpr int kMaxDeltas = 192;

enum class EType : uint8_t { None = 0, Player, Hound, Corpse, Thumper, Trooper, Spotter };
enum class Ai : uint8_t { Idle = 0, Wander, Hunt, Guard };

struct Entity {
    EType type = EType::None;
    Ai ai = Ai::Idle;
    uint8_t zone = 0;
    int16_t x = 0, y = 0;
    int16_t hp = 0;
    int16_t energy = 0;
    int16_t data = 0;  // corpse: water yield
};

struct Player {
    int hp = tune::kPlayerHpMax;
    int water = tune::kStartWater;
    int exposure = 0;
    int turn = 0;
    int thumpers = 3;
    int charges = 1;   // demolition
    int poisons = 1;   // hold toxin
    int spice = 0;     // grains
    bool sandwalk = false;
    bool dead = false;
};

// Persistent per-zone terrain change (worm churn); replayed after regen.
struct Delta {
    uint8_t zone, x, y, floor;
};

struct GameState {
    GameServices services{};
    RngSet rng;
    uint64_t seed = 0;

    map::Zone zones[map::kZoneCount];
    uint8_t cur_zone = map::kZoneSietch;
    uint8_t spawned_mask = 0;  // zones whose wildlife has been placed
    int shade_hour = -1;       // hour the shade map was computed for

    Entity ents[kMaxEnts];  // ents[0] = player
    Player pl;

    worm::Worm wrm;
    worm::NoiseField noise;
    harvest::Op op;
    int alarm_turns = 0;       // local reinforcement pressure after sabotage
    int alarm_spawn_cd = 0;
    uint8_t noise_meter = 0;          // HUD value (near player)
    uint8_t seismo[64] = {};          // history ring for the trace
    uint8_t seismo_head = 0;

    Delta deltas[kMaxDeltas];
    int delta_count = 0;

    int16_t dmap[map::N];
    iso::Camera cam;
    iso::DrawList draw_list;
    ui::Log log;

    int anim_tick = 0;
    int fade = 0;
    uint8_t fade_r = 255, fade_g = 255, fade_b = 255;
};
GameState g;

worm::Tuning worm_tuning() {
    worm::Tuning t;
    t.active = g.cur_zone != map::kZoneSietch;
    if (g.cur_zone == map::kZoneDeepPan) { t.stir_threshold = 25; t.hunt_threshold = 50; }
    return t;
}

Entity& player_ent() { return g.ents[0]; }
map::Zone& zone() { return g.zones[g.cur_zone]; }

int ambient_now() { return tune::kAmbientByHour[(g.pl.turn % tune::kTurnsPerDay) * 24 / tune::kTurnsPerDay]; }
int hour_now() { return (g.pl.turn % tune::kTurnsPerDay) * 24 / tune::kTurnsPerDay; }
bool is_night() { return ambient_now() <= 2; }

int fov_radius() {
    if (g.pl.water <= 0) return tune::kFovDehydrated;
    return is_night() ? tune::kFovNight : tune::kFovDay;
}

void flash(uint8_t r, uint8_t gg, uint8_t b) {
    g.fade_r = r; g.fade_g = gg; g.fade_b = b;
    g.fade = 120;
}

Entity* entity_at(int x, int y, uint8_t zone_id, EType want = EType::None) {
    for (Entity& e : g.ents) {
        if (e.type == EType::None || e.zone != zone_id) continue;
        if (e.x != x || e.y != y) continue;
        if (want == EType::None || e.type == want) return &e;
    }
    return nullptr;
}

Entity* alloc_entity() {
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::None) return &g.ents[i];
    return nullptr;
}

void spawn_zone_wildlife(uint8_t zone_id) {
    if (g.spawned_mask & (1 << zone_id)) return;
    g.spawned_mask |= uint8_t(1 << zone_id);
    if (zone_id == map::kZoneSietch) return;
    map::Zone& z = g.zones[zone_id];
    const int want = zone_id == map::kZoneDeepPan ? 4 : tune::kHoundCount;
    int placed = 0, guard = 0;
    while (placed < want && ++guard < 2000) {
        const int x = g.rng.world.range(8, map::W - 2);
        const int y = g.rng.world.range(2, map::H - 2);
        if (!z.walkable(x, y) || entity_at(x, y, zone_id)) continue;
        Entity* e = alloc_entity();
        if (!e) break;
        *e = Entity{EType::Hound, Ai::Wander, zone_id,
                    int16_t(x), int16_t(y), tune::kHoundHp, 0, 0};
        ++placed;
    }
}

// Place the Deep Pan's harvest op: crawler on open sand, spotters on the
// nearest rock, an escort ring of troopers. (M4 schedules these; M3 has one.)
void spawn_op(uint8_t zone_id) {
    if (zone_id != map::kZoneDeepPan || g.op.state != harvest::OpState::None) return;
    map::Zone& z = g.zones[zone_id];

    // find an open-sand anchor away from the west entry
    int ax = 0, ay = 0, guard = 0;
    while (++guard < 4000) {
        const int x = g.rng.world.range(24, map::W - 8);
        const int y = g.rng.world.range(8, map::H - 8);
        bool clear = true;
        for (int dy = -2; dy <= 1 && clear; ++dy)
            for (int dx = -2; dx <= 1; ++dx)
                if (!z.walkable(x + dx, y + dy)) { clear = false; break; }
        if (!clear) continue;
        ax = x; ay = y;
        break;
    }
    if (!ax) return;

    g.op = harvest::Op{};
    g.op.state = harvest::OpState::Working;
    g.op.x = int16_t(ax);
    g.op.y = int16_t(ay);
    g.op.zone = zone_id;

    // crawler footprint blocks movement (but the worm eats through it)
    for (int dy = -1; dy <= 0; ++dy)
        for (int dx = -1; dx <= 0; ++dx)
            z.flags[(ay + dy) * map::W + ax + dx] |= map::TF_BLOCK;

    // spotters: two, on the nearest rock ground within 12 tiles (else sand)
    int placed = 0;
    for (int r = 2; r <= 14 && placed < 2; ++r) {
        for (int dy = -r; dy <= r && placed < 2; ++dy)
            for (int dx = -r; dx <= r && placed < 2; ++dx) {
                if (dx * dx + dy * dy > r * r || dx * dx + dy * dy < (r - 1) * (r - 1)) continue;
                const int x = ax + dx, y = ay + dy;
                if (x < 1 || x >= map::W - 1 || y < 1 || y >= map::H - 1) continue;
                if (!z.walkable(x, y) || entity_at(x, y, zone_id)) continue;
                const uint8_t fl = z.floor[y * map::W + x];
                const bool rocky = fl == 3 || fl == 4;
                if (r <= 12 && !rocky) continue;  // prefer rock perches
                if (Entity* e = alloc_entity()) {
                    *e = Entity{EType::Spotter, Ai::Idle, zone_id,
                                int16_t(x), int16_t(y), 4, 0, 0};
                    ++placed;
                }
            }
    }

    // escort: three troopers around the crawler
    for (int i = 0, tries = 0; i < 3 && tries < 200; ++tries) {
        const int x = ax + g.rng.world.range(-4, 3);
        const int y = ay + g.rng.world.range(-4, 3);
        if (!z.walkable(x, y) || entity_at(x, y, zone_id) || g.op.tile_on(x, y)) continue;
        if (Entity* e = alloc_entity()) {
            *e = Entity{EType::Trooper, Ai::Guard, zone_id, int16_t(x), int16_t(y), 8, 0, 0};
            ++i;
        }
    }
}

void apply_deltas(uint8_t zone_id) {
    for (int i = 0; i < g.delta_count; ++i)
        if (g.deltas[i].zone == zone_id)
            g.zones[zone_id].floor[g.deltas[i].y * map::W + g.deltas[i].x] = g.deltas[i].floor;
}

// Sun-tracked shade: shadows fall away from the sun. Morning sun in the east
// casts westward shade; afternoon the reverse; midday almost none.
void recompute_shade(uint8_t zone_id, int hour) {
    map::Zone& z = g.zones[zone_id];
    for (int i = 0; i < map::N; ++i) z.flags[i] &= uint8_t(~map::TF_SHADE);
    if (zone_id == map::kZoneSietch) return;

    int dir, len;
    if (hour < 6 || hour > 18) { dir = 0; len = 1; }        // night: lee of rock
    else if (hour < 10) { dir = -1; len = 2; }              // long west shadows
    else if (hour <= 14) { dir = -1; len = 0; }             // overhead: none
    else { dir = 1; len = 2; }                              // long east shadows

    for (int y = 0; y < map::H; ++y)
        for (int x = 0; x < map::W; ++x) {
            if (!(z.flags[y * map::W + x] & (map::TF_ROCKPROP | map::TF_WALL))) continue;
            if (len == 0 && dir != 0) continue;
            if (dir == 0) {
                // night "shade" = adjacent to rock (wind lee, warmth)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && nx < map::W && ny >= 0 && ny < map::H &&
                            !(z.flags[ny * map::W + nx] & map::TF_BLOCK))
                            z.flags[ny * map::W + nx] |= map::TF_SHADE;
                    }
            } else {
                for (int d = 1; d <= len; ++d) {
                    const int nx = x + dir * d;
                    if (nx < 0 || nx >= map::W) break;
                    if (z.flags[y * map::W + nx] & map::TF_BLOCK) break;
                    z.flags[y * map::W + nx] |= map::TF_SHADE;
                }
            }
        }
    g.shade_hour = hour;
}

void refresh_fov_and_paths() {
    map::compute_fov(zone(), player_ent().x, player_ent().y, fov_radius());
    map::compute_dijkstra(zone(), player_ent().x, player_ent().y, g.dmap);
}

void die(const char* cause) {
    g.pl.dead = true;
    g.log.push("%s", cause);
    g.services.mixer->push({audio::Cmd::PlaySfx, 0 /* roar */, 200, 96});
}

// ---- worm callbacks ----------------------------------------------------------

void op_denied(const char* how) {
    if (g.op.denied_reported) return;
    g.op.denied_reported = true;
    g.log.push("%s", how);
    g.log.push("The harvest is DENIED. The Baron will rage.");
    g.alarm_turns = 50;
    g.alarm_spawn_cd = 6;
}

void worm_cb_crush(void*, int x, int y) {
    for (Entity& e : g.ents) {
        if (e.type == EType::None || e.zone != g.cur_zone) continue;
        if (e.x != x || e.y != y) continue;
        if (e.type == EType::Player) {
            die("Swallowed whole. The desert keeps its own.");
        } else {
            if (e.type == EType::Hound) g.log.push("A dust hound vanishes into the maw.");
            if (e.type == EType::Trooper) g.log.push("A trooper is snatched screaming into the sand.");
            e.type = EType::None;
        }
    }
    if (g.op.live() && g.op.zone == g.cur_zone && g.op.tile_on(x, y)) {
        g.op.state = harvest::OpState::Destroyed;
        op_denied("The worm takes the crawler whole.");
    }
}

void worm_cb_message(void*, const char* text) { g.log.push("%s", text); }

void worm_cb_churn(void*, int x, int y) {
    if (x < 0 || x >= map::W || y < 0 || y >= map::H) return;
    map::Zone& z = g.zones[g.cur_zone];
    if (z.flags[y * map::W + x] & map::TF_BLOCK) return;
    if (z.floor[y * map::W + x] == 6) return;
    z.floor[y * map::W + x] = 6;  // churned sand (loud, disturbed)
    if (g.delta_count < kMaxDeltas)
        g.deltas[g.delta_count++] = Delta{g.cur_zone, uint8_t(x), uint8_t(y), 6};
}

worm::Callbacks worm_callbacks() {
    return worm::Callbacks{nullptr, worm_cb_crush, worm_cb_message, worm_cb_churn};
}

// Noise made by the player stepping on a floor type.
int step_noise(uint8_t floor) {
    int n = 0;
    if (floor <= 2 || floor == 5) n = 8;   // sand, spice pan
    else if (floor == 6) n = 30;           // drum / churned sand booms
    if (g.pl.sandwalk) n /= 2;
    return n;
}

void damage_player(int dmg, const char* cause) {
    g.pl.hp -= dmg;
    flash(220, 30, 20);
    if (g.pl.hp <= 0) die(cause);
}

// ---- world turn --------------------------------------------------------------

// Troopers guard the crawler; they hunt (and shout) if they spot you close.
void trooper_act(Entity& e) {
    const map::Zone& z = zone();
    const Entity& p = player_ent();
    const int pdist = g.dmap[e.y * map::W + e.x];
    const bool sees = (z.vis[e.y * map::W + e.x] & map::V_VISIBLE) && pdist >= 0 &&
                      pdist <= (g.alarm_turns > 0 ? 12 : 6);

    if (sees && e.ai != Ai::Hunt) {
        e.ai = Ai::Hunt;
        g.log.push("A Harkonnen trooper shouts an alarm!");
        g.noise.add(e.x, e.y, 15);
    }
    if (e.ai == Ai::Hunt) {
        if (pdist < 0 || pdist > 14) { e.ai = Ai::Guard; return; }
        if (pdist == 1) {
            const int dmg = g.rng.combat.range(2, 3);
            g.log.push("The trooper's blade finds you for %d.", dmg);
            damage_player(dmg, "Cut down by Harkonnen steel.");
            return;
        }
        int best_x = e.x, best_y = e.y;
        int16_t best = INT16_MAX;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = e.x + dx, ny = e.y + dy;
                if (!z.walkable(nx, ny) || g.op.tile_on(nx, ny)) continue;
                if (entity_at(nx, ny, e.zone)) continue;
                if (nx == p.x && ny == p.y) continue;
                const int16_t d = g.dmap[ny * map::W + nx];
                if (d >= 0 && d < best) { best = d; best_x = nx; best_y = ny; }
            }
        e.x = int16_t(best_x);
        e.y = int16_t(best_y);
        g.noise.add(e.x, e.y, 4);
    } else if (g.op.live() && g.rng.combat.chance(1, 3)) {
        // drift back toward the crawler
        const int gx = g.op.x, gy = g.op.y;
        const int dx = gx > e.x ? 1 : gx < e.x ? -1 : 0;
        const int dy = gy > e.y ? 1 : gy < e.y ? -1 : 0;
        const int dist2 = (gx - e.x) * (gx - e.x) + (gy - e.y) * (gy - e.y);
        if (dist2 > 25) {
            const int nx = e.x + dx, ny = e.y + dy;
            if (z.walkable(nx, ny) && !entity_at(nx, ny, e.zone) &&
                !g.op.tile_on(nx, ny) && !(nx == p.x && ny == p.y)) {
                e.x = int16_t(nx);
                e.y = int16_t(ny);
            }
        }
    }
}

bool spotters_alive() {
    for (const Entity& e : g.ents)
        if (e.type == EType::Spotter && e.zone == g.cur_zone) return true;
    return false;
}

// One turn of the harvest operation.
void op_turn() {
    if (!g.op.live() || g.op.zone != g.cur_zone) return;
    harvest::Op& op = g.op;
    const bool stirring = int(g.wrm.state) >= int(worm::State::Stirring);
    const bool hunting = int(g.wrm.state) >= int(worm::State::Hunting);

    switch (op.state) {
        case harvest::OpState::Working:
            op.hold = int16_t(op.hold + harvest::kHoldPerTurn);
            g.noise.add(op.x, op.y, harvest::kCrawlerNoise);
            if (stirring && spotters_alive()) {
                // hard shutdown: spotters kill the engines and the accumulated
                // racket dies with them — this is what makes them worth knifing
                op.state = harvest::OpState::Paused;
                g.noise.dampen(op.x, op.y, 10, 3);
                g.log.push("Spotters wave wormsign flags. The crawler falls silent.");
            } else if (op.hold >= 100) {
                op.state = harvest::OpState::Evac;
                op.eta = harvest::kEvacTurns;
                g.log.push("Hold full. The carryall is inbound.");
            }
            break;
        case harvest::OpState::Paused:
            g.noise.dampen(op.x, op.y, 4, 1);
            if (hunting) {
                op.state = harvest::OpState::Evac;
                op.eta = harvest::kEvacTurns;
                g.log.push("Too close! The carryall is called in early.");
            } else if (!stirring) {
                op.state = harvest::OpState::Working;
                g.log.push("The crawler grinds back to life.");
            }
            break;
        case harvest::OpState::Evac:
            g.noise.add(op.x, op.y, 8);  // idling engines
            if (--op.eta <= 0) {
                op.state = harvest::OpState::Shipped;
                if (op.poisoned) {
                    op_denied("The load lifts away - laced with rot they will "
                              "not find until the refinery.");
                } else {
                    g.log.push("The carryall lifts the load into the sky. Lost.");
                }
            }
            break;
        default: break;
    }
}

// Reinforcement pressure after a denied harvest.
void alarm_turn() {
    if (g.alarm_turns <= 0) return;
    --g.alarm_turns;
    if (g.cur_zone == map::kZoneSietch) return;
    if (--g.alarm_spawn_cd <= 0) {
        g.alarm_spawn_cd = 12;
        int troopers = 0;
        for (const Entity& e : g.ents)
            if (e.type == EType::Trooper && e.zone == g.cur_zone) ++troopers;
        if (troopers < 6) {
            map::Zone& z = zone();
            for (int placed = 0, tries = 0; placed < 2 && tries < 100; ++tries) {
                const int edge = int(g.rng.combat.below(4));
                const int x = edge < 2 ? (edge == 0 ? 1 : map::W - 2)
                                       : g.rng.combat.range(1, map::W - 2);
                const int y = edge < 2 ? g.rng.combat.range(1, map::H - 2)
                                       : (edge == 2 ? 1 : map::H - 2);
                if (!z.walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
                if (Entity* e = alloc_entity()) {
                    *e = Entity{EType::Trooper, Ai::Hunt, g.cur_zone,
                                int16_t(x), int16_t(y), 8, 0, 0};
                    ++placed;
                }
            }
            g.log.push("An ornithopter thunders over - troopers rope down!");
            g.services.mixer->push({audio::Cmd::PlaySfx, 0, 160, 150});
        }
    }
}

void hound_act(Entity& e) {
    const map::Zone& z = zone();
    const Entity& p = player_ent();
    const int pdist = g.dmap[e.y * map::W + e.x];
    const bool sees = (z.vis[e.y * map::W + e.x] & map::V_VISIBLE) &&
                      pdist >= 0 && pdist <= tune::kHoundSense;

    if (sees) e.ai = Ai::Hunt;
    else if (e.ai == Ai::Hunt && (pdist < 0 || pdist > tune::kHoundSense + 4))
        e.ai = Ai::Wander;

    if (e.ai == Ai::Hunt) {
        if (pdist == 1) {  // adjacent: bite
            const int dmg = g.rng.combat.range(tune::kHoundAtkMin, tune::kHoundAtkMax);
            g.log.push("The dust hound bites you for %d.", dmg);
            damage_player(dmg, "Torn apart by dust hounds.");
            return;
        }
        // roll downhill on the dijkstra map
        int best_x = e.x, best_y = e.y;
        int16_t best = pdist >= 0 ? pdist : INT16_MAX;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = e.x + dx, ny = e.y + dy;
                if (!z.walkable(nx, ny)) continue;
                if (entity_at(nx, ny, e.zone, EType::Hound)) continue;
                if (nx == p.x && ny == p.y) continue;
                const int16_t d = g.dmap[ny * map::W + nx];
                if (d >= 0 && d < best) { best = d; best_x = nx; best_y = ny; }
            }
        e.x = int16_t(best_x);
        e.y = int16_t(best_y);
    } else if (e.ai == Ai::Wander && g.rng.combat.chance(1, 4)) {
        const int dx = g.rng.combat.range(-1, 1), dy = g.rng.combat.range(-1, 1);
        const int nx = e.x + dx, ny = e.y + dy;
        if (z.walkable(nx, ny) && !entity_at(nx, ny, e.zone) &&
            !(nx == p.x && ny == p.y)) {
            e.x = int16_t(nx);
            e.y = int16_t(ny);
        }
    }
}

// Per-turn survival upkeep: thirst, heat, exposure.
void upkeep() {
    ++g.pl.turn;
    const map::Zone& z = zone();
    const uint8_t tf = z.at(player_ent().x, player_ent().y);
    const uint8_t floor = z.floor[player_ent().y * map::W + player_ent().x];

    // thirst
    int base = tune::kThirstRock;
    if (tf & map::TF_INDOOR) base = tune::kThirstIndoor;
    else if (floor <= 2 || floor == 5 || floor == 6) base = tune::kThirstSand;
    const int amb = ambient_now();
    int drain = base * tune::kThirstHeatMul4[amb] * tune::kSuitTier1Mul4 / 16;
    if (base > 0 && drain < 1) drain = 1;
    g.pl.water -= drain;
    if (g.pl.water < 0) g.pl.water = 0;
    if (g.pl.water == 0) {
        damage_player(tune::kDehydrateDamage, "Died of thirst in the Tanzerouft.");
        if ((g.pl.turn % 5) == 0) g.log.push("Your throat is cracked glass. Water. Water.");
    } else if (g.pl.water == 100) {
        g.log.push("Your water is nearly gone.");
    }

    // heat exposure
    const bool sheltered = (tf & (map::TF_INDOOR | map::TF_SHADE)) != 0;
    if (amb >= tune::kExposureHotThreshold && !sheltered) {
        g.pl.exposure += tune::kExposureGain;
        if (g.pl.exposure == tune::kExposureGain)
            g.log.push("The sun hammers down. Find shade.");
    } else {
        g.pl.exposure -= tune::kExposureRecover;
    }
    if (g.pl.exposure < 0) g.pl.exposure = 0;
    if (g.pl.exposure > 100) g.pl.exposure = 100;
    if (g.pl.exposure >= tune::kExposureCollapse) {
        g.log.push("Heatstroke! Your vision swims.");
        damage_player(tune::kExposureDamage, "Cooked by the open sun.");
    }
}

// The player spent a turn: world advances.
void advance_world() {
    upkeep();
    if (g.pl.dead) return;
    refresh_fov_and_paths();
    for (int i = 1; i < kMaxEnts; ++i) {
        Entity& e = g.ents[i];
        if (e.zone != g.cur_zone) continue;
        if (e.type == EType::Thumper) {
            g.noise.add(e.x, e.y, 40);
            if (--e.data <= 0) {
                e.type = EType::None;
                g.log.push("The thumper falls silent.");
            }
            continue;
        }
        if (e.type == EType::Trooper) {
            e.energy += 100;
            while (e.energy >= 100 && !g.pl.dead) {
                e.energy -= 100;
                trooper_act(e);
            }
            continue;
        }
        if (e.type != EType::Hound) continue;
        e.energy += 100;
        while (e.energy >= 100 && !g.pl.dead) {
            e.energy -= 100;
            hound_act(e);
        }
    }

    op_turn();
    alarm_turn();

    // the worm's turn (also decays the noise field)
    const worm::State before = g.wrm.state;
    worm::turn(g.wrm, g.noise, zone(), g.rng.combat, worm_tuning(), worm_callbacks());
    if (g.wrm.state != before) {
        int intensity = 0;
        switch (g.wrm.state) {
            case worm::State::Dormant: intensity = 0; break;
            case worm::State::Stirring: intensity = 60; break;
            case worm::State::Hunting: intensity = 140; break;
            case worm::State::Breaching: intensity = 230; break;
            case worm::State::Surfaced: intensity = 190; break;
        }
        g.services.mixer->push({audio::Cmd::Rumble, 0, uint8_t(intensity), 0});
        if (g.wrm.state == worm::State::Surfaced)
            g.services.mixer->push({audio::Cmd::PlaySfx, 0, 255, 110});
    }

    // hour changed? re-cast the shadows
    if (hour_now() != g.shade_hour) recompute_shade(g.cur_zone, hour_now());

    // HUD meter + seismograph history
    const int nn = g.noise.near(player_ent().x, player_ent().y, 3);
    g.noise_meter = uint8_t(nn > 100 ? 100 : nn);
    g.seismo[g.seismo_head] = g.noise_meter;
    g.seismo_head = (g.seismo_head + 1) & 63;

    refresh_fov_and_paths();
}

bool worm_occupies(int x, int y) {
    if (g.wrm.state != worm::State::Surfaced) return false;
    if (g.wrm.x == x && g.wrm.y == y) return true;
    for (int i = 0; i < worm::kSegments; ++i)
        if (g.wrm.seg_x[i] == x && g.wrm.seg_y[i] == y) return true;
    return false;
}

// ---- player actions ------------------------------------------------------------

// use_spawn: reposition the player at the zone's default spawn point.
void enter_zone(uint8_t id, bool use_spawn, int at_x = 0, int at_y = 0) {
    g.cur_zone = id;
    int sx, sy;
    map::generate(zone(), g.seed, map::ZoneId(id), sx, sy);
    apply_deltas(id);
    spawn_zone_wildlife(id);
    spawn_op(id);
    // crawler footprint re-blocks after regen
    if (g.op.live() && g.op.zone == id)
        for (int dy = -1; dy <= 0; ++dy)
            for (int dx = -1; dx <= 0; ++dx)
                zone().flags[(g.op.y + dy) * map::W + g.op.x + dx] |= map::TF_BLOCK;

    // crossing a region resets the worm; it hunts by local noise
    g.wrm = worm::Worm{};
    g.noise.clear();
    g.services.mixer->push({audio::Cmd::Rumble, 0, 0, 0});

    Entity& p = player_ent();
    if (use_spawn) { p.x = int16_t(sx); p.y = int16_t(sy); }
    else { p.x = int16_t(at_x); p.y = int16_t(at_y); }
    // nudge to walkable if the landing tile is blocked
    for (int r = 0; r < 6 && !zone().walkable(p.x, p.y); ++r) {
        if (zone().walkable(p.x, p.y + r)) p.y = int16_t(p.y + r);
        else if (zone().walkable(p.x, p.y - r)) p.y = int16_t(p.y - r);
    }
    p.zone = id;
    recompute_shade(id, hour_now());
    refresh_fov_and_paths();
    flash(10, 10, 10);
}

void try_move(int dx, int dy) {
    Entity& p = player_ent();
    const int nx = p.x + dx, ny = p.y + dy;
    map::Zone& z = zone();

    // walking off the east/west edge travels between desert regions
    if (nx >= map::W && g.cur_zone == map::kZoneDesert) {
        g.log.push("You cross into the Deep Pan. Open erg. Worm country.");
        enter_zone(map::kZoneDeepPan, false, 1, p.y);
        advance_world();
        return;
    }
    if (nx < 0 && g.cur_zone == map::kZoneDeepPan) {
        g.log.push("Back across the Rimwall fringe.");
        enter_zone(map::kZoneDesert, false, map::W - 2, p.y);
        advance_world();
        return;
    }
    if (nx < 0 || nx >= map::W || ny < 0 || ny >= map::H) return;
    const uint8_t tf = z.at(nx, ny);

    // the surfaced worm is a wall of flesh — bump = strike
    if (worm_occupies(nx, ny)) {
        const int dealt = worm::hit(g.wrm, nx, ny, tune::kPlayerAtk, worm_callbacks());
        if (dealt > 0) g.log.push("Your blade bites the soft flesh of the maw!");
        g.noise.add(p.x, p.y, 12);
        advance_world();
        return;
    }

    if (tf & map::TF_CISTERN) {
        if (g.pl.water < tune::kWaterMax) {
            g.pl.water = tune::kWaterMax;
            g.log.push("You drink deep from the cistern. The tribe provides.");
            g.services.mixer->push({audio::Cmd::PlaySfx, 1, 150, 100});
        } else {
            g.log.push("Your water is full.");
        }
        advance_world();
        return;
    }

    Entity* victim = entity_at(nx, ny, g.cur_zone, EType::Hound);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::Trooper);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::Spotter);
    if (victim) {
        victim->hp -= tune::kPlayerAtk;
        g.services.mixer->push({audio::Cmd::PlaySfx, 1, 200, 60});
        g.noise.add(nx, ny, victim->type == EType::Spotter ? 4 : 12);  // blades are quiet
        if (victim->hp <= 0) {
            switch (victim->type) {
                case EType::Hound:
                    g.log.push("The dust hound collapses. Its water is yours.");
                    break;
                case EType::Trooper:
                    g.log.push("The trooper falls. His water belongs to the tribe now.");
                    break;
                case EType::Spotter:
                    g.log.push("The spotter crumples. No eyes on the sand now.");
                    break;
                default: break;
            }
            const int16_t yield_ = victim->type == EType::Hound
                                       ? int16_t(tune::kCorpseWaterHound) : int16_t(140);
            *victim = Entity{EType::Corpse, Ai::Idle, uint8_t(g.cur_zone),
                             victim->x, victim->y, 0, 0, yield_};
        } else {
            g.log.push("You strike %s.",
                       victim->type == EType::Hound ? "the dust hound" :
                       victim->type == EType::Trooper ? "the trooper" : "the spotter");
        }
        advance_world();
        return;
    }

    if (tf & map::TF_BLOCK) return;  // bump silently

    p.x = int16_t(nx);
    p.y = int16_t(ny);

    if (tf & map::TF_DOOR) {
        if (g.cur_zone == map::kZoneSietch) {
            g.log.push("You step out into the Tanzerouft.");
            enter_zone(map::kZoneDesert, true);
        } else {
            g.log.push("You slip back into the sietch's cool dark.");
            enter_zone(map::kZoneSietch, true);
        }
        advance_world();
        return;
    }

    const uint8_t floor = z.floor[ny * map::W + nx];
    const int noise = step_noise(floor);
    if (noise > 0) g.noise.add(nx, ny, noise);
    if (floor == 6 && noise >= 20) g.log.push("BOOM. Drum sand resounds underfoot.");

    if (entity_at(nx, ny, g.cur_zone, EType::Corpse))
        g.log.push("A corpse lies here. G harvests its water.");

    advance_world();
    if (g.pl.sandwalk && !g.pl.dead) advance_world();  // slow, broken rhythm
}

void place_thumper() {
    if (g.cur_zone == map::kZoneSietch) {
        g.log.push("Not inside the sietch.");
        return;
    }
    if (g.pl.thumpers <= 0) {
        g.log.push("No thumpers left.");
        return;
    }
    Entity* t = alloc_entity();
    if (!t) return;
    Entity& p = player_ent();
    *t = Entity{EType::Thumper, Ai::Idle, g.cur_zone, p.x, p.y, 1, 0, 15};
    --g.pl.thumpers;
    g.log.push("Thumper planted. lump... lump... lump...");
    g.services.mixer->push({audio::Cmd::PlaySfx, 1, 180, 70});
    advance_world();
}

bool player_adjacent_op() {
    const Entity& p = player_ent();
    return g.op.live() && g.op.zone == g.cur_zone && g.op.adjacent(p.x, p.y);
}

void try_plant() {
    if (!player_adjacent_op()) {
        g.log.push("Nothing here worth a charge.");
        return;
    }
    if (g.pl.charges <= 0) {
        g.log.push("No demolition charges.");
        return;
    }
    ++g.op.plant_progress;
    if (g.op.plant_progress < harvest::kPlantTurns) {
        g.log.push("Working the charge into the hull seam... (%d/%d)",
                   int(g.op.plant_progress), harvest::kPlantTurns);
    } else {
        --g.pl.charges;
        g.op.state = harvest::OpState::Destroyed;
        g.noise.add(g.op.x, g.op.y, 90);
        g.services.mixer->push({audio::Cmd::PlaySfx, 0, 255, 140});
        flash(255, 180, 60);
        op_denied("The crawler blossoms into fire and shrapnel.");
    }
    advance_world();
}

void try_poison() {
    if (!player_adjacent_op()) {
        g.log.push("No spice hold in reach.");
        return;
    }
    if (g.pl.poisons <= 0) {
        g.log.push("No toxin vials left.");
        return;
    }
    if (g.op.poisoned) {
        g.log.push("The hold is already spoiled.");
        return;
    }
    --g.pl.poisons;
    g.op.poisoned = true;
    g.log.push("You slip the toxin into the hold. No one sees a thing.");
    advance_world();
}

void try_harvest() {
    Entity& p = player_ent();
    // stealing from the crawler hold takes priority when adjacent to it
    if (player_adjacent_op() && g.op.hold >= 15) {
        g.op.hold = int16_t(g.op.hold - 15);
        g.pl.spice += 5;
        g.op.stolen = true;
        g.noise.add(p.x, p.y, 8);
        g.log.push("You scoop raw spice from the hold. +5 grains.");
        advance_world();
        return;
    }
    if (Entity* c = entity_at(p.x, p.y, g.cur_zone, EType::Corpse)) {
        g.pl.water += c->data;
        if (g.pl.water > tune::kWaterMax) g.pl.water = tune::kWaterMax;
        g.log.push("The deathstill takes its due. +%d water.", int(c->data));
        c->type = EType::None;
        g.services.mixer->push({audio::Cmd::PlaySfx, 1, 150, 140});
        advance_world();
    } else {
        g.log.push("Nothing to harvest here.");
    }
}

void restart() {
    const GameServices services = g.services;
    const uint64_t seed = g.seed + 1;
    game_init(seed, services);
}

}  // namespace

void game_init(uint64_t seed, const GameServices& services) {
    g = GameState{};
    g.services = services;
    g.seed = seed;
    g.rng.init(seed);

    int sx, sy;
    map::generate(g.zones[map::kZoneSietch], seed, map::kZoneSietch, sx, sy);
    g.ents[0] = Entity{EType::Player, Ai::Idle, map::kZoneSietch,
                       int16_t(sx), int16_t(sy), int16_t(tune::kPlayerHpMax), 0, 0};
    g.spawned_mask = 1 << map::kZoneSietch;

    g.log.push("The Naib's word: deny them the spice. Go.");
    g.log.push("Arrows move. G harvest. T thumper. C sandwalk.");
    refresh_fov_and_paths();

    int px, py;
    iso::tile_to_screen(g.ents[0].x, g.ents[0].y, iso::Camera{}, px, py);
    g.cam.x = px - fb::W / 2;
    g.cam.y = py - fb::H / 2;
}

bool game_update(const InputState& in) {
    ++g.anim_tick;
    if (g.fade > 0) g.fade -= 6;

    if (g.pl.dead) {
        if (in.was_pressed(Key::Confirm)) restart();
        return !in.was_pressed(Key::Cancel);
    }

    int dx = 0, dy = 0;
    if (in.pressed_or_repeat(Key::Up)) { dy = -1; }
    else if (in.pressed_or_repeat(Key::Down)) { dy = 1; }
    else if (in.pressed_or_repeat(Key::Left)) { dx = -1; }
    else if (in.pressed_or_repeat(Key::Right)) { dx = 1; }
    else if (in.pressed_or_repeat(Key::UpLeft)) { dx = -1; dy = -1; }
    else if (in.pressed_or_repeat(Key::UpRight)) { dx = 1; dy = -1; }
    else if (in.pressed_or_repeat(Key::DownLeft)) { dx = -1; dy = 1; }
    else if (in.pressed_or_repeat(Key::DownRight)) { dx = 1; dy = 1; }

    if (dx || dy) try_move(dx, dy);
    else if (in.pressed_or_repeat(Key::Wait)) advance_world();
    else if (in.was_pressed(Key::Harvest)) try_harvest();
    else if (in.was_pressed(Key::Thumper)) place_thumper();
    else if (in.was_pressed(Key::Plant)) try_plant();
    else if (in.was_pressed(Key::Poison)) try_poison();
    else if (in.was_pressed(Key::Sandwalk)) {
        g.pl.sandwalk = !g.pl.sandwalk;
        g.log.push(g.pl.sandwalk ? "You break your rhythm. Walk without wormsign."
                                 : "You stride freely again.");
    }

    if (in.was_pressed(Key::Save)) {
        SaveWriter w;
        w.begin_chunk(kChunkGame);
        game_serialize(w);
        w.end_chunk();
        if (save_write_file("save.dat", w.data())) {
            g.log.push("Saved.");
            flash(240, 240, 240);
        }
    }
    if (in.was_pressed(Key::Load)) {
        std::vector<uint8_t> payload;
        if (save_read_file("save.dat", payload)) {
            SaveReader r(std::move(payload));
            if (r.open_chunk(kChunkGame) && game_deserialize(r)) {
                g.log.push("Loaded.");
                flash(240, 240, 240);
            }
        }
    }

    // camera follows the player (offset by half-screen so they sit centered,
    // nudged up a bit to favor the map over the HUD panel)
    int px, py;
    iso::tile_to_screen(player_ent().x, player_ent().y, iso::Camera{}, px, py);
    const int tx = px - fb::W / 2, ty = py - (fb::H - ui::kPanelH) / 2;
    g.cam.x += (tx - g.cam.x) / 6;
    g.cam.y += (ty - g.cam.y) / 6;

    return !in.was_pressed(Key::Cancel);
}

void game_render(Framebuffer& frame) {
    DrawCtx ctx{&frame, g.services.cmaps};
    frame.clear(1);

    const Assets& assets = *g.services.assets;
    const SpriteSheet* tiles = assets.get("tiles");
    const SpriteSheet* player_spr = assets.get("player");
    const SpriteSheet* hound_spr = assets.get("hound");
    const SpriteSheet* corpse_spr = assets.get("corpse");
    const SpriteSheet* rock_spr = assets.get("rock");
    const SpriteSheet* wall_spr = assets.get("wall");
    const SpriteSheet* cistern_spr = assets.get("cistern");
    const SpriteSheet* ripple_spr = assets.get("ripple");
    const SpriteSheet* mound_spr = assets.get("mound");
    const SpriteSheet* wormhead_spr = assets.get("wormhead");
    const SpriteSheet* wormseg_spr = assets.get("wormseg");
    const SpriteSheet* thumper_spr = assets.get("thumper");
    const SpriteSheet* crawler_spr = assets.get("crawler");
    const SpriteSheet* trooper_spr = assets.get("trooper");
    const SpriteSheet* spotter_spr = assets.get("spotter");
    const SpriteSheet* carryall_spr = assets.get("carryall");

    const map::Zone& z = zone();
    const bool night = is_night();

    // screen shake (visual only; fx stream never touches gameplay)
    iso::Camera cam = g.cam;
    int shake = 0;
    switch (g.wrm.state) {
        case worm::State::Hunting: shake = 1; break;
        case worm::State::Breaching: shake = 3; break;
        case worm::State::Surfaced: shake = 2; break;
        default: break;
    }
    if (shake > 0) {
        cam.x += int(g.rng.fx.below(uint32_t(shake * 2 + 1))) - shake;
        cam.y += int(g.rng.fx.below(uint32_t(shake * 2 + 1))) - shake;
    }
    const iso::Camera saved_cam = g.cam;
    g.cam = cam;

    for (int ty = 0; ty < map::H; ++ty) {
        for (int tx = 0; tx < map::W; ++tx) {
            const int idx = ty * map::W + tx;
            const uint8_t vis = z.vis[idx];
            if (!(vis & (map::V_VISIBLE | map::V_REMEMBERED))) continue;
            int sx, sy;
            iso::tile_to_screen(tx, ty, g.cam, sx, sy);
            if (sx < -32 || sx > fb::W + 32 || sy < -48 || sy > fb::H + 48) continue;

            int light;
            if (vis & map::V_VISIBLE) {
                light = night ? 16 : 31;
                if (!(z.flags[idx] & map::TF_INDOOR) && (z.flags[idx] & map::TF_SHADE) && !night)
                    light = 24;  // shade reads visibly cooler
                if (z.flags[idx] & map::TF_INDOOR) light = 20;
            } else {
                light = 7;  // remembered
            }

            draw_sprite(ctx, *tiles, z.floor[idx], sx, sy, light);
            const uint8_t tf = z.flags[idx];
            if (tf & map::TF_WALL) g.draw_list.add(wall_spr, 0, sx, sy + 8, light, 0);
            if (tf & map::TF_ROCKPROP) g.draw_list.add(rock_spr, 0, sx, sy + 6, light, 0);
            if (tf & map::TF_CISTERN) g.draw_list.add(cistern_spr, 0, sx, sy + 4, light, 0);
        }
    }

    for (const Entity& e : g.ents) {
        if (e.type == EType::None || e.zone != g.cur_zone) continue;
        const int idx = e.y * map::W + e.x;
        if (!(z.vis[idx] & map::V_VISIBLE)) continue;
        int sx, sy;
        iso::tile_to_screen(e.x, e.y, g.cam, sx, sy);
        const int light = night ? 18 : 31;
        switch (e.type) {
            case EType::Player:
                g.draw_list.add(player_spr, (g.anim_tick / 9) % 4, sx, sy + 6, light, 1);
                break;
            case EType::Hound:
                g.draw_list.add(hound_spr, (g.anim_tick / 7) % 2, sx, sy + 6, light, 1);
                break;
            case EType::Corpse:
                g.draw_list.add(corpse_spr, 0, sx, sy + 6, light, 0);
                break;
            case EType::Thumper:
                g.draw_list.add(thumper_spr, (g.anim_tick / 8) % 2, sx, sy + 4, light, 1);
                break;
            case EType::Trooper:
                g.draw_list.add(trooper_spr, (g.anim_tick / 8) % 2, sx, sy + 6, light, 1);
                break;
            case EType::Spotter:
                g.draw_list.add(spotter_spr, (g.anim_tick / 14) % 2, sx, sy + 6, light, 1);
                break;
            default: break;
        }
    }

    // The harvest crawler + carryall shadow.
    if (g.op.live() && g.op.zone == g.cur_zone) {
        const int idx = g.op.y * map::W + g.op.x;
        if (z.vis[idx] & (map::V_VISIBLE | map::V_REMEMBERED)) {
            int sx, sy;
            iso::tile_to_screen(g.op.x, g.op.y, g.cam, sx, sy);
            const int light = (z.vis[idx] & map::V_VISIBLE) ? (night ? 18 : 31) : 7;
            const bool churning = g.op.state == harvest::OpState::Working;
            g.draw_list.add(crawler_spr, churning ? (g.anim_tick / 6) % 2 : 0,
                            sx - 16, sy + 4, light, 1);
            if (g.op.state == harvest::OpState::Evac && (z.vis[idx] & map::V_VISIBLE)) {
                const int bob = (g.anim_tick / 12) % 3 - 1;
                g.draw_list.add(carryall_spr, 0, sx - 10, sy - 40 + bob, light, 3);
            }
        }
    }

    // The worm. Burrowed states telegraph with ripples; breaching mounds;
    // surfaced draws the whole segment chain.
    {
        const int wlight = night ? 18 : 31;
        int sx, sy;
        switch (g.wrm.state) {
            case worm::State::Hunting:
                for (int i = 0; i < worm::kSegments; i += 2) {
                    iso::tile_to_screen(g.wrm.seg_x[i], g.wrm.seg_y[i], g.cam, sx, sy);
                    g.draw_list.add(ripple_spr, (g.anim_tick / 6 + i) % 2, sx, sy, wlight, 0);
                }
                iso::tile_to_screen(g.wrm.x, g.wrm.y, g.cam, sx, sy);
                g.draw_list.add(ripple_spr, (g.anim_tick / 4) % 2, sx, sy, wlight, 0);
                break;
            case worm::State::Breaching:
                iso::tile_to_screen(g.wrm.x, g.wrm.y, g.cam, sx, sy);
                g.draw_list.add(mound_spr, 2 - (g.wrm.timer > 2 ? 2 : g.wrm.timer), sx,
                                sy + 8, wlight, 1);
                break;
            case worm::State::Surfaced:
                for (int i = worm::kSegments - 1; i >= 0; --i) {
                    iso::tile_to_screen(g.wrm.seg_x[i], g.wrm.seg_y[i], g.cam, sx, sy);
                    g.draw_list.add(wormseg_spr, 0, sx, sy + 10, wlight, 1);
                }
                iso::tile_to_screen(g.wrm.x, g.wrm.y, g.cam, sx, sy);
                g.draw_list.add(wormhead_spr, (g.anim_tick / 10) % 2, sx, sy + 12, wlight, 2);
                break;
            default: break;
        }
    }
    g.draw_list.flush(ctx);
    g.cam = saved_cam;

    ui::HudState hud{g.pl.hp, tune::kPlayerHpMax, g.pl.water, tune::kWaterMax,
                     g.pl.exposure, hour_now(), ambient_now(),
                     g.pl.turn / tune::kTurnsPerDay + 1,
                     (z.at(player_ent().x, player_ent().y) & map::TF_INDOOR) != 0,
                     g.noise_meter, uint8_t(g.wrm.state), g.seismo, g.seismo_head,
                     g.pl.thumpers, g.pl.sandwalk};
    ui::draw_hud(ctx, hud, g.log, g.anim_tick);

    // Harvest op status line (top of screen while an op is live in this zone).
    if (g.op.zone == g.cur_zone && g.op.state != harvest::OpState::None) {
        char buf[64];
        switch (g.op.state) {
            case harvest::OpState::Working:
                std::snprintf(buf, sizeof buf, "CRAWLER WORKING - HOLD %d%%", int(g.op.hold));
                break;
            case harvest::OpState::Paused:
                std::snprintf(buf, sizeof buf, "CRAWLER SILENT - wormsign hold %d%%", int(g.op.hold));
                break;
            case harvest::OpState::Evac:
                std::snprintf(buf, sizeof buf, "CARRYALL INBOUND - %d turns", int(g.op.eta));
                break;
            case harvest::OpState::Shipped:
                std::snprintf(buf, sizeof buf, g.op.poisoned ? "LOAD SHIPPED (SPOILED)" : "LOAD SHIPPED");
                break;
            case harvest::OpState::Destroyed:
                std::snprintf(buf, sizeof buf, "HARVEST DENIED");
                break;
            default: buf[0] = 0; break;
        }
        draw_text_shadow(ctx, buf, 4, 4,
                         g.op.state == harvest::OpState::Destroyed ? 245 :
                         g.op.state == harvest::OpState::Evac ? 210 : 230, 1);
        if (g.alarm_turns > 0) {
            std::snprintf(buf, sizeof buf, "ALARM %d", g.alarm_turns);
            draw_text_shadow(ctx, buf, 4, 14, 208, 1);
        }
    }

    if (g.pl.dead) {
        fill_rect(ctx, fb::W / 2 - 150, 120, 300, 74, 2);
        draw_text_shadow(ctx, "You are one with the sand now.", fb::W / 2 - 120, 132, 210, 1);
        draw_text_shadow(ctx, "Fear is the mind-killer.", fb::W / 2 - 96, 148, 24, 1);
        char buf[48];
        std::snprintf(buf, sizeof buf, "seed %llu", (unsigned long long)g.seed);
        draw_text_shadow(ctx, buf, fb::W / 2 - 60, 164, 14, 1);
        draw_text_shadow(ctx, "ENTER: rise again", fb::W / 2 - 68, 178, 230, 1);
    }
}

PaletteFade game_fade() {
    if (g.fade <= 0) return PaletteFade{};
    return PaletteFade{g.fade_r, g.fade_g, g.fade_b, uint16_t(g.fade)};
}

void game_serialize(SaveWriter& w) {
    w.u64(g.seed);
    w.u8(g.cur_zone);
    w.i32(g.pl.hp);
    w.i32(g.pl.water);
    w.i32(g.pl.exposure);
    w.i32(g.pl.turn);
    w.i32(g.pl.thumpers);
    w.i32(g.pl.charges);
    w.i32(g.pl.poisons);
    w.i32(g.pl.spice);
    w.u8(g.pl.sandwalk ? 1 : 0);
    w.u8(g.pl.dead ? 1 : 0);
    w.u8(g.spawned_mask);
    w.bytes(&g.op, sizeof g.op);
    w.i32(g.alarm_turns);
    w.i32(g.alarm_spawn_cd);
    w.bytes(g.ents, sizeof g.ents);
    w.bytes(&g.rng.world, sizeof g.rng.world);
    w.bytes(&g.rng.combat, sizeof g.rng.combat);
    w.bytes(&g.rng.loot, sizeof g.rng.loot);
    w.bytes(&g.wrm, sizeof g.wrm);
    w.bytes(g.noise.at, sizeof g.noise.at);
    w.i32(g.delta_count);
    w.bytes(g.deltas, sizeof g.deltas);
    for (int i = 0; i < map::kZoneCount; ++i)
        w.bytes(g.zones[i].vis, sizeof g.zones[i].vis);
}

bool game_deserialize(SaveReader& r) {
    const uint64_t seed = r.u64();
    const uint8_t cur = r.u8();
    Player pl;
    pl.hp = r.i32();
    pl.water = r.i32();
    pl.exposure = r.i32();
    pl.turn = r.i32();
    pl.thumpers = r.i32();
    pl.charges = r.i32();
    pl.poisons = r.i32();
    pl.spice = r.i32();
    pl.sandwalk = r.u8() != 0;
    pl.dead = r.u8() != 0;
    const uint8_t spawned = r.u8();
    harvest::Op op;
    r.bytes(&op, sizeof op);
    const int alarm_turns = r.i32();
    const int alarm_cd = r.i32();
    Entity ents[kMaxEnts];
    r.bytes(ents, sizeof ents);
    Pcg32 world, combat, loot;
    r.bytes(&world, sizeof world);
    r.bytes(&combat, sizeof combat);
    r.bytes(&loot, sizeof loot);
    worm::Worm wrm;
    r.bytes(&wrm, sizeof wrm);
    static int16_t noise_buf[map::N];
    r.bytes(noise_buf, sizeof noise_buf);
    const int delta_count = r.i32();
    Delta deltas[kMaxDeltas];
    r.bytes(deltas, sizeof deltas);
    static uint8_t vis[map::kZoneCount][map::N];
    for (int i = 0; i < map::kZoneCount; ++i) r.bytes(vis[i], sizeof vis[i]);
    if (!r.ok() || cur >= map::kZoneCount || delta_count > kMaxDeltas) return false;

    g.seed = seed;
    g.cur_zone = cur;
    g.pl = pl;
    g.spawned_mask = spawned;
    g.op = op;
    g.alarm_turns = alarm_turns;
    g.alarm_spawn_cd = alarm_cd;
    std::memcpy(g.ents, ents, sizeof g.ents);
    g.rng.world = world;
    g.rng.combat = combat;
    g.rng.loot = loot;
    g.wrm = wrm;
    std::memcpy(g.noise.at, noise_buf, sizeof noise_buf);
    g.delta_count = delta_count;
    std::memcpy(g.deltas, deltas, sizeof g.deltas);
    int sx_, sy_;
    for (int i = 0; i < map::kZoneCount; ++i) {
        map::generate(g.zones[i], seed, map::ZoneId(i), sx_, sy_);
        apply_deltas(uint8_t(i));
        std::memcpy(g.zones[i].vis, vis[i], sizeof vis[i]);
    }
    if (g.op.live())
        for (int dy = -1; dy <= 0; ++dy)
            for (int dx = -1; dx <= 0; ++dx)
                g.zones[g.op.zone].flags[(g.op.y + dy) * map::W + g.op.x + dx] |= map::TF_BLOCK;
    g.shade_hour = -1;
    recompute_shade(g.cur_zone, hour_now());
    refresh_fov_and_paths();
    return true;
}

bool game_is_dead() { return g.pl.dead; }

int game_worm_state() { return int(g.wrm.state); }

void game_debug_warp(int zone_id, int x, int y) {
    enter_zone(uint8_t(zone_id), false, x, y);
}

void game_debug_op_info(int* state, int* x, int* y) {
    if (state) *state = int(g.op.state);
    if (x) *x = g.op.x;
    if (y) *y = g.op.y;
}

void game_debug_kill_spotters() {
    for (Entity& e : g.ents)
        if (e.type == EType::Spotter) e.type = EType::None;
}

void game_debug_refill() {
    g.pl.water = tune::kWaterMax;
    g.pl.hp = tune::kPlayerHpMax;
    g.pl.exposure = 0;
}

uint64_t game_state_hash() {
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001b3ULL; };
    mix(g.seed);
    mix(uint64_t(g.cur_zone));
    mix(uint64_t(uint32_t(g.pl.hp)));
    mix(uint64_t(uint32_t(g.pl.water)));
    mix(uint64_t(uint32_t(g.pl.exposure)));
    mix(uint64_t(uint32_t(g.pl.turn)));
    for (const Entity& e : g.ents) {
        mix(uint64_t(uint8_t(e.type)) | (uint64_t(e.zone) << 8) |
            (uint64_t(uint16_t(e.x)) << 16) | (uint64_t(uint16_t(e.y)) << 32) |
            (uint64_t(uint16_t(e.hp)) << 48));
    }
    mix(g.rng.world.state);
    mix(g.rng.combat.state);
    mix(g.rng.loot.state);
    return h;
}
