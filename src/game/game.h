#pragma once
// The seam between platform/engine and game. The platform owns the window,
// tick loop, input gathering, and present; the game owns everything else.
// Plain free functions, one game instance per process.
#include <cstdint>

#include "engine/animator.h"
#include "engine/assets.h"
#include "engine/audio.h"
#include "engine/framebuffer.h"
#include "engine/iso.h"
#include "engine/palette.h"
#include "engine/save.h"
#include "platform/input.h"

struct GameServices {
    audio::Mixer* mixer;
    const Assets* assets;
    const Colormaps* cmaps;
    const anim::Registry* anims = nullptr;
    bool interactive = false;  // true: show the background chooser on new runs
    bool debug_anim = false;   // overlay: state/frame/facing
};

void game_init(uint64_t seed, const GameServices& services);

// Runs at a fixed 60Hz. Returns false when the game wants to quit.
bool game_update(const InputState& input);

void game_render(Framebuffer& frame);

// Current global palette fade (damage flash, transitions). Applied at present.
PaletteFade game_fade();

void game_serialize(SaveWriter& w);
bool game_deserialize(SaveReader& r);

// Order-independent digest of gameplay state; drives the determinism test.
uint64_t game_state_hash();

// Test/debug hooks.
bool game_is_dead();
int game_worm_state();   // worm::State as int
void game_debug_warp(int zone, int x, int y);
void game_debug_op_info(int* state, int* x, int* y);  // harvest::OpState as int
void game_debug_kill_spotters();
void game_debug_refill();  // water + hp top-up (test harness only)
void game_debug_campaign(int* shipped, int* denied, int* alert, int* standing, int* ending);
void game_debug_set_alert(int v);
void game_debug_op_zone(int* zone_out);
void game_debug_spawn_shield_adjacent(int* hp_out);
int game_debug_shield_hp();
void game_debug_clear_hostiles();
void game_debug_rpg(int* level, int* inv_count, int* stash_count);
void game_debug_grab_loot();
void game_debug_vault_deposit_all();
int game_debug_vault_withdraw_one();
void game_debug_grant_hooks();
bool game_debug_riding();
void game_debug_warp_to_worm();
void game_debug_worm_pos(int* x, int* y);
