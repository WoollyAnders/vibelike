// E5 stub game: proves the whole seam. A player walks tile-by-tile across the
// procgen desert from E3, steps make noise (blip), F5/F9 save/load, and all
// gameplay state round-trips through serialize/deserialize.
#include "game/game.h"

#include <cstdio>

#include "engine/render.h"
#include "engine/rng.h"

namespace {

constexpr int kMapW = 64, kMapH = 64;
constexpr uint32_t kChunkGame = 0x454D4147;  // 'GAME'

struct GameState {
    GameServices services{};
    RngSet rng;
    uint64_t seed = 0;

    uint8_t floor[kMapW * kMapH];
    bool rock[kMapW * kMapH];  // impassable

    int px = kMapW / 2, py = kMapH / 2;
    int steps = 0;
    int anim_tick = 0;
    int fade_amount = 0;  // save/load flash

    iso::Camera cam;
    iso::DrawList draw_list;
};
GameState g;

void generate_map() {
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            const uint32_t coarse = hash_combine(uint32_t(g.seed), hash_combine(x >> 3, y >> 3)) & 255;
            const uint32_t fine = hash_combine(uint32_t(g.seed) + 1, hash_combine(x, y)) & 255;
            const int v = int(coarse * 3 + fine) / 4;
            uint8_t f;
            bool blocked = false;
            if (v > 190) {
                f = uint8_t(3 + (fine & 1));
                blocked = fine > 210;
            } else if (v < 40) {
                f = 5;
            } else if (fine > 251) {
                f = 6;
            } else {
                f = uint8_t(fine % 3);
            }
            g.floor[y * kMapW + x] = f;
            g.rock[y * kMapW + x] = blocked;
        }
    }
    // never trap the spawn
    g.rock[g.py * kMapW + g.px] = false;
}

void snap_camera() {
    int sx, sy;
    iso::tile_to_screen(g.px, g.py, iso::Camera{}, sx, sy);
    g.cam.x = sx;
    g.cam.y = sy;
}

void try_move(int dx, int dy) {
    const int nx = g.px + dx, ny = g.py + dy;
    if (nx < 0 || nx >= kMapW || ny < 0 || ny >= kMapH) return;
    if (g.rock[ny * kMapW + nx]) return;
    g.px = nx;
    g.py = ny;
    ++g.steps;
    g.services.mixer->push({audio::Cmd::PlaySfx, 1 /* blip */, 120, 128});
}

}  // namespace

void game_init(uint64_t seed, const GameServices& services) {
    g = GameState{};
    g.services = services;
    g.seed = seed;
    g.rng.init(seed);
    generate_map();
    snap_camera();
}

bool game_update(const InputState& in) {
    ++g.anim_tick;
    if (g.fade_amount > 0) g.fade_amount -= 8;

    int dx = 0, dy = 0;
    if (in.pressed_or_repeat(Key::Up)) dy = -1;
    else if (in.pressed_or_repeat(Key::Down)) dy = 1;
    else if (in.pressed_or_repeat(Key::Left)) dx = -1;
    else if (in.pressed_or_repeat(Key::Right)) dx = 1;
    if (dx || dy) try_move(dx, dy);

    if (in.was_pressed(Key::Save)) {
        SaveWriter w;
        w.begin_chunk(kChunkGame);
        game_serialize(w);
        w.end_chunk();
        if (save_write_file("save.dat", w.data())) g.fade_amount = 128;
    }
    if (in.was_pressed(Key::Load)) {
        std::vector<uint8_t> payload;
        if (save_read_file("save.dat", payload)) {
            SaveReader r(std::move(payload));
            if (r.open_chunk(kChunkGame)) {
                if (game_deserialize(r)) g.fade_amount = 128;
            }
        }
    }

    // smooth camera toward the player
    int tx_px, ty_px;
    iso::tile_to_screen(g.px, g.py, iso::Camera{}, tx_px, ty_px);
    g.cam.x += (tx_px - g.cam.x) / 8;
    g.cam.y += (ty_px - g.cam.y) / 8;

    return !in.was_pressed(Key::Cancel);
}

void game_render(Framebuffer& frame) {
    DrawCtx ctx{&frame, g.services.cmaps};
    frame.clear(1);

    const SpriteSheet* tiles = g.services.assets->get("tiles");
    const SpriteSheet* player = g.services.assets->get("player");
    const SpriteSheet* rock = g.services.assets->get("rock");

    for (int ty = 0; ty < kMapH; ++ty) {
        for (int tx = 0; tx < kMapW; ++tx) {
            int sx, sy;
            iso::tile_to_screen(tx, ty, g.cam, sx, sy);
            if (sx < -32 || sx > fb::W + 32 || sy < -32 || sy > fb::H + 32) continue;
            draw_sprite(ctx, *tiles, g.floor[ty * kMapW + tx], sx, sy, 31);
            if (g.rock[ty * kMapW + tx]) g.draw_list.add(rock, 0, sx, sy + 6, 31, 0);
        }
    }
    {
        int sx, sy;
        iso::tile_to_screen(g.px, g.py, g.cam, sx, sy);
        g.draw_list.add(player, (g.anim_tick / 9) % 4, sx, sy + 6, 31, 1);
    }
    g.draw_list.flush(ctx);

    char buf[64];
    std::snprintf(buf, sizeof buf, "steps %d  tile %d,%d", g.steps, g.px, g.py);
    draw_text_shadow(ctx, buf, 4, 4, 230, 1);
    draw_text_shadow(ctx, "arrows move  F5 save  F9 load  ESC quit", 4, fb::H - 12, 24, 1);
}

PaletteFade game_fade() {
    return PaletteFade{255, 255, 255, uint16_t(g.fade_amount > 0 ? g.fade_amount : 0)};
}

void game_serialize(SaveWriter& w) {
    w.u64(g.seed);
    w.i32(g.px);
    w.i32(g.py);
    w.i32(g.steps);
    w.bytes(&g.rng.world, sizeof g.rng.world);
    w.bytes(&g.rng.combat, sizeof g.rng.combat);
    w.bytes(&g.rng.loot, sizeof g.rng.loot);
}

bool game_deserialize(SaveReader& r) {
    const uint64_t seed = r.u64();
    const int px = r.i32(), py = r.i32(), steps = r.i32();
    Pcg32 world, combat, loot;
    r.bytes(&world, sizeof world);
    r.bytes(&combat, sizeof combat);
    r.bytes(&loot, sizeof loot);
    if (!r.ok()) return false;

    g.seed = seed;
    generate_map();  // deterministic from seed
    g.px = px;
    g.py = py;
    g.steps = steps;
    g.rng.world = world;
    g.rng.combat = combat;
    g.rng.loot = loot;
    return true;
}

uint64_t game_state_hash() {
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001b3ULL; };
    mix(g.seed);
    mix(uint64_t(g.px));
    mix(uint64_t(g.py));
    mix(uint64_t(g.steps));
    mix(g.rng.world.state);
    mix(g.rng.combat.state);
    mix(g.rng.loot.state);
    return h;
}
