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
int game_debug_guild_debt();          // B6: current Guild tithe debt
int game_debug_guild_difficulty();    // B6: effective tithe difficulty (debt clamped to cap)
void game_debug_add_guild_debt(int n);  // B6: accrue n unpaid runs
void game_debug_pay_guild(int reduce);  // B6: pay the tithe down
void game_debug_break_guild();          // B6: Guild Navigator falls -> raise the cap
int game_debug_foe_hp(int etype);       // B6: scaled foe HP at the current depth + tithe
void game_debug_spawn_sideobj(int kind, int x, int y);  // B7: place+materialize a wandering vendor
int game_debug_footprint_count();       // B7: footprint-trail length
int game_debug_footprint_ok();          // B7: 1 if the trail connects the vendor to the player
int game_debug_sideobj_active();        // B7: current sideobj kind (0 = none)
void game_debug_expire_sideobj();       // B7: simulate the vendor's expiry despawn
void game_debug_revelation(int* stage, int* inv_count);  // B2b: Revelation stage + inv item count
void game_debug_revelation_pick(int idx);  // B2b: choose row idx + confirm the Revelation
int game_debug_cur_node();  // B1.3: current graph node index
void game_debug_extract();  // Step 1a: fade-the-vision extraction (headless)
void game_debug_warp_to_node(int idx);  // B3: enter a specific graph node
int game_debug_hostile_count();  // B3: hostiles in the resident zone
void game_debug_awaken(int* unlocked, int* cleared, int* banked_spice);  // B5: Awaken ladder + wallet
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
void game_debug_scorch_player(int dmg, int turns);  // C3: apply a Scorch (Heat) DoT to the player
void game_debug_bleed_player(int dmg, int turns);   // C3: apply a Bleed (physical) DoT to the player
void game_debug_blind_player(int turns);            // C3: apply Blind to the player
int game_debug_player_tohit(int ac);                // C3: deterministic player to-hit vs ac_dodge
void game_debug_freeze_first_enemy(int turns);      // C3: freeze the first spawned ShieldTrooper
int game_debug_enemy_frozen_turns();                // C3: frozen turns on the first ShieldTrooper
int game_debug_enemy_poison_turns();  // C3: poison turns on the first spawned ShieldTrooper
int game_debug_enemy_stun_turns();    // RT2: stun turns on the first spawned ShieldTrooper
int game_debug_freeze_ticks(int cold_dmg, int max_life);            // RT2: Freeze duration formula (ticks)
int game_debug_stun_ticks(int hit_dmg, int max_life, int mult, int reduce);   // RT2: Stun duration formula
int game_debug_stun_chance(int hit_dmg, int max_life, int mult, int reduce);  // RT2: Stun chance%
void game_debug_stun_player(int ticks);  // RT2: force a Stun on the player
int game_debug_player_stun_turns();      // RT2: player's remaining Stun ticks
int game_debug_player_ready();           // RT2: 1 if the player may act (not frozen/stunned/on cooldown)
void game_debug_apply_base(int bc);      // C3: set the base class (classtest)
void game_debug_class(int* base, int* asc);           // C3: read live base class + ascendancy
void game_debug_unlock_awaken(int lvl);  // C3: set meta awaken_unlocked (gate the rite)
int game_debug_ascend(int asc);          // C3: ascend into ascendancy `asc` (1 = success)
int game_debug_enemy_element_chance(int depth);   // C3: elemental enemy-hit chance% at an Awaken depth
void game_debug_inflict_element(int elem, int mag);  // C3: inflict element (0 Heat/1 Cold/2 Storm/3 Toxin)
int game_debug_player_frozen_turns();     // C3: player's remaining Freeze ticks
void game_debug_meta_reload_class(int* base, int* asc);  // C3: reload meta.dat + report persisted class
int game_debug_item_name(int where, int idx, char* out, int cap);  // G2: where 0=inv 1=equip
