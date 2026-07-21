// M1 vertical slice: sietch + one desert region, energy turns, FOV, bump
// combat vs dust hounds, water/heat/exposure survival, HUD, permadeath.
#include "game/game.h"

#include <cstdio>
#include <cstring>

#include "engine/animator.h"
#include "engine/particles.h"
#include "engine/passes.h"
#include "engine/render.h"
#include "engine/rng.h"
#include "game/campaign.h"
#include "game/map.h"
#include "game/harvest.h"
#include "game/light.h"
#include "game/tunables.h"
#include "game/ui.h"
#include "game/worm.h"

namespace {

constexpr uint32_t kChunkGame = 0x454D4147;  // 'GAME'
constexpr int kMaxEnts = 64;
constexpr int kMaxDeltas = 192;

enum class EType : uint8_t { None = 0, Player, Hound, Corpse, Thumper, Trooper, Spotter,
                             Glowglobe, ShieldTrooper, Naib, Sayyadina, Smuggler,
                             PanAdder, Hermit, HunterSeeker, SpiceCache };
enum class Ai : uint8_t { Idle = 0, Wander, Hunt, Guard };

struct Entity {
    EType type = EType::None;
    Ai ai = Ai::Idle;
    uint8_t zone = 0;
    int16_t x = 0, y = 0;
    int16_t hp = 0;
    int16_t energy = 0;
    int16_t data = 0;  // corpse: water yield
    // G1 presentation-only state (persisted with the entity, never gameplay):
    int16_t vpx = 0, vpy = 0;   // visual world-pixel position (glides)
    anim::Animator anim;
    uint8_t hurt_flash = 0;     // G3: bright flinch frames remaining
};

enum class Background : uint8_t { Fedaykin = 0, Saboteur, Sayyadina, Count };

struct BackgroundDef {
    const char* name;
    const char* sprite;
    const char* blurb1;
    const char* blurb2;
};
constexpr BackgroundDef kBackgrounds[] = {
    {"FEDAYKIN", "player_fedaykin",
     "Death commando of the inner sietch.", "Crysknife-trained: strikes hit harder."},
    {"SABOTEUR", "player_saboteur",
     "Quiet hands, loud results.", "Extra charges, toxin and thumpers."},
    {"SAYYADINA-TOUCHED", "player_sayyadina",
     "The spice has opened one eye.", "Carries spice; the desert drinks slower."},
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
    int glowglobes = 2;
    int atk = tune::kPlayerAtk;
    bool maker_hooks = false;  // Honored: the right to ride
    bool riding = false;
    int ride_turns = 0;
    int tox = 0;               // spice tolerance burn, decays 1/turn
    int tox_lifetime = 0;      // total ever — the Eyes come at 40
    bool eyes_ibad = false;
    int litany_turns = 0;      // active suppression
    int litany_day = -1;       // last day the Litany was spoken
    int free_moves = 0;        // Threadwalk strides (no world advance)
    Background background = Background::Fedaykin;
    bool sandwalk = false;
    bool dead = false;
};

void apply_background(Player& pl, Background bg) {
    pl.background = bg;
    switch (bg) {
        case Background::Fedaykin:
            pl.atk = tune::kPlayerAtk + 2;
            break;
        case Background::Saboteur:
            pl.charges = 3;
            pl.poisons = 2;
            pl.thumpers = 5;
            break;
        case Background::Sayyadina:
            pl.spice = 10;
            pl.water = tune::kStartWater + 200;
            break;
        default: break;
    }
}

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

    uint8_t lightmap[map::N];  // M7: per-tile light levels, rebuilt per turn
    light::Source flashes[8];  // transient sources (explosions, breaches)
    int flash_count = 0;

    campaign::State season;    // M4: the harvest season
    int menu_open = 0;         // 0 none / 1 naib / 2 sayyadina / 3 smuggler
    int menu_sel = 0;
    int last_decay_day = 0;

    // Naib task: 0 none, 1 kill hounds, 2 gather spice, 3 quiet denial
    int quest_kind = 0;
    int quest_need = 0;
    int quest_progress = 0;

    int storm_turns = 0;       // M5: Coriolis storm remaining
    int poison_turns = 0;      // pan adder venom: +thirst while active
    int16_t thopter_x = -1, thopter_y = 0;  // ornithopter sweep during alarms
    int8_t thopter_dx = 1;
    uint8_t noise_meter = 0;          // HUD value (near player)
    uint8_t seismo[64] = {};          // history ring for the trace
    uint8_t seismo_head = 0;

    Delta deltas[kMaxDeltas];
    int delta_count = 0;

    int16_t dmap[map::N];
    iso::Camera cam;
    iso::DrawList draw_list;
    particles::Pool particles;
    ui::Log log;

    int16_t impact_x = -1, impact_y = -1;  // where the player's blow will land
    int current_song = -1;
    int16_t hover_tx = -1, hover_ty = -1;
    int anim_tick = 0;
    int fade = 0;
    uint8_t fade_r = 255, fade_g = 255, fade_b = 255;

    bool choosing = false;      // background chooser (interactive new runs)
    int choose_idx = 0;
};
GameState g;

void op_denied(const char* how);
void op_shipped_quota();
uint8_t facing_from_tile_delta(int dx, int dy);
void apply_deltas(uint8_t zone_id);
void end_season(uint8_t ending);
void spawn_zone_wildlife(uint8_t zone_id);

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
    if (g.storm_turns > 0) return 3;  // the storm swallows the world
    if (g.pl.water <= 0 && g.pl.litany_turns == 0) return tune::kFovDehydrated;
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

void place_npc_near(EType type, uint8_t zone_id, int px, int py) {
    map::Zone& z = g.zones[zone_id];
    for (int r = 0; r < 8; ++r)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                const int x = px + dx, y = py + dy;
                if (x < 1 || x >= map::W - 1 || y < 1 || y >= map::H - 1) continue;
                if (!z.walkable(x, y) || entity_at(x, y, zone_id)) continue;
                if (Entity* e = alloc_entity()) {
                    *e = Entity{type, Ai::Idle, zone_id, int16_t(x), int16_t(y), 10, 0, 0};
                    return;
                }
            }
}

void spawn_zone_wildlife(uint8_t zone_id) {
    if (g.spawned_mask & (1 << zone_id)) return;
    g.spawned_mask |= uint8_t(1 << zone_id);
    if (zone_id == map::kZoneSietch) {
        place_npc_near(EType::Naib, zone_id, 30, 27);
        place_npc_near(EType::Sayyadina, zone_id, 36, 25);
        return;
    }
    if (zone_id == map::kZoneDesert)
        place_npc_near(EType::Smuggler, zone_id, 12, 30);  // camp off the door road
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
    // ambush adders (wormier zones have more) and the odd spice-mad hermit
    const int adders = zone_id == map::kZoneDeepPan ? 3 : 1;
    for (int i = 0, tries = 0; i < adders && tries < 400; ++tries) {
        const int x = g.rng.world.range(6, map::W - 2);
        const int y = g.rng.world.range(2, map::H - 2);
        if (!z.walkable(x, y) || entity_at(x, y, zone_id)) continue;
        if (Entity* e = alloc_entity()) {
            *e = Entity{EType::PanAdder, Ai::Idle, zone_id, int16_t(x), int16_t(y), 3, 0, 0};
            ++i;
        }
    }
    if (g.rng.world.chance(1, 2))
        for (int tries = 0; tries < 200; ++tries) {
            const int x = g.rng.world.range(10, map::W - 4);
            const int y = g.rng.world.range(4, map::H - 4);
            if (!z.walkable(x, y) || entity_at(x, y, zone_id)) continue;
            if (Entity* e = alloc_entity())
                *e = Entity{EType::Hermit, Ai::Wander, zone_id, int16_t(x), int16_t(y),
                            8, 0, int16_t(g.rng.world.range(5, 8))};
            break;
        }
}

