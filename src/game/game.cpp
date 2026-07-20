// M1 vertical slice: sietch + one desert region, energy turns, FOV, bump
// combat vs dust hounds, water/heat/exposure survival, HUD, permadeath.
#include "game/game.h"

#include <cstdio>
#include <cstring>

#include "engine/render.h"
#include "engine/rng.h"
#include "game/map.h"
#include "game/tunables.h"
#include "game/ui.h"

namespace {

constexpr uint32_t kChunkGame = 0x454D4147;  // 'GAME'
constexpr int kMaxEnts = 64;

enum class EType : uint8_t { None = 0, Player, Hound, Corpse };
enum class Ai : uint8_t { Idle = 0, Wander, Hunt };

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
    bool dead = false;
};

struct GameState {
    GameServices services{};
    RngSet rng;
    uint64_t seed = 0;

    map::Zone zones[2];
    uint8_t cur_zone = map::kZoneSietch;

    Entity ents[kMaxEnts];  // ents[0] = player
    Player pl;

    int16_t dmap[map::N];
    iso::Camera cam;
    iso::DrawList draw_list;
    ui::Log log;

    int anim_tick = 0;
    int fade = 0;
    uint8_t fade_r = 255, fade_g = 255, fade_b = 255;
};
GameState g;

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

void spawn_hounds() {
    map::Zone& desert = g.zones[map::kZoneDesert];
    int placed = 0;
    int guard = 0;
    while (placed < tune::kHoundCount && ++guard < 2000) {
        const int x = g.rng.world.range(8, map::W - 2);
        const int y = g.rng.world.range(2, map::H - 2);
        if (!desert.walkable(x, y) || entity_at(x, y, map::kZoneDesert)) continue;
        Entity* e = alloc_entity();
        if (!e) break;
        *e = Entity{EType::Hound, Ai::Wander, map::kZoneDesert,
                    int16_t(x), int16_t(y), tune::kHoundHp, 0, 0};
        ++placed;
    }
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

void damage_player(int dmg, const char* cause) {
    g.pl.hp -= dmg;
    flash(220, 30, 20);
    if (g.pl.hp <= 0) die(cause);
}

// ---- world turn --------------------------------------------------------------

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
        if (e.type != EType::Hound || e.zone != g.cur_zone) continue;
        e.energy += 100;
        while (e.energy >= 100 && !g.pl.dead) {
            e.energy -= 100;
            hound_act(e);
        }
    }
    refresh_fov_and_paths();
}

// ---- player actions ------------------------------------------------------------

void enter_zone(uint8_t id) {
    g.cur_zone = id;
    int sx, sy;
    map::generate(zone(), g.seed, map::ZoneId(id), sx, sy);
    // zone regen wipes vis but entities persist (they store their zone)
    player_ent().x = int16_t(sx);
    player_ent().y = int16_t(sy);
    player_ent().zone = id;
    refresh_fov_and_paths();
    flash(10, 10, 10);
}

void try_move(int dx, int dy) {
    Entity& p = player_ent();
    const int nx = p.x + dx, ny = p.y + dy;
    map::Zone& z = zone();

    if (nx < 0 || nx >= map::W || ny < 0 || ny >= map::H) return;
    const uint8_t tf = z.at(nx, ny);

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

    if (Entity* h = entity_at(nx, ny, g.cur_zone, EType::Hound)) {
        h->hp -= tune::kPlayerAtk;
        g.services.mixer->push({audio::Cmd::PlaySfx, 1, 200, 60});
        if (h->hp <= 0) {
            g.log.push("The dust hound collapses. Its water is yours.");
            *h = Entity{EType::Corpse, Ai::Idle, uint8_t(g.cur_zone),
                        h->x, h->y, 0, 0, tune::kCorpseWaterHound};
        } else {
            g.log.push("You slash the dust hound.");
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
            enter_zone(map::kZoneDesert);
        } else {
            g.log.push("You slip back into the sietch's cool dark.");
            enter_zone(map::kZoneSietch);
        }
        advance_world();
        return;
    }
    if (entity_at(nx, ny, g.cur_zone, EType::Corpse))
        g.log.push("A corpse lies here. G harvests its water.");
    advance_world();
}

void try_harvest() {
    Entity& p = player_ent();
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
    // desert generated now for hound spawns; regenerated identically on entry
    int dx_, dy_;
    map::generate(g.zones[map::kZoneDesert], seed, map::kZoneDesert, dx_, dy_);
    spawn_hounds();

    g.log.push("The Naib's word: deny them the spice. Go.");
    g.log.push("Arrows move. G harvests. F5/F9 save/load.");
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

    const map::Zone& z = zone();
    const bool night = is_night();

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
            default: break;
        }
    }
    g.draw_list.flush(ctx);

    ui::HudState hud{g.pl.hp, tune::kPlayerHpMax, g.pl.water, tune::kWaterMax,
                     g.pl.exposure, hour_now(), ambient_now(),
                     g.pl.turn / tune::kTurnsPerDay + 1,
                     (z.at(player_ent().x, player_ent().y) & map::TF_INDOOR) != 0};
    ui::draw_hud(ctx, hud, g.log, g.anim_tick);

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
    w.u8(g.pl.dead ? 1 : 0);
    w.bytes(g.ents, sizeof g.ents);
    w.bytes(&g.rng.world, sizeof g.rng.world);
    w.bytes(&g.rng.combat, sizeof g.rng.combat);
    w.bytes(&g.rng.loot, sizeof g.rng.loot);
    w.bytes(g.zones[0].vis, sizeof g.zones[0].vis);
    w.bytes(g.zones[1].vis, sizeof g.zones[1].vis);
}

bool game_deserialize(SaveReader& r) {
    GameState fresh{};
    const uint64_t seed = r.u64();
    const uint8_t cur = r.u8();
    Player pl;
    pl.hp = r.i32();
    pl.water = r.i32();
    pl.exposure = r.i32();
    pl.turn = r.i32();
    pl.dead = r.u8() != 0;
    Entity ents[kMaxEnts];
    r.bytes(ents, sizeof ents);
    Pcg32 world, combat, loot;
    r.bytes(&world, sizeof world);
    r.bytes(&combat, sizeof combat);
    r.bytes(&loot, sizeof loot);
    uint8_t vis0[map::N], vis1[map::N];
    r.bytes(vis0, sizeof vis0);
    r.bytes(vis1, sizeof vis1);
    if (!r.ok() || cur > 1) return false;

    g.seed = seed;
    g.cur_zone = cur;
    g.pl = pl;
    std::memcpy(g.ents, ents, sizeof g.ents);
    g.rng.world = world;
    g.rng.combat = combat;
    g.rng.loot = loot;
    int sx_, sy_;
    map::generate(g.zones[0], seed, map::kZoneSietch, sx_, sy_);
    map::generate(g.zones[1], seed, map::kZoneDesert, sx_, sy_);
    std::memcpy(g.zones[0].vis, vis0, sizeof vis0);
    std::memcpy(g.zones[1].vis, vis1, sizeof vis1);
    refresh_fov_and_paths();
    return true;
}

bool game_is_dead() { return g.pl.dead; }

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
