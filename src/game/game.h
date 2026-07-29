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
    const Palette* pal = nullptr;  // R1: index -> RGB for the RGBA render path
};

void game_init(uint64_t seed, const GameServices& services);

// Runs at a fixed 60Hz. Returns false when the game wants to quit.
bool game_update(const InputState& input);

void game_render(Framebuffer& frame);

// Current global palette fade (damage flash, transitions). Applied at present.
PaletteFade game_fade();

// Options-screen state the platform layer acts on. game_muted(): pause audio.
// game_consume_fullscreen_request(): true once after the player picks Fullscreen.
bool game_muted();
bool game_consume_fullscreen_request();

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
void game_debug_warp_to_boss(int* atk_dx, int* atk_dy);
bool game_debug_boss_alive();
bool game_debug_boss_is_baron();
const char* game_debug_death_cause();
void game_debug_buff();
void game_debug_grant_hooks();
bool game_debug_riding();
void game_debug_warp_to_worm();
void game_debug_worm_pos(int* x, int* y);
void game_debug_player_pos(int* x, int* y);
void game_debug_click(int tx, int ty);  // A4: headless click-to-move goal
void game_debug_use_door();  // headless: leave the sietch (doors are click-only now)
void game_debug_revelation(int* stage, int* inv_count);  // B2b: Revelation stage + inv item count
void game_debug_revelation_pick(int idx);  // B2b: choose row idx + confirm the Revelation
int game_debug_cur_node();  // B1.3: current graph node index
void game_debug_warp_to_node(int idx);  // B3: enter a specific graph node
int game_debug_hostile_count();  // B3: hostiles in the resident zone
void game_debug_awaken(int* unlocked, int* cleared, int* banked_spice);  // B5: Awaken ladder + wallet
void game_debug_set_awaken_choice(int lvl);  // B5: choose the launch Awaken level
int game_debug_char_level();  // B5: persistent character level
void game_debug_player_stats(int* hp, int* max_hp, int* water, int* max_water, int* level, int* spice);  // G2
void game_debug_hurt(int dmg);  // C2: apply damage through the shield/Life pipeline (harness only)
void game_debug_shield_state(int* shield, int* max_shield, int* hp);  // C2: read Focus shield + Life
void game_debug_equip_affix(int kind, int value);  // D1: equip a synthetic 1-affix carrier
void game_debug_derived(int* max_hp, int* leth, int* max_shield, int* max_will,
                        int* armor, int* dodge, int* crit_mult);  // D1: read derived stats
void game_debug_roll_item(int ilvl, int rarity_bonus, int slot,
                          int* rarity, int* naffix, int* max_tier);  // D1: roll + report
int game_debug_meta_migrate_selftest();  // D1: DME3->DME4 vault back-compat self-test
void game_debug_poison_player(int dmg, int turns);  // C3: apply a poison ailment to the player
int game_debug_enemy_poison_turns();  // C3: poison turns on the first spawned ShieldTrooper
int game_debug_item_name(int where, int idx, char* out, int cap);  // G2: where 0=inv 1=equip
