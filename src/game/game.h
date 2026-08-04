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
void game_debug_add_spice(int n);       // test helper: grant spice
int game_debug_signal_guild();          // B7: direct tithe payment (1 = paid)
void game_debug_enter_rendezvous();     // B7: ride out to the Ninth Rock
void game_debug_return_rendezvous();    // B7: return from the Ninth Rock
int game_debug_at_rendezvous();         // B7: 1 if out at the Ninth Rock
void game_debug_spawn_patrol(int x, int y);  // B8: place a Harkonnen patroller (Ai::Patrol) at (x,y)
void game_debug_world_tick(int n);           // B8: advance n world steps (headless)
int game_debug_patrol_info(int* x, int* y, int* ai, int* ax, int* ay);  // B8: count + first patroller
int game_debug_cur_zone();                   // B9: current resident zone id
void game_debug_spawn_cave_mouth(int x, int y);  // B9: place a cavern mouth in the current zone
void game_debug_enter_cave();                // B9: duck into a hidden cavern
void game_debug_exit_cave();                 // B9: climb back out to the field
int game_debug_in_cave();                    // B9: 1 if inside a cavern excursion
int game_debug_cave_reward_count();          // B9: loot + spice piles in the current zone
int game_debug_trials();                     // Desert Trials: bitmask of ordeals passed
int game_debug_can_ascend();                 // Desert Trials: 1 if the Awakening rite is open
void game_debug_trigger_trial(int which);    // Desert Trials: pass ordeal which (0/1/2) via mark_trial
void game_debug_complete_trials();           // Desert Trials: open the gate outright (all three)
int game_debug_spice_essence();              // D3e: banked Spice Essence (the Awakening resource)
void game_debug_grant_spice_essence(int n);  // D3e: set the banked Spice Essence count
int game_debug_endgame_open();               // endgame: 1 once the Chosen Path (Jihad) is open
void game_debug_open_chosen_path();          // endgame: simulate the Baron's fall (open the Jihad)
int game_debug_jihad_pressure(int f);        // endgame: per-faction directed pressure (0..100)
int game_debug_commission_strike(int f, int tier);  // endgame: pay spice + press faction f (1=done)
int game_debug_pinnacle_available(int f);    // endgame: 1 if faction f's pinnacle is open
void game_debug_clear_pinnacle(int f);       // endgame: defeat f's pinnacle (shard + guild_broken)
int game_debug_golden_shards();              // endgame: bitmask of Golden Path shards taken
int game_debug_capstone_available();         // endgame: 1 once all five shards are gathered
void game_debug_finish_capstone();           // endgame: refuse the Golden Path (capstone done)
int game_debug_guild_broken_flag();          // endgame: 1 once the Guild Navigator has fallen
void game_debug_press_faction(int f, int tier);  // endgame: press faction f directly (data-core)
int game_debug_in_strike();                  // B10.2: 1 while inside a strike excursion
int game_debug_strike_boss_count();          // B10.2: strike objectives (bosses) in the raid zone
void game_debug_resolve_strike();            // B10.2: clear the objective (press + return to hub)
void game_debug_abandon_strike();            // B10.2: break off the strike (return, no press)
int game_debug_assault_pinnacle(int f);      // B10.3: enter faction f's pinnacle if available (1=in)
int game_debug_in_pinnacle();                // B10.3: 1 while inside a pinnacle assault
int game_debug_pinnacle_boss_count();        // B10.3: homeworld bosses in the arena
void game_debug_resolve_pinnacle();          // B10.3: slay the boss (shard + return to hub)
void game_debug_abandon_pinnacle();          // B10.3: withdraw from the homeworld (no shard)
int game_debug_confront_capstone();          // B10.4: enter the Golden Path if all shards gathered
int game_debug_in_capstone();                // B10.4: 1 while facing the alternate Paul
void game_debug_resolve_capstone();          // B10.4: defeat the mirror (refuse the Path + return)
int game_debug_capstone_done();              // B10.4: 1 once the Golden Path is refused
int game_debug_ride_begin();                 // Long Ride: mount the Maker (1 if the ride starts)
int game_debug_ride_push();                  // Long Ride: ride on — 0 rode on, 1 the Maker bucked
void game_debug_ride_bank();                 // Long Ride: bank the haul + walk home
int game_debug_in_ride();                    // Long Ride: 1 while riding
int game_debug_ride_leg();                   // Long Ride: current leg
int game_debug_ride_haul();                  // Long Ride: current un-banked spice haul
int game_debug_maker_hides();                // Long Ride: Maker Hides banked (Jacurutu jackpots)
int game_debug_best_ride_leg();              // Long Ride: deepest leg reached (record)
int game_debug_choam_control();              // Landsraad: CHOAM control 0..100 (conquest + allies)
int game_debug_house_stance(int f);          // Landsraad: 0 neutral / 1 ally / 2 opponent
int game_debug_court_house(int f);           // Landsraad: court House f into your bloc (1 = done)
int game_debug_collect_dividend();           // Landsraad: collect the CHOAM dividend (spice)
int game_debug_opposing_houses();            // Landsraad: count of Great Powers opposing you
void game_debug_grant_currency(int kind, int n);  // D3: add n of currency `kind` to the pack
int game_debug_currency_count(int kind);      // D3: how many of a currency the pack holds
void game_debug_setup_gear(int idx, int rarity, int ilvl);  // D3: place a known armour item in inv[idx]
int game_debug_bench(int cur, int idx);       // D3: apply currency `cur` to inv[idx] (1 = applied)
int game_debug_item_sig(int idx, int* rarity, int* naffix, int* frac);  // D3: inspect inv[idx]; returns slot
int game_debug_item_valuesum(int idx);        // D3: sum of inv[idx] affix values
void game_debug_set_unidentified(int idx, int on);  // D3b: set/clear the unID bit on inv[idx]
int game_debug_item_state(int idx);           // D3b: istate bits (1 unID / 2 corrupt / 4 mirror)
int game_debug_item_quality(int idx);         // D3b: item quality %
int game_debug_item_implicit(int idx);        // D3b: implicit % (0 = default)
void game_debug_setup_weapon(int idx, int ilvl);    // D3b: place a common weapon in inv[idx]
int game_debug_flask_quality();               // D3b: current flask quality
int game_debug_inv_used();                    // D3b: non-empty pack cells (for the Mirror copy)
// --- G grid hooks ---
void game_debug_clear_pack();                 // G: empty the whole pack pool
int game_debug_pack_free_cells();             // G: free grid cells in the pack (of kPackW*kPackH)
int game_debug_grid_cell_of(int idx);         // G: top-left grid cell of pool entry idx (-1 if empty)
int game_debug_grid_size_of(int idx, int* w, int* h);  // G: footprint of pool entry idx (cells; -1 empty)
int game_debug_grid_item_at(int cell);        // G: pool index whose footprint covers grid cell (-1)
int game_debug_pack_add_slot(int slot);       // G: first-fit an item of slot `slot`; returns its cell (-1)
int game_debug_grid_move(int idx, int cell);  // G: collision-checked move of entry idx to top-left cell
// --- G2 currency-table hooks ---
void game_debug_grant_currency_grid(int kind, int n);  // G2: carried currency into the pack grid
int game_debug_currency_count_grid(int kind);          // G2: carried (grid) count of a currency
int game_debug_tab_deposit(int idx);                   // G2: move a grid currency stack -> the table
int game_debug_tab_withdraw(int kind, int amount);     // G2: pull currency from the table -> the grid
int game_debug_quickmove(int idx);                     // G2: Ctrl-click (currency->table, gear->equip)
int game_debug_force_currency_drop();         // D3c: drop a currency at the player; returns its kind
int game_debug_kill_drops(int n);             // D3c: n Trooper loot rolls; returns currency drops
void game_debug_spawn_currency(int kind);     // D3d: place a currency entity on the player's tile
int game_debug_cast_ability(int ab, int tx, int ty);  // C7: 1 Voice, 2 Blink, 3 Prescience (1=cast)
int game_debug_will();                       // C7: current Will
void game_debug_set_will(int w);             // C7: set current Will (test setup)
int game_debug_ability_cd();                 // C7: shared ability cooldown remaining (world turns)
void game_debug_open_menu(int m, int sel);   // hub UIs: force a menu open at a row (8-11 endgame)
int game_debug_menu_open();                  // hub UIs: current open menu id (0 = none)
int game_debug_use_flask(int slot);          // flasks: 0 Life / 1 Will / 2 Utility (1 = drunk)
int game_debug_flask_charges(int slot);      // flasks: current charges in a slot
void game_debug_set_flask_charges(int slot, int c);  // flasks: set charges (test setup)
void game_debug_flask_gain(int n);           // flasks: add n charges to every flask (capped)
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