// Place a harvest op in a zone: crawler on open sand, spotters on the nearest
// rock, an escort ring of troopers scaled by mutators and alert.
void spawn_op_in(uint8_t zone_id, uint8_t mutators) {
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
    g.op.mutators = mutators;
    g.op.deadline = g.pl.turn + campaign::kOpDeadline;
    if (mutators & campaign::MUT_CLIMAX) g.op.hull = 60;
    else if (mutators & campaign::MUT_ELITE) g.op.hull = 45;

    // crawler footprint blocks movement (but the worm eats through it)
    for (int dy = -1; dy <= 0; ++dy)
        for (int dx = -1; dx <= 0; ++dx)
            z.flags[(ay + dy) * map::W + ax + dx] |= map::TF_BLOCK;

    // spotters on the nearest rock ground within 12 tiles (else sand)
    const int want_spotters =
        (mutators & (campaign::MUT_TWIN_SPOTTERS | campaign::MUT_CLIMAX)) ? 4 : 2;
    int placed = 0;
    for (int r = 2; r <= 14 && placed < want_spotters; ++r) {
        for (int dy = -r; dy <= r && placed < want_spotters; ++dy)
            for (int dx = -r; dx <= r && placed < want_spotters; ++dx) {
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

    // escort scaled by mutators/alert; elites bring Holtzman shields
    const bool elite = (mutators & (campaign::MUT_ELITE | campaign::MUT_CLIMAX)) ||
                       g.season.alert >= campaign::kAlertEliteAt;
    const int troopers = (mutators & campaign::MUT_CLIMAX) ? 5 : elite ? 4 : 3;
    const int shields = (mutators & campaign::MUT_CLIMAX) ? 2 : elite ? 1 : 0;
    for (int i = 0, tries = 0; i < troopers + shields && tries < 300; ++tries) {
        const int x = ax + g.rng.world.range(-4, 3);
        const int y = ay + g.rng.world.range(-4, 3);
        if (!z.walkable(x, y) || entity_at(x, y, zone_id) || g.op.tile_on(x, y)) continue;
        if (Entity* e = alloc_entity()) {
            const bool shielded = i >= troopers;
            *e = Entity{shielded ? EType::ShieldTrooper : EType::Trooper, Ai::Guard,
                        zone_id, int16_t(x), int16_t(y), int16_t(shielded ? 10 : 8), 0, 0};
            ++i;
        }
    }
}

// Season scheduler: spawns the next op when its clock comes due.
void schedule_ops() {
    if (g.season.ending != campaign::kNone) return;
    if (g.op.live()) {
        // unattended ops ship on their deadline (whether or not you watched)
        if (g.pl.turn >= g.op.deadline) {
            g.op.state = harvest::OpState::Shipped;
            if (g.op.poisoned) {
                op_denied("Word comes: the shipped load was rotten. Quiet work.");
            } else {
                g.log.push("A carryall lifted a full hold from the %s.",
                           g.op.zone == map::kZoneDeepPan ? "Deep Pan" : "Rimwall fringe");
                op_shipped_quota();
            }
        }
        return;
    }
    if (g.pl.turn < g.season.next_op_turn) return;

    uint8_t mut = 0;
    uint8_t target_zone;
    if (g.season.climax_pending) {
        mut = campaign::MUT_CLIMAX;
        target_zone = map::kZoneDeepPan;
        g.season.climax_pending = 2;  // spawned
    } else {
        target_zone = uint8_t(g.rng.world.chance(2, 5) ? map::kZoneDesert
                                                       : map::kZoneDeepPan);
        const uint8_t pool[4] = {campaign::MUT_ELITE, campaign::MUT_SLOPPY,
                                 campaign::MUT_TWIN_SPOTTERS, campaign::MUT_WORMSIGN};
        const int rolls = int(g.rng.world.below(3));  // 0..2 mutators
        for (int i = 0; i < rolls; ++i) mut |= pool[g.rng.world.below(4)];
    }

    // ensure the target zone's terrain exists for placement
    if (!(g.spawned_mask & (1 << target_zone))) {
        int sx_, sy_;
        map::generate(g.zones[target_zone], g.seed, map::ZoneId(target_zone), sx_, sy_);
        apply_deltas(target_zone);
        spawn_zone_wildlife(target_zone);
    }
    spawn_op_in(target_zone, mut);
    if (!g.op.live()) return;  // no anchor found; retry next turn

    if (g.cur_zone == target_zone) {
        // freshly spawned around the player: re-block footprint on live zone
    } else if (mut & campaign::MUT_CLIMAX) {
        g.log.push("Scouts running: the GREAT HARVEST deploys in the Deep Pan!");
        g.log.push("Twin escorts. Shielded blades. End it, Fedaykin.");
    } else {
        const bool precise = g.season.standing >= 25;
        if (precise)
            g.log.push("Scouts: a crawler at %d,%d in the %s.%s", int(g.op.x), int(g.op.y),
                       target_zone == map::kZoneDeepPan ? "Deep Pan" : "Rimwall fringe",
                       (mut & campaign::MUT_ELITE) ? " Heavy escort." : "");
        else
            g.log.push("Scouts: harvest smoke over the %s.",
                       target_zone == map::kZoneDeepPan ? "Deep Pan" : "Rimwall fringe");
    }
    if (mut & campaign::MUT_WORMSIGN)
        g.log.push("They chose poor ground - wormsign already in that quarter.");
}

// Per-turn season upkeep: alert decay, sietch discovery countdown.
void season_turn() {
    if (g.season.ending != campaign::kNone) return;

    const int day = g.pl.turn / tune::kTurnsPerDay;
    if (day != g.last_decay_day) {
        g.last_decay_day = day;
        if (g.season.alert > 0) {
            g.season.alert -= campaign::kAlertDecayPerDay;
            if (g.season.alert < 0) g.season.alert = 0;
        }
    }

    if (g.season.alert >= campaign::kAlertDiscoveryAt) {
        ++g.season.discovery;
        if (g.season.discovery == campaign::kDiscoveryRaidAt / 2)
            g.log.push("Thopters quarter the sky in search lanes. They are close.");
        if (g.season.discovery >= campaign::kDiscoveryRaidAt) {
            end_season(campaign::kRaidLoss);
            return;
        }
    } else if (g.season.discovery > 0) {
        --g.season.discovery;
    }

    schedule_ops();
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

void add_flash(int x, int y, int radius, int intensity, int ttl) {
    if (g.flash_count < 8)
        g.flashes[g.flash_count++] = light::Source{int16_t(x), int16_t(y),
                                                   uint8_t(radius), uint8_t(intensity),
                                                   int8_t(ttl)};
}

// Rebuild the per-tile lightmap from this turn's sources.
void recompute_light() {
    light::Source srcs[40];
    int n = 0;
    const map::Zone& z = zone();

    // the player's suit lamp
    srcs[n++] = {player_ent().x, player_ent().y, 7, 26, 0};

    // placed lights and machines
    for (const Entity& e : g.ents) {
        if (e.zone != g.cur_zone || n >= 36) continue;
        if (e.type == EType::Glowglobe) srcs[n++] = {e.x, e.y, 6, 29, 0};
        else if (e.type == EType::Thumper) srcs[n++] = {e.x, e.y, 2, 14, 0};
    }
    if (g.op.live() && g.op.zone == g.cur_zone)
        srcs[n++] = {g.op.x, g.op.y, 8, 26, 0};  // crawler floodlights

    // the worm's violence glows with static discharge
    if (g.wrm.state == worm::State::Breaching || g.wrm.state == worm::State::Surfaced)
        if (n < 38) srcs[n++] = {g.wrm.x, g.wrm.y, 5, 18, 0};

    // sietch lamps: cistern + door pads
    if (g.cur_zone == map::kZoneSietch) {
        for (int i = 0; i < map::N && n < 38; ++i) {
            if (z.flags[i] & map::TF_CISTERN)
                srcs[n++] = {int16_t(i % map::W), int16_t(i / map::W), 4, 24, 0};
            else if (z.flags[i] & map::TF_DOOR)
                srcs[n++] = {int16_t(i % map::W), int16_t(i / map::W), 2, 16, 0};
        }
    }

    // ornithopter searchlight
    if (g.thopter_x >= 0 && n < 39)
        srcs[n++] = {g.thopter_x, g.thopter_y, 4, 30, 0};

    // transient flashes
    for (int i = 0; i < g.flash_count && n < 40; ++i) srcs[n++] = g.flashes[i];

    light::compute(z, srcs, n, light::kAmbientByHour[hour_now()], g.lightmap);
}

void refresh_fov_and_paths() {
    map::compute_fov(zone(), player_ent().x, player_ent().y, fov_radius());
    map::compute_dijkstra(zone(), player_ent().x, player_ent().y, g.dmap);
    recompute_light();
}

// meta.dat: tiny cross-run memory — seasons played, endings witnessed.
struct Meta {
    uint32_t runs = 0;
    uint8_t endings_seen = 0;  // bit per campaign::Ending
};
Meta g_meta;

void meta_load() {
    FILE* f = std::fopen("meta.dat", "rb");
    if (!f) return;
    uint32_t magic = 0;
    if (std::fread(&magic, 4, 1, f) == 1 && magic == 0x54454D44u) {
        (void)!std::fread(&g_meta.runs, 4, 1, f);
        (void)!std::fread(&g_meta.endings_seen, 1, 1, f);
    }
    std::fclose(f);
}

void meta_save() {
    FILE* f = std::fopen("meta.dat", "wb");
    if (!f) return;
    const uint32_t magic = 0x54454D44u;  // 'DMET'
    std::fwrite(&magic, 4, 1, f);
    std::fwrite(&g_meta.runs, 4, 1, f);
    std::fwrite(&g_meta.endings_seen, 1, 1, f);
    std::fclose(f);
}

void die(const char* cause) {
    g.pl.dead = true;
    g.log.push("%s", cause);
    g.services.mixer->push({audio::Cmd::PlaySfx, 0 /* roar */, 200, 96});
    ++g_meta.runs;
    meta_save();
}

void end_season(uint8_t ending) {
    if (g.season.ending != campaign::kNone) return;
    g.season.ending = ending;
    switch (ending) {
        case campaign::kVictory:
            g.log.push("The great harvest burns. The season is BROKEN.");
            flash(240, 200, 80);
            break;
        case campaign::kQuotaLoss:
            g.log.push("The eighth load reaches orbit. The Baron has his quota.");
            flash(120, 120, 140);
            break;
        case campaign::kRaidLoss:
            g.log.push("Thopters at dawn. Fire in the cave mouths. They found us.");
            flash(255, 60, 20);
            break;
    }
    ++g_meta.runs;
    g_meta.endings_seen |= uint8_t(1 << ending);
    meta_save();
}

// ---- worm callbacks ----------------------------------------------------------

void end_season(uint8_t ending);

void raise_alert(int amount) {
    if (amount <= 0) return;
    g.season.alert += amount;
    if (g.season.alert > 100) g.season.alert = 100;
    if (g.season.alert >= campaign::kAlertDiscoveryAt)
        g.log.push("ALERT %d: survey thopters triangulate the deep desert.",
                   int(g.season.alert));
}

void raise_standing(int amount) {
    g.season.standing += amount;
    if (g.season.standing > 100) g.season.standing = 100;
    if (g.season.standing < 0) g.season.standing = 0;
    if (!g.season.crysknife_granted &&
        g.season.standing >= campaign::kStandingCrysknifeAt) {
        g.season.crysknife_granted = 1;
        g.pl.atk += 2;
        g.pl.maker_hooks = true;
        g.log.push("The Naib unwraps a pale blade. 'Honored one. It is yours.'");
        g.log.push("Crysknife and MAKER HOOKS taken. R mounts a surfaced worm.");
    }
}

void op_denied(const char* how) {
    if (g.op.denied_reported) return;
    g.op.denied_reported = true;
    g.log.push("%s", how);
    g.alarm_turns = 50;
    g.alarm_spawn_cd = 6;

    ++g.season.denied;
    raise_standing(campaign::kStandingPerDenial);
    // quest: the quiet denial
    if (g.quest_kind == 3) {
        const bool quiet = g.op.spotters_knifed || g.op.poisoned;
        if (quiet) {
            g.quest_kind = 0;
            raise_standing(15);
            g.log.push("The Naib nods once. Quiet work. +15 standing.");
        }
    }
    int alert_gain = campaign::kAlertWormBait;
    if (g.op.poisoned && g.op.state == harvest::OpState::Shipped) alert_gain = 0;
    else if (g.op.state == harvest::OpState::Destroyed && g.op.plant_progress >= 3)
        alert_gain = campaign::kAlertDemolition;
    else if (g.op.spotters_knifed)
        alert_gain = campaign::kAlertQuietWorm;
    raise_alert(alert_gain);

    if (g.pl.eyes_ibad) alert_gain += alert_gain > 0 ? 1 : 0;  // a marked face
    if (g.op.mutators & campaign::MUT_CLIMAX) {
        end_season(campaign::kVictory);
        return;
    }
    g.log.push("The harvest is DENIED. (%d of %d) The Baron will rage.",
               int(g.season.denied), campaign::kDenialsForClimax + 1);
    if (g.season.denied >= campaign::kDenialsForClimax && !g.season.climax_pending) {
        g.season.climax_pending = 1;
        g.log.push("Smuggler word: the Baron stakes the season on ONE great harvest.");
    }
    g.season.next_op_turn =
        g.pl.turn + g.rng.world.range(campaign::kOpGapMin, campaign::kOpGapMax);
}

void op_shipped_quota() {
    ++g.season.shipped;
    g.log.push("CHOAM ledger: %d loads shipped, %d denied.",
               int(g.season.shipped), int(g.season.denied));
    if (g.season.shipped >= campaign::kQuotaLossAt) {
        end_season(campaign::kQuotaLoss);
        return;
    }
    g.season.next_op_turn =
        g.pl.turn + g.rng.world.range(campaign::kOpGapMin, campaign::kOpGapMax);
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
            e.anim.facing = facing_from_tile_delta(p.x - e.x, p.y - e.y);
            e.anim.set(anim::State::Attack);
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
            if (!(op.mutators & campaign::MUT_SLOPPY) || (g.pl.turn & 1))
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
            if (g.storm_turns > 0) break;  // carryalls don't fly in a Coriolis
            if (--op.eta <= 0) {
                op.state = harvest::OpState::Shipped;
                if (op.poisoned) {
                    op_denied("The load lifts away - laced with rot they will "
                              "not find until the refinery.");
                } else {
                    g.log.push("The carryall lifts the load into the sky. Lost.");
                    op_shipped_quota();
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

// The pan adder strikes what steps close; the hermit shambles; the seeker homes.
void critter_act(Entity& e) {
    const map::Zone& z = zone();
    const Entity& p = player_ent();
    const int pdist = g.dmap[e.y * map::W + e.x];

    if (e.type == EType::HunterSeeker) {
        if (pdist == 1) {
            g.log.push("The sliver strikes like a wasp - and spends itself.");
            damage_player(4, "A hunter-seeker found its mark.");
            e.type = EType::None;
            return;
        }
        int best_x = e.x, best_y = e.y;
        int16_t best = pdist >= 0 ? pdist : INT16_MAX;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = e.x + dx, ny = e.y + dy;
                if (!z.walkable(nx, ny) || entity_at(nx, ny, e.zone)) continue;
                if (nx == p.x && ny == p.y) continue;
                const int16_t d = g.dmap[ny * map::W + nx];
                if (d >= 0 && d < best) { best = d; best_x = nx; best_y = ny; }
            }
        e.x = int16_t(best_x);
        e.y = int16_t(best_y);
        return;
    }

    if (e.type == EType::PanAdder) {
        if (pdist == 1 && g.rng.combat.chance(2, 3)) {
            g.log.push("The pan adder STRIKES - venom burns in your blood.");
            damage_player(1, "Venom in the blood, sand in the mouth.");
            g.poison_turns = 20;
        }
        return;  // ambusher: it does not chase
    }

    if (e.type == EType::Hermit) {
        if (pdist == 1) {
            const int dmg = g.rng.combat.range(2, 4);
            g.log.push("The spice-mad hermit claws at you, shrieking prophecy!");
            damage_player(dmg, "Beaten down by a prophet of the sand.");
            return;
        }
        if (g.rng.combat.chance(1, 2)) {
            const int dx = g.rng.combat.range(-1, 1), dy = g.rng.combat.range(-1, 1);
            const int nx = e.x + dx, ny = e.y + dy;
            if (z.walkable(nx, ny) && !entity_at(nx, ny, e.zone) &&
                !(nx == p.x && ny == p.y)) {
                e.x = int16_t(nx);
                e.y = int16_t(ny);
            }
        }
        return;
    }
}

// Ornithopter sweep: while the alarm is up (and no storm), a searchlight
// quarters the zone. Caught in the beam = reinforcements on your head.
void thopter_turn() {
    if (g.alarm_turns <= 0 || g.storm_turns > 0 || g.cur_zone == map::kZoneSietch) {
        g.thopter_x = -1;
        return;
    }
    if (g.thopter_x < 0) {
        g.thopter_x = 0;
        g.thopter_y = int16_t(g.rng.combat.range(8, map::H - 8));
        g.thopter_dx = 1;
        g.log.push("Engine drone overhead - a thopter begins its sweep.");
    }
    g.thopter_x = int16_t(g.thopter_x + g.thopter_dx * 3);
    if (g.thopter_x >= map::W) {
        g.thopter_x = 0;
        g.thopter_y = int16_t(g.rng.combat.range(8, map::H - 8));
    }
    const Entity& p = player_ent();
    const int dx = p.x - g.thopter_x, dy = p.y - g.thopter_y;
    const bool indoors = (zone().at(p.x, p.y) & map::TF_INDOOR) != 0;
    if (dx * dx + dy * dy <= 9 && !indoors) {
        g.log.push("The searchlight pins you! Voices scream coordinates.");
        g.alarm_turns += 15;
        if (g.alarm_turns > 90) g.alarm_turns = 90;
        raise_alert(1);
        g.alarm_spawn_cd = 1;  // reinforcements immediately
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
            e.anim.facing = facing_from_tile_delta(p.x - e.x, p.y - e.y);
            e.anim.set(anim::State::Attack);
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
    if (g.pl.tox > 60) drain = drain * 3 / 2;  // the spice burns water
    if (base > 0 && drain < 1) drain = 1;
    g.pl.water -= drain;
    if (g.pl.water < 0) g.pl.water = 0;
    if (g.pl.water == 0) {
        damage_player(tune::kDehydrateDamage, "Died of thirst in the Tanzerouft.");
        if ((g.pl.turn % 5) == 0) g.log.push("Your throat is cracked glass. Water. Water.");
    } else if (g.pl.water == 100) {
        g.log.push("Your water is nearly gone.");
    }

    // quest: gather spice (progress = spice gained since taking the task)
    if (g.quest_kind == 2 && g.quest_progress + g.pl.spice >= g.quest_need) {
        g.quest_kind = 0;
        raise_standing(12);
        g.log.push("Grains enough for the Sayyadina's rites. +12 standing.");
    }

    // spice tolerance burns down; the Litany holds the body together
    if (g.pl.tox > 0) --g.pl.tox;
    if (g.pl.litany_turns > 0) --g.pl.litany_turns;
    if (!g.pl.eyes_ibad && g.pl.tox_lifetime >= 40) {
        g.pl.eyes_ibad = true;
        g.log.push("Your reflection in a blade: eyes gone BLUE WITHIN BLUE.");
        g.log.push("The tribe will know you. So will the Harkonnens.");
    }
    if (g.pl.tox > 60 && (g.pl.turn % 7) == 0 && g.pl.litany_turns == 0) {
        static const char* kVisions[4] = {
            "The dunes breathe. You are sure of it.",
            "A voice you buried speaks your water-name.",
            "Futures branch before you like cracked glass.",
            "The sun blinks. Slowly. Like an eye.",
        };
        g.log.push("%s", kVisions[g.rng.fx.below(4)]);
    }

    // pan adder venom burns water
    if (g.poison_turns > 0) {
        --g.poison_turns;
        g.pl.water -= 1;
        if (g.pl.water < 0) g.pl.water = 0;
    }

    // storm scour: sand-blast exposure regardless of hour
    if (g.storm_turns > 0) {
        --g.storm_turns;
        if (g.storm_turns == 0) g.log.push("The storm passes. The desert exhales.");
        if (!(tf & (map::TF_INDOOR | map::TF_SHADE))) {
            g.pl.exposure += 2;
            if (g.pl.exposure > 100) g.pl.exposure = 100;
        }
        g.noise.decay();  // the roar swallows every other sound
    }

    // heat exposure
    const bool sheltered = (tf & (map::TF_INDOOR | map::TF_SHADE)) != 0;
    if (amb >= tune::kExposureHotThreshold && !sheltered) {
        g.pl.exposure += g.pl.litany_turns > 0 ? tune::kExposureGain / 2
                                               : tune::kExposureGain;
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

    // transient light flashes decay
    for (int i = 0; i < g.flash_count;) {
        if (--g.flashes[i].ttl <= 0)
            g.flashes[i] = g.flashes[--g.flash_count];
        else
            ++i;
    }

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
        if (e.type == EType::Trooper || e.type == EType::ShieldTrooper) {
            if (e.type == EType::ShieldTrooper)
                g.noise.add(e.x, e.y, 8);  // Holtzman fields madden the worm
            e.energy += 100;
            while (e.energy >= 100 && !g.pl.dead) {
                e.energy -= 100;
                trooper_act(e);
            }
            continue;
        }
        if (e.type == EType::PanAdder || e.type == EType::Hermit ||
            e.type == EType::HunterSeeker) {
            e.energy += int16_t(e.type == EType::HunterSeeker ? 150 : 100);
            while (e.energy >= 100 && !g.pl.dead) {
                e.energy -= 100;
                critter_act(e);
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
    thopter_turn();
    season_turn();

    // the worm's turn (also decays the noise field); a ridden worm obeys
    const worm::State before = g.wrm.state;
    if (g.pl.riding) g.noise.decay();
    else worm::turn(g.wrm, g.noise, zone(), g.rng.combat, worm_tuning(), worm_callbacks());
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
        if (g.wrm.state == worm::State::Surfaced) {
            g.services.mixer->push({audio::Cmd::PlaySfx, 0, 255, 110});
            add_flash(g.wrm.x, g.wrm.y, 8, 24, 2);  // breach static-discharge
        }
    }

    // hour changed? re-cast the shadows
    if (hour_now() != g.shade_hour) recompute_shade(g.cur_zone, hour_now());

    // context music: the sietch is home, wormsign is dread, the erg is vast
    {
        int want = 0;  // ambient
        if (g.cur_zone == map::kZoneSietch) want = 1;
        else if (int(g.wrm.state) >= int(worm::State::Hunting) || g.alarm_turns > 0 ||
                 g.storm_turns > 0) want = 2;
        if (want != g.current_song) {
            g.current_song = want;
            g.services.mixer->push({audio::Cmd::PlaySong, uint8_t(want), 0, 0});
        }
    }

    // HUD meter + seismograph history
    const int nn = g.noise.near(player_ent().x, player_ent().y, 3);
    g.noise_meter = uint8_t(nn > 100 ? 100 : nn);
    g.seismo[g.seismo_head] = g.noise_meter;
    g.seismo_head = (g.seismo_head + 1) & 63;

    refresh_fov_and_paths();
}

// ---- worm-riding ---------------------------------------------------------------

constexpr int kMaxRideTurns = 40;

bool worm_rideable_at(int x, int y) {
    if (g.wrm.state != worm::State::Surfaced) return false;
    const int dx0 = x - g.wrm.x, dy0 = y - g.wrm.y;
    if (dx0 * dx0 <= 1 && dy0 * dy0 <= 1) return true;
    for (int i = 0; i < worm::kSegments; ++i) {
        const int dx = x - g.wrm.seg_x[i], dy = y - g.wrm.seg_y[i];
        if (dx * dx <= 1 && dy * dy <= 1) return true;
    }
    return false;
}

void dismount(const char* why) {
    g.pl.riding = false;
    g.pl.ride_turns = 0;
    // land beside the head on the first walkable tile
    Entity& p = player_ent();
    for (int r = 1; r <= 4; ++r)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                const int nx = g.wrm.x + dx, ny = g.wrm.y + dy;
                if (!zone().walkable(nx, ny) || entity_at(nx, ny, g.cur_zone)) continue;
                p.x = int16_t(nx);
                p.y = int16_t(ny);
                g.log.push("%s", why);
                return;
            }
    g.log.push("%s", why);
}

void try_ride() {
    Entity& p = player_ent();
    if (g.pl.riding) {
        dismount("You plant the hooks and slide down the flank.");
        advance_world();
        return;
    }
    if (!g.pl.maker_hooks) {
        g.log.push("You have no maker hooks. The tribe grants them to the Honored.");
        return;
    }
    if (!worm_rideable_at(p.x, p.y)) {
        g.log.push("No surfaced worm in reach of your hooks.");
        return;
    }
    g.pl.riding = true;
    g.pl.ride_turns = 0;
    g.log.push("Hooks bite between the rings. The desert tilts - YOU RIDE.");
    g.services.mixer->push({audio::Cmd::PlaySfx, 0, 255, 90});
    flash(240, 220, 120);
    advance_world();
}

// Steer the ridden worm: the head takes two strides, the body follows, and
// everything on sand beneath it dies. The rider makes no noise of their own.
void ride_move(int dx, int dy) {
    const worm::Callbacks cb = worm_callbacks();
    for (int step = 0; step < 2; ++step) {
        const int nx = g.wrm.x + dx, ny = g.wrm.y + dy;
        if (nx < 1 || nx >= map::W - 1 || ny < 1 || ny >= map::H - 1) break;
        const uint8_t floor = zone().floor[ny * map::W + nx];
        const bool sand = floor <= 2 || floor == 5 || floor == 6;
        if (!sand && !g.op.tile_on(nx, ny)) {
            if (step == 0) g.log.push("The worm shies from the rock.");
            break;
        }
        for (int i = worm::kSegments - 1; i > 0; --i) {
            g.wrm.seg_x[i] = g.wrm.seg_x[i - 1];
            g.wrm.seg_y[i] = g.wrm.seg_y[i - 1];
        }
        g.wrm.seg_x[0] = g.wrm.x;
        g.wrm.seg_y[0] = g.wrm.y;
        g.wrm.x = int16_t(nx);
        g.wrm.y = int16_t(ny);
        // crush everything but the rider
        for (Entity& e : g.ents) {
            if (e.type == EType::None || e.type == EType::Player) continue;
            if (e.zone != g.cur_zone || e.x != nx || e.y != ny) continue;
            if (e.type == EType::Trooper || e.type == EType::ShieldTrooper)
                g.log.push("A trooper vanishes beneath you. A good sound.");
            e.type = EType::None;
        }
        if (g.op.live() && g.op.zone == g.cur_zone && g.op.tile_on(nx, ny)) {
            g.op.state = harvest::OpState::Destroyed;
            op_denied("You drive the Maker THROUGH the crawler.");
        }
        worm_cb_churn(nullptr, nx, ny);
    }
    // the rider goes where the worm goes
    player_ent().x = g.wrm.x;
    player_ent().y = g.wrm.y;
    g.wrm.timer = 12;  // stays surfaced while ridden

    if (++g.pl.ride_turns >= kMaxRideTurns) {
        dismount("The Maker tires and bucks you into the sand. It dives.");
        g.wrm.state = worm::State::Dormant;
        g.noise.clear();
        g.services.mixer->push({audio::Cmd::Rumble, 0, 0, 0});
    }
    advance_world();
    (void)cb;
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
    // crawler footprint re-blocks after regen
    if (g.op.live() && g.op.zone == id)
        for (int dy = -1; dy <= 0; ++dy)
            for (int dx = -1; dx <= 0; ++dx)
                zone().flags[(g.op.y + dy) * map::W + g.op.x + dx] |= map::TF_BLOCK;

    // crossing a region resets the worm; it hunts by local noise
    g.wrm = worm::Worm{};
    g.noise.clear();
    g.services.mixer->push({audio::Cmd::Rumble, 0, 0, 0});
    if (g.op.live() && g.op.zone == id && (g.op.mutators & campaign::MUT_WORMSIGN))
        g.wrm.state = worm::State::Stirring;

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

    // one weighted desert-event roll per entry (outdoors only)
    g.storm_turns = 0;
    if (id != map::kZoneSietch) {
        const int roll = int(g.rng.world.below(100));
        if (roll < 12) {
            g.storm_turns = g.rng.world.range(60, 120);
            g.log.push("The horizon goes brown-black. CORIOLIS STORM.");
            g.log.push("Sight dies, flesh scours - but so does every eye and ear.");
        } else if (roll < 22) {
            // spice blow eruption: riches and a ringing dinner bell
            int bx = 0, by = 0;
            for (int tries = 0; tries < 60; ++tries) {
                const int x = p.x + g.rng.world.range(-16, 16);
                const int y = p.y + g.rng.world.range(-16, 16);
                if (x < 2 || x >= map::W - 2 || y < 2 || y >= map::H - 2) continue;
                const uint8_t fl = zone().floor[y * map::W + x];
                if (!zone().walkable(x, y) || (fl != 0 && fl != 1 && fl != 2)) continue;
                bx = x; by = y;
                break;
            }
            if (bx) {
                g.log.push("The ground BURSTS - a spice blow erupts to the %s!",
                           bx > p.x ? "east" : "west");
                g.noise.add(bx, by, 250);
                add_flash(bx, by, 8, 26, 3);
                const int piles = g.rng.world.range(3, 5);
                for (int i = 0; i < piles; ++i) {
                    const int cx = bx + g.rng.world.range(-2, 2);
                    const int cy = by + g.rng.world.range(-2, 2);
                    if (!zone().walkable(cx, cy) || entity_at(cx, cy, id)) continue;
                    worm_cb_churn(nullptr, cx, cy);
                    if (Entity* e = alloc_entity())
                        *e = Entity{EType::SpiceCache, Ai::Idle, id,
                                    int16_t(cx), int16_t(cy), 1, 0, 2};
                }
            }
        } else if (roll < 28 && g.season.alert >= 60) {
            for (int tries = 0; tries < 40; ++tries) {
                const int x = p.x + g.rng.world.range(-12, 12);
                const int y = p.y + g.rng.world.range(-12, 12);
                if (!zone().walkable(x, y) || entity_at(x, y, id)) continue;
                if (Entity* e = alloc_entity()) {
                    *e = Entity{EType::HunterSeeker, Ai::Hunt, id,
                                int16_t(x), int16_t(y), 1, 0, 0};
                    g.log.push("A metallic whine - HUNTER-SEEKER on the wind!");
                }
                break;
            }
        } else if (roll < 36) {
            g.pl.water += 150;
            if (g.pl.water > tune::kWaterMax) g.pl.water = tune::kWaterMax;
            raise_standing(2);
            g.log.push("A Fremen scout melts from the rocks, shares water, is gone.");
        }
    }

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
        const int dealt = worm::hit(g.wrm, nx, ny, g.pl.atk, worm_callbacks());
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

    if (entity_at(nx, ny, g.cur_zone, EType::Naib)) { g.menu_open = 1; g.menu_sel = 0; return; }
    if (entity_at(nx, ny, g.cur_zone, EType::Sayyadina)) { g.menu_open = 2; g.menu_sel = 0; return; }
    if (entity_at(nx, ny, g.cur_zone, EType::Smuggler)) { g.menu_open = 3; g.menu_sel = 0; return; }

    Entity* victim = entity_at(nx, ny, g.cur_zone, EType::Hound);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::Trooper);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::Spotter);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::ShieldTrooper);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::PanAdder);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::Hermit);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::HunterSeeker);
    if (victim) {
        player_ent().anim.facing = facing_from_tile_delta(dx, dy);
        player_ent().anim.set(anim::State::Attack);
        g.impact_x = int16_t(nx);
        g.impact_y = int16_t(ny);
        if (victim->type == EType::ShieldTrooper && !g.pl.sandwalk) {
            g.log.push("Your quick blade GLANCES off the shield shimmer.");
            g.log.push("The slow blade penetrates. (C: sandwalk, then strike.)");
            g.noise.add(nx, ny, 10);
            advance_world();
            return;
        }
        victim->hp -= g.pl.atk;
        g.noise.add(nx, ny, victim->type == EType::Spotter ? 4 : 12);  // blades are quiet
        if (victim->hp <= 0) {
            switch (victim->type) {
                case EType::Hound:
                    g.log.push("The dust hound collapses. Its water is yours.");
                    if (g.quest_kind == 1 && ++g.quest_progress >= g.quest_need) {
                        g.quest_kind = 0;
                        raise_standing(12);
                        g.log.push("The catch-basins are safe. The Naib hears. +12 standing.");
                    }
                    break;
                case EType::Trooper:
                    g.log.push("The trooper falls. His water belongs to the tribe now.");
                    break;
                case EType::ShieldTrooper:
                    g.log.push("Slow and true - the blade slips the field. He falls.");
                    break;
                case EType::Spotter:
                    g.log.push("The spotter crumples. No eyes on the sand now.");
                    break;
                case EType::PanAdder:
                    g.log.push("You pin the adder behind the head. Still.");
                    break;
                case EType::Hermit:
                    g.log.push("The hermit falls, laughing. His rags spill SPICE.");
                    break;
                case EType::HunterSeeker:
                    g.log.push("You swat the sliver from the air. Fine work.");
                    raise_standing(2);
                    break;
                default: break;
            }
            if (g.season.crysknife_granted &&
                (victim->type == EType::Trooper || victim->type == EType::ShieldTrooper))
                raise_standing(1);  // the crysknife drinks; the tribe sees
            const bool was_spotter = victim->type == EType::Spotter;
            if (victim->type == EType::HunterSeeker) {
                victim->type = EType::None;  // machines leave no water
            } else if (victim->type == EType::Hermit) {
                *victim = Entity{EType::SpiceCache, Ai::Idle, uint8_t(g.cur_zone),
                                 victim->x, victim->y, 1, 0,
                                 int16_t(victim->data > 0 ? victim->data : 6)};
            } else {
                const int16_t yield_ =
                    victim->type == EType::Hound ? int16_t(tune::kCorpseWaterHound)
                    : victim->type == EType::PanAdder ? int16_t(20) : int16_t(140);
                *victim = Entity{EType::Corpse, Ai::Idle, uint8_t(g.cur_zone),
                                 victim->x, victim->y, 0, 0, yield_};
            }
            if (was_spotter && !spotters_alive() && g.op.live())
                g.op.spotters_knifed = true;  // the quiet route
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

    if (g.pl.free_moves > 0) {  // Threadwalk: the world holds its breath
        --g.pl.free_moves;
        refresh_fov_and_paths();
        return;
    }
    advance_world();
    if (g.pl.sandwalk && !g.pl.dead) advance_world();  // slow, broken rhythm
}

void place_glowglobe() {
    Entity& p = player_ent();
    // standing on one? pick it back up
    if (Entity* globe = entity_at(p.x, p.y, g.cur_zone, EType::Glowglobe)) {
        globe->type = EType::None;
        ++g.pl.glowglobes;
        g.log.push("You pocket the glowglobe.");
        advance_world();
        return;
    }
    if (g.pl.glowglobes <= 0) {
        g.log.push("No glowglobes left.");
        return;
    }
    Entity* globe = alloc_entity();
    if (!globe) return;
    p.anim.set(anim::State::Use);
    *globe = Entity{EType::Glowglobe, Ai::Idle, g.cur_zone, p.x, p.y, 1, 0, 0};
    --g.pl.glowglobes;
    g.log.push("The glowglobe rises and burns steady. A pool of light.");
    g.services.mixer->push({audio::Cmd::PlaySfx, 1, 140, 150});
    advance_world();
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
    p.anim.set(anim::State::Use);
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
    player_ent().anim.set(anim::State::Use);
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
        add_flash(g.op.x, g.op.y, 10, 31, 3);  // fireball light
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
        g.pl.spice += (g.op.mutators & campaign::MUT_SLOPPY) ? 8 : 5;
        g.op.stolen = true;
        g.noise.add(p.x, p.y, 8);
        g.log.push("You scoop raw spice from the hold. +5 grains.");
        advance_world();
        return;
    }
    if (Entity* cache = entity_at(p.x, p.y, g.cur_zone, EType::SpiceCache)) {
        p.anim.set(anim::State::Use);
        g.pl.spice += cache->data;
        g.log.push("You scoop %d grains of raw spice from the sand.", int(cache->data));
        cache->type = EType::None;
        g.noise.add(p.x, p.y, 6);
        g.services.mixer->push({audio::Cmd::PlaySfx, 1, 150, 140});
        advance_world();
        return;
    }
    if (Entity* c = entity_at(p.x, p.y, g.cur_zone, EType::Corpse)) {
        p.anim.set(anim::State::Use);
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
    game_init(seed, services);  // interactive runs return to the chooser
}


}  // namespace

void game_init(uint64_t seed, const GameServices& services) {
    g = GameState{};
    g.services = services;
    g.seed = seed;
    g.rng.init(seed);
    meta_load();

    int sx, sy;
    map::generate(g.zones[map::kZoneSietch], seed, map::kZoneSietch, sx, sy);
    g.ents[0] = Entity{EType::Player, Ai::Idle, map::kZoneSietch,
                       int16_t(sx), int16_t(sy), int16_t(tune::kPlayerHpMax), 0, 0};
    g.spawned_mask = 1 << map::kZoneSietch;

    g.choosing = services.interactive;
    if (!g.choosing) apply_background(g.pl, Background::Fedaykin);

    g.log.push("The Naib's word: deny them the spice. Go.");
    g.log.push("Arrows move. G harvest. T thumper. C sandwalk.");
    refresh_fov_and_paths();

    int px, py;
    iso::tile_to_screen(g.ents[0].x, g.ents[0].y, iso::Camera{}, px, py);
    g.cam.x = px - fb::W / 2;
    g.cam.y = py - fb::H / 2;
}

namespace {

uint8_t facing_from_tile_delta(int dx, int dy) {
    const int wx = dx - dy, wy = dx + dy;  // world-pixel direction sign
    if (wx > 0 && wy == 0) return 0;
    if (wx > 0 && wy < 0) return 1;
    if (wx == 0 && wy < 0) return 2;
    if (wx < 0 && wy < 0) return 3;
    if (wx < 0 && wy == 0) return 4;
    if (wx < 0 && wy > 0) return 5;
    if (wx == 0 && wy > 0) return 6;
    return 7;
}

// ---- G1: visual glide + animator ------------------------------------------------

void world_px_of(int tx, int ty, int& wx, int& wy) {
    wx = (tx - ty) * iso::kHalfW;
    wy = (tx + ty) * iso::kHalfH;
}

const anim::Set* anim_set_for(const Entity& e) {
    if (!g.services.anims) return nullptr;
    switch (e.type) {
        case EType::Player:
            return g.services.anims->get(kBackgrounds[int(g.pl.background)].sprite);
        case EType::Hound: return g.services.anims->get("hound");
        case EType::Trooper:
        case EType::ShieldTrooper: return g.services.anims->get("trooper");
        case EType::Hermit: return g.services.anims->get("hermit");
        default: return nullptr;
    }
}

bool glide_done(const Entity& e) {
    int wx, wy;
    world_px_of(e.x, e.y, wx, wy);
    const int dx = wx - e.vpx, dy = wy - e.vpy;
    return dx * dx + dy * dy <= 16;
}

// Per render tick (60Hz): every visible entity glides toward its logical
// tile, walks while gliding, breathes while still. Purely presentational.
void visual_tick() {
    for (Entity& e : g.ents) {
        if (e.type == EType::None || e.zone != g.cur_zone) continue;
        int wx, wy;
        world_px_of(e.x, e.y, wx, wy);
        int dx = wx - e.vpx, dy = wy - e.vpy;
        if (dx * dx + dy * dy > 96 * 96) {  // teleport/spawn: snap
            e.vpx = int16_t(wx);
            e.vpy = int16_t(wy);
            dx = dy = 0;
        }
        const int step = 5;
        const bool moving = dx || dy;
        if (moving) {
            e.vpx = int16_t(e.vpx + (dx > step ? step : dx < -step ? -step : dx));
            e.vpy = int16_t(e.vpy + (dy > step ? step : dy < -step ? -step : dy));
            // 8-way facing from travel direction (E=0, going CCW)
            if (dx > 0 && dy == 0) e.anim.facing = 0;
            else if (dx > 0 && dy < 0) e.anim.facing = 1;
            else if (dx == 0 && dy < 0) e.anim.facing = 2;
            else if (dx < 0 && dy < 0) e.anim.facing = 3;
            else if (dx < 0 && dy == 0) e.anim.facing = 4;
            else if (dx < 0 && dy > 0) e.anim.facing = 5;
            else if (dx == 0 && dy > 0) e.anim.facing = 6;
            else e.anim.facing = 7;
        }
        const anim::Set* set = anim_set_for(e);
        if (!set) continue;
        if (anim::State(e.anim.state) == anim::State::Idle && moving)
            e.anim.set(anim::State::Walk);
        else if (anim::State(e.anim.state) == anim::State::Walk && !moving)
            e.anim.set(anim::State::Idle);
        if (e.hurt_flash > 0) --e.hurt_flash;
        const uint8_t ev = e.anim.tick(set);
        if (ev == anim::EV_FOOTSTEP) {
            // dust at the feet (fx-only)
            const int sx = e.vpx - g.cam.x + fb::W / 2;
            const int sy = e.vpy - g.cam.y + fb::H / 2;
            if (sx >= 0 && sx < fb::W && sy >= 0 && sy < fb::H)
                g.particles.burst_dust(g.rng.fx, sx, sy + 10, 1, 66);
        } else if (ev == anim::EV_IMPACT) {
            g.services.mixer->push({audio::Cmd::PlaySfx, 1, 200, 60});
            if (e.type == EType::Player && g.impact_x >= 0) {
                for (Entity& v : g.ents)
                    if (v.type != EType::None && v.zone == g.cur_zone &&
                        v.x == g.impact_x && v.y == g.impact_y)
                        v.hurt_flash = 8;
                g.impact_x = -1;
            } else if (e.type != EType::Player) {
                player_ent().hurt_flash = 8;  // their blow lands on you
            }
        }
    }
}

const char* describe_at(int tx, int ty) {
    if (tx < 0 || tx >= map::W || ty < 0 || ty >= map::H) return nullptr;
    if (!(zone().vis[ty * map::W + tx] & (map::V_VISIBLE | map::V_REMEMBERED)))
        return nullptr;
    for (const Entity& e : g.ents) {
        if (e.type == EType::None || e.zone != g.cur_zone) continue;
        if (e.x != tx || e.y != ty) continue;
        switch (e.type) {
            case EType::Player: return "you";
            case EType::Hound: return "dust hound";
            case EType::Corpse: return "corpse (G: deathstill)";
            case EType::Thumper: return "thumper, pounding";
            case EType::Trooper: return "Harkonnen trooper";
            case EType::ShieldTrooper: return "SHIELDED trooper (slow blade!)";
            case EType::Spotter: return "Harkonnen spotter";
            case EType::Glowglobe: return "glowglobe (L: retrieve)";
            case EType::Naib: return "the Naib";
            case EType::Sayyadina: return "the Sayyadina";
            case EType::Smuggler: return "Greybeard the smuggler";
            case EType::PanAdder: return "pan adder - give it room";
            case EType::Hermit: return "spice-mad hermit";
            case EType::HunterSeeker: return "HUNTER-SEEKER";
            case EType::SpiceCache: return "raw spice (G: gather)";
            default: break;
        }
    }
    if (g.op.live() && g.op.zone == g.cur_zone && g.op.tile_on(tx, ty))
        return "harvester crawler";
    if (worm_occupies(tx, ty)) return "SHAI-HULUD";
    const uint8_t fl = zone().floor[ty * map::W + tx];
    const uint8_t tf = zone().at(tx, ty);
    if (tf & map::TF_DOOR) return "passage";
    if (tf & map::TF_CISTERN) return "the cistern";
    if (tf & map::TF_ROCKPROP) return "rock outcrop (worm-safe)";
    if (tf & map::TF_WALL) return "carved stone";
    switch (fl) {
        case 3: case 4: return "rock ground (worm-safe)";
        case 5: return "spice-veined sand";
        case 6: return "drum sand - LOUD underfoot";
        case 8: return "sietch floor";
        default: return "open sand";
    }
}

}  // namespace

bool game_update(const InputState& in) {
    ++g.anim_tick;
    if (!g.choosing) visual_tick();
    g.hover_tx = -1;
    if (in.mouse_x >= 0 && !g.choosing && !g.menu_open) {
        int tx, ty;
        iso::screen_to_tile(in.mouse_x, in.mouse_y, g.cam, tx, ty);
        g.hover_tx = int16_t(tx);
        g.hover_ty = int16_t(ty);
    }
    if (g.fade > 0) g.fade -= 6;

    if (g.choosing) {
        const int n = int(Background::Count);
        if (in.was_pressed(Key::Left)) g.choose_idx = (g.choose_idx + n - 1) % n;
        if (in.was_pressed(Key::Right)) g.choose_idx = (g.choose_idx + 1) % n;
        if (in.was_pressed(Key::Confirm)) {
            apply_background(g.pl, Background(g.choose_idx));
            g.choosing = false;
            g.log.push("You are %s. The Tanzerouft waits.", kBackgrounds[g.choose_idx].name);
            flash(10, 10, 10);
        }
        return !in.was_pressed(Key::Cancel);
    }

    if (g.pl.dead || g.season.ending != campaign::kNone) {
        if (in.was_pressed(Key::Confirm)) restart();
        return !in.was_pressed(Key::Cancel);
    }

    // vendor menus pause the world (browsing is free)
    if (g.menu_open) {
        const int counts[5] = {0, 4, 3, 5, 4};  // options incl. Leave
        const int n = counts[g.menu_open];
        if (in.was_pressed(Key::Up)) g.menu_sel = (g.menu_sel + n - 1) % n;
        if (in.was_pressed(Key::Down)) g.menu_sel = (g.menu_sel + 1) % n;
        if (in.was_pressed(Key::Cancel)) g.menu_open = 0;
        if (in.was_pressed(Key::Confirm)) {
            switch (g.menu_open * 10 + g.menu_sel) {
                case 10:  // Naib: tribute water
                    if (g.pl.water >= 300) {
                        g.pl.water -= 200;
                        raise_standing(10);
                        g.log.push("'The tribe drinks. The tribe remembers.' +10 standing.");
                    } else {
                        g.log.push("'You need it more than we do, child.'");
                    }
                    break;
                case 11:  // Naib: the season
                    g.log.push("'%d loads shipped. %d denied. Alert %d.'",
                               int(g.season.shipped), int(g.season.denied),
                               int(g.season.alert));
                    g.log.push(g.season.climax_pending
                                   ? "'Their great harvest comes. Be ready.'"
                                   : "'Bleed them slowly. Water discipline.'");
                    break;
                case 12:  // Naib: a task for the tribe
                    if (g.quest_kind != 0) {
                        g.log.push("'Finish what you carry first.'");
                    } else {
                        const int pick = int(g.rng.world.below(3));
                        g.quest_kind = pick + 1;
                        g.quest_progress = 0;
                        if (g.quest_kind == 1) {
                            g.quest_need = 3;
                            g.log.push("'Dust hounds raid our catch-basins. Kill three.'");
                        } else if (g.quest_kind == 2) {
                            g.quest_need = 8;
                            g.quest_progress = -g.pl.spice;  // count what you gather anew
                            g.log.push("'Bring the Sayyadina eight grains of spice.'");
                        } else {
                            g.quest_need = 1;
                            g.log.push("'Deny the next harvest QUIETLY. No spectacle.'");
                        }
                    }
                    break;
                case 13: g.menu_open = 0; break;  // leave
                case 20:  // Sayyadina: spice tribute
                    if (g.pl.spice >= 5) {
                        g.pl.spice -= 5;
                        raise_standing(g.pl.eyes_ibad ? 15 : 12);
                        g.log.push("Blue eyes hold yours. 'The tribe sees you.' +12 standing.");
                    } else {
                        g.log.push("'Bring spice when you have it to give.'");
                    }
                    break;
                case 21:  // Sayyadina: healing trance
                    if (g.pl.spice >= 6 && g.pl.hp < tune::kPlayerHpMax) {
                        g.pl.spice -= 6;
                        g.pl.hp = tune::kPlayerHpMax;
                        g.pl.exposure = 0;
                        g.log.push("She sings the water back into your flesh.");
                    } else {
                        g.log.push(g.pl.spice < 6 ? "'Six grains for the trance.'"
                                                  : "'You are whole already.'");
                    }
                    break;
                case 22: g.menu_open = 0; break;
                case 30:  // Smuggler stock
                    if (g.pl.spice >= 5) { g.pl.spice -= 5; ++g.pl.charges;
                        g.log.push("A demolition charge, no questions. -5 spice."); }
                    else g.log.push("'Five grains. Boom is not cheap.'");
                    break;
                case 31:
                    if (g.pl.spice >= 3) { g.pl.spice -= 3; g.pl.thumpers += 2;
                        g.log.push("Two thumpers off the truck. -3 spice."); }
                    else g.log.push("'Three grains for the pair.'");
                    break;
                case 32:
                    if (g.pl.spice >= 2) { g.pl.spice -= 2; ++g.pl.poisons;
                        g.log.push("A vial of refinery rot. -2 spice."); }
                    else g.log.push("'Two grains. Nasty stuff.'");
                    break;
                case 33:
                    if (g.pl.spice >= 3) { g.pl.spice -= 3; ++g.pl.glowglobes;
                        g.log.push("A glowglobe, barely used. -3 spice."); }
                    else g.log.push("'Three grains and it's yours.'");
                    break;
                case 34: g.menu_open = 0; break;
                case 40:  // Farsight: the spice unfolds the map
                    if (g.pl.spice >= 2) {
                        g.pl.spice -= 2;
                        g.pl.tox += 10;
                        g.pl.tox_lifetime += 10;
                        for (int i = 0; i < map::N; ++i)
                            zone().vis[i] |= map::V_REMEMBERED;
                        g.log.push("The spice unfolds. You see the whole quarter at once.");
                        flash(120, 80, 200);
                        g.menu_open = 0;
                    } else g.log.push("Two grains for the long sight.");
                    break;
                case 41:  // Threadwalk: two free strides outside time
                    if (g.pl.spice >= 3) {
                        g.pl.spice -= 3;
                        g.pl.tox += 15;
                        g.pl.tox_lifetime += 15;
                        g.pl.free_moves = 2;
                        g.log.push("Time goes thick as syrup. You move BETWEEN moments.");
                        flash(150, 100, 220);
                        g.menu_open = 0;
                    } else g.log.push("Three grains to walk the thread.");
                    break;
                case 42: {  // the Litany against fear
                    const int day = g.pl.turn / tune::kTurnsPerDay;
                    if (g.pl.litany_day == day) {
                        g.log.push("The words are spent until another sun.");
                    } else {
                        g.pl.litany_day = day;
                        g.pl.litany_turns = 20;
                        g.log.push("I must not fear. Fear is the mind-killer...");
                        g.log.push("Stillness settles over your body's complaints.");
                        g.menu_open = 0;
                    }
                    break;
                }
                case 43: g.menu_open = 0; break;
            }
        }
        return true;
    }

    // a fresh press moves at once; a held key re-steps as the glide lands
    const bool can_continue = glide_done(player_ent());
    auto move_input = [&](Key k) {
        return in.was_pressed(k) ||
               (in.is_down(k) && can_continue && in.held_ticks[int(k)] >= 6);
    };
    int dx = 0, dy = 0;
    if (move_input(Key::Up)) { dy = -1; }
    else if (move_input(Key::Down)) { dy = 1; }
    else if (move_input(Key::Left)) { dx = -1; }
    else if (move_input(Key::Right)) { dx = 1; }
    else if (move_input(Key::UpLeft)) { dx = -1; dy = -1; }
    else if (move_input(Key::UpRight)) { dx = 1; dy = -1; }
    else if (move_input(Key::DownLeft)) { dx = -1; dy = 1; }
    else if (move_input(Key::DownRight)) { dx = 1; dy = 1; }

    if (dx || dy) {
        if (g.pl.riding) ride_move(dx, dy);
        else try_move(dx, dy);
    }
    else if (in.pressed_or_repeat(Key::Wait)) advance_world();
    else if (in.was_pressed(Key::Ride)) try_ride();
    else if (in.was_pressed(Key::Powers)) { g.menu_open = 4; g.menu_sel = 0; }
    else if (in.was_pressed(Key::Harvest)) try_harvest();
    else if (in.was_pressed(Key::Thumper)) place_thumper();
    else if (in.was_pressed(Key::Glowglobe)) place_glowglobe();
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

    // camera follows the player's gliding visual position
    const int tx = player_ent().vpx;
    const int ty = player_ent().vpy + fb::H / 2 - (fb::H - ui::kPanelH) / 2;
    g.cam.x += (tx - g.cam.x) / 6;
    g.cam.y += (ty - g.cam.y) / 6;

    return !in.was_pressed(Key::Cancel);
}

// ---- render passes (engine/passes.h documents the order) ----------------------

namespace {

struct RenderCtx {
    DrawCtx ctx;
    const map::Zone* z;
    bool night;
    int base_light;    // full-bright level for the time of day
    const Assets* assets;
};

int tile_light(const RenderCtx& r, int idx) {
    const uint8_t vis = r.z->vis[idx];
    if (!(vis & map::V_VISIBLE)) return light::kRemembered;
    return g.lightmap[idx];
}

int light_at(int x, int y) { return g.lightmap[y * map::W + x]; }

// Ground: floor diamonds, immediate (no overlap between floors).
void pass_ground(RenderCtx& r) {
    const SpriteSheet* tiles = r.assets->get("tiles");
    for (int ty = 0; ty < map::H; ++ty)
        for (int tx = 0; tx < map::W; ++tx) {
            const int idx = ty * map::W + tx;
            if (!(r.z->vis[idx] & (map::V_VISIBLE | map::V_REMEMBERED))) continue;
            int sx, sy;
            iso::tile_to_screen(tx, ty, g.cam, sx, sy);
            if (sx < -64 || sx > fb::W + 64 || sy < -96 || sy > fb::H + 96) continue;
            draw_sprite(r.ctx, *tiles, r.z->floor[idx], sx, sy, tile_light(r, idx));
        }
}

// GroundDecals: flat-on-sand overlays — wormsign ripples (hunt telegraph).
void pass_ground_decals(RenderCtx& r) {
    if (g.wrm.state != worm::State::Hunting) return;
    const SpriteSheet* ripple = r.assets->get("ripple");
    int sx, sy;
    for (int i = 0; i < worm::kSegments; i += 2) {
        iso::tile_to_screen(g.wrm.seg_x[i], g.wrm.seg_y[i], g.cam, sx, sy);
        draw_sprite(r.ctx, *ripple, (g.anim_tick / 6 + i) % 2, sx, sy,
                    light_at(g.wrm.seg_x[i], g.wrm.seg_y[i]));
    }
    iso::tile_to_screen(g.wrm.x, g.wrm.y, g.cam, sx, sy);
    draw_sprite(r.ctx, *ripple, (g.anim_tick / 4) % 2, sx, sy,
                light_at(g.wrm.x, g.wrm.y));
}

// Walls: architecture, into the shared y-sorted scene list.
void pass_walls(RenderCtx& r) {
    const SpriteSheet* wall = r.assets->get("wall");
    for (int ty = 0; ty < map::H; ++ty)
        for (int tx = 0; tx < map::W; ++tx) {
            const int idx = ty * map::W + tx;
            if (!(r.z->flags[idx] & map::TF_WALL)) continue;
            if (!(r.z->vis[idx] & (map::V_VISIBLE | map::V_REMEMBERED))) continue;
            int sx, sy;
            iso::tile_to_screen(tx, ty, g.cam, sx, sy);
            if (sx < -64 || sx > fb::W + 64 || sy < -96 || sy > fb::H + 96) continue;
            g.draw_list.add(wall, 0, sx, sy + 16, tile_light(r, idx), 0);
        }
}

// Objects: props and machinery, into the scene list.
void pass_objects(RenderCtx& r) {
    const SpriteSheet* rock = r.assets->get("rock");
    const SpriteSheet* cistern = r.assets->get("cistern");
    for (int ty = 0; ty < map::H; ++ty)
        for (int tx = 0; tx < map::W; ++tx) {
            const int idx = ty * map::W + tx;
            const uint8_t tf = r.z->flags[idx];
            if (!(tf & (map::TF_ROCKPROP | map::TF_CISTERN))) continue;
            if (!(r.z->vis[idx] & (map::V_VISIBLE | map::V_REMEMBERED))) continue;
            int sx, sy;
            iso::tile_to_screen(tx, ty, g.cam, sx, sy);
            if (sx < -64 || sx > fb::W + 64 || sy < -96 || sy > fb::H + 96) continue;
            const int light = tile_light(r, idx);
            if (tf & map::TF_ROCKPROP) g.draw_list.add(rock, 0, sx, sy + 12, light, 0);
            if (tf & map::TF_CISTERN) g.draw_list.add(cistern, 0, sx, sy + 8, light, 0);
        }

    // crawler (remembered like architecture) + breach mound
    if (g.op.live() && g.op.zone == g.cur_zone) {
        const int idx = g.op.y * map::W + g.op.x;
        if (r.z->vis[idx] & (map::V_VISIBLE | map::V_REMEMBERED)) {
            int sx, sy;
            iso::tile_to_screen(g.op.x, g.op.y, g.cam, sx, sy);
            const int light = (r.z->vis[idx] & map::V_VISIBLE)
                                  ? g.lightmap[idx] : light::kRemembered;
            const bool churning = g.op.state == harvest::OpState::Working;
            g.draw_list.add(r.assets->get("crawler"), churning ? (g.anim_tick / 6) % 2 : 0,
                            sx - 32, sy + 8, light, 1);
        }
    }
    if (g.wrm.state == worm::State::Breaching) {
        int sx, sy;
        iso::tile_to_screen(g.wrm.x, g.wrm.y, g.cam, sx, sy);
        g.draw_list.add(r.assets->get("mound"), 2 - (g.wrm.timer > 2 ? 2 : g.wrm.timer),
                        sx, sy + 16, light_at(g.wrm.x, g.wrm.y), 1);
    }
}

// Entities: actors (and the surfaced worm), into the scene list.
void pass_entities(RenderCtx& r) {
    const SpriteSheet* player_spr =
        r.assets->get(kBackgrounds[int(g.pl.background)].sprite);
    if (!player_spr) player_spr = r.assets->get("player");
    const SpriteSheet* hound_spr = r.assets->get("hound");
    const SpriteSheet* corpse_spr = r.assets->get("corpse");
    const SpriteSheet* thumper_spr = r.assets->get("thumper");
    const SpriteSheet* trooper_spr = r.assets->get("trooper");
    const SpriteSheet* spotter_spr = r.assets->get("spotter");

    for (const Entity& e : g.ents) {
        if (e.type == EType::None || e.zone != g.cur_zone) continue;
        const int idx = e.y * map::W + e.x;
        if (!(r.z->vis[idx] & map::V_VISIBLE)) continue;

        // animated actors draw from their gliding visual position
        if (const anim::Set* set = anim_set_for(e)) {
            int frame = 0;
            const SpriteSheet* sheet = e.anim.current(set, &frame);
            if (sheet) {
                int sx = e.vpx - g.cam.x + fb::W / 2;
                const int sy = e.vpy - g.cam.y + fb::H / 2;
                int view_;
                bool flip;
                anim::facing_to_view5(e.anim.facing, view_, flip);
                int light2 = g.lightmap[idx];
                if (e.hurt_flash > 0) {
                    light2 = 31;
                    sx += int(g.rng.fx.below(3)) - 1;
                }
                const bool riding = e.type == EType::Player && g.pl.riding;
                g.draw_list.add(sheet, frame, sx, sy + (riding ? -20 : 12),
                                light2, riding ? 3 : 1, flip);
                if (e.type == EType::ShieldTrooper && g.anim_tick % 5 == 0)
                    g.particles.spawn(sx - 10 + int(g.rng.fx.below(21)),
                                      sy - 20 + int(g.rng.fx.below(25)),
                                      0, -20, 10, 182);
                continue;
            }
        }

        int sx, sy;
        iso::tile_to_screen(e.x, e.y, g.cam, sx, sy);
        const int light = g.lightmap[idx];
        switch (e.type) {
            case EType::Player:
                g.draw_list.add(player_spr, (g.anim_tick / 9) % 4, sx,
                                sy + (g.pl.riding ? -20 : 12), light,
                                g.pl.riding ? 3 : 1);
                break;
            case EType::Hound:
                g.draw_list.add(hound_spr, (g.anim_tick / 7) % 2, sx, sy + 12, light, 1);
                break;
            case EType::Corpse:
                g.draw_list.add(corpse_spr, 0, sx, sy + 12, light, 0);
                break;
            case EType::Thumper:
                g.draw_list.add(thumper_spr, (g.anim_tick / 8) % 2, sx, sy + 8, light, 1);
                break;
            case EType::Trooper:
                g.draw_list.add(trooper_spr, (g.anim_tick / 8) % 2, sx, sy + 12, light, 1);
                break;
            case EType::Spotter:
                g.draw_list.add(spotter_spr, (g.anim_tick / 14) % 2, sx, sy + 12, light, 1);
                break;
            case EType::Glowglobe: {
                const int bob = (g.anim_tick / 16) % 2;
                g.draw_list.add(r.assets->get("glowglobe"), 0, sx, sy + 4 - bob, 31, 1);
                break;
            }
            case EType::ShieldTrooper:
                g.draw_list.add(trooper_spr, (g.anim_tick / 8) % 2, sx, sy + 12, light, 1);
                if (g.anim_tick % 5 == 0)  // Holtzman shimmer
                    g.particles.spawn(sx - 10 + int(g.rng.fx.below(21)),
                                      sy - 20 + int(g.rng.fx.below(25)),
                                      0, -20, 10, 182);
                break;
            case EType::Naib:
                g.draw_list.add(r.assets->get("naib"), 0, sx, sy + 12, light, 1);
                break;
            case EType::Sayyadina:
                g.draw_list.add(r.assets->get("player_sayyadina"),
                                (g.anim_tick / 20) % 4, sx, sy + 12, light, 1);
                break;
            case EType::Smuggler:
                g.draw_list.add(r.assets->get("smuggler"), 0, sx, sy + 12, light, 1);
                break;
            case EType::PanAdder:
                g.draw_list.add(r.assets->get("panadder"), (g.anim_tick / 12) % 2, sx,
                                sy + 12, light, 1);
                break;
            case EType::Hermit:
                g.draw_list.add(r.assets->get("hermit"), (g.anim_tick / 10) % 2, sx,
                                sy + 12, light, 1);
                break;
            case EType::HunterSeeker: {
                const int bob = (g.anim_tick / 6) % 3 - 1;
                g.draw_list.add(r.assets->get("hunterseeker"), (g.anim_tick / 4) % 2, sx,
                                sy + 2 + bob, 31, 2);
                break;
            }
            case EType::SpiceCache:
                g.draw_list.add(r.assets->get("spicecache"), 0, sx, sy + 12, light, 0);
                break;
            default: break;
        }
    }

    if (g.wrm.state == worm::State::Surfaced) {
        const SpriteSheet* seg = r.assets->get("wormseg");
        int sx, sy;
        for (int i = worm::kSegments - 1; i >= 0; --i) {
            iso::tile_to_screen(g.wrm.seg_x[i], g.wrm.seg_y[i], g.cam, sx, sy);
            g.draw_list.add(seg, 0, sx, sy + 20,
                            light_at(g.wrm.seg_x[i], g.wrm.seg_y[i]), 1);
        }
        iso::tile_to_screen(g.wrm.x, g.wrm.y, g.cam, sx, sy);
        g.draw_list.add(r.assets->get("wormhead"), (g.anim_tick / 10) % 2, sx, sy + 24,
                        light_at(g.wrm.x, g.wrm.y), 2);
    }
}

// Effects: above-scene sprites — the looming carryall shadow.
void pass_effects(RenderCtx& r) {
    if (g.op.state == harvest::OpState::Evac && g.op.zone == g.cur_zone) {
        const int idx = g.op.y * map::W + g.op.x;
        if (r.z->vis[idx] & map::V_VISIBLE) {
            int sx, sy;
            iso::tile_to_screen(g.op.x, g.op.y, g.cam, sx, sy);
            const int bob = (g.anim_tick / 12) % 3 - 1;
            draw_sprite(r.ctx, *r.assets->get("carryall"), 0, sx - 20, sy - 80 + bob,
                        light_at(g.op.x, g.op.y));
        }
    }
}

// Particles: dust from the worm's violence, crawler intake haze.
void pass_particles(RenderCtx& r) {
    if (g.thopter_x >= 0) {
        int sx, sy;
        iso::tile_to_screen(g.thopter_x, g.thopter_y, g.cam, sx, sy);
        if (sx > -60 && sx < fb::W + 60 && sy > -60 && sy < fb::H + 60)
            draw_sprite(r.ctx, *r.assets->get("carryall"), 0, sx, sy - 60, 31);
    }
    int sx, sy;
    if (g.wrm.state == worm::State::Breaching) {
        iso::tile_to_screen(g.wrm.x, g.wrm.y, g.cam, sx, sy);
        if (g.anim_tick % 3 == 0) g.particles.burst_dust(g.rng.fx, sx, sy + 8, 3, 68);
    }
    if (g.wrm.state == worm::State::Surfaced && g.anim_tick % 4 == 0) {
        iso::tile_to_screen(g.wrm.x, g.wrm.y, g.cam, sx, sy);
        g.particles.burst_dust(g.rng.fx, sx, sy - 10, 2, 70);
    }
    if (g.op.state == harvest::OpState::Working && g.op.zone == g.cur_zone &&
        g.anim_tick % 10 == 0) {
        const int idx = g.op.y * map::W + g.op.x;
        if (r.z->vis[idx] & map::V_VISIBLE) {
            iso::tile_to_screen(g.op.x, g.op.y, g.cam, sx, sy);
            g.particles.burst_dust(g.rng.fx, sx + 20, sy, 1, 140);
        }
    }
    g.particles.step_and_draw(r.ctx);
}

// Lighting: M7 — per-tile light-source composition remaps this framebuffer.
void pass_lighting(RenderCtx&) {}

// Weather: the Coriolis storm — driven sand streaks + dithered haze.
void pass_weather(RenderCtx& r) {
    if (g.storm_turns <= 0) return;
    // horizontal sand streaks (fx particles, heavy)
    for (int i = 0; i < 6; ++i)
        g.particles.spawn(fb::W - 1, int(g.rng.fx.below(fb::H)),
                          -(600 + int(g.rng.fx.below(500))),
                          int(g.rng.fx.below(80)) - 40,
                          40 + int(g.rng.fx.below(30)), uint8_t(64 + g.rng.fx.below(16)));
    // dithered haze veil
    const int phase = g.anim_tick / 2;
    for (int y = 0; y < fb::H; ++y) {
        uint8_t* row = r.ctx.frame->px + y * fb::W;
        for (int x = (y + phase) & 3; x < fb::W; x += 4)
            row[x] = uint8_t(60 + ((x + y) % 5));
    }
}

// UI: HUD panel, banners, death screen.
void pass_ui(RenderCtx& r) {
    DrawCtx& ctx = r.ctx;
    const map::Zone& z = *r.z;

    if (g.choosing) {
        fill_rect(ctx, 0, 0, fb::W, fb::H, 1);
        draw_text_shadow(ctx, "D U S T F A L L", fb::W / 2 - 64, 60, 230, 1);
        draw_text_shadow(ctx, "Choose your path", fb::W / 2 - 64, 84, 24, 1);

        const int n = int(Background::Count);
        for (int i = 0; i < n; ++i) {
            const int cx = fb::W / 2 + (i - 1) * 170;
            const bool sel = i == g.choose_idx;
            if (const SpriteSheet* spr = r.assets->get(kBackgrounds[i].sprite)) {
                if (sel)
                    fill_rect(ctx, cx - 40, 130, 80, 100, 3);
                draw_sprite(ctx, *spr, (g.anim_tick / 9) % spr->frames, cx, 220,
                            sel ? 31 : 24);
            }
            const int name_x = cx - int(std::strlen(kBackgrounds[i].name)) * 4;
            draw_text_shadow(ctx, kBackgrounds[i].name, name_x, 240,
                             sel ? 245 : 12, 1);
        }
        const BackgroundDef& b = kBackgrounds[g.choose_idx];
        draw_text_shadow(ctx, b.blurb1, fb::W / 2 - int(std::strlen(b.blurb1)) * 4, 290, 28, 1);
        draw_text_shadow(ctx, b.blurb2, fb::W / 2 - int(std::strlen(b.blurb2)) * 4, 304, 230, 1);
        draw_text_shadow(ctx, "left/right choose   ENTER begin", fb::W / 2 - 124, 360, 14, 1);
        if (g_meta.runs > 0) {
            char mbuf[64];
            std::snprintf(mbuf, sizeof mbuf, "seasons ended %u   endings seen:%s%s%s",
                          unsigned(g_meta.runs),
                          (g_meta.endings_seen & (1 << campaign::kVictory)) ? " VICTORY" : "",
                          (g_meta.endings_seen & (1 << campaign::kQuotaLoss)) ? " QUOTA" : "",
                          (g_meta.endings_seen & (1 << campaign::kRaidLoss)) ? " RAID" : "");
            draw_text(ctx, mbuf, fb::W / 2 - int(std::strlen(mbuf)) * 4, 420, 12);
        }
        return;
    }

    ui::HudState hud{g.pl.hp, tune::kPlayerHpMax, g.pl.water, tune::kWaterMax,
                     g.pl.exposure, hour_now(), ambient_now(),
                     g.pl.turn / tune::kTurnsPerDay + 1,
                     (z.at(player_ent().x, player_ent().y) & map::TF_INDOOR) != 0,
                     g.noise_meter, uint8_t(g.wrm.state), g.seismo, g.seismo_head,
                     g.pl.thumpers, g.pl.sandwalk};
    ui::draw_hud(ctx, hud, g.log, g.anim_tick, *r.assets);

    // Harvest op status line (top of screen while an op is live in this zone).
    if (g.op.zone == g.cur_zone && g.op.state != harvest::OpState::None) {
        if (const SpriteSheet* plaque = r.assets->get("ui_plaque"))
            draw_sprite(ctx, *plaque, 0, 2, 1, 31);
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
        draw_text_shadow(ctx, buf, 12, 9,
                         g.op.state == harvest::OpState::Destroyed ? 245 :
                         g.op.state == harvest::OpState::Evac ? 210 : 230, 1);
        if (g.alarm_turns > 0) {
            std::snprintf(buf, sizeof buf, "ALARM %d", g.alarm_turns);
            draw_text_shadow(ctx, buf, 12, 28, 208, 1);
        }
    }

    // hover description + personal gauges, just above the panel
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "spice %d  tox %d%s%s", g.pl.spice, g.pl.tox,
                      g.pl.eyes_ibad ? "  [EYES OF IBAD]" : "",
                      g.pl.riding ? "  [RIDING]" : "");
        draw_text_shadow(ctx, buf, 4, ui::kPanelY - 12, uint8_t(g.pl.tox > 60 ? 208 : 136), 1);
        if (g.hover_tx >= 0)
            if (const char* what = describe_at(g.hover_tx, g.hover_ty))
                draw_text_shadow(ctx, what, fb::W / 2 - int(std::strlen(what)) * 4,
                                 ui::kPanelY - 12, 24, 1);
        if (g.quest_kind != 0) {
            std::snprintf(buf, sizeof buf, "TASK: %s (%d/%d)",
                          g.quest_kind == 1 ? "cull the hounds"
                          : g.quest_kind == 2 ? "gather spice" : "quiet denial",
                          g.quest_kind == 2 ? g.quest_progress + g.pl.spice
                                            : g.quest_progress,
                          g.quest_need);
            draw_text_shadow(ctx, buf, fb::W - 8 - int(std::strlen(buf)) * 8,
                             ui::kPanelY - 12, 240, 1);
        }
    }

    if (g.services.debug_anim) {
        const Entity& p = player_ent();
        int wx, wy;
        world_px_of(p.x, p.y, wx, wy);
        char dbuf[64];
        std::snprintf(dbuf, sizeof dbuf, "anim s=%d f=%d face=%d glide=%d,%d",
                      int(p.anim.state), int(p.anim.frame), int(p.anim.facing),
                      wx - p.vpx, wy - p.vpy);
        draw_text_shadow(ctx, dbuf, 4, 34, 31, 1);
    }

    // CHOAM ledger + season gauges, top-right.
    {
        char buf[48];
        std::snprintf(buf, sizeof buf, "SHIPPED %d/%d  DENIED %d", int(g.season.shipped),
                      campaign::kQuotaLossAt, int(g.season.denied));
        draw_text_shadow(ctx, buf, fb::W - 8 - int(std::strlen(buf)) * 8, 4, 224, 1);
        std::snprintf(buf, sizeof buf, "ALERT %d  STANDING %d", int(g.season.alert),
                      int(g.season.standing));
        draw_text_shadow(ctx, buf, fb::W - 8 - int(std::strlen(buf)) * 8, 14,
                         uint8_t(g.season.alert >= campaign::kAlertDiscoveryAt ? 208 : 14), 1);
        if (g.season.alert >= campaign::kAlertDiscoveryAt) {
            std::snprintf(buf, sizeof buf, "DISCOVERY %d%%",
                          g.season.discovery * 100 / campaign::kDiscoveryRaidAt);
            draw_text_shadow(ctx, buf, fb::W - 8 - int(std::strlen(buf)) * 8, 24, 208, 1);
        }
    }

    // Vendor menu overlay.
    if (g.menu_open) {
        static const char* kMenus[5][5] = {
            {},
            {"Tribute 200 water  (+10 standing)", "Ask about the season",
             "A task for the tribe", "Leave"},
            {"Offer 5 spice  (+12 standing)", "Healing trance  (6 spice)", "Leave", nullptr},
            {"Demolition charge  (5 spice)", "Two thumpers  (3 spice)",
             "Toxin vial  (2 spice)", "Glowglobe  (3 spice)", "Leave"},
            {"FARSIGHT - unfold the map  (2 spice)", "THREADWALK - two free strides  (3 spice)",
             "The Litany against fear  (free, once a day)", "Close"},
        };
        static const char* kTitles[5] = {"", "THE NAIB", "THE SAYYADINA",
                                         "GREYBEARD THE SMUGGLER", "PRESCIENCE"};
        const int x0 = fb::W / 2 - 150, y0 = 140;
        fill_rect(ctx, x0, y0, 300, 110, 2);
        fill_rect(ctx, x0, y0, 300, 1, 228);
        fill_rect(ctx, x0, y0 + 109, 300, 1, 228);
        draw_text_shadow(ctx, kTitles[g.menu_open], x0 + 12, y0 + 8, 230, 1);
        char res[48];
        std::snprintf(res, sizeof res, "water %d  spice %d", g.pl.water, g.pl.spice);
        draw_text(ctx, res, x0 + 12, y0 + 20, 14);
        for (int i = 0; i < 5 && kMenus[g.menu_open][i]; ++i) {
            const bool sel = i == g.menu_sel;
            if (sel) fill_rect(ctx, x0 + 8, y0 + 34 + i * 14 - 2, 284, 12, 4);
            draw_text(ctx, kMenus[g.menu_open][i], x0 + 16, y0 + 34 + i * 14,
                      uint8_t(sel ? 245 : 20));
        }
    }

    // Season endings + death.
    if (g.season.ending != campaign::kNone) {
        static const char* kTitle[4] = {"", "THE SEASON IS BROKEN", "THE QUOTA SHIPS",
                                        "THEY FOUND THE SIETCH"};
        static const char* kLine1[4] = {
            "",
            "The Baron ships nothing. The Guild asks what he cannot pay.",
            "Eight loads to orbit. The Baron toasts his ledger.",
            "Thopters at dawn. Fire in the cave mouths.",
        };
        static const char* kLine2[4] = {
            "", "The desert remembers its own.",
            "The desert waits for another.", "No one is left to carry water.",
        };
        const int e = g.season.ending;
        fill_rect(ctx, fb::W / 2 - 240, 150, 480, 90, 2);
        draw_text_shadow(ctx, kTitle[e], fb::W / 2 - int(std::strlen(kTitle[e])) * 4, 162,
                         uint8_t(e == campaign::kVictory ? 245 : 210), 1);
        draw_text_shadow(ctx, kLine1[e], fb::W / 2 - int(std::strlen(kLine1[e])) * 4, 182, 24, 1);
        draw_text_shadow(ctx, kLine2[e], fb::W / 2 - int(std::strlen(kLine2[e])) * 4, 196, 14, 1);
        draw_text_shadow(ctx, "ENTER: another season", fb::W / 2 - 84, 216, 230, 1);
    } else if (g.pl.dead) {
        fill_rect(ctx, fb::W / 2 - 150, 120, 300, 74, 2);
        draw_text_shadow(ctx, "You are one with the sand now.", fb::W / 2 - 120, 132, 210, 1);
        draw_text_shadow(ctx, "Fear is the mind-killer.", fb::W / 2 - 96, 148, 24, 1);
        char buf[48];
        std::snprintf(buf, sizeof buf, "seed %llu", (unsigned long long)g.seed);
        draw_text_shadow(ctx, buf, fb::W / 2 - 60, 164, 14, 1);
        draw_text_shadow(ctx, "ENTER: rise again", fb::W / 2 - 68, 178, 230, 1);
    }
}

}  // namespace

void game_render(Framebuffer& frame) {
    frame.clear(1);

    RenderCtx r{DrawCtx{&frame, g.services.cmaps}, &zone(), is_night(),
                is_night() ? 18 : 31, g.services.assets};

    // screen shake (visual only; fx stream never touches gameplay)
    const iso::Camera saved_cam = g.cam;
    int shake = 0;
    switch (g.wrm.state) {
        case worm::State::Hunting: shake = 1; break;
        case worm::State::Breaching: shake = 3; break;
        case worm::State::Surfaced: shake = 2; break;
        default: break;
    }
    if (shake > 0) {
        g.cam.x += int(g.rng.fx.below(uint32_t(shake * 2 + 1))) - shake;
        g.cam.y += int(g.rng.fx.below(uint32_t(shake * 2 + 1))) - shake;
    }

    pass_ground(r);
    pass_ground_decals(r);
    pass_walls(r);      // \
    pass_objects(r);    //  > populate the y-sorted scene list
    pass_entities(r);   // /
    g.draw_list.flush(r.ctx);
    pass_effects(r);
    pass_particles(r);
    pass_lighting(r);
    pass_weather(r);

    g.cam = saved_cam;
    pass_ui(r);
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
    w.i32(g.pl.glowglobes);
    w.i32(g.pl.atk);
    w.u8(g.pl.maker_hooks ? 1 : 0);
    w.u8(g.pl.riding ? 1 : 0);
    w.i32(g.pl.ride_turns);
    w.i32(g.pl.tox);
    w.i32(g.pl.tox_lifetime);
    w.u8(g.pl.eyes_ibad ? 1 : 0);
    w.i32(g.pl.litany_turns);
    w.i32(g.pl.litany_day);
    w.i32(g.pl.free_moves);
    w.i32(g.quest_kind);
    w.i32(g.quest_need);
    w.i32(g.quest_progress);
    w.u8(uint8_t(g.pl.background));
    w.u8(g.pl.sandwalk ? 1 : 0);
    w.u8(g.pl.dead ? 1 : 0);
    w.u8(g.spawned_mask);
    w.bytes(&g.op, sizeof g.op);
    w.i32(g.alarm_turns);
    w.i32(g.alarm_spawn_cd);
    w.i32(g.flash_count);
    w.bytes(g.flashes, sizeof g.flashes);
    w.bytes(&g.season, sizeof g.season);
    w.i32(g.last_decay_day);
    w.i32(g.storm_turns);
    w.i32(g.poison_turns);
    w.i32(g.thopter_x);
    w.i32(g.thopter_y);
    w.i32(g.thopter_dx);
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
    pl.glowglobes = r.i32();
    pl.atk = r.i32();
    pl.maker_hooks = r.u8() != 0;
    pl.riding = r.u8() != 0;
    pl.ride_turns = r.i32();
    pl.tox = r.i32();
    pl.tox_lifetime = r.i32();
    pl.eyes_ibad = r.u8() != 0;
    pl.litany_turns = r.i32();
    pl.litany_day = r.i32();
    pl.free_moves = r.i32();
    const int quest_kind = r.i32();
    const int quest_need = r.i32();
    const int quest_progress = r.i32();
    pl.background = Background(r.u8());
    pl.sandwalk = r.u8() != 0;
    pl.dead = r.u8() != 0;
    const uint8_t spawned = r.u8();
    harvest::Op op;
    r.bytes(&op, sizeof op);
    const int alarm_turns = r.i32();
    const int alarm_cd = r.i32();
    const int flash_count = r.i32();
    light::Source flashes[8];
    r.bytes(flashes, sizeof flashes);
    campaign::State season;
    r.bytes(&season, sizeof season);
    const int last_decay_day = r.i32();
    const int storm_turns = r.i32();
    const int poison_turns = r.i32();
    const int thopter_x = r.i32();
    const int thopter_y = r.i32();
    const int thopter_dx = r.i32();
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
    g.flash_count = flash_count >= 0 && flash_count <= 8 ? flash_count : 0;
    std::memcpy(g.flashes, flashes, sizeof g.flashes);
    g.season = season;
    g.last_decay_day = last_decay_day;
    g.quest_kind = quest_kind;
    g.quest_need = quest_need;
    g.quest_progress = quest_progress;
    g.storm_turns = storm_turns;
    g.poison_turns = poison_turns;
    g.thopter_x = int16_t(thopter_x);
    g.thopter_y = int16_t(thopter_y);
    g.thopter_dx = int8_t(thopter_dx);
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

void game_debug_campaign(int* shipped, int* denied, int* alert, int* standing,
                         int* ending) {
    if (shipped) *shipped = g.season.shipped;
    if (denied) *denied = g.season.denied;
    if (alert) *alert = g.season.alert;
    if (standing) *standing = g.season.standing;
    if (ending) *ending = g.season.ending;
}

void game_debug_set_alert(int v) { g.season.alert = v; }

void game_debug_op_zone(int* zone_out) { if (zone_out) *zone_out = g.op.zone; }

void game_debug_spawn_shield_adjacent(int* hp_out) {
    Entity& p = g.ents[0];
    // prefer due east so the test can attack with a Right input
    static const int kOrder[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                     {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    for (const auto& d : kOrder) {
        const int dx = d[0], dy = d[1];
        if (!zone().walkable(p.x + dx, p.y + dy)) continue;
        if (entity_at(p.x + dx, p.y + dy, g.cur_zone)) continue;
        for (Entity& e : g.ents)
            if (e.type == EType::None) {
                e = Entity{EType::ShieldTrooper, Ai::Idle, g.cur_zone,
                           int16_t(p.x + dx), int16_t(p.y + dy), 10, 0, 0};
                if (hp_out) *hp_out = e.hp;
                return;
            }
    }
    if (hp_out) *hp_out = -1;
}

void game_debug_grant_hooks() { g.pl.maker_hooks = true; }

bool game_debug_riding() { return g.pl.riding; }

void game_debug_warp_to_worm() {
    for (int r = 1; r <= 3; ++r)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                const int nx = g.wrm.x + dx, ny = g.wrm.y + dy;
                if (!zone().walkable(nx, ny) || entity_at(nx, ny, g.cur_zone)) continue;
                g.ents[0].x = int16_t(nx);
                g.ents[0].y = int16_t(ny);
                refresh_fov_and_paths();
                return;
            }
}

void game_debug_worm_pos(int* x, int* y) {
    if (x) *x = g.wrm.x;
    if (y) *y = g.wrm.y;
}

void game_debug_clear_hostiles() {
    for (Entity& e : g.ents)
        if (e.type == EType::Trooper || e.type == EType::ShieldTrooper ||
            e.type == EType::Hound)
            e.type = EType::None;
}

int game_debug_shield_hp() {
    for (const Entity& e : g.ents)
        if (e.type == EType::ShieldTrooper && e.zone == g.cur_zone) return e.hp;
    return -1;
}

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
    mix(uint64_t(uint8_t(g.pl.background)) | (uint64_t(uint32_t(g.pl.atk)) << 8));
    mix(uint64_t(uint32_t(g.season.shipped)) | (uint64_t(uint32_t(g.season.denied)) << 16) |
        (uint64_t(uint32_t(g.season.alert)) << 32) |
        (uint64_t(uint32_t(g.season.standing)) << 48));
    mix(uint64_t(g.season.ending));
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
