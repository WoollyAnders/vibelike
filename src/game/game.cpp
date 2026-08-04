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
#include "game/items.h"
#include "game/light.h"
#include "game/tunables.h"
#include "game/ui.h"
#include "game/world.h"
#include "game/worm.h"

namespace {

constexpr uint32_t kChunkGame = 0x454D4147;  // 'GAME'
constexpr int kMaxEnts = 64;
constexpr int kMaxDeltas = 192;

enum class EType : uint8_t { None = 0, Player, Hound, Corpse, Thumper, Trooper, Spotter,
                             Glowglobe, ShieldTrooper, Naib, Sayyadina, Smuggler,
                             PanAdder, Hermit, HunterSeeker, SpiceCache, Loot, Boss,
                             Caravan, Envoy, Fremen, Paul, CaveMouth, WarRoom, Currency, Vault };
enum class Ai : uint8_t { Idle = 0, Wander, Hunt, Guard, Patrol };  // Patrol (B8): route-walker

struct Entity {
    EType type = EType::None;
    Ai ai = Ai::Idle;
    uint8_t zone = 0;
    int16_t x = 0, y = 0;
    int16_t hp = 0;
    int16_t energy = 0;
    int16_t data = 0;   // corpse: water yield | loot: def|prefix<<8
    int16_t data2 = 0;  // loot: suffix|rarity<<8
    // G1 presentation-only state (persisted with the entity, never gameplay):
    int16_t vpx = 0, vpy = 0;   // visual world-pixel position (glides)
    anim::Animator anim;
    uint8_t hurt_flash = 0;     // G3: bright flinch frames remaining
    int16_t poison_dmg = 0;     // C3: combat-poison ailment — damage per world turn
    int16_t poison_turns = 0;   // C3: remaining poison ticks
    int16_t scorch_dmg = 0, scorch_turns = 0;  // C3: Scorch DoT (Heat)
    int16_t bleed_dmg = 0, bleed_turns = 0;     // C3: Bleed DoT (physical, bypasses armor)
    int16_t blind_turns = 0;                    // C3: Blind (Storm) — this foe's accuracy drops
    int16_t frozen_turns = 0;                   // C3: Freeze (Cold) — this foe loses its turns
    int16_t stun_turns = 0;                     // RT2: Stun — this foe loses its turns (brief)
};

// Sietch furniture/decor: static props placed in the hub chambers. Regenerated
// deterministically on sietch entry (not serialized).
struct Decor {
    int16_t x, y;
    uint8_t prop;  // index into kDecorProps
};
struct DecorDef {
    const char* key;  // atlas sprite
    int8_t seat;      // vertical seating offset (screen px)
    uint8_t layer;    // draw-list layer bias (0 = floor rug, 1 = standing prop)
    bool block;       // occupies the tile (TF_BLOCK)
    bool opaque;      // also blocks sight (TF_OPAQUE) — tall racks/banners
};
constexpr DecorDef kDecorProps[] = {
    {"prop_rug", 2, 0, false, false},        // 0
    {"prop_brazier", 6, 1, true, false},     // 1
    {"prop_weaponrack", 8, 1, true, true},   // 2
    {"prop_suitrack", 8, 1, true, true},     // 3
    {"prop_urn", 6, 1, true, false},         // 4
    {"prop_banner", 6, 1, true, true},       // 5
    {"prop_baskets", 6, 1, true, false},     // 6
    {"prop_table", 6, 1, true, false},       // 7
    {"prop_spicesacks", 6, 1, true, false},  // 8
};

// Dialog-only Fremen: clicking one shows a flavor bark (full dialog deferred).
// Rendered with a varied existing NPC sprite; e.data indexes the bark/sprite.
constexpr const char* kFremenSprites[] = {"fremen",  "fremen2", "fremen3", "fremen4",
                                          "fremen5", "fremen6", "fremen7", "naib"};
constexpr const char* kFremenBarks[] = {
    "\"The desert gives, and the desert takes. Walk without rhythm, water-brother.\"",
    "\"Bless the Maker and His water. Bless the coming and going of Him.\"",
    "\"My stillsuit's reclaimed near a litre today. A good day.\"",
    "\"They say Muad'Dib will come from the outworld. I only pray for rain that never falls.\"",
    "\"Keep your knife hand loose. The Harkonnen scouts range far this season.\"",
    "\"I lost a son to the sand. The tribe drank his water. It is our way.\"",
    "\"Careful by the cistern, little one. A tribe's wealth is measured in water.\"",
    "\"Shai-Hulud stirs in the deep bled. Best you not wake Him.\"",
};

// C3: three basic Fremen BASE CLASSES (one per attribute); each ASCENDS at a sietch rite into one
// of its three ascendancies (PoE base->ascendancy, branching). Base ordinals are preserved from the
// old Background enum (Knifehand<-Fedaykin, Wanderer<-Saboteur, Oracle<-Sayyadina) so the hash/save
// value for the base is unchanged. Fedaykin/Saboteur/Sayyadina are now ASCENDANCIES, not bases.
enum class BaseClass : uint8_t { Knifehand = 0, Wanderer, Oracle, Count };
enum class Ascendancy : uint8_t {
    None = 0,
    Fedaykin, DesertGhost, WaterReaver,      // Knifehand (Conditioning)
    Saboteur, Wayfinder, Sunwalker,          // Wanderer  (Versatility)
    Sayyadina, ReverendMother, Spiceweaver,  // Oracle    (Awareness)
    Count
};
// The base each ascendancy belongs to (index by Ascendancy; None -> 0xFF). Gates valid ascensions.
constexpr uint8_t kAscendancyBase[int(Ascendancy::Count)] = {0xFF, 0, 0, 0, 1, 1, 1, 2, 2, 2};
constexpr const char* kAscendancyNames[int(Ascendancy::Count)] = {
    "None", "Fedaykin", "Desert Ghost", "Water Reaver",
    "Saboteur", "Wayfinder", "Sunwalker",
    "Sayyadina", "Reverend Mother", "Spiceweaver",
};

struct BaseClassDef {
    const char* name;
    const char* sprite;
    const char* blurb1;
    const char* blurb2;
};
constexpr BaseClassDef kBaseClasses[] = {
    {"KNIFEHAND", "player_fedaykin",
     "Fremen blade, close and certain.", "Ascends: Fedaykin / Desert Ghost / Water Reaver."},
    {"WANDERER", "player_saboteur",
     "Sietch outrider of tools and traps.", "Ascends: Saboteur / Wayfinder / Sunwalker."},
    {"ORACLE", "player_sayyadina",
     "The spice has opened one eye.", "Ascends: Sayyadina / Reverend Mother / Spiceweaver."},
};

// X3: talents — three branches, three talents each, ranks 0..3.
// Blade:      0 Crysknife Discipline (+2 dmg/rank)
//             1 Slow Blade Mastery   (+3 dmg/rank while sandwalking)
//             2 Red Rhythm           (+4% crit/rank)
// Stealth:    3 Featherstep          (-2 step noise/rank)
//             4 Demolitions          (charge plants 1 turn faster/rank, min 1)
//             5 Scavenger            (+8% loot chance/rank)
// Prescience: 6 Spice Economy        (power costs -1 spice at rank 2)
//             7 Water Discipline     (thirst -12%/rank)
//             8 Inner Eye            (+4 dodge/rank)
inline constexpr int kTalentCount = 9;
inline constexpr const char* kTalentNames[kTalentCount] = {
    "Crysknife Discipline", "Slow Blade Mastery", "Red Rhythm",
    "Featherstep", "Demolitions", "Scavenger",
    "Spice Economy", "Water Discipline", "Inner Eye",
};

// C1: the three-attribute ARPG foundation. Each attribute moves a derived PAIR:
//   Conditioning -> Life + Lethality   (body: how much you take, how hard you hit)
//   Versatility  -> Precision + Flow   (skill: how often you land, how well you evade)
//   Awareness    -> Focus + Will-max   (mind: the energy shield, the ability pool)
// Crit is deliberately NOT an attribute derivation anymore — it comes from gear (AK_CRIT).
struct CharStats {
    int16_t cond = 12, vers = 12, awar = 12;  // Conditioning / Versatility / Awareness
    int16_t level = 1, statpts = 0, talentpts = 0;
    int32_t xp = 0;
    uint8_t talents[kTalentCount] = {};
};

struct Player {
    CharStats st;
    items::Inst inv[tune::kInvCap];               // G: the pack pool (8x12 grid). inv_cell[i] = item i's top-left
    uint8_t inv_cell[tune::kInvCap] = {};         // grid cell of pool entry i (meaningful only when !inv[i].empty())
    uint16_t cur_tab[items::CUR_COUNT] = {};      // G2: the crafting table's dedicated currency slots (per-type)
    items::Inst equip[int(items::Slot::Count)];  // by items::Slot (B2a: 10-slot Fremen paperdoll)
    int hp = tune::kPlayerHpMax;
    int water = tune::kStartWater;
    int reclaimed = 0;     // C5: run-scoped reclaimed water (the Deathstill stake) -> Litrejons on extraction
    uint64_t passives = 0; // C4: allocated passive-tree node mask (persists with the character)
    int exposure = 0;
    int turn = 0;
    int thumpers = 3;
    int charges = 1;   // demolition
    int poisons = 1;   // hold toxin
    int spice = 0;     // grains
    int shield = 0;        // C2: current energy shield (Focus) — soaks damage before Life
    int shield_delay = 0;  // C2: turns until the shield recharges (reset on taking a hit)
    int will = 0;          // D1: current Will (ability fuel); C7 abilities spend it
    int ability_cd = 0;    // C7: shared ability cooldown (world turns remaining)
    int flask_charges[tune::kFlaskSlots] = {tune::kFlaskMaxCharges, tune::kFlaskMaxCharges,
                                            tune::kFlaskMaxCharges};  // Life / Will / Utility belt
    int flask_quality = 0;  // D3: Distillate Microtubing raises it (0..20); scales flask effects
    int ail_poison_dmg = 0;    // C3: combat-poison on the player — damage/turn (DT_TOX, resisted)
    int ail_poison_turns = 0;  // C3: remaining poison ticks on the player
    int ail_scorch_dmg = 0;    // C3: Scorch DoT — DT_HEAT (Heat Resistance mitigates)
    int ail_scorch_turns = 0;
    int ail_bleed_dmg = 0;     // C3: Bleed DoT — physical, bypasses armor (DT_BLEED)
    int ail_bleed_turns = 0;
    int ail_blind_turns = 0;   // C3: Blind (Storm) on the player — reduces the player's accuracy
    int ail_frozen_turns = 0;  // C3/RT2: Freeze (Cold) on the player — locks actions while > 0
    int ail_stun_turns = 0;    // RT2: Stun on the player — locks actions while > 0 (brief)
    int glowglobes = 2;
    int atk = tune::kPlayerAtk;
    bool maker_hooks = false;  // Honored: the right to ride
    bool riding = false;
    int ride_turns = 0;
    int litany_turns = 0;      // active suppression
    int litany_day = -1;       // last day the Litany was spoken
    int free_moves = 0;        // Threadwalk strides (no world advance)
    uint32_t next_act_tick = 0;  // RT1.2: earliest sim_clock at which the player may act again (cooldown)
    BaseClass base_class = BaseClass::Knifehand;  // C3: the chosen base (one per attribute)
    Ascendancy ascendancy = Ascendancy::None;     // C3: granted at the sietch rite; persists
    bool sandwalk = false;
    bool dead = false;
};

void apply_base_class(Player& pl, BaseClass bc) {
    pl.base_class = bc;
    pl.ascendancy = Ascendancy::None;  // ascension happens later, at the sietch rite
    switch (bc) {
        case BaseClass::Knifehand:
            // the plain blade — baseline kit (C3: 0 additional attributes for every class)
            break;
        case BaseClass::Wanderer:
            pl.charges = 3;
            pl.poisons = 2;
            pl.thumpers = 5;
            break;
        case BaseClass::Oracle:
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

    map::Zone zone_;      // B1.2: the single resident zone (regenerated on node entry)
    world::Graph graph;   // B1: per-run node graph (built from run seed)
    int cur_node = 0;     // B1.3: current graph node (0 = sietch hub); drives the resident zone
    uint32_t sim_clock = 0;  // RT1: monotonic sim-tick counter (fixed timestep); the real-time clock
    int world_accum = 0;     // RT1: ticks since the last world step (drives world_tick from RT1.2)
    uint8_t cur_zone = map::kZoneSietch;  // derived from the current node's Stage (biome/membership tag)
    // B2b: the Revelation (a prescient gear suggestion at each forward door). Transient (reset on load).
    int revelation_stage = 0;              // 0 none, 1 pick slot, 2 pick item
    int rev_sel = 0;
    int rev_dest = 0;                      // node to enter once the pick resolves
    uint8_t rev_slot_offer[3] = {0, 0, 0};
    items::Inst rev_item_offer[3];
    uint8_t spawned_mask = 0;  // zones whose wildlife has been placed
    int shade_hour = -1;       // hour the shade map was computed for

    Entity ents[kMaxEnts];  // ents[0] = player
    items::Inst loot_inst[kMaxEnts];  // D1: full item for each EType::Loot entity (by slot index)
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
    int menu_open = 0;  // 1 naib / 2 sayyadina / 3 smuggler / 5 char / 6 inv / 7 vault
                        // 8 war council / 9 jihad table / 10 landsraad / 11 crafting bench
    int menu_sel = 0;
    int jihad_tier = 1;  // transient UI: the commission tier chosen at the Jihad Table (Left/Right)
    int bench_cur = 0;   // transient UI: the currency picked at the bench (0 = choosing a currency)
    int vault_page = 0;
    int stash_tab = 0;   // G3: the stash tab the Vault screen is viewing (0..stash_tabs-1)
    int drag_kind = 0;   // G4 drag source: 0 none, 1 pack, 2 equip, 3 stash, 4 currency-tab
    int drag_idx = -1;   // pool index / equip slot / stash pool index / currency kind of the dragged item
    int ui_mx = -1, ui_my = -1;  // G4: last menu mouse position (for grid hover + the dragged item)
    int last_decay_day = 0;

    // Naib task: 0 none, 1 kill hounds, 2 gather spice, 3 quiet denial
    int quest_kind = 0;
    int quest_need = 0;
    int quest_progress = 0;

    // Y4: one timed side objective at a time (1 caravan / 2 guild envoy)
    int sideobj_kind = 0;
    uint8_t sideobj_zone = 0;
    int16_t sideobj_x = 0, sideobj_y = 0;
    int32_t sideobj_expires = 0;
    bool sideobj_spawned = false;  // entity placed in-zone
    bool at_rendezvous = false;    // B7: the player is out at the Ninth Rock (Guild spice exchange)
    // B9: a transient hidden-cavern excursion off a field node. cur_node is unchanged while inside;
    // the field zone resumes (at cave_ret_*) on exit. cave_seed drives the cavern layout + load-regen.
    bool in_cave = false;
    uint64_t cave_seed = 0;
    int16_t cave_ret_x = 0, cave_ret_y = 0;
    uint8_t cave_ret_zone = 0;
    // B10.2: a Jihad strike excursion. Commissioned at the hub; you raid a procedural off-world zone
    // and clearing its objective presses the target Great Power. cur_node stays 0 (the hub) throughout.
    bool in_strike = false;
    uint8_t strike_faction = 0;
    uint8_t strike_tier = 0;
    uint64_t strike_seed = 0;
    // B10.3: a homeworld pinnacle assault (fixed-seed arena). Entered when a faction's Jihad-pressure
    // is full; clearing its named boss takes that Great Power's Golden Path shard.
    bool in_pinnacle = false;
    uint8_t pinnacle_faction = 0;
    // B10.4: the Golden Path capstone — the alternate-Paul mirror boss. Entered once all five shards
    // are gathered; defeating it refuses the Path (the game's apex trophy).
    bool in_capstone = false;
    // The Long Ride: the deep-desert greed gauntlet. Abstract state (the playable worm-ride zone,
    // reusing ride_move, layers on later); the leg/haul/restlessness math is here.
    bool in_ride = false;
    int ride_leg = 0;       // legs ridden this run
    int ride_haul = 0;      // un-banked spice haul (lost if the Maker bucks)
    int ride_restless = 0;  // restlessness — climbs each leg, drives the buck chance's flavor

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
    // B7: footprint trail from a materialized wandering vendor to the player. Transient/derived from
    // dmap each refresh (RNG-free) — NOT serialized; recomputed on load, so it's save/load-identical.
    static constexpr int kMaxFootprints = 96;
    int16_t footprints_x[kMaxFootprints] = {}, footprints_y[kMaxFootprints] = {};
    int footprint_count = 0;
    // B7 (generalized): an explicit way-finding target any system can set (rendezvous, B10 Jihad-Table
    // points of interest, ...). -1 = none -> the trail falls back to the wandering sideobj vendor.
    int16_t footprint_tx = -1, footprint_ty = -1;
    iso::Camera cam;
    iso::DrawList draw_list;
    particles::Pool particles;
    ui::Log log;

    int16_t impact_x = -1, impact_y = -1;  // where the player's blow will land
    uint8_t impact_kind = 0;               // 0 flesh, 1 shield/metal
    int16_t worm_vis[(worm::kSegments + 1) * 2] = {};  // G4: slithering visuals
    int hitstop = 0;        // G5: frames frozen after a killing blow
    int microshake = 0;     // G5: frames of 1px shake from your own hits
    int disp_hp = -1, disp_water = -1;  // G5: orb slosh (displayed values)
    int current_song = -1;
    int16_t hover_tx = -1, hover_ty = -1;
    int16_t move_goal_x = -1, move_goal_y = -1;  // A4: click-to-move destination (-1 = none)
    // Click-only interaction: what the current move-goal is heading to interact with
    // once adjacent. 0 none, 1 Naib, 2 Sayyadina, 3 Smuggler, 4 door, 5 Fremen (bark).
    uint8_t interact_kind = 0;
    std::vector<Decor> decor;  // sietch hub furniture (regenerated on entry)
    bool show_map = false;   // A5: minimap toggle (M)
    uint8_t map_scope = 0;   // A5: 0 = local zone; reserved for Phase-B world/section overview
    int anim_tick = 0;
    int fade = 0;
    uint8_t fade_r = 255, fade_g = 255, fade_b = 255;

    bool choosing = false;      // background chooser (interactive new runs)
    int choose_idx = 0;
    // Front-end screens shown before/around gameplay (the title "login" screen,
    // an options panel, and a paged field manual). Play == in the world.
    enum class Screen : uint8_t { Menu, Options, Manual, Play };
    Screen screen = Screen::Play;
    int menu_idx = 0;       // main-menu selection
    int options_idx = 0;    // options selection
    int manual_page = 0;    // field-manual page
    bool opt_muted = false;         // options: sound on/off (read by the platform)
    bool fullscreen_req = false;    // options: one-shot fullscreen-toggle request
};
GameState g;

void op_denied(const char* how);
void op_shipped_quota();
uint8_t facing_from_tile_delta(int dx, int dy);
void apply_deltas(map::Zone& z, uint8_t zone_id);
void end_season(uint8_t ending);
void spawn_zone_wildlife(map::Zone& z, uint8_t zone_id);
bool endgame_open();  // fwd: the Jihad (endgame) is open once the Baron has fallen

worm::Tuning worm_tuning() {
    worm::Tuning t;
    t.active = g.cur_zone != map::kZoneSietch;
    if (g.cur_zone == map::kZoneDeepPan) { t.stir_threshold = 25; t.hunt_threshold = 50; }
    return t;
}

// Y3: champions — randomly generated bosses. Entity.data packs
// (tier | traits<<8); data2 packs (name_a | name_b<<8). Traits are bits:
// C2 retired BT_SHIELDED (the Holtzman slow-blade trait); bit 0 is now reserved/inert.
inline constexpr uint8_t BT_RESERVED0 = 1, BT_SWIFT = 2, BT_BRUTE = 4,
                         BT_STALWART = 8, BT_TOXIC = 16;
inline constexpr const char* kBossTitles[8] = {
    "Warmaster", "Executor", "Beast-Handler", "Count", "Whipmaster",
    "Provost", "Blade-Tutor", "Overseer"};
inline constexpr const char* kBossNames[8] = {
    "Dreth", "Gorou", "Vask", "Ilbam", "Krayt", "Feyd-Rausha", "Mordo", "Skarn"};
inline constexpr const char* kBossEpithets[8] = {
    "the Unbowed", "the Flenser", "of the Tithe", "the Whip-Hand",
    "the Waterless", "the Gilded", "the Patient", "the Cruel"};

// B3: the LOCKED named pinnacle roster, indexed by node.awaken (1..10). A named pinnacle is an
// EType::Boss whose data2 = kPinnacleTag - roster_idx (a range distinct from champion name-packs
// >= 0 and the old Baron sentinel -1). Slaying the Awaken-10 Baron (idx 9) is the campaign victory.
struct Pinnacle { const char* name; const char* note; };
inline constexpr Pinnacle kPinnacles[10] = {
    {"Esmar Tuek", "the turned smuggler"},        // Awaken 1  (deep desert)
    {"Wellington Yueh", "who survived Piter's blade"},  // 2
    {"Caid Orlan Kest", "the taskmaster"},        // 3  (rimwall)
    {"Iakin Nefud", "Guard Captain"},             // 4
    {"Thufir Hawat", "the poisoned Mentat"},      // 5  (Shield Wall; the Baron flees)
    {"Bashar Vorek Harkonnen", "the Bashar"},     // 6
    {"Piter de Vries", "the Tleilaxu clone"},     // 7  (outskirts)
    {"Glossu Rabban", "the Beast"},               // 8
    {"Feyd-Rautha", "the na-Baron"},              // 9  (Arrakeen)
    {"Baron Vladimir Harkonnen", "at last"},      // 10 (the Residency; victory)
};
inline constexpr int16_t kPinnacleTag = -100;  // data2 = kPinnacleTag - idx  ->  -100..-109
inline bool boss_is_pinnacle(int16_t data2) { return data2 <= kPinnacleTag && data2 >= -109; }
inline int boss_pinnacle_idx(int16_t data2) { return int(kPinnacleTag - data2); }

// X2: Diablo-style enemy statlines.
struct Foe { uint8_t level, tohit, dmin, dmax, ac_dodge; uint16_t xp; uint16_t hp; };
Foe boss_foe(const Entity& e) {
    const int tier = e.data & 0xFF;
    const uint8_t traits = uint8_t(e.data >> 8);
    Foe f{};
    f.level = uint8_t(3 + tier);
    f.tohit = uint8_t(64 + tier * 2);
    f.dmin = uint8_t(2 + tier + ((traits & BT_BRUTE) ? 2 : 0));
    f.dmax = uint8_t(5 + tier * 2 + ((traits & BT_BRUTE) ? 3 : 0));
    f.ac_dodge = uint8_t(8 + tier);
    f.xp = uint16_t(50 + tier * 30);
    return f;
}

const Foe* foe_stats(EType t) {
    //                    level tohit dmin dmax ac_dodge  xp   hp   (hp = base Life at Awaken 1)
    static constexpr Foe kHound{1, 60, 1, 3, 8, 12, 14};
    static constexpr Foe kAdder{2, 72, 1, 2, 4, 15, 10};
    static constexpr Foe kHermit{3, 55, 2, 5, 6, 28, 20};
    static constexpr Foe kTrooper{3, 62, 2, 5, 8, 35, 24};
    static constexpr Foe kShield{5, 68, 4, 9, 14, 80, 48};  // heavy-armor BULWARK (tanky)
    static constexpr Foe kSeeker{4, 88, 4, 4, 22, 40, 12};
    switch (t) {
        case EType::Hound: return &kHound;
        case EType::PanAdder: return &kAdder;
        case EType::Hermit: return &kHermit;
        case EType::Trooper: return &kTrooper;
        case EType::ShieldTrooper: return &kShield;
        case EType::HunterSeeker: return &kSeeker;
        default: return nullptr;
    }
}

// C3: a foe's spawn Life, scaled by the run's Awaken depth (deeper = tankier). Replaces the
// ad-hoc per-site hp literals so mobs have real, scaling HP (players' Life scales via player_max_hp).
int guild_difficulty();  // B6 fwd (defined after g_meta); the Guild-tithe difficulty level

int foe_hp(EType t) {
    const Foe* f = foe_stats(t);
    if (!f) return 1;
    int awk = 1;
    if (g.cur_node >= 0 && g.cur_node < world::kMaxNodes) awk = g.graph.nodes[g.cur_node].awaken;
    if (awk < 1) awk = 1;
    int hp = f->hp + f->hp * (awk - 1) / 4;  // +25% of base per Awaken level of depth
    hp += hp * guild_difficulty() * tune::kGuildHpPerDebt / 100;  // B6: unpaid tithe -> tougher mobs
    return hp;
}

Entity& player_ent() { return g.ents[0]; }

int sietch_affix_bonus(uint8_t kind);   // C5b: fwd — the sietch tree contributes to every derived stat
int passive_affix_bonus(uint8_t kind);  // C4: fwd — the character's allocated passive tree, likewise
int affix_total(uint8_t kind) {
    int p = sietch_affix_bonus(kind) + passive_affix_bonus(kind);  // tree bonuses, on top of equipped gear
    for (const items::Inst& it : g.pl.equip) p += it.affix_power(kind);
    return p;
}

// D1 effective attributes: base + flat attribute affixes (+all-attribute).
int eff_cond() { return g.pl.st.cond + affix_total(items::AK_ADD_COND) + affix_total(items::AK_ADD_ALL); }
int eff_vers() { return g.pl.st.vers + affix_total(items::AK_ADD_VERS) + affix_total(items::AK_ADD_ALL); }
int eff_awar() { return g.pl.st.awar + affix_total(items::AK_ADD_AWAR) + affix_total(items::AK_ADD_ALL); }
// flat first, then scale by an "increased %" affix.
int with_inc(int base, uint8_t inc_kind) { return base * (100 + affix_total(inc_kind)) / 100; }

// C1/D1 derived stats. Conditioning -> Life+Lethality; Versatility -> Precision+Flow;
// Awareness -> Focus+Will. Each = flat (attr + AK_* adds), then ×(increased %).
int player_max_hp() {  // Life — flat per-level (class-agnostic) + Conditioning + gear
    const int base = 10 + eff_cond() * 2 + (g.pl.st.level - 1) * tune::kLifePerLevel + affix_total(items::AK_HP);
    return with_inc(base, items::AK_INC_LIFE);
}
int player_lethality() {  // melee damage bonus
    return with_inc(eff_cond() / 4 + affix_total(items::AK_DMG), items::AK_INC_DMG);
}
int player_max_shield() {  // Focus: only Awareness INVESTED above baseline (+gear) buys a shield
    const int focus = eff_awar() > 12 ? (eff_awar() - 12) * 3 : 0;  // unspecced == 0 (no behavior change)
    return with_inc(focus + affix_total(items::AK_SHIELD), items::AK_INC_FOCUS);
}
int player_max_will() {  // Will — flat per-level (class-agnostic) + Awareness + gear
    return 10 + eff_awar() * 2 + (g.pl.st.level - 1) * tune::kWillPerLevel + affix_total(items::AK_WILL);
}
int player_max_water() { return tune::kWaterMax + affix_total(items::AK_WATER); }
int player_armor() {
    return with_inc(affix_total(items::AK_ARMOR), items::AK_INC_ARMOR);  // Guarded/implicit + Plated %
}
// B2a: thirst multiplier (x/4) from summed stillsuit reclamation (AK_RECLAIM).
// 4 = no reclaim ... 1 = a full, sealed stillsuit.
int player_suit_mul4() {
    int cut = affix_total(items::AK_RECLAIM) / 2;
    if (cut > 3) cut = 3;
    return 4 - cut;
}
int player_dodge() {  // Flow: Versatility -> evasion
    const int base = eff_vers() / 2 + affix_total(items::AK_DODGE) + 4 * g.pl.st.talents[8];
    return with_inc(base, items::AK_INC_DODGE);
}

// Player melee swing vs a foe. Returns damage dealt (0 = miss); fills crit.
int player_attack_roll(const Foe& f, bool* crit_out) {
    int acc = with_inc(55 + eff_vers() * 2 + affix_total(items::AK_TOHIT), items::AK_INC_TOHIT);  // Precision
    if (g.pl.ail_blind_turns > 0) acc -= tune::kBlindAcc;  // C3: Blinded — the player misses more
    const int tohit = acc - f.ac_dodge;
    const int chance = tohit < 15 ? 15 : tohit > 95 ? 95 : tohit;
    if (int(g.rng.combat.below(100)) >= chance) return 0;
    const items::Inst& w = g.pl.equip[int(items::Slot::Weapon)];
    const int wq = w.empty() ? 0 : w.quality;  // D3: weapon quality (Worm Tooth) scales base damage
    const int wmin = (w.empty() ? 1 : w.d().a) * (100 + wq) / 100;
    const int wmax = (w.empty() ? 2 : w.d().b) * (100 + wq) / 100;
    int dmg = g.rng.combat.range(wmin, wmax) + player_lethality() + 2 * g.pl.st.talents[0];  // Lethality
    if (g.pl.sandwalk) dmg += 3 * g.pl.st.talents[1];
    const int critc = affix_total(items::AK_CRIT) + 4 * g.pl.st.talents[2];  // crit from GEAR (+talent), not attributes
    const bool crit = int(g.rng.combat.below(100)) < critc;
    if (crit) dmg = dmg * (200 + affix_total(items::AK_CRIT_MULT)) / 100;  // Vital Strike Damage (base x2)
    if (crit_out) *crit_out = crit;
    return dmg;
}

// Enemy swing at the player. Returns damage taken (0 = miss/dodge). `attacker_blind` (C3) is the
// Blind accuracy penalty when the attacking foe is Blinded (Storm ailment — e.g. Desert Ghost).
int foe_attack_roll(const Foe& f, int attacker_blind = 0) {
    const int chance0 = int(f.tohit) - player_dodge() - attacker_blind;
    const int chance = chance0 < 15 ? 15 : chance0 > 95 ? 95 : chance0;
    if (int(g.rng.combat.below(100)) >= chance) return 0;
    int dmg = g.rng.combat.range(f.dmin, f.dmax) - player_armor() / 2;
    return dmg < 1 ? 1 : dmg;
}

int xp_to_level(int level) { return 30 * level * level; }

void grant_xp(int amount);
map::Zone& zone() { return g.zone_; }

int ambient_now() { return tune::kAmbientByHour[(g.pl.turn % tune::kTurnsPerDay) * 24 / tune::kTurnsPerDay]; }
int hour_now() { return (g.pl.turn % tune::kTurnsPerDay) * 24 / tune::kTurnsPerDay; }
bool is_night() { return ambient_now() <= 2; }

int fov_radius() {
    if (g.storm_turns > 0) return 3;  // the storm swallows the world
    if (g.pl.water <= 0 && g.pl.litany_turns == 0) return tune::kFovDehydrated;
    // Indoors (sietch, caves) the interior is lit day or night; the enclosed
    // room is fully revealed by reveal_room regardless of this radius.
    if (zone().at(player_ent().x, player_ent().y) & map::TF_INDOOR) return tune::kFovIndoor;
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

void place_npc_near(EType type, map::Zone& z, uint8_t zone_id, int px, int py, int16_t data = 0) {
    for (int r = 0; r < 8; ++r)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                const int x = px + dx, y = py + dy;
                if (x < 1 || x >= map::W - 1 || y < 1 || y >= map::H - 1) continue;
                if (!z.walkable(x, y) || entity_at(x, y, zone_id)) continue;
                if (Entity* e = alloc_entity()) {
                    *e = Entity{type, Ai::Idle, zone_id, int16_t(x), int16_t(y), 10, 0, data};
                    return;
                }
            }
}

// Drop a decor prop on a sietch floor tile (skips solid rock), tagging the tile
// blocking/opaque per its DecorDef so props are solid and racks/banners occlude.
void add_decor(uint8_t prop, int x, int y) {
    map::Zone& z = zone();  // B1.2: sietch decor lands on the resident zone
    if (x < 1 || x >= map::W - 1 || y < 1 || y >= map::H - 1) return;
    const int idx = y * map::W + x;
    if (!(z.flags[idx] & map::TF_INDOOR) || (z.flags[idx] & map::TF_BLOCK)) return;
    g.decor.push_back(Decor{int16_t(x), int16_t(y), prop});
    const DecorDef& d = kDecorProps[prop];
    if (d.block) z.flags[idx] |= map::TF_BLOCK;
    if (d.opaque) z.flags[idx] |= map::TF_OPAQUE;
}

// Furnish the sietch warren chambers. Deterministic. Props sit against the back
// (north) and side walls of each chamber — where furniture actually goes — so the
// hub reads as furnished rather than props scattered mid-floor. Rugs lie flat in
// the open. (Chamber-relative: north = low y = the far wall the camera sees behind.)
void place_sietch_decor() {
    g.decor.clear();
    // Only clearly-NON-human props (rug 0, brazier 1, urn 4, baskets 6, table 7,
    // spice sacks 8). The weapon/stillsuit racks and banners were figure-height and
    // read as extra standing people (the "copy-paste NPC" clusters), so they're out.
    // great hall: a big central rug, braziers by the north wall, a table
    add_decor(0, 32, 31);                                  // central rug
    add_decor(1, 24, 23);  add_decor(1, 40, 23);          // braziers (north corners)
    add_decor(7, 25, 24);                                  // table (against NW wall)
    // cistern chamber: urns flanking the basin
    add_decor(4, 30, 12);  add_decor(4, 34, 12);
    // living quarters: a sleeping rug + baskets + a warm brazier
    add_decor(0, 12, 32);  add_decor(6, 8, 25);  add_decor(1, 18, 26);
    // storeroom: spice sacks + baskets + an urn along the back walls
    add_decor(8, 46, 23);  add_decor(8, 55, 23);  add_decor(6, 46, 34);
    add_decor(4, 55, 30);
    // armory: a low rug + baskets (no figure-like racks)
    add_decor(0, 20, 47);  add_decor(6, 16, 43);  add_decor(6, 25, 51);
    // sleeping chamber: a rug + baskets
    add_decor(0, 46, 46);  add_decor(6, 42, 42);  add_decor(6, 50, 50);
}

void spawn_zone_wildlife(map::Zone& z, uint8_t zone_id) {
    if (g.spawned_mask & (1 << zone_id)) return;
    g.spawned_mask |= uint8_t(1 << zone_id);
    if (zone_id == map::kZoneSietch) {
        place_sietch_decor();
        // vendors, spread across the warren
        place_npc_near(EType::Paul, z, zone_id, 32, 22);       // Muad'Dib: the vision ritual
        place_npc_near(EType::Naib, z, zone_id, 30, 30);       // great hall
        place_npc_near(EType::Sayyadina, z, zone_id, 32, 16);  // cistern chamber
        place_npc_near(EType::Smuggler, z, zone_id, 50, 28);   // storeroom
        place_npc_near(EType::Vault, z, zone_id, 52, 25);      // G3: the sietch vault (its own storeroom prop)
        // dialog-only Fremen — one per chamber, each in open floor well away from
        // the vendors and the wall-hugging props (so no NPC+prop "huddle" reads as
        // a cluster), each a distinct tribesperson (data = sprite/bark index).
        place_npc_near(EType::Fremen, z, zone_id, 13, 30, 0);  // living quarters
        place_npc_near(EType::Fremen, z, zone_id, 20, 47, 1);  // armory
        place_npc_near(EType::Fremen, z, zone_id, 46, 47, 2);  // sleeping chamber
        place_npc_near(EType::Fremen, z, zone_id, 53, 32, 3);  // storeroom
        place_npc_near(EType::Fremen, z, zone_id, 36, 28, 4);  // great hall (east)
        place_npc_near(EType::Fremen, z, zone_id, 26, 32, 5);  // great hall (west)
        // The War Council table — appears only once the Jihad opens (the Baron has fallen). Gated so
        // pre-endgame play (and the sietch-bound determinism tests) never see it.
        if (endgame_open()) place_npc_near(EType::WarRoom, z, zone_id, 38, 24);
        return;
    }
    if (zone_id == map::kZoneCave) return;  // B9: caves are hand-populated (see populate_cave)
    if (zone_id == map::kZoneDesert)
        place_npc_near(EType::Smuggler, z, zone_id, 12, 30);  // camp off the door road
    const int want = zone_id == map::kZoneDeepPan ? 4 : tune::kHoundCount;
    int placed = 0, guard = 0;
    while (placed < want && ++guard < 2000) {
        const int x = g.rng.world.range(8, map::W - 2);
        const int y = g.rng.world.range(2, map::H - 2);
        if (!z.walkable(x, y) || entity_at(x, y, zone_id)) continue;
        Entity* e = alloc_entity();
        if (!e) break;
        *e = Entity{EType::Hound, Ai::Wander, zone_id,
                    int16_t(x), int16_t(y), int16_t(foe_hp(EType::Hound)), 0, 0};
        ++placed;
    }
    // ambush adders (wormier zones have more) and the odd spice-mad hermit
    const int adders = zone_id == map::kZoneDeepPan ? 3 : 1;
    for (int i = 0, tries = 0; i < adders && tries < 400; ++tries) {
        const int x = g.rng.world.range(6, map::W - 2);
        const int y = g.rng.world.range(2, map::H - 2);
        if (!z.walkable(x, y) || entity_at(x, y, zone_id)) continue;
        if (Entity* e = alloc_entity()) {
            *e = Entity{EType::PanAdder, Ai::Idle, zone_id, int16_t(x), int16_t(y), int16_t(foe_hp(EType::PanAdder)), 0, 0};
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
                            int16_t(foe_hp(EType::Hermit)), 0, int16_t(g.rng.world.range(5, 8))};
            break;
        }
}

// Place a harvest op in a zone: crawler on open sand, spotters on the nearest
// rock, an escort ring of troopers scaled by mutators and alert.
void spawn_op_in(map::Zone& z, uint8_t zone_id, uint8_t mutators) {

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

    // escort scaled by mutators/alert; elites bring heavy-armor bulwark troopers
    const bool elite = (mutators & (campaign::MUT_ELITE | campaign::MUT_CLIMAX)) ||
                       g.season.alert >= campaign::kAlertEliteAt;
    const int troopers = (mutators & campaign::MUT_CLIMAX) ? 5 : elite ? 4 : 3;
    const int shields = (mutators & campaign::MUT_CLIMAX) ? 2 : elite ? 1 : 0;
    for (int i = 0, tries = 0; i < troopers + shields && tries < 300; ++tries) {
        const int x = ax + g.rng.world.range(-4, 3);
        const int y = ay + g.rng.world.range(-4, 3);
        if (!z.walkable(x, y) || entity_at(x, y, zone_id) || g.op.tile_on(x, y)) continue;
        if (Entity* e = alloc_entity()) {
            const bool bulwark = i >= troopers;
            *e = Entity{bulwark ? EType::ShieldTrooper : EType::Trooper, Ai::Guard,
                        zone_id, int16_t(x), int16_t(y),
                        int16_t(foe_hp(bulwark ? EType::ShieldTrooper : EType::Trooper)), 0, 0};
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

    // B1.2: with a single resident zone, a look-ahead into a NON-current zone
    // regenerates it into a temp buffer just for placement (it rebuilds
    // deterministically when the player actually enters). Current target uses the
    // resident zone directly. (The whole op look-ahead is retired in B3.)
    static map::Zone lookahead;  // static: avoids a ~12KB stack frame
    map::Zone& tz = (target_zone == g.cur_zone) ? zone() : lookahead;
    if (target_zone != g.cur_zone) {
        int sx_, sy_;
        map::generate(lookahead, g.seed, map::ZoneId(target_zone), sx_, sy_);
        apply_deltas(lookahead, target_zone);
    }
    spawn_zone_wildlife(tz, target_zone);  // self-guards via spawned_mask
    spawn_op_in(tz, target_zone, mut);
    if (!g.op.live()) return;  // no anchor found; retry next turn

    if (g.cur_zone == target_zone) {
        // freshly spawned around the player: re-block footprint on live zone
    } else if (mut & campaign::MUT_CLIMAX) {
        g.log.push("Scouts running: the GREAT HARVEST deploys in the Deep Pan!");
        g.log.push("Twin escorts. Heavy bulwarks. End it, Fedaykin.");
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

    // Y4: side objectives bloom and wither on their own clocks
    if (g.sideobj_kind == 0) {
        if (g.rng.world.chance(1, 140)) {
            g.sideobj_kind = 1 + int(g.rng.world.below(2));
            g.sideobj_zone = uint8_t(g.rng.world.chance(1, 2) ? map::kZoneDesert
                                                              : map::kZoneDeepPan);
            g.sideobj_x = int16_t(g.rng.world.range(8, map::W - 8));
            g.sideobj_y = int16_t(g.rng.world.range(8, map::H - 8));
            g.sideobj_expires = g.pl.turn + 220;
            g.sideobj_spawned = false;
            g.log.push(g.sideobj_kind == 1
                           ? "Word passes: a smuggler CARAVAN camps in the %s."
                           : "Word passes: a GUILD ENVOY waits in the %s. Bring spice.",
                       g.sideobj_zone == map::kZoneDesert ? "Rimwall" : "Deep Pan");
        }
    } else if (g.pl.turn >= g.sideobj_expires) {
        g.log.push(g.sideobj_kind == 1 ? "The caravan has moved on."
                                       : "The Guild envoy departs, unpaid.");
        for (Entity& e : g.ents)
            if (e.type == EType::Caravan || e.type == EType::Envoy) e.type = EType::None;
        g.sideobj_kind = 0;
    } else if (!g.sideobj_spawned && g.cur_zone == g.sideobj_zone) {
        // materialize the meeting when you enter its region
        map::Zone& z2 = zone();
        for (int r2 = 0; r2 < 10 && !g.sideobj_spawned; ++r2)
            for (int dy2 = -r2; dy2 <= r2 && !g.sideobj_spawned; ++dy2)
                for (int dx2 = -r2; dx2 <= r2; ++dx2) {
                    const int x = g.sideobj_x + dx2, y = g.sideobj_y + dy2;
                    if (x < 1 || x >= map::W - 1 || y < 1 || y >= map::H - 1) continue;
                    if (!z2.walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
                    for (Entity& e : g.ents)
                        if (e.type == EType::None) {
                            e = Entity{g.sideobj_kind == 1 ? EType::Caravan : EType::Envoy,
                                       Ai::Idle, g.cur_zone, int16_t(x), int16_t(y),
                                       10, 0, 0};
                            g.sideobj_x = int16_t(x);
                            g.sideobj_y = int16_t(y);
                            g.sideobj_spawned = true;
                            break;
                        }
                    if (g.sideobj_spawned) break;
                }
    }

    schedule_ops();
}

void apply_deltas(map::Zone& z, uint8_t zone_id) {
    for (int i = 0; i < g.delta_count; ++i)
        if (g.deltas[i].zone == zone_id)
            z.floor[g.deltas[i].y * map::W + g.deltas[i].x] = g.deltas[i].floor;
}

// Sun-tracked shade: shadows fall away from the sun. Morning sun in the east
// casts westward shade; afternoon the reverse; midday almost none.
void recompute_shade(uint8_t zone_id, int hour) {
    map::Zone& z = zone();  // B1.2: shade always applies to the resident zone
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
        srcs[n++] = {g.op.x, g.op.y, 8, uint8_t(25 + g.rng.fx.below(3)), 0};  // floodlights flicker

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

// B7: lay the wandering vendor's footprint trail. Reuses the player-centered dmap just computed:
// walk from the vendor tile steepest-downhill (toward the player) collecting tiles — the shortest
// path between them, no extra Dijkstra, RNG-free. Only when a vendor is materialized in this zone.
void compute_footprints() {
    g.footprint_count = 0;
    // Target precedence: an explicit way-finding target (rendezvous / B10 POI) wins; otherwise fall
    // back to the wandering sideobj vendor materialized in this zone (derived, no persistence needed).
    int cx, cy;
    if (g.footprint_tx >= 0) { cx = g.footprint_tx; cy = g.footprint_ty; }
    else if (g.sideobj_spawned && g.sideobj_zone == g.cur_zone) { cx = g.sideobj_x; cy = g.sideobj_y; }
    else return;
    const int gx = cx, gy = cy;  // the target tile (for skip-own-tile below)
    if (cx < 0 || cx >= map::W || cy < 0 || cy >= map::H) return;
    if (g.dmap[cy * map::W + cx] < 0) return;  // target unreachable — no trail
    const Entity& p = player_ent();
    for (int steps = 0; steps < GameState::kMaxFootprints; ++steps) {
        if (cx == p.x && cy == p.y) break;                 // reached the player
        if (!(cx == gx && cy == gy)) {                     // skip the target's own tile
            g.footprints_x[g.footprint_count] = int16_t(cx);
            g.footprints_y[g.footprint_count] = int16_t(cy);
            ++g.footprint_count;
        }
        int best = g.dmap[cy * map::W + cx], bx = cx, by = cy;  // step toward the player (lower dmap)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = cx + dx, ny = cy + dy;
                if (nx < 0 || nx >= map::W || ny < 0 || ny >= map::H) continue;
                const int d = g.dmap[ny * map::W + nx];
                if (d >= 0 && d < best) { best = d; bx = nx; by = ny; }
            }
        if (bx == cx && by == cy) break;  // no downhill step (stuck)
        cx = bx; cy = by;
    }
}

void refresh_fov_and_paths() {
    map::compute_fov(zone(), player_ent().x, player_ent().y, fov_radius());
    // An enclosed indoor room is lit whole (no black no-man's-land in the sietch).
    map::reveal_room(zone(), player_ent().x, player_ent().y);
    map::compute_dijkstra(zone(), player_ent().x, player_ent().y, g.dmap);
    compute_footprints();  // B7: vendor trail (derived from dmap)
    recompute_light();
}

// C5b: the Sietch talent tree. A meta tree bought with Litrejons (the tribe's water), in two tracks —
// SANDCRAFT (the warrior's edge) and TRIBE'S BLESSING (the tribe's sustain). Each node grants ranks of
// an AffixKind bonus folded into affix_total(), so every derived stat picks it up; ranks persist across
// death (meta). Cost per rank = cost_base * (rank+1) Litrejons.
struct SietchNode { const char* name; uint8_t kind; int per_rank; uint8_t track; uint8_t max_rank; int cost_base; };
inline constexpr SietchNode kSietchNodes[] = {
    // --- track 0: SANDCRAFT ---
    {"Blade Discipline", items::AK_INC_DMG,   5, 0, 3, 20},  // +5% Lethality / rank
    {"Killer's Eye",     items::AK_CRIT,      3, 0, 3, 20},  // +3% Vital Strike chance / rank
    {"Spice Sense",      items::AK_SPICE_FIND, 12, 0, 3, 15}, // +12% Spice Find / rank
    {"Sure Strike",      items::AK_INC_TOHIT, 6, 0, 3, 15},  // +6% Precision / rank
    // --- track 1: TRIBE'S BLESSING ---
    {"Tribe's Vigor",    items::AK_INC_LIFE,  6, 1, 3, 20},  // +6% Life / rank
    {"Stillsuit Weave",  items::AK_ARMOR,     4, 1, 3, 20},  // +4 armor / rank
    {"Cistern Share",    items::AK_WATER,    40, 1, 3, 15},  // +40 max Water / rank
    {"Prescient Calm",   items::AK_SHIELD,   15, 1, 3, 20},  // +15 Focus / rank
};
inline constexpr int kSietchNodeCount = int(sizeof kSietchNodes / sizeof *kSietchNodes);

// C4: the passive skill tree — one shared node graph, a class-start anchor per corner, adjacency-gated
// allocation, one point per level. Each allocated node grants an AffixKind bonus folded into
// affix_total() (like the sietch tree). The 3 START nodes are pure anchors (value 0) so a fresh
// auto-allocated character perturbs no derived stat (dettest stays put). Up to 64 nodes (a u64 mask).
struct PassiveNode { const char* name; uint8_t x, y, kind; int value; int8_t start_class; };
inline constexpr PassiveNode kPassiveNodes[] = {
    {"Knifehand",       3,  3,  items::AK_ADD_ALL,    0,  0},  // 0  class-start anchors (no bonus)
    {"Wanderer",        13, 3,  items::AK_ADD_ALL,    0,  1},  // 1
    {"Oracle",          8,  14, items::AK_ADD_ALL,    0,  2},  // 2
    {"The Weirding Way",8,  8,  items::AK_ADD_ALL,    2, -1},  // 3  central hub (+2 all attributes)
    {"Killer's Edge",   5,  4,  items::AK_INC_DMG,    6, -1},  // 4  Knifehand arm
    {"Bloodletting",    6,  6,  items::AK_CRIT,       4, -1},  // 5
    {"War Discipline",  7,  7,  items::AK_INC_DMG,    6, -1},  // 6
    {"Scarred Hide",    3,  6,  items::AK_HP,        20, -1},  // 7  (K leaf)
    {"Sure Foot",       11, 4,  items::AK_INC_DODGE,  6, -1},  // 8  Wanderer arm
    {"Keen Aim",        10, 6,  items::AK_INC_TOHIT,  6, -1},  // 9
    {"Sand Sense",      9,  7,  items::AK_INC_DODGE,  6, -1},  // 10
    {"Storm-ward",      13, 6,  items::AK_RES_STORM, 12, -1},  // 11 (W leaf)
    {"Inner Calm",      7,  11, items::AK_SHIELD,    10, -1},  // 12 Oracle arm
    {"Deep Will",       8,  10, items::AK_WILL,       8, -1},  // 13
    {"Prescience",      8,  9,  items::AK_INC_FOCUS, 10, -1},  // 14
    {"Water Discipline",5,  12, items::AK_WATER,     40, -1},  // 15 (O leaf)
    {"Bladestorm",      6,  8,  items::AK_CRIT_MULT, 12, -1},  // 16 notable (behind center)
    {"Ghoststep",       10, 8,  items::AK_INC_DODGE, 10, -1},  // 17 notable
};
inline constexpr int kPassiveNodeCount = int(sizeof kPassiveNodes / sizeof *kPassiveNodes);
inline constexpr uint8_t kPassiveEdges[][2] = {
    {0,4},{4,5},{5,6},{6,3},{4,7},          // Knifehand arm -> center + leaf
    {1,8},{8,9},{9,10},{10,3},{8,11},       // Wanderer arm -> center + leaf
    {2,12},{12,13},{13,14},{14,3},{12,15},  // Oracle arm -> center + leaf
    {6,16},{16,3},{10,17},{17,3},           // notables gated behind the center
};
inline constexpr int kPassiveEdgeCount = int(sizeof kPassiveEdges / sizeof *kPassiveEdges);

// meta.dat: cross-run memory. B5 grows it to hold the Awaken ladder, a banked spice wallet,
// and the PERSISTENT character (level/attributes/gear survive death — the ARPG spine).
struct Meta {
    uint32_t runs = 0;
    uint8_t endings_seen = 0;      // bit per campaign::Ending
    // G3 (DMEG): the Vault is a set of PoE-style stash tabs, each a 12x12 grid. `stash_tabs` owned (>=1),
    // bought with spice. (The old flat stash[24]/stash_count retire; old saves migrate into tab 0.)
    uint8_t stash_tabs = 1;
    items::Inst stash[tune::kMaxStashTabs][tune::kStashCap];
    uint8_t stash_cell[tune::kMaxStashTabs][tune::kStashCap] = {};
    // --- B5 (DME3) ---
    uint8_t awaken_unlocked = 1;   // highest Awaken level the player may launch (1..10)
    uint8_t awaken_cleared = 0;    // highest pinnacle defeated
    uint8_t has_char = 0;          // 1 once a persistent character exists
    int32_t banked_spice = 0;      // safe wallet; run spice banks here on extraction
    int32_t litrejons = 0;         // C5 (DMEH): tribal water banked from the Deathstill; the sietch-tree currency
    uint8_t sietch_rank[kSietchNodeCount] = {};  // C5b (DMEI): allocated ranks per sietch-tree node
    uint64_t passives = 0;                       // C4 (DMEJ): the character's allocated passive-tree nodes
    CharStats st;                  // persistent level / attributes / talents / xp
    items::Inst equip[int(items::Slot::Count)];
    items::Inst inv[tune::kInvCap];              // G (DMEE): the persisted pack pool (8x12 grid)
    uint8_t inv_cell[tune::kInvCap] = {};        // grid cell of pool entry i
    uint16_t cur_tab[items::CUR_COUNT] = {};     // G2 (DMEF): persisted crafting-table currency slots
    // --- C3 (DME5) --- base class + ascendancy persist across death (character identity)
    uint8_t base_class = 0;        // BaseClass
    uint8_t ascendancy = 0;        // Ascendancy (0 = None until the rite)
    // --- B6 (DME6) --- the Guild tithe: an optional difficulty/reward dial
    uint8_t guild_debt = 0;        // how overdue the tithe is (0 = paid up; higher = harder + richer)
    uint8_t guild_broken = 0;      // 1 once the Guild Navigator falls -> raises the debt cap
    // --- Desert Trials (DME7) --- the ascension gate (replaces the placeholder Awaken>=2 gate).
    // A bitmask of the three ordeals passed in a vision-run; all three open the Awakening rite.
    uint8_t trials_done = 0;
    // --- Endgame (DME8) --- the Jihad. Unlocked when the Baron falls; the player commissions strikes
    // that build directed Jihad-pressure per Great Power until that power's homeworld pinnacle opens.
    // Each pinnacle yields a Golden Path shard; all five open the capstone (the alternate-Paul mirror).
    uint8_t chosen_path = 0;         // 1 once the Baron falls: the Jihad (endgame) opens
    uint8_t jihad_pressure[5] = {0, 0, 0, 0, 0};  // per-faction directed pressure (0..kPinnaclePressure)
    uint8_t pinnacle_cleared = 0;    // bit per faction: its homeworld pinnacle defeated
    uint8_t golden_shards = 0;       // bit per faction: Golden Path shard taken from that pinnacle
    uint8_t capstone_done = 0;       // 1 once the alternate Paul (the Golden Path) is refused
    // --- The Long Ride (DME9) ---
    uint8_t maker_hides = 0;         // Jacurutu jackpots banked (the Divine reagent; D2 crafting)
    uint8_t best_ride_leg = 0;       // deepest Long Ride leg reached (a persistent record)
    // --- Landsraad / CHOAM (DME10) --- the Imperial politics layer, powered by CHOAM. House alliances
    // persist; CHOAM control is DERIVED from toppled homeworlds (conquest) + courted Houses (diplomacy).
    uint8_t house_allied = 0;        // bit per faction: courted as a CHOAM ally
    // --- D2 crafting (DME11) --- retired counters (kept for DMEB back-compat; migrated to currency).
    uint8_t worm_teeth = 0;
    // --- D3e (DMED) --- Spice Essence: an ascension resource (NOT a bench currency). A rare deep-desert
    // find; the Awakening rite can be opened by it as an alternative to the Desert Trials, and it is
    // consumed when so used.
    uint8_t spice_essence = 0;
};
Meta g_meta;

// The five Great Powers of the Jihad endgame (the pinnacle factions).
enum Faction : uint8_t {
    FAC_BENE_GESSERIT = 0,  // Wallach IX — the Mother School
    FAC_TLEILAXU,           // Bandalong — the axlotl labs
    FAC_IXIAN,              // Ix — the forbidden machine-halls
    FAC_CORRINO,            // Kaitain / Salusa Secundus — Shaddam + Sardaukar
    FAC_GUILD,              // Junction — the Navigator (kill -> guild_broken)
    FAC_COUNT,
};
constexpr uint8_t kAllShards = (1 << FAC_COUNT) - 1;  // 0b11111

// Desert Trials: the three ordeals of the deep desert (PoE Lab-Trials analog). Passing all three
// earns the Awakening (the sietch ascension rite). Faced inside vision-runs, persisted in meta.
enum TrialBit : uint8_t {
    TRIAL_SANDWALK = 1 << 0,  // cross a leg unheard (reach the forward door in sandwalk)
    TRIAL_STORM    = 1 << 1,  // ride out a Coriolis storm and live
    TRIAL_WORM     = 1 << 2,  // survive a Maker surfacing at your heels
    TRIAL_ALL      = 0x7,
};

// B6: the Guild-tithe difficulty level = the overdue debt, clamped to the active cap (campaign, or a
// higher endgame cap once the Guild Navigator has fallen). 0 = baseline (no scaling; determinism-safe).
int guild_difficulty() {
    const int cap = g_meta.guild_broken ? tune::kGuildDebtEndgameCap : tune::kGuildDebtCampaignCap;
    int d = g_meta.guild_debt;
    return d < 0 ? 0 : d > cap ? cap : d;
}

inline constexpr uint32_t kMetaVJ = 0x4A454D44u;  // 'DMEJ' (C4: + passives, the passive-tree allocation mask)
inline constexpr uint32_t kMetaVI = 0x49454D44u;  // 'DMEI' (C5b: + sietch_rank, the sietch talent tree)
inline constexpr uint32_t kMetaVH = 0x48454D44u;  // 'DMEH' (C5: + litrejons, the sietch-tree currency)
inline constexpr uint32_t kMetaVG = 0x47454D44u;  // 'DMEG' (G3: multi-tab grid stash + stash_tabs)
inline constexpr uint32_t kMetaVF = 0x46454D44u;  // 'DMEF' (G2: + cur_tab, the crafting-table currency slots)
inline constexpr uint32_t kMetaVE = 0x45454D44u;  // 'DMEE' (G: pack grid 12->96 + inv_cell)
inline constexpr uint32_t kMetaVD = 0x44454D44u;  // 'DMED' (D3e: + spice_essence, the Awakening resource)
inline constexpr uint32_t kMetaVC = 0x43454D44u;  // 'DMEC' (D3: widened Inst — currency stacks in inv)
inline constexpr uint32_t kMetaVB = 0x42454D44u;  // 'DMEB' (D2 crafting: + worm_teeth)
inline constexpr uint32_t kMetaVA = 0x41454D44u;  // 'DMEA' (Landsraad/CHOAM: + house_allied)
inline constexpr uint32_t kMetaV9 = 0x39454D44u;  // 'DME9' (Long Ride: + maker_hides / best_ride_leg)
inline constexpr uint32_t kMetaV8 = 0x38454D44u;  // 'DME8' (endgame: + Jihad pressure/pinnacles/shards)
inline constexpr uint32_t kMetaV7 = 0x37454D44u;  // 'DME7' (Desert Trials: + trials_done)
inline constexpr uint32_t kMetaV6 = 0x36454D44u;  // 'DME6' (B6: + guild tithe debt / broken)
inline constexpr uint32_t kMetaV5 = 0x35454D44u;  // 'DME5' (C3: + base class / ascendancy)
inline constexpr uint32_t kMetaV4 = 0x34454D44u;  // 'DME4' (D1: widened Inst)
inline constexpr uint32_t kMetaV3 = 0x33454D44u;  // 'DME3'
inline constexpr uint32_t kMetaV2 = 0x32454D44u;  // 'DME2'
inline constexpr uint32_t kMetaV1 = 0x54454D44u;  // 'DMET'

// D1 back-compat: the pre-D1 4-byte item + the old fixed prefix/suffix tables (kind,power),
// baked in here so a DME2/DME3 vault converts to the widened Inst without losing banked stats.
struct LegacyInst { uint8_t def, prefix, suffix, rarity; };
items::Inst convert_legacy(const LegacyInst& L) {
    items::Inst it;
    if (L.def == 0xFF || L.def >= items::kDefCount) return it;
    static const struct { uint8_t k; int16_t p; } OP[8] = {
        {items::AK_DMG, 2}, {items::AK_ARMOR, 2}, {items::AK_WATER, 150}, {items::AK_TOHIT, 8},
        {items::AK_NOISE, 3}, {items::AK_HP, 6}, {items::AK_RECLAIM, 1}, {items::AK_HEAT, 1}};
    static const struct { uint8_t k; int16_t p; } OS[8] = {
        {items::AK_TOHIT, 10}, {items::AK_NOISE, 4}, {items::AK_TOX, 20}, {items::AK_DODGE, 8},
        {items::AK_WATER, 250}, {items::AK_HP, 10}, {items::AK_RECLAIM, 1}, {items::AK_HEAT, 1}};
    it.def = L.def;
    it.rarity = L.rarity <= items::R_RARE ? L.rarity : items::R_COMMON;
    it.ilvl = uint8_t(it.d().level < 1 ? 1 : it.d().level);
    if (L.prefix < 8) { it.pre[0] = {OP[L.prefix].k, 1, OP[L.prefix].p}; it.nprefix = 1; }
    if (L.suffix < 8) { it.suf[0] = {OS[L.suffix].k, 1, OS[L.suffix].p}; it.nsuffix = 1; }
    return it;
}

// D3 back-compat: DME4..DMEB stored the pre-D3 (D1-widened) Inst. Read those blobs as OldInst and
// convert to the widened D3 Inst (currency/stack/fractured/istate default 0 = plain gear).
struct OldInst {
    uint8_t def, rarity, ilvl, nprefix, nsuffix, rare_name, quality, enchant;
    items::RolledAffix pre[items::kMaxPrefix];
    items::RolledAffix suf[items::kMaxSuffix];
};
items::Inst from_old_inst(const OldInst& o) {
    items::Inst it;
    it.def = o.def; it.rarity = o.rarity; it.ilvl = o.ilvl;
    it.nprefix = o.nprefix; it.nsuffix = o.nsuffix; it.rare_name = o.rare_name;
    it.quality = o.quality; it.enchant = o.enchant;
    for (int i = 0; i < items::kMaxPrefix; ++i) it.pre[i] = o.pre[i];
    for (int i = 0; i < items::kMaxSuffix; ++i) it.suf[i] = o.suf[i];
    return it;
}
template <int N>
void read_inst_blob(FILE* f, items::Inst (&dst)[N], bool current) {
    if (current) { (void)!std::fread(dst, sizeof dst, 1, f); return; }
    OldInst old[N];
    (void)!std::fread(old, sizeof old, 1, f);
    for (int i = 0; i < N; ++i) dst[i] = from_old_inst(old[i]);
}
void regrid_pool(items::Inst* pool, uint8_t* cell, int cap, int W, int H);  // G: fwd (defined below)
// D3/G2: add `amount` of currency `kind` to the persisted crafting-table slots. Used to migrate the old
// Maker Hide / Worm Tooth counters into currency (their new roles are armour / weapon quality).
void meta_grant_currency(uint8_t kind, int amount) {
    if (amount <= 0 || kind == 0 || kind >= items::CUR_COUNT) return;
    g_meta.cur_tab[kind] = uint16_t(g_meta.cur_tab[kind] + amount);
}

void meta_save();  // fwd (defined below) — buy_sietch_node banks the purchase immediately
// C5b: the sietch tree's total contribution of AffixKind `kind` (summed over allocated node ranks).
int sietch_affix_bonus(uint8_t kind) {
    int p = 0;
    for (int i = 0; i < kSietchNodeCount; ++i)
        if (kSietchNodes[i].kind == kind) p += g_meta.sietch_rank[i] * kSietchNodes[i].per_rank;
    return p;
}
int sietch_node_cost(int idx) {  // Litrejons for the NEXT rank of node idx (0 if maxed / invalid)
    if (idx < 0 || idx >= kSietchNodeCount || g_meta.sietch_rank[idx] >= kSietchNodes[idx].max_rank) return 0;
    return kSietchNodes[idx].cost_base * (g_meta.sietch_rank[idx] + 1);
}
bool buy_sietch_node(int idx) {  // spend Litrejons to rank up a node; persists
    const int cost = sietch_node_cost(idx);
    if (cost <= 0 || g_meta.litrejons < cost) return false;
    g_meta.litrejons -= cost;
    ++g_meta.sietch_rank[idx];
    meta_save();
    return true;
}

// ---- C4: the passive skill tree (per-character; allocations live in g.pl.passives, persist on extract) ----
int passive_start_of(int base_class) {  // the anchor node for a base class (-1 if none)
    for (int i = 0; i < kPassiveNodeCount; ++i) if (kPassiveNodes[i].start_class == base_class) return i;
    return -1;
}
bool passive_has(int n) { return n >= 0 && n < kPassiveNodeCount && (g.pl.passives & (1ull << n)); }
void passive_ensure_start() {  // the character's own class-start anchor is always allocated (free)
    const int s = passive_start_of(int(g.pl.base_class));
    if (s >= 0) g.pl.passives |= (1ull << s);
}
int passive_affix_bonus(uint8_t kind) {  // sum of every allocated node's bonus of this AffixKind
    int p = 0;
    for (int i = 0; i < kPassiveNodeCount; ++i)
        if (passive_has(i) && kPassiveNodes[i].kind == kind) p += kPassiveNodes[i].value;
    return p;
}
int passive_points_available() {  // earned by level, minus spent (the free own-start does not count)
    const int earned = (g.pl.st.level - 1) * tune::kPassivePerLevel;
    int spent = __builtin_popcountll(g.pl.passives);
    if (passive_has(passive_start_of(int(g.pl.base_class)))) --spent;  // the anchor is free
    const int avail = earned - spent;
    return avail < 0 ? 0 : avail;
}
bool passive_adjacent_to_allocated(int n) {  // is n on an edge with an already-allocated node?
    for (int e = 0; e < kPassiveEdgeCount; ++e) {
        const int a = kPassiveEdges[e][0], b = kPassiveEdges[e][1];
        if (a == n && passive_has(b)) return true;
        if (b == n && passive_has(a)) return true;
    }
    return false;
}
bool passive_allocatable(int n) {  // unallocated, reachable from your tree, and you have a point
    return n >= 0 && n < kPassiveNodeCount && !passive_has(n) &&
           passive_points_available() > 0 && passive_adjacent_to_allocated(n);
}
bool passive_alloc(int n) {  // allocate node n (persists to meta on extraction, like level/gear)
    if (!passive_allocatable(n)) return false;
    g.pl.passives |= (1ull << n);
    return true;
}

void meta_load() {
    FILE* f = std::fopen("meta.dat", "rb");
    if (!f) return;
    uint32_t magic = 0;
    if (std::fread(&magic, 4, 1, f) == 1) {
        if (magic >= kMetaV4 && magic <= kMetaVJ && magic != kMetaV1) {  // native widened-Inst format (DME4..DMEJ)
            const bool cur = (magic >= kMetaVC);  // DMEC+ = new Inst layout; older = OldInst (convert)
            (void)!std::fread(&g_meta.runs, 4, 1, f);
            (void)!std::fread(&g_meta.endings_seen, 1, 1, f);
            if (magic >= kMetaVG) {  // G3: the multi-tab grid stash (Inst layout is current here)
                (void)!std::fread(&g_meta.stash_tabs, 1, 1, f);
                (void)!std::fread(g_meta.stash, sizeof g_meta.stash, 1, f);
                (void)!std::fread(g_meta.stash_cell, sizeof g_meta.stash_cell, 1, f);
            } else {  // DMEF and earlier: a flat 24-slot stash -> migrate the banked items into tab 0
                uint8_t old_count = 0;
                (void)!std::fread(&old_count, 1, 1, f);
                items::Inst old_stash[24];
                read_inst_blob(f, old_stash, cur);
                for (int t = 0; t < tune::kMaxStashTabs; ++t)
                    for (int i = 0; i < tune::kStashCap; ++i) g_meta.stash[t][i] = items::Inst{};
                for (int i = 0; i < old_count && i < 24; ++i) g_meta.stash[0][i] = old_stash[i];
                regrid_pool(g_meta.stash[0], g_meta.stash_cell[0], tune::kStashCap, tune::kStashW, tune::kStashH);
                g_meta.stash_tabs = 1;
            }
            (void)!std::fread(&g_meta.awaken_unlocked, 1, 1, f);
            (void)!std::fread(&g_meta.awaken_cleared, 1, 1, f);
            (void)!std::fread(&g_meta.has_char, 1, 1, f);
            (void)!std::fread(&g_meta.banked_spice, 4, 1, f);
            (void)!std::fread(&g_meta.st, sizeof g_meta.st, 1, f);
            read_inst_blob(f, g_meta.equip, cur);
            if (magic >= kMetaVE) {  // G: the pack grid is 96 pool slots + inv_cell
                read_inst_blob(f, g_meta.inv, cur);
                (void)!std::fread(g_meta.inv_cell, sizeof g_meta.inv_cell, 1, f);
            } else {  // DMED and earlier: a flat 12-slot pack -> migrate into the grid (first-fit repack)
                items::Inst old_inv[12];
                read_inst_blob(f, old_inv, cur);
                for (items::Inst& s : g_meta.inv) s = items::Inst{};
                for (int i = 0; i < 12; ++i) g_meta.inv[i] = old_inv[i];
                regrid_pool(g_meta.inv, g_meta.inv_cell, tune::kInvCap, tune::kPackW, tune::kPackH);
            }
            if (magic >= kMetaV5) {  // C3: base class + ascendancy (DME4 -> 0/None)
                (void)!std::fread(&g_meta.base_class, 1, 1, f);
                (void)!std::fread(&g_meta.ascendancy, 1, 1, f);
            }
            if (magic >= kMetaV6) {  // B6: guild tithe (DME5/DME4 leave them at 0)
                (void)!std::fread(&g_meta.guild_debt, 1, 1, f);
                (void)!std::fread(&g_meta.guild_broken, 1, 1, f);
            }
            if (magic >= kMetaV7) {  // Desert Trials (DME6 and earlier leave it at 0)
                (void)!std::fread(&g_meta.trials_done, 1, 1, f);
            }
            if (magic >= kMetaV8) {  // endgame Jihad state (DME7 and earlier -> 0)
                (void)!std::fread(&g_meta.chosen_path, 1, 1, f);
                (void)!std::fread(g_meta.jihad_pressure, sizeof g_meta.jihad_pressure, 1, f);
                (void)!std::fread(&g_meta.pinnacle_cleared, 1, 1, f);
                (void)!std::fread(&g_meta.golden_shards, 1, 1, f);
                (void)!std::fread(&g_meta.capstone_done, 1, 1, f);
            }
            if (magic >= kMetaV9) {  // The Long Ride (DME8 and earlier leave it at 0)
                (void)!std::fread(&g_meta.maker_hides, 1, 1, f);
                (void)!std::fread(&g_meta.best_ride_leg, 1, 1, f);
            }
            if (magic >= kMetaVA) {  // Landsraad/CHOAM (DME9 and earlier -> 0)
                (void)!std::fread(&g_meta.house_allied, 1, 1, f);
            }
            if (magic >= kMetaVB) {  // D2 crafting (DMEA and earlier leave it at 0)
                (void)!std::fread(&g_meta.worm_teeth, 1, 1, f);
            }
            if (magic >= kMetaVD) {  // D3e: the Awakening resource (DMEC and earlier -> 0)
                (void)!std::fread(&g_meta.spice_essence, 1, 1, f);
            }
            if (magic >= kMetaVF) {  // G2: the crafting-table currency slots (DMEE and earlier -> 0)
                (void)!std::fread(g_meta.cur_tab, sizeof g_meta.cur_tab, 1, f);
            }
            if (magic >= kMetaVH) {  // C5: banked Litrejons (DMEG and earlier -> 0)
                (void)!std::fread(&g_meta.litrejons, 4, 1, f);
            }
            if (magic >= kMetaVI) {  // C5b: the sietch talent tree ranks (DMEH and earlier -> 0)
                (void)!std::fread(g_meta.sietch_rank, sizeof g_meta.sietch_rank, 1, f);
            }
            if (magic >= kMetaVJ) {  // C4: the character's passive-tree allocation (DMEI and earlier -> 0)
                (void)!std::fread(&g_meta.passives, 8, 1, f);
            }
            // D3 migration: the old Maker Hide / Worm Tooth counters become currency (their new roles:
            // armour / weapon quality), banked to the crafting table. No-op on a DMEC+ file (already 0).
            if (g_meta.maker_hides > 0) { meta_grant_currency(items::CUR_ARMORQUAL, g_meta.maker_hides); g_meta.maker_hides = 0; }
            if (g_meta.worm_teeth > 0) { meta_grant_currency(items::CUR_WEAPONQUAL, g_meta.worm_teeth); g_meta.worm_teeth = 0; }
        } else if (magic == kMetaV3 || magic == kMetaV2) {  // legacy 4-byte items -> convert
            (void)!std::fread(&g_meta.runs, 4, 1, f);
            (void)!std::fread(&g_meta.endings_seen, 1, 1, f);
            uint8_t old_count = 0;
            (void)!std::fread(&old_count, 1, 1, f);
            LegacyInst ls[24] = {};
            (void)!std::fread(ls, sizeof ls, 1, f);
            for (int i = 0; i < old_count && i < 24; ++i) g_meta.stash[0][i] = convert_legacy(ls[i]);  // G3: tab 0
            regrid_pool(g_meta.stash[0], g_meta.stash_cell[0], tune::kStashCap, tune::kStashW, tune::kStashH);
            g_meta.stash_tabs = 1;
            if (magic == kMetaV3) {
                (void)!std::fread(&g_meta.awaken_unlocked, 1, 1, f);
                (void)!std::fread(&g_meta.awaken_cleared, 1, 1, f);
                (void)!std::fread(&g_meta.has_char, 1, 1, f);
                (void)!std::fread(&g_meta.banked_spice, 4, 1, f);
                (void)!std::fread(&g_meta.st, sizeof g_meta.st, 1, f);
                LegacyInst le[int(items::Slot::Count)] = {}, li[12] = {};
                (void)!std::fread(le, sizeof le, 1, f);
                (void)!std::fread(li, sizeof li, 1, f);
                for (int i = 0; i < int(items::Slot::Count); ++i) g_meta.equip[i] = convert_legacy(le[i]);
                for (int i = 0; i < 12; ++i) g_meta.inv[i] = convert_legacy(li[i]);
                regrid_pool(g_meta.inv, g_meta.inv_cell, tune::kInvCap, tune::kPackW, tune::kPackH);  // G
            }
        } else if (magic == kMetaV1) {  // v1: carry runs/endings forward only
            (void)!std::fread(&g_meta.runs, 4, 1, f);
            (void)!std::fread(&g_meta.endings_seen, 1, 1, f);
        }
    }
    std::fclose(f);
}

void meta_save() {
    FILE* f = std::fopen("meta.dat", "wb");
    if (!f) return;
    std::fwrite(&kMetaVJ, 4, 1, f);  // C4: DMEJ (+ passives; earlier blobs ride along)
    std::fwrite(&g_meta.runs, 4, 1, f);
    std::fwrite(&g_meta.endings_seen, 1, 1, f);
    std::fwrite(&g_meta.stash_tabs, 1, 1, f);                       // G3: owned tab count
    std::fwrite(g_meta.stash, sizeof g_meta.stash, 1, f);          // G3: per-tab grid pools
    std::fwrite(g_meta.stash_cell, sizeof g_meta.stash_cell, 1, f);  // G3: per-tab grid cells
    std::fwrite(&g_meta.awaken_unlocked, 1, 1, f);
    std::fwrite(&g_meta.awaken_cleared, 1, 1, f);
    std::fwrite(&g_meta.has_char, 1, 1, f);
    std::fwrite(&g_meta.banked_spice, 4, 1, f);
    std::fwrite(&g_meta.st, sizeof g_meta.st, 1, f);
    std::fwrite(g_meta.equip, sizeof g_meta.equip, 1, f);
    std::fwrite(g_meta.inv, sizeof g_meta.inv, 1, f);          // G: 96-slot pack pool
    std::fwrite(g_meta.inv_cell, sizeof g_meta.inv_cell, 1, f);  // G: grid cells (DMEE)
    std::fwrite(&g_meta.base_class, 1, 1, f);  // C3 (DME5)
    std::fwrite(&g_meta.ascendancy, 1, 1, f);  // C3 (DME5)
    std::fwrite(&g_meta.guild_debt, 1, 1, f);    // B6 (DME6)
    std::fwrite(&g_meta.guild_broken, 1, 1, f);  // B6 (DME6)
    std::fwrite(&g_meta.trials_done, 1, 1, f);    // Desert Trials (DME7)
    std::fwrite(&g_meta.chosen_path, 1, 1, f);           // endgame (DME8)
    std::fwrite(g_meta.jihad_pressure, sizeof g_meta.jihad_pressure, 1, f);
    std::fwrite(&g_meta.pinnacle_cleared, 1, 1, f);
    std::fwrite(&g_meta.golden_shards, 1, 1, f);
    std::fwrite(&g_meta.capstone_done, 1, 1, f);
    std::fwrite(&g_meta.maker_hides, 1, 1, f);      // The Long Ride (DME9)
    std::fwrite(&g_meta.best_ride_leg, 1, 1, f);
    std::fwrite(&g_meta.house_allied, 1, 1, f);     // Landsraad/CHOAM (DMEA)
    std::fwrite(&g_meta.worm_teeth, 1, 1, f);       // D2 crafting (DMEB)
    std::fwrite(&g_meta.spice_essence, 1, 1, f);    // D3e: the Awakening resource (DMED)
    std::fwrite(g_meta.cur_tab, sizeof g_meta.cur_tab, 1, f);  // G2: crafting-table currency (DMEF)
    std::fwrite(&g_meta.litrejons, 4, 1, f);                   // C5: banked Litrejons (DMEH)
    std::fwrite(g_meta.sietch_rank, sizeof g_meta.sietch_rank, 1, f);  // C5b: sietch tree ranks (DMEI)
    std::fwrite(&g_meta.passives, 8, 1, f);                            // C4: passive-tree mask (DMEJ)
    std::fclose(f);
}

// B5: snapshot the live character into meta so it survives death/extraction.
void persist_character() {
    g_meta.has_char = 1;
    g_meta.st = g.pl.st;
    g_meta.base_class = uint8_t(g.pl.base_class);   // C3: class identity survives death
    g_meta.ascendancy = uint8_t(g.pl.ascendancy);
    g_meta.passives = g.pl.passives;                // C4: the passive tree persists with the character
    std::memcpy(g_meta.equip, g.pl.equip, sizeof g_meta.equip);
    std::memcpy(g_meta.inv, g.pl.inv, sizeof g_meta.inv);
    std::memcpy(g_meta.inv_cell, g.pl.inv_cell, sizeof g_meta.inv_cell);  // G: grid layout persists
    std::memcpy(g_meta.cur_tab, g.pl.cur_tab, sizeof g_meta.cur_tab);     // G2: crafting-table currency persists
    meta_save();
}

// B6: the Guild tithe. Accrue on each extracted run (the Guild takes its cut of every haul); pay it
// down at the Guild rep (field envoy) or by signalling the Guild from the sietch hub.
void guild_tithe_accrue() {  // called from extract_run; persist_character() saves meta right after
    const int cap = g_meta.guild_broken ? tune::kGuildDebtEndgameCap : tune::kGuildDebtCampaignCap;
    int d = int(g_meta.guild_debt) + tune::kGuildDebtPerRun;
    g_meta.guild_debt = uint8_t(d > cap ? cap : d);
}
void guild_tithe_pay(int reduce) {
    int d = int(g_meta.guild_debt) - reduce;
    g_meta.guild_debt = uint8_t(d < 0 ? 0 : d);
    meta_save();
}

// A vision-run leg (not the sietch hub, a rendezvous, or a cave excursion) — where the Trials are faced.
bool in_vision_run() { return g.cur_node > 0 && !g.at_rendezvous && !g.in_cave; }

// Desert Trials: mark an ordeal passed. The first time all three are done, the Awakening opens.
// Only counts inside a vision-run. RNG-free + meta-only (never touches the sim hash).
void mark_trial(TrialBit bit) {
    if (!in_vision_run() || (g_meta.trials_done & bit)) return;
    g_meta.trials_done |= bit;
    static const char* kNames[] = {"the Silent Sandwalk", "the Coriolis", "the Maker's Passing"};
    const int idx = bit == TRIAL_SANDWALK ? 0 : bit == TRIAL_STORM ? 1 : 2;
    g.log.push("A Trial of the deep desert is passed: %s.", kNames[idx]);
    if ((g_meta.trials_done & TRIAL_ALL) == TRIAL_ALL)
        g.log.push("The Desert Trials are complete. The Awakening awaits you in the sietch.");
    meta_save();  // the Trials persist across death, like the character
}

// C3: the sietch rite of passage. Opened by the DESERT TRIALS (all three ordeals passed) OR, D3e, by a
// banked Spice Essence — the Water of Life shortcut, which the rite CONSUMES (the Trials path is free).
// The chosen ascendancy must belong to your base class. You ascend once; it persists across death.
bool can_ascend() {
    return g.pl.ascendancy == Ascendancy::None &&
           ((g_meta.trials_done & TRIAL_ALL) == TRIAL_ALL || g_meta.spice_essence > 0);
}
bool ascend(Ascendancy asc) {
    if (!can_ascend()) return false;
    if (int(asc) <= 0 || int(asc) >= int(Ascendancy::Count)) return false;
    if (kAscendancyBase[int(asc)] != uint8_t(g.pl.base_class)) return false;  // must match the base
    const bool by_trials = (g_meta.trials_done & TRIAL_ALL) == TRIAL_ALL;
    if (!by_trials && g_meta.spice_essence > 0) {  // the Water of Life path — spends the Essence
        g_meta.spice_essence--;
        g.log.push("You drink the Water of Life. It transforms, and does not kill you.");
    }
    g.pl.ascendancy = asc;
    g.log.push("The Awakening takes you. The sietch names you: %s.", kAscendancyNames[int(asc)]);
    persist_character();  // the naming survives death
    meta_save();          // bank the spent Essence
    return true;
}

void grant_xp(int amount) {
    if (g.pl.st.level >= 30) return;
    amount = amount * (100 + affix_total(items::AK_XP)) / 100;  // D1: Experience Gain
    g.pl.st.xp += amount;
    while (g.pl.st.xp >= xp_to_level(g.pl.st.level)) {
        g.pl.st.xp -= xp_to_level(g.pl.st.level);
        ++g.pl.st.level;
        g.pl.st.statpts += 2;
        ++g.pl.st.talentpts;
        g.pl.hp = player_max_hp();
        g.pl.shield = player_max_shield();
        g.pl.will = player_max_will();
        g.log.push("LEVEL %d. The desert tempers you. (+2 stats, +1 talent: P)",
                   int(g.pl.st.level));
        flash(240, 220, 120);
        g.services.mixer->push({audio::Cmd::PlaySfx, 1, 220, 150});
    }
}

// Y2: run difficulty — each denied harvester and every tier of alert deepens
// the loot tables. Risk feeds reward.
int run_difficulty() { return g.season.denied + g.season.alert / 25 + guild_difficulty(); }  // B6: unpaid tithe -> richer loot

// B7: signal the Spacing Guild from the sietch hub to pay the tithe down on demand (the reliable
// counterpart to bumping the wandering field Envoy). Costs spice; returns false if you can't afford it.
bool signal_guild() {
    const int cost = 8 + run_difficulty();
    if (g.pl.spice < cost) return false;
    g.pl.spice -= cost;
    guild_tithe_pay(tune::kGuildBribeReduce);
    return true;
}

// D1: item level of a drop = the current graph node's Awaken level (1..10). Deeper runs roll
// higher tiers. run_difficulty() still feeds the rarity bonus.
int drop_ilvl() {
    int a = 1;
    if (g.cur_node >= 0 && g.cur_node < world::kMaxNodes) a = g.graph.nodes[g.cur_node].awaken;
    return a < 1 ? 1 : a > 10 ? 10 : a;
}

// D3: the unified AREA LEVEL that gates currency drops + endgame loot tiers. Campaign = the node's
// Awaken depth (drop_ilvl); the endgame content never sets cur_node, so derive its level from the
// active activity (strike tier / pinnacle / capstone / ride leg). Range ~1..16.
int area_level() {
    if (g.in_capstone) return 16;
    if (g.in_pinnacle) return 14;
    if (g.in_strike) return 10 + (g.strike_tier < 1 ? 1 : g.strike_tier);   // 11..15
    if (g.in_ride) return 10 + (g.ride_leg > 6 ? 6 : g.ride_leg);           // 10..16
    return drop_ilvl();
}

// Weighted draw on the loot stream (PoE-style spawn weights).
int weighted_pick(const int* w, int n) {
    int total = 0;
    for (int i = 0; i < n; ++i) total += w[i];
    if (total <= 0) return int(g.rng.loot.below(uint32_t(n)));
    int r = int(g.rng.loot.below(uint32_t(total)));
    for (int i = 0; i < n; ++i) { if (r < w[i]) return i; r -= w[i]; }
    return n - 1;
}

// Roll `count` distinct affixes of one side (prefix/suffix) onto `it`, respecting slot-gating,
// ilvl-gated + weighted tiers, and no duplicate kinds.
void roll_one_side(items::Inst& it, int ilvl, bool prefix_side, int count) {
    const items::Slot slot = it.d().slot;
    for (int c = 0; c < count; ++c) {
        int fam_idx[items::kFamilyCount], fam_w[items::kFamilyCount], n = 0;
        for (int i = 0; i < items::kFamilyCount; ++i) {
            const items::AffixFamily& f = items::kFamilies[i];
            if (f.prefix != prefix_side || !(f.slots & items::sm(slot))) continue;
            bool dup = false;
            for (int p = 0; p < it.nprefix; ++p) if (it.pre[p].kind == f.kind) dup = true;
            for (int s = 0; s < it.nsuffix; ++s) if (it.suf[s].kind == f.kind) dup = true;
            if (dup) continue;
            fam_idx[n] = i; fam_w[n] = f.weight; ++n;
        }
        if (!n) return;
        const items::AffixFamily& f = items::kFamilies[fam_idx[weighted_pick(fam_w, n)]];
        int t_idx[items::kMaxTier], t_w[items::kMaxTier], tn = 0;
        for (int t = 0; t < items::kMaxTier; ++t)
            if (items::kTierMinIlvl[t] <= ilvl) { t_idx[tn] = t; t_w[tn] = items::kTierWeight[t]; ++tn; }
        const int t = t_idx[weighted_pick(t_w, tn)];
        items::RolledAffix ra;
        ra.kind = f.kind;
        ra.tier = uint8_t(t + 1);
        ra.value = int16_t(g.rng.loot.range(f.lo[t], f.hi[t]));
        if (prefix_side) it.pre[it.nprefix++] = ra; else it.suf[it.nsuffix++] = ra;
    }
}

// Assign rarity + roll the explicit affixes for an item already holding a `def`.
void roll_affixes(items::Inst& it, int ilvl, int rarity_bonus) {
    if (ilvl < 1) ilvl = 1;
    it.ilvl = uint8_t(ilvl > 255 ? 255 : ilvl);
    it.nprefix = it.nsuffix = 0;
    const int r = int(g.rng.loot.below(100)) - rarity_bonus;
    it.rarity = r < 10 ? items::R_RARE : r < 40 ? items::R_MAGIC : items::R_COMMON;
    if (it.rarity == items::R_MAGIC) {
        roll_one_side(it, ilvl, true, 1);
        roll_one_side(it, ilvl, false, 1);
    } else if (it.rarity == items::R_RARE) {
        const int np = 1 + int(g.rng.loot.below(items::kMaxPrefix));
        const int ns = 1 + int(g.rng.loot.below(items::kMaxSuffix));
        roll_one_side(it, ilvl, true, np);
        roll_one_side(it, ilvl, false, ns);
        it.rare_name = uint8_t(int(g.rng.loot.below(8)) | (int(g.rng.loot.below(8)) << 4));
    }
}

items::Inst roll_item(int item_level, int rarity_bonus) {
    int candidates[items::kDefCount], n = 0;
    for (int i = 0; i < items::kDefCount; ++i)
        if (items::kDefs[i].level <= item_level + 1 && items::kDefs[i].level + 3 >= item_level)
            candidates[n++] = i;
    items::Inst it;
    if (!n) return it;
    it.def = uint8_t(candidates[g.rng.loot.below(uint32_t(n))]);
    roll_affixes(it, area_level(), rarity_bonus);  // D3c: endgame gear finally rolls high tiers
    return it;
}

// ---- G: grid packing (variable-size items) ---------------------------------------------------------
// A grid is a pool of items::Inst[cap] + a parallel cell[] of top-left indices; only non-empty pool
// entries occupy space. Every helper is parameterized by (W,H) so the pack (8x12) and, later, the stash
// tabs (12x12) share one implementation. Grid position is unhashed (off the determinism firewall).
inline constexpr int kGridCellsMax = 12 * 12;  // biggest grid we build (a 12x12 stash tab)

// Occupancy of a W*H grid from every placed pool item except `ignore` (-1 = none).
void grid_occupancy(const items::Inst* pool, const uint8_t* cell, int cap, int W, int H,
                    int ignore, bool* occ) {
    for (int i = 0; i < W * H; ++i) occ[i] = false;
    for (int i = 0; i < cap; ++i) {
        if (i == ignore || pool[i].empty()) continue;
        const items::GridSize s = pool[i].size();
        const int cx = cell[i] % W, cy = cell[i] / W;
        for (int dy = 0; dy < s.h; ++dy)
            for (int dx = 0; dx < s.w; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < W && y < H) occ[y * W + x] = true;
            }
    }
}
// First-fit (row-major) top-left cell for a w x h footprint in `occ`, or -1 if it does not fit.
int grid_find_fit(const bool* occ, int W, int H, int w, int h) {
    for (int y = 0; y + h <= H; ++y)
        for (int x = 0; x + w <= W; ++x) {
            bool ok = true;
            for (int dy = 0; dy < h && ok; ++dy)
                for (int dx = 0; dx < w; ++dx)
                    if (occ[(y + dy) * W + (x + dx)]) { ok = false; break; }
            if (ok) return y * W + x;
        }
    return -1;
}
// Can footprint `s` sit with top-left at `topleft`, ignoring the item at pool index `moving`?
bool grid_can_place(const items::Inst* pool, const uint8_t* cell, int cap, int W, int H,
                    int moving, int topleft, items::GridSize s) {
    const int cx = topleft % W, cy = topleft / W;
    if (topleft < 0 || cx + s.w > W || cy + s.h > H) return false;
    bool occ[kGridCellsMax];
    grid_occupancy(pool, cell, cap, W, H, moving, occ);
    for (int dy = 0; dy < s.h; ++dy)
        for (int dx = 0; dx < s.w; ++dx)
            if (occ[(cy + dy) * W + (cx + dx)]) return false;
    return true;
}
// Add `it` to the grid at the first free pool slot + first-fit cell. Returns the pool index, or -1 if
// the grid (or pool) has no room.
int grid_add(items::Inst* pool, uint8_t* cell, int cap, int W, int H, const items::Inst& it) {
    if (it.empty()) return -1;
    bool occ[kGridCellsMax];
    grid_occupancy(pool, cell, cap, W, H, -1, occ);
    const items::GridSize s = it.size();
    const int c = grid_find_fit(occ, W, H, s.w, s.h);
    if (c < 0) return -1;
    for (int i = 0; i < cap; ++i)
        if (pool[i].empty()) { pool[i] = it; cell[i] = uint8_t(c); return i; }
    return -1;
}
// Assign every placed item in a pool a first-fit grid cell, in pool order (used to migrate a flat
// pre-grid save into the grid: the items are already loaded; only their cells need computing).
void regrid_pool(items::Inst* pool, uint8_t* cell, int cap, int W, int H) {
    bool occ[kGridCellsMax];
    for (int i = 0; i < W * H; ++i) occ[i] = false;
    for (int i = 0; i < cap; ++i) {
        if (pool[i].empty()) { cell[i] = 0; continue; }
        const items::GridSize s = pool[i].size();
        int c = grid_find_fit(occ, W, H, s.w, s.h);
        if (c < 0) c = 0;  // overflow guard (a dozen migrated items always fit an empty grid)
        cell[i] = uint8_t(c);
        const int cx = c % W, cy = c / W;
        for (int dy = 0; dy < s.h; ++dy)
            for (int dx = 0; dx < s.w; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < W && y < H) occ[y * W + x] = true;
            }
    }
}
// The pool item whose footprint covers grid cell `gc` (or -1). Used by the mouse UI + tests.
int grid_item_at(const items::Inst* pool, const uint8_t* cell, int cap, int W, int gc) {
    const int gx = gc % W, gy = gc / W;
    for (int i = 0; i < cap; ++i) {
        if (pool[i].empty()) continue;
        const items::GridSize s = pool[i].size();
        const int cx = cell[i] % W, cy = cell[i] / W;
        if (gx >= cx && gx < cx + s.w && gy >= cy && gy < cy + s.h) return i;
    }
    return -1;
}
// The pack (g.pl.inv) is the canonical 8x12 grid. Thin wrappers keep call sites terse.
int pack_add(const items::Inst& it) {
    return grid_add(g.pl.inv, g.pl.inv_cell, tune::kInvCap, tune::kPackW, tune::kPackH, it);
}
int pack_free_cells() {
    bool occ[kGridCellsMax];
    grid_occupancy(g.pl.inv, g.pl.inv_cell, tune::kInvCap, tune::kPackW, tune::kPackH, -1, occ);
    int n = 0;
    for (int i = 0; i < tune::kPackW * tune::kPackH; ++i) if (!occ[i]) ++n;
    return n;
}
// Put `it` at a caller-chosen pool index and give it a valid first-fit grid cell (used by the
// debug/setup hooks, which address the pool by index rather than by grid position).
void pack_place_at(int idx, const items::Inst& it) {
    g.pl.inv[idx] = it;
    bool occ[kGridCellsMax];
    grid_occupancy(g.pl.inv, g.pl.inv_cell, tune::kInvCap, tune::kPackW, tune::kPackH, idx, occ);
    const items::GridSize s = it.size();
    const int c = grid_find_fit(occ, tune::kPackW, tune::kPackH, s.w, s.h);
    g.pl.inv_cell[idx] = uint8_t(c < 0 ? 0 : c);
}

// ---- G3: the Vault — PoE-style stash tabs (each a 12x12 grid), bought with spice -------------------
int stash_tab_cost() {  // spice for the NEXT tab (doubling from a base); 0 if maxed out
    if (g_meta.stash_tabs >= tune::kMaxStashTabs) return 0;
    return tune::kStashTabBase << (g_meta.stash_tabs - 1);  // 2nd tab = base, 3rd = 2*base, ...
}
bool buy_stash_tab() {
    const int cost = stash_tab_cost();
    if (cost <= 0 || g.pl.spice < cost) return false;
    g.pl.spice -= cost;
    ++g_meta.stash_tabs;
    meta_save();
    return true;
}
int stash_used(int t) {  // non-empty entries in tab t
    if (t < 0 || t >= tune::kMaxStashTabs) return 0;
    int n = 0;
    for (int i = 0; i < tune::kStashCap; ++i) if (!g_meta.stash[t][i].empty()) ++n;
    return n;
}
// Deposit pack pool entry `pidx` into stash tab `t` (first-fit), freeing the pack cell. -> ok?
bool stash_deposit(int pidx, int t) {
    if (pidx < 0 || pidx >= tune::kInvCap || g.pl.inv[pidx].empty()) return false;
    if (t < 0 || t >= g_meta.stash_tabs) return false;
    if (grid_add(g_meta.stash[t], g_meta.stash_cell[t], tune::kStashCap, tune::kStashW, tune::kStashH,
                 g.pl.inv[pidx]) < 0) return false;  // no room in the tab
    g.pl.inv[pidx] = items::Inst{};
    meta_save();
    return true;
}
// Withdraw stash tab `t` pool entry `sidx` back into the pack (first-fit). -> ok?
bool stash_withdraw(int t, int sidx) {
    if (t < 0 || t >= g_meta.stash_tabs || sidx < 0 || sidx >= tune::kStashCap) return false;
    if (g_meta.stash[t][sidx].empty()) return false;
    if (pack_add(g_meta.stash[t][sidx]) < 0) return false;  // no pack room
    g_meta.stash[t][sidx] = items::Inst{};
    meta_save();
    return true;
}

// ---- G4: the PoE mouse UI — geometry + hit-testing (the render draws at these same coordinates) ----
namespace gridui {
inline constexpr int CELL = 42;                   // px per grid cell
inline constexpr int EQ_W = 300, EQ_H = 40;       // an equip-slot row box
// Inventory screen (menu 6): equip paperdoll (left column) + the 8x12 pack grid (right).
inline int inv_pack_x() { return fb::W / 2 + 40; }
inline int inv_pack_y() { return 250; }
inline int inv_eq_x()   { return fb::W / 2 - 360; }
inline int inv_eq_y()   { return 250; }
// Vault screen (menu 7): the 12x12 stash tab (left) + the pack grid (right); tab strip + Buy above.
inline int vault_stash_x() { return fb::W / 2 - 560; }
inline int vault_stash_y() { return 320; }
inline int vault_pack_x()  { return fb::W / 2 + 160; }
inline int vault_pack_y()  { return 320; }
inline int vault_tab_y()   { return vault_stash_y() - 54; }
// Bench screen (menu 11): the pack grid (left) + the currency-tab slots (right).
inline int bench_pack_x()  { return fb::W / 2 - 520; }
inline int bench_pack_y()  { return 260; }
inline int bench_tab_x()   { return fb::W / 2 + 40; }
inline int bench_tab_y()   { return 260; }
inline constexpr int TAB_COLS = 6;                 // currency-tab slot grid columns

// Which grid cell (mx,my) lands in for a W x H grid at (ox,oy), or -1.
inline int cell_at(int mx, int my, int ox, int oy, int W, int H) {
    const int cx = (mx - ox) / CELL, cy = (my - oy) / CELL;
    if (mx < ox || my < oy || cx < 0 || cy < 0 || cx >= W || cy >= H) return -1;
    return cy * W + cx;
}
}  // namespace gridui

// C4: the passive-tree screen (menu 13) — node graph geometry shared by its dispatch + render.
namespace passiveui {
inline constexpr int SCALE = 44, OY = 150, R = 16;
inline int ox() { return fb::W / 2 - 8 * SCALE; }
inline void node_screen(int i, int& sx, int& sy) {
    sx = ox() + kPassiveNodes[i].x * SCALE;
    sy = OY + kPassiveNodes[i].y * SCALE;
}
inline int node_at(int mx, int my) {  // the node whose disc contains (mx,my), or -1
    if (mx < 0) return -1;
    for (int i = 0; i < kPassiveNodeCount; ++i) {
        int sx, sy; node_screen(i, sx, sy);
        const int dx = mx - sx, dy = my - sy;
        if (dx * dx + dy * dy <= (R + 5) * (R + 5)) return i;
    }
    return -1;
}
}  // namespace passiveui

// A hit on some interactive region of a grid screen.
enum class UIHit { None, PackCell, EquipSlot, StashCell, TabButton, BuyTab, CurTabSlot };
struct UIHitResult { UIHit what = UIHit::None; int idx = -1; };

// Classify the mouse position on whichever grid screen (menu 6/7/11) is open.
UIHitResult ui_hit_test(int mx, int my) {
    using namespace gridui;
    UIHitResult h;
    if (mx < 0) return h;
    if (g.menu_open == 6) {  // inventory: pack grid + equip rows
        const int c = cell_at(mx, my, inv_pack_x(), inv_pack_y(), tune::kPackW, tune::kPackH);
        if (c >= 0) { h.what = UIHit::PackCell; h.idx = c; return h; }
        for (int i = 0; i < int(items::Slot::Count); ++i) {
            const int y = inv_eq_y() + i * (EQ_H + 4);
            if (mx >= inv_eq_x() && mx < inv_eq_x() + EQ_W && my >= y && my < y + EQ_H) {
                h.what = UIHit::EquipSlot; h.idx = i; return h;
            }
        }
    } else if (g.menu_open == 7) {  // vault: stash tab grid + pack grid + tab strip + Buy
        int c = cell_at(mx, my, vault_stash_x(), vault_stash_y(), tune::kStashW, tune::kStashH);
        if (c >= 0) { h.what = UIHit::StashCell; h.idx = c; return h; }
        c = cell_at(mx, my, vault_pack_x(), vault_pack_y(), tune::kPackW, tune::kPackH);
        if (c >= 0) { h.what = UIHit::PackCell; h.idx = c; return h; }
        for (int t = 0; t < g_meta.stash_tabs; ++t) {
            const int x = vault_stash_x() + t * 44;
            if (mx >= x && mx < x + 40 && my >= vault_tab_y() && my < vault_tab_y() + 40) {
                h.what = UIHit::TabButton; h.idx = t; return h;
            }
        }
        const int bx = vault_stash_x() + tune::kMaxStashTabs * 44 + 20;
        if (mx >= bx && mx < bx + 220 && my >= vault_tab_y() && my < vault_tab_y() + 40) {
            h.what = UIHit::BuyTab; return h;
        }
    } else if (g.menu_open == 11) {  // bench: pack grid + currency-tab slots
        int c = cell_at(mx, my, bench_pack_x(), bench_pack_y(), tune::kPackW, tune::kPackH);
        if (c >= 0) { h.what = UIHit::PackCell; h.idx = c; return h; }
        c = cell_at(mx, my, bench_tab_x(), bench_tab_y(), TAB_COLS, (items::CUR_COUNT + TAB_COLS - 1) / TAB_COLS);
        if (c >= 0 && c + 1 < items::CUR_COUNT) { h.what = UIHit::CurTabSlot; h.idx = c + 1; return h; }
    }
    return h;
}

// The pool index of the item under a PackCell / StashCell hit (or -1 if the cell is empty).
int ui_item_under(const UIHitResult& h) {
    if (h.what == UIHit::PackCell)
        return grid_item_at(g.pl.inv, g.pl.inv_cell, tune::kInvCap, tune::kPackW, h.idx);
    if (h.what == UIHit::StashCell)
        return grid_item_at(g_meta.stash[g.stash_tab], g_meta.stash_cell[g.stash_tab], tune::kStashCap,
                            tune::kStashW, h.idx);
    return -1;
}

// Currency-table / equip ops (defined below, after apply_orb) — forward-declared for ui_grid_input.
bool quickmove(int idx);
bool tab_deposit(int idx);
bool tab_withdraw(uint8_t kind, int amount);
bool bench_apply(int cur, int inv_idx);

// G4: mouse drag-drop + Ctrl-click on the grid screens (menu 6/7/11). Returns true if it consumed the
// click. All moves route through the tested model ops (quickmove / grid_can_place / tab_* / stash_*).
bool ui_grid_input(const InputState& in) {
    if (g.menu_open != 6 && g.menu_open != 7 && g.menu_open != 11) return false;
    const UIHitResult h = ui_hit_test(in.mouse_x, in.mouse_y);

    // Ctrl+left-click: instant quick-move (no drag).
    if (in.ctrl && in.mouse_pressed[0]) {
        g.drag_kind = 0; g.drag_idx = -1;
        if (g.menu_open == 6) {
            if (h.what == UIHit::PackCell) { int i = ui_item_under(h); if (i >= 0) quickmove(i); }
            else if (h.what == UIHit::EquipSlot && !g.pl.equip[h.idx].empty()) {
                if (pack_add(g.pl.equip[h.idx]) >= 0) g.pl.equip[h.idx] = items::Inst{};  // unequip
            }
        } else if (g.menu_open == 7) {
            if (h.what == UIHit::PackCell) { int i = ui_item_under(h); if (i >= 0) stash_deposit(i, g.stash_tab); }
            else if (h.what == UIHit::StashCell) { int i = ui_item_under(h); if (i >= 0) stash_withdraw(g.stash_tab, i); }
        } else {  // menu 11 bench
            if (h.what == UIHit::PackCell) { int i = ui_item_under(h); if (i >= 0 && g.pl.inv[i].is_currency()) tab_deposit(i); }
            else if (h.what == UIHit::CurTabSlot && g.pl.cur_tab[h.idx] > 0) tab_withdraw(uint8_t(h.idx), 1);
        }
        return true;
    }

    // Tab strip + Buy (menu 7): a plain left-press switches tab / buys.
    if (in.mouse_pressed[0] && g.menu_open == 7) {
        if (h.what == UIHit::TabButton) { g.stash_tab = h.idx; return true; }
        if (h.what == UIHit::BuyTab) { buy_stash_tab(); return true; }
    }

    // Begin a drag: press on an item.
    if (in.mouse_pressed[0] && g.drag_kind == 0) {
        if (h.what == UIHit::PackCell) { int i = ui_item_under(h); if (i >= 0) { g.drag_kind = 1; g.drag_idx = i; } }
        else if (h.what == UIHit::EquipSlot && !g.pl.equip[h.idx].empty()) { g.drag_kind = 2; g.drag_idx = h.idx; }
        else if (h.what == UIHit::StashCell) { int i = ui_item_under(h); if (i >= 0) { g.drag_kind = 3; g.drag_idx = i; } }
        else if (h.what == UIHit::CurTabSlot && g.pl.cur_tab[h.idx] > 0) { g.drag_kind = 4; g.drag_idx = h.idx; }
        if (g.drag_kind) return true;
    }

    // Resolve a drop on release.
    if (in.mouse_released[0] && g.drag_kind) {
        const int k = g.drag_kind, s = g.drag_idx;
        g.drag_kind = 0; g.drag_idx = -1;
        if (k == 1) {  // dragging a pack item
            if (h.what == UIHit::PackCell) {  // move within the pack grid to the hovered top-left
                if (grid_can_place(g.pl.inv, g.pl.inv_cell, tune::kInvCap, tune::kPackW, tune::kPackH,
                                   s, h.idx, g.pl.inv[s].size())) g.pl.inv_cell[s] = uint8_t(h.idx);
            } else if (h.what == UIHit::EquipSlot) {          // equip if the slot matches
                if (!g.pl.inv[s].is_currency() && !g.pl.inv[s].unidentified() &&
                    int(g.pl.inv[s].d().slot) == h.idx) quickmove(s);
            } else if (h.what == UIHit::StashCell) {          // deposit into the open tab (menu 7)
                stash_deposit(s, g.stash_tab);
            } else if (h.what == UIHit::CurTabSlot || (g.menu_open == 11 && g.pl.inv[s].is_currency())) {
                if (g.pl.inv[s].is_currency()) tab_deposit(s);  // deposit currency into the table (menu 11)
            }
        } else if (k == 2) {  // dragging an equipped item -> drop into the pack unequips
            if (h.what == UIHit::PackCell && pack_add(g.pl.equip[s]) >= 0) g.pl.equip[s] = items::Inst{};
        } else if (k == 3) {  // dragging a stash item -> the pack withdraws it (menu 7)
            if (h.what == UIHit::PackCell) stash_withdraw(g.stash_tab, s);
        } else if (k == 4) {  // dragging a currency orb from the table (menu 11)
            if (h.what == UIHit::PackCell) {
                const int over = ui_item_under(h);
                if (over >= 0 && !g.pl.inv[over].is_currency()) bench_apply(s, over);  // drop on gear = apply
                else tab_withdraw(uint8_t(s), g.pl.cur_tab[s]);                          // drop on empty = withdraw
            }
        }
        return true;
    }
    return false;
}

// ---- D2 crafting: the sietch bench spends materials to work gear -----------------------------------

// The Maker Hide (Divine reagent): reroll the VALUE of every affix within its own tier, keeping the
// affixes and their tiers. Needs at least one rolled affix. Returns false if it cannot apply.
bool craft_divine(items::Inst& it) {
    if (it.empty() || (it.nprefix + it.nsuffix) == 0) return false;
    auto reroll = [&](items::RolledAffix& ra) {
        const items::AffixFamily* fam = items::family_of(ra.kind);
        if (!fam || ra.tier < 1 || ra.tier > items::kMaxTier) return;
        ra.value = int16_t(g.rng.loot.range(fam->lo[ra.tier - 1], fam->hi[ra.tier - 1]));
    };
    for (int i = 0; i < it.nprefix; ++i) reroll(it.pre[i]);
    for (int i = 0; i < it.nsuffix; ++i) reroll(it.suf[i]);
    return true;
}

// Add one affix to `it` on a side with room (magic caps 1/1, rare caps 3/3), gated by area_level().
// Returns false if there is no legal side/family. side: -1 = either, 0 = prefix, 1 = suffix.
bool add_one_affix(items::Inst& it, int side) {
    const int cap = it.rarity == items::R_RARE ? items::kMaxPrefix : 1;
    bool canP = side != 1 && it.nprefix < cap;
    bool canS = side != 0 && it.nsuffix < cap;
    if (!canP && !canS) return false;
    const bool pre = canP && (!canS || g.rng.loot.below(2) == 0);
    const int before = pre ? it.nprefix : it.nsuffix;
    roll_one_side(it, area_level(), pre, 1);
    return (pre ? it.nprefix : it.nsuffix) > before;  // false if no legal family remained
}

// Reforge `it`'s affixes for its CURRENT rarity, preserving any fractured (locked) mods (Alteration /
// Chaos). Fractured mods keep their kind+tier+value and their locked bit; the rest are re-rolled.
void reroll_keep_rarity(items::Inst& it) {
    items::RolledAffix kp[items::kMaxPrefix], ks[items::kMaxSuffix];
    int np = 0, ns = 0;
    for (int i = 0; i < it.nprefix; ++i) if (it.frac_pre(i)) kp[np++] = it.pre[i];
    for (int i = 0; i < it.nsuffix; ++i) if (it.frac_suf(i)) ks[ns++] = it.suf[i];
    it.nprefix = it.nsuffix = 0;
    it.fractured = 0;
    for (int i = 0; i < np; ++i) { it.pre[it.nprefix] = kp[i]; it.fractured |= uint8_t(1 << it.nprefix); ++it.nprefix; }
    for (int i = 0; i < ns; ++i) { it.suf[it.nsuffix] = ks[i]; it.fractured |= uint8_t(1 << (3 + it.nsuffix)); ++it.nsuffix; }
    const int tp = it.rarity == items::R_RARE ? 1 + int(g.rng.loot.below(items::kMaxPrefix)) : 1;
    const int ts = it.rarity == items::R_RARE ? 1 + int(g.rng.loot.below(items::kMaxSuffix)) : 1;
    for (int guard = 0; it.nprefix < tp && guard < 8; ++guard) { int b = it.nprefix; roll_one_side(it, area_level(), true, 1); if (it.nprefix == b) break; }
    for (int guard = 0; it.nsuffix < ts && guard < 8; ++guard) { int b = it.nsuffix; roll_one_side(it, area_level(), false, 1); if (it.nsuffix == b) break; }
}

// Remove one random (non-fractured) affix (Annulment). Returns false if none removable.
bool remove_random_affix(items::Inst& it) {
    int idx[items::kMaxAffix], side[items::kMaxAffix], n = 0;
    for (int i = 0; i < it.nprefix; ++i) if (!it.frac_pre(i)) { idx[n] = i; side[n] = 0; ++n; }
    for (int i = 0; i < it.nsuffix; ++i) if (!it.frac_suf(i)) { idx[n] = i; side[n] = 1; ++n; }
    if (!n) return false;
    const int k = int(g.rng.loot.below(uint32_t(n)));
    if (side[k] == 0) {  // shift prefixes down, shift the fractured bits with them
        for (int i = idx[k]; i + 1 < it.nprefix; ++i) it.pre[i] = it.pre[i + 1];
        --it.nprefix;
        uint8_t lo = uint8_t(it.fractured & 0x07), hi = uint8_t(it.fractured & 0x38);
        uint8_t keep = 0; for (int i = 0, o = 0; i < it.nprefix + 1; ++i) if (i != idx[k] && (lo & (1 << i))) keep |= uint8_t(1 << o++); else if (i != idx[k]) ++o;
        it.fractured = uint8_t(hi | keep);
    } else {
        for (int i = idx[k]; i + 1 < it.nsuffix; ++i) it.suf[i] = it.suf[i + 1];
        --it.nsuffix;
        uint8_t lo = uint8_t(it.fractured & 0x07), hi = uint8_t((it.fractured >> 3) & 0x07);
        uint8_t keep = 0; for (int i = 0, o = 0; i < it.nsuffix + 1; ++i) if (i != idx[k] && (hi & (1 << i))) keep |= uint8_t(1 << o++); else if (i != idx[k]) ++o;
        it.fractured = uint8_t(lo | (keep << 3));
    }
    return true;
}

// D3: apply one currency orb to a gear item. Returns false (no consume) if it cannot apply. The
// D3b sub-system ops (Wisdom / Blessed / quality / Vaal / Mirror) are added in D3b.
bool apply_orb(uint8_t cur, items::Inst& it) {
    if (it.empty() || it.is_currency() || it.corrupted() || it.mirrored()) return false;
    if (it.unidentified() && cur != items::CUR_WISDOM) return false;  // must identify first
    using namespace items;
    switch (cur) {
        case CUR_TRANSMUTE:
            if (it.rarity != R_COMMON) return false;
            it.rarity = R_MAGIC; it.nprefix = it.nsuffix = 0; it.fractured = 0;
            add_one_affix(it, -1);  // 1 mod (a magic can later be Augmented to its 2nd)
            return true;
        case CUR_ALCHEMY:
            if (it.rarity != R_COMMON) return false;
            it.rarity = R_RARE; it.nprefix = it.nsuffix = 0; it.fractured = 0;
            roll_one_side(it, area_level(), true, 1 + int(g.rng.loot.below(kMaxPrefix)));
            roll_one_side(it, area_level(), false, 1 + int(g.rng.loot.below(kMaxSuffix)));
            it.rare_name = uint8_t(int(g.rng.loot.below(8)) | (int(g.rng.loot.below(8)) << 4));
            return true;
        case CUR_AUGMENT:
            return it.rarity == R_MAGIC && add_one_affix(it, -1);
        case CUR_EXALT:
            return it.rarity == R_RARE && add_one_affix(it, -1);
        case CUR_REGAL:
            if (it.rarity != R_MAGIC) return false;
            it.rarity = R_RARE;
            it.rare_name = uint8_t(int(g.rng.loot.below(8)) | (int(g.rng.loot.below(8)) << 4));
            add_one_affix(it, -1);
            return true;
        case CUR_ALTERATION:
            if (it.rarity != R_MAGIC) return false;
            reroll_keep_rarity(it); return true;
        case CUR_CHAOS:
            if (it.rarity != R_RARE) return false;
            reroll_keep_rarity(it); return true;
        case CUR_SCOURING:
            if (it.rarity == R_COMMON || it.fractured) return false;  // can't scour a fractured item
            it.rarity = R_COMMON; it.nprefix = it.nsuffix = 0; it.fractured = 0; it.rare_name = 0;
            return true;
        case CUR_ANNUL:
            return remove_random_affix(it);
        case CUR_DIVINE:
            return craft_divine(it);
        case CUR_FRACTURE: {
            if (it.fractured) return false;  // one fracture per item
            const int total = it.nprefix + it.nsuffix;
            if (!total) return false;
            const int pick = int(g.rng.loot.below(uint32_t(total)));
            if (pick < it.nprefix) it.fractured = uint8_t(1 << pick);
            else it.fractured = uint8_t(1 << (3 + (pick - it.nprefix)));
            return true;
        }
        // ---- D3b new-mechanic orbs ----
        case CUR_WISDOM:  // Ancient Scroll: identify (the unID guard above only lets Wisdom through)
            if (!it.unidentified()) return false;
            it.istate &= uint8_t(~1);
            return true;
        case CUR_BLESSED:  // Osmotic Membrane: reroll the implicit value (non-weapons carry an implicit)
            if (it.d().slot == Slot::Weapon || it.d().b == 0) return false;
            it.enchant = uint8_t(g.rng.loot.range(75, 125));  // implicit % of the def default
            return true;
        case CUR_ARMORQUAL:  // Maker Hide: +armour (gear) quality
            if (it.d().slot == Slot::Weapon || it.quality >= 20) return false;
            it.quality = uint8_t(it.quality + 5 > 20 ? 20 : it.quality + 5);
            return true;
        case CUR_WEAPONQUAL:  // Worm Tooth: +weapon quality
            if (it.d().slot != Slot::Weapon || it.quality >= 20) return false;
            it.quality = uint8_t(it.quality + 5 > 20 ? 20 : it.quality + 5);
            return true;
        case CUR_VAAL: {  // Unstable Catalyst: a risky random outcome, then LOCK (corrupted)
            const int r = int(g.rng.loot.below(100));
            if (r < 25) { /* unchanged — a lucky corrupt */ }
            else if (r < 55) {  // reforge (upgrade to rare if not already)
                if (it.rarity != R_RARE) { it.rarity = R_RARE;
                    it.rare_name = uint8_t(int(g.rng.loot.below(8)) | (int(g.rng.loot.below(8)) << 4)); }
                it.fractured = 0; reroll_keep_rarity(it);
            } else if (r < 80) {  // a bonus mod
                add_one_affix(it, -1);
            } else {  // brick: strip to Common
                it.rarity = R_COMMON; it.nprefix = it.nsuffix = 0; it.fractured = 0; it.rare_name = 0;
            }
            it.istate |= 2;  // corrupted — locked from further crafting
            return true;
        }
        case CUR_MIRROR: {  // Echo of Shai'Hulud: duplicate into a free pack cell (both become unmodifiable)
            items::Inst copy = it;
            copy.istate |= 4;
            if (pack_add(copy) < 0) return false;  // no room in the grid for the copy
            it.istate |= 4;  // both mirrored
            return true;
        }
        default: return false;
    }
}

// G2: the crafting table is the currency store. currency_count / spend_currency / the bench read the
// TABLE'S dedicated slots (g.pl.cur_tab); grid stacks are carried currency, moved in/out via
// tab_deposit / tab_withdraw (the drag / Ctrl-click gestures the mouse UI drives in G4).
int currency_count(uint8_t kind) {
    return (kind == 0 || kind >= items::CUR_COUNT) ? 0 : g.pl.cur_tab[kind];
}
bool spend_currency(uint8_t kind) {
    if (kind == 0 || kind >= items::CUR_COUNT || g.pl.cur_tab[kind] == 0) return false;
    --g.pl.cur_tab[kind];
    return true;
}
// Carried currency of `kind` sitting in the pack grid (the ground-pickup store, distinct from the table).
int currency_count_grid(uint8_t kind) {
    int n = 0;
    for (const items::Inst& s : g.pl.inv) if (s.currency == kind) n += s.stack;
    return n;
}
// Move a whole grid currency stack (pool entry idx) into the table's slots, freeing its cell. -> ok?
bool tab_deposit(int idx) {
    if (idx < 0 || idx >= tune::kInvCap || !g.pl.inv[idx].is_currency()) return false;
    const uint8_t k = g.pl.inv[idx].currency;
    g.pl.cur_tab[k] = uint16_t(g.pl.cur_tab[k] + g.pl.inv[idx].stack);
    g.pl.inv[idx] = items::Inst{};
    return true;
}
// Pull `amount` of currency `kind` from the table back into a grid cell (merges a same-type stack). -> ok?
bool tab_withdraw(uint8_t kind, int amount) {
    if (kind == 0 || kind >= items::CUR_COUNT || amount <= 0 || g.pl.cur_tab[kind] < amount) return false;
    for (items::Inst& s : g.pl.inv)  // merge into an existing carried stack if we have one
        if (s.currency == kind) { s.stack = uint16_t(s.stack + amount); g.pl.cur_tab[kind] -= uint16_t(amount); return true; }
    items::Inst c; c.currency = kind; c.stack = uint16_t(amount);
    if (pack_add(c) < 0) return false;  // no grid room
    g.pl.cur_tab[kind] -= uint16_t(amount);
    return true;
}
// D3d bench UI: distinct currency kinds the TABLE holds (>0), and the pack indices holding craftable
// gear. Shared by the bench menu's dispatch + render so their row->item mapping agrees.
int list_owned_currency(uint8_t* out) {
    int n = 0;
    for (int c = 1; c < items::CUR_COUNT; ++c) if (g.pl.cur_tab[c] > 0) out[n++] = uint8_t(c);
    return n;
}
int list_gear(int* out) {
    int n = 0;
    for (int i = 0; i < tune::kInvCap; ++i)
        if (!g.pl.inv[i].empty() && !g.pl.inv[i].is_currency()) out[n++] = i;
    return n;
}
// Ctrl-click quick-move from the pack (G4 drives this): currency -> the table's slots; gear -> equip,
// swapping any current occupant back into the grid. Returns true if something moved.
bool quickmove(int idx) {
    if (idx < 0 || idx >= tune::kInvCap || g.pl.inv[idx].empty()) return false;
    if (g.pl.inv[idx].is_currency()) return tab_deposit(idx);
    if (g.pl.inv[idx].unidentified()) return false;  // an unID item can't be equipped
    const items::Inst prev = g.pl.equip[int(g.pl.inv[idx].d().slot)];
    g.pl.equip[int(g.pl.inv[idx].d().slot)] = g.pl.inv[idx];
    pack_place_at(idx, prev);  // the displaced item (may be empty) drops back into the grid, re-fitted
    if (g.pl.hp > player_max_hp()) g.pl.hp = player_max_hp();
    if (g.pl.water > player_max_water()) g.pl.water = player_max_water();
    return true;
}
// Bench: apply currency `cur` (owned in the pack) to the gear item at inv_idx.
bool bench_apply(int cur, int inv_idx) {
    if (cur <= 0 || cur >= items::CUR_COUNT) return false;
    if (currency_count(uint8_t(cur)) < 1) return false;
    if (cur == items::CUR_FLASKQUAL) {  // Distillate Microtubing targets the flask belt, not a gear item
        if (g.pl.flask_quality >= 20) return false;
        g.pl.flask_quality = g.pl.flask_quality + 5 > 20 ? 20 : g.pl.flask_quality + 5;
        spend_currency(uint8_t(cur));
        g.log.push("The flasks distil cleaner. Flask quality rises.");
        return true;
    }
    if (inv_idx < 0 || inv_idx >= tune::kInvCap) return false;
    if (!apply_orb(uint8_t(cur), g.pl.inv[inv_idx])) return false;
    spend_currency(uint8_t(cur));
    g.log.push("Bench: applied %s.", items::currency_name(uint8_t(cur)));
    return true;
}

// B2b: slot-locked roll for a Revelation offer.
items::Inst roll_item_for_slot(int item_level, int rarity_bonus, items::Slot s) {
    int candidates[items::kDefCount], n = 0;
    for (int i = 0; i < items::kDefCount; ++i)
        if (items::kDefs[i].slot == s && items::kDefs[i].level <= item_level + 2 &&
            items::kDefs[i].level + 4 >= item_level)
            candidates[n++] = i;
    items::Inst it;
    if (!n)
        for (int i = 0; i < items::kDefCount; ++i)
            if (items::kDefs[i].slot == s) candidates[n++] = i;
    if (!n) return it;
    it.def = uint8_t(candidates[g.rng.loot.below(uint32_t(n))]);
    roll_affixes(it, area_level(), rarity_bonus);  // D3c: endgame gear finally rolls high tiers
    return it;
}

// D3c: grant a currency stack straight into the pack (used where there's no ground drop, e.g. the
// Long Ride's Jacurutu jackpot). Merges an existing stack, else takes a free cell.
void grant_currency_to_pack(uint8_t kind, int amount) {
    if (amount <= 0 || kind == 0) return;
    for (items::Inst& s : g.pl.inv) if (s.currency == kind) { s.stack = uint16_t(s.stack + amount); return; }
    items::Inst c; c.currency = kind; c.stack = uint16_t(amount);  // a fresh 1x1 currency stack
    pack_add(c);
}

// D3c: a weighted currency kind eligible at area level `alvl` (Divine-tier only falls deep), or 0.
int pick_currency(int alvl) {
    int w[items::CUR_COUNT], idx[items::CUR_COUNT], n = 0;
    for (int c = 1; c < items::CUR_COUNT; ++c)
        if (items::kCurrency[c].min_alvl <= alvl) { idx[n] = c; w[n] = items::kCurrency[c].weight; ++n; }
    return n ? idx[weighted_pick(w, n)] : 0;
}
// Drop a piece of crafting currency on the ground (PoE area-level drop logic).
void drop_currency(int x, int y) {
    const int c = pick_currency(area_level());
    if (!c) return;
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::None) {
            g.ents[i] = Entity{EType::Currency, Ai::Idle, g.cur_zone, int16_t(x), int16_t(y),
                               1, 0, int16_t(c), 1};
            g.log.push("Currency drops: %s.", items::currency_name(uint8_t(c)));
            return;
        }
}
// Put a rolled gear item into a free pack cell (used by the Jacurutu vault burst on the Long Ride,
// which has no zone to spill onto yet — the playable ride room comes later).
void grant_gear_to_pack(const items::Inst& it) {
    if (it.empty()) return;
    pack_add(it);
}

// D3d: auto-collect any ground currency the player is standing on, into the pack (currency needs no
// equip decision, so — unlike gear — it's picked up on walk-over). Called each world tick.
void pickup_currency() {
    const Entity& p = player_ent();
    for (int i = 1; i < kMaxEnts; ++i) {
        Entity& e = g.ents[i];
        if (e.type != EType::Currency || e.zone != g.cur_zone || e.x != p.x || e.y != p.y) continue;
        grant_currency_to_pack(uint8_t(e.data), e.data2 > 0 ? e.data2 : 1);
        g.log.push("Picked up %s.", items::currency_name(uint8_t(e.data)));
        e = Entity{};
    }
}

void maybe_drop_loot(EType t, int x, int y) {
    const Foe* f = foe_stats(t);
    if (!f) return;
    const bool human = t == EType::Trooper || t == EType::ShieldTrooper ||
                       t == EType::Spotter || t == EType::Hermit;
    if (!human && t != EType::Boss) return;
    const int chance = (t == EType::Boss ? 100 : 40) + 8 * g.pl.st.talents[5];
    if (int(g.rng.loot.below(100)) < chance) {  // the gear drop (unchanged draw order)
        const int diff = run_difficulty();
        items::Inst it = roll_item(f->level + diff / 2, diff * 6 + (t == EType::Boss ? 60 : 0));
        if (t == EType::Boss) it.ilvl = uint8_t(it.ilvl < 10 ? it.ilvl + 1 : 10);
        // D3c: deep gear can drop unidentified (draw gated by area, so shallow runs are unperturbed)
        if (area_level() >= tune::kUnidMinArea && it.rarity != items::R_COMMON &&
            g.rng.loot.below(tune::kUnidChanceDenom) == 0)
            it.istate |= 1;
        for (int i = 1; i < kMaxEnts; ++i)
            if (g.ents[i].type == EType::None) {
                g.ents[i] = Entity{EType::Loot, Ai::Idle, g.cur_zone, int16_t(x), int16_t(y), 1, 0,
                                   int16_t(it.def), 0};
                g.loot_inst[i] = it;
                char nm[64];
                it.full_name(nm, sizeof nm);
                g.log.push("%s dropped: %s",
                           it.rarity == items::R_RARE ? "RARE" : it.rarity ? "Magic" : "Gear", nm);
                break;
            }
    }
    // D3c: an INDEPENDENT currency drop, after the gear roll (so the gear roll is byte-identical) and
    // gated to area >= kDropMinArea (so the shallowest areas — where the determinism tests kill — never
    // draw for it, keeping their loot stream unshifted). PoE-faithful whole-game currency.
    if (area_level() >= tune::kDropMinArea && g.rng.loot.below(tune::kCurrencyDropDenom) == 0)
        drop_currency(x, y);
    // D3e: Spice Essence — the Awakening resource. A rare find in the deepest content only; not a bench
    // currency, so it banks straight to meta (persists across runs, like the Desert Trials). Drawn last
    // and gated deeper than currency, so nothing shallower is perturbed.
    if (area_level() >= tune::kSpiceEssenceMinArea && g.rng.loot.below(tune::kSpiceEssenceDenom) == 0) {
        if (g_meta.spice_essence < 250) g_meta.spice_essence++;
        meta_save();
        g.log.push("A vein of raw Spice Essence — the Water of Life stirs.");
    }
}

items::Inst loot_of(const Entity& e) {
    const int idx = int(&e - &g.ents[0]);
    if (idx >= 0 && idx < kMaxEnts) return g.loot_inst[idx];
    return items::Inst{};
}

const char* g_last_death = "";
void die(const char* cause) {
    g_last_death = cause;
    g.pl.dead = true;
    g.log.push("%s", cause);
    g.services.mixer->push({audio::Cmd::PlaySfx, 0 /* roar */, 200, 96});
    ++g_meta.runs;
    // B5: death keeps the character (level / attributes / gear) but NOT the run's spice haul.
    // banked_spice is left untouched -- only extraction banks it -- so the vision's spice is lost.
    persist_character();
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
        { items::Inst c; c.def = 4; pack_add(c); }  // the Crysknife itself, into the grid
        g.pl.maker_hooks = true;
        g.log.push("The Naib unwraps a pale blade. 'Honored one. It is yours.'");
        g.log.push("Crysknife and MAKER HOOKS taken. R mounts a surfaced worm.");
    }
}

// B3: spawn the node's NAMED pinnacle boss, scaled by its Awaken level (1..10).
void spawn_pinnacle(int awaken) {
    if (awaken < 1) awaken = 1;
    if (awaken > 10) awaken = 10;
    const int idx = awaken - 1;
    map::Zone& z = zone();
    for (int tries = 0; tries < 400; ++tries) {
        const int edge = int(g.rng.combat.below(4));
        const int x = edge < 2 ? (edge == 0 ? 2 : map::W - 3) : g.rng.combat.range(2, map::W - 3);
        const int y = edge < 2 ? g.rng.combat.range(2, map::H - 3) : (edge == 2 ? 2 : map::H - 3);
        if (!z.walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
        Entity* e = alloc_entity();
        if (!e) return;
        uint8_t traits = BT_BRUTE | BT_STALWART;
        if (awaken >= 7) traits |= BT_SWIFT;
        const int tier = awaken + 2;            // feeds boss_foe's tier scaling
        const int hp = 30 + awaken * 12;
        *e = Entity{EType::Boss, Ai::Hunt, g.cur_zone, int16_t(x), int16_t(y),
                    int16_t(hp), 0, int16_t(tier | (traits << 8)), int16_t(kPinnacleTag - idx)};
        g.log.push("%s stands in your path -- %s.", kPinnacles[idx].name, kPinnacles[idx].note);
        if (awaken == 5) g.log.push("The Baron watches from a gilded 'thopter, out of reach.");
        if (awaken == 10) g.log.push("BARON HARKONNEN. End the vision here.");
        return;
    }
}

// B3: an elite Harkonnen escort guards each field node's objective (count scales with Awaken).
void spawn_escort(int awaken) {
    const int want = 2 + awaken / 2 + guild_difficulty() / tune::kGuildMobDiv;  // B6: unpaid tithe -> more troopers
    const Entity& p = player_ent();
    int placed = 0;
    for (int tries = 0; tries < 3000 && placed < want; ++tries) {
        const int x = g.rng.world.range(6, map::W - 3);
        const int y = g.rng.world.range(2, map::H - 3);
        if (!zone().walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
        const int ddx = x > p.x ? x - p.x : p.x - x;
        const int ddy = y > p.y ? y - p.y : p.y - y;
        if (ddx + ddy < 8) continue;            // give the player breathing room on entry
        Entity* e = alloc_entity();
        if (!e) break;
        const int hp = 6 + awaken * 2;
        *e = Entity{EType::Trooper, Ai::Hunt, g.cur_zone,
                    int16_t(x), int16_t(y), int16_t(hp), 0, 0};
        ++placed;
    }
}

// B5: clearing a pinnacle marks its node and advances the Awaken ladder (unlock the next level).
void award_pinnacle_clear(int node) {
    world::Node& n = g.graph.nodes[node];
    if (n.flags & world::NF_CLEARED) return;  // once
    n.flags |= world::NF_CLEARED;
    const int aw = n.awaken;
    if (aw > g_meta.awaken_cleared) g_meta.awaken_cleared = uint8_t(aw);
    if (aw == g_meta.awaken_unlocked && g_meta.awaken_unlocked < 10) {
        ++g_meta.awaken_unlocked;
        g.log.push("The path to Arrakeen deepens. Vision %d of 10 opens to Muad'Dib.",
                   g_meta.awaken_unlocked);
    }
    if (aw >= 10 && !g_meta.chosen_path) {  // the Baron falls: the Chosen Path opens the Jihad endgame
        g_meta.chosen_path = 1;
        g.log.push("The Baron is dead. The Fremen legions look to you. Enact the Jihad.");
        g.log.push("The Jihad Table and the Long Ride open to Muad'Dib at the sietch.");
    }
    meta_save();
}

// ---- Endgame: the Jihad (spice-commissioned strikes -> directed pressure -> pinnacles) ----------

bool endgame_open() { return g_meta.chosen_path != 0; }

// A commissioned strike at `tier` (1..kStrikeTiers) presses its target Great Power. At full pressure
// that faction's homeworld pinnacle opens. Pressure is persistent (meta), capped, per-faction.
void press_faction(int f, int tier) {
    if (f < 0 || f >= FAC_COUNT || tier < 1) return;
    int p = g_meta.jihad_pressure[f] + tier * tune::kStrikePressPerTier;
    if (p > tune::kPinnaclePressure) p = tune::kPinnaclePressure;
    g_meta.jihad_pressure[f] = uint8_t(p);
    if (p >= tune::kPinnaclePressure && !(g_meta.pinnacle_cleared & (1 << f)))
        g.log.push("Jihad-pressure peaks. A homeworld pinnacle opens at the Jihad Table.");
    meta_save();
}

// The spice cost to commission a strike at `tier` (higher tier = harder + richer).
int strike_cost(int tier) { return tier * tune::kStrikeSpiceBase; }

// A faction's homeworld pinnacle is available once the Jihad is open, its pressure is full, and it
// has not already fallen.
bool pinnacle_available(int f) {
    return endgame_open() && f >= 0 && f < FAC_COUNT &&
           g_meta.jihad_pressure[f] >= tune::kPinnaclePressure &&
           !(g_meta.pinnacle_cleared & (1 << f));
}

// Defeat a faction's pinnacle: mark it cleared, take its Golden Path shard, and — for the Guild —
// break the spice monopoly (raising the tithe cap; the hook shipped in B6).
void clear_pinnacle(int f) {
    if (f < 0 || f >= FAC_COUNT) return;
    g_meta.pinnacle_cleared |= uint8_t(1 << f);
    g_meta.golden_shards |= uint8_t(1 << f);
    if (f == FAC_GUILD) g_meta.guild_broken = 1;  // the Navigator falls -> the monopoly breaks
    g.log.push("The homeworld falls. A shard of the Golden Path is yours (%d/5).",
               __builtin_popcount(g_meta.golden_shards));
    if ((g_meta.golden_shards & kAllShards) == kAllShards)
        g.log.push("All five shards gathered. The Golden Path can be confronted.");
    meta_save();
}

// The grand capstone (the alternate Paul who accepted the Golden Path) opens once all five shards
// are gathered and it has not yet been refused.
bool capstone_available() {
    return endgame_open() && (g_meta.golden_shards & kAllShards) == kAllShards && !g_meta.capstone_done;
}
void finish_capstone() {
    g_meta.capstone_done = 1;
    g.log.push("You face your other self — and refuse the Golden Path. One day, Leto will not.");
    meta_save();
}

// fwds (defined below): the strike excursion enters a raid zone and resolves back to the hub node 0.
void enter_zone(uint8_t id, uint64_t gen_seed, bool use_spawn, int at_x, int at_y, bool roll_events);
int opposing_houses();  // Landsraad: Great Powers opposing you reinforce your strikes (defined below)

const char* faction_name(int f) {
    static const char* kNames[] = {"the Bene Gesserit", "the Tleilaxu", "the Ixians",
                                    "House Corrino", "the Spacing Guild"};
    return (f >= 0 && f < FAC_COUNT) ? kNames[f] : "the enemy";
}

// B10.2: stock a strike zone — the faction's champion (the objective; killing it clears the strike)
// plus a trooper escort, both scaled by the strike tier.
void spawn_strike_force(int tier) {
    map::Zone& z = zone();
    const Entity& p = player_ent();
    for (int tries = 0; tries < 300; ++tries) {  // the objective: a scaled champion, placed to the east
        const int x = g.rng.combat.range(map::W / 2, map::W - 4);
        const int y = g.rng.combat.range(4, map::H - 4);
        if (!z.walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
        const uint8_t traits = uint8_t(1 << g.rng.loot.below(5)) | uint8_t(1 << g.rng.loot.below(5));
        const int16_t name_pack = int16_t(g.rng.loot.below(8) | (g.rng.loot.below(8) << 4) |
                                          (g.rng.loot.below(8) << 8));  // positive -> not a pinnacle tag
        const int hp = 18 + tier * 8 + ((traits & BT_STALWART) ? 10 : 0);
        if (Entity* e = alloc_entity())
            *e = Entity{EType::Boss, Ai::Hunt, g.cur_zone, int16_t(x), int16_t(y),
                        int16_t(hp), 0, int16_t((tier & 0xFF) | (traits << 8)), name_pack};
        break;
    }
    int troops = 2 + tier + opposing_houses();  // the escort — opposing Houses reinforce the strike
    if (troops > 8) troops = 8;
    for (int i = 0, tries = 0; i < troops && tries < 400; ++tries) {
        const int x = g.rng.combat.range(6, map::W - 4), y = g.rng.combat.range(4, map::H - 4);
        if (!z.walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
        const int ddx = x > p.x ? x - p.x : p.x - x, ddy = y > p.y ? y - p.y : p.y - y;
        if (ddx + ddy < 6) continue;  // breathing room on the drop
        if (Entity* e = alloc_entity())
            *e = Entity{EType::Trooper, Ai::Hunt, g.cur_zone, int16_t(x), int16_t(y),
                        int16_t(8 + tier * 3), 0, 0};
        ++i;
    }
}

void enter_strike(int f, int tier) {
    g.in_strike = true;
    g.strike_faction = uint8_t(f);
    g.strike_tier = uint8_t(tier);
    g.strike_seed = world::node_hash(g.seed ^ (uint64_t(f) << 20) ^ (uint64_t(tier) << 12),
                                     uint64_t(g.pl.turn));
    for (int i = 1; i < kMaxEnts; ++i) g.ents[i] = Entity{};  // wipe the hub's occupants
    g.op = harvest::Op{};
    g.alarm_turns = 0; g.alarm_spawn_cd = 0;
    g.delta_count = 0;
    g.spawned_mask = uint8_t(1 << map::kZoneDeepPan);  // skip auto-wildlife; the strike force is hand-placed
    enter_zone(map::kZoneDeepPan, g.strike_seed, /*use_spawn=*/true, 0, 0, /*roll_events=*/false);
    spawn_strike_force(tier);
    g.log.push("The Fedaykin strike at %s. Kill their champion; the desert takes you home after.",
               faction_name(f));
    refresh_fov_and_paths();
}

void enter_node(int idx);  // fwd (defined below) — strike resolution returns to the hub node 0

// The strike objective falls: press the target Power, take a tier-scaled prize, melt back to the hub.
void resolve_strike() {
    const int f = g.strike_faction, tier = g.strike_tier;
    g.in_strike = false;
    press_faction(f, tier);
    items::Inst it = roll_item(tier + 2, 40 + tier * 12);  // the richer-at-higher-tier payoff
    if (pack_add(it) >= 0) {
        char nm[64];
        it.full_name(nm, sizeof nm);
        g.log.push("Strike spoils: %s.", nm);
    }
    g.pl.spice += tier * 4;
    raise_standing(4);
    g.log.push("The strike succeeds. The Jihad tightens on %s.", faction_name(f));
    enter_node(0);  // the Fedaykin melt back to the sietch
    refresh_fov_and_paths();
}

// Break off a strike before the objective falls: home empty-handed, no pressure gained.
void abandon_strike() {
    g.in_strike = false;
    g.log.push("You break off the strike and melt back into the desert, the deed undone.");
    enter_node(0);
    refresh_fov_and_paths();
}

// Commission a strike at the Jihad Table: pay spice, then raid. Returns false (no charge) if the
// Jihad is shut or the commission is unaffordable. Pressure is applied on clear (resolve_strike).
bool commission_strike(int f, int tier) {
    if (!endgame_open() || f < 0 || f >= FAC_COUNT || tier < 1 || tier > tune::kStrikeTiers) return false;
    const int cost = strike_cost(tier);
    if (g.pl.spice < cost) return false;
    g.pl.spice -= cost;
    enter_strike(f, tier);
    return true;
}

// B10.3: the five homeworld pinnacles. Each is a FIXED-seed arena (a stable set-piece, unlike the
// rolled strikes) with a named faction boss. Full mechanical identity (Voice, Face-Dancers, ...) is
// deferred to the content phase; the MVP fields a tough named boss + honor guard per Great Power.
constexpr uint64_t kPinnacleSeed[FAC_COUNT] = {
    0xB0DE515700000001ull,  // Bene Gesserit — Wallach IX
    0x71E11A0000000002ull,  // Tleilaxu — Bandalong
    0x19000000000003ull,    // Ixian — Ix
    0xC02217A000000004ull,  // Corrino — Kaitain / Salusa
    0x6111D00000000005ull,  // Guild — Junction
};
const char* pinnacle_boss_name(int f) {
    static const char* kNames[] = {"a Reverend Mother of Wallach IX", "a Tleilaxu Master",
                                    "an Ixian magnate", "Emperor Shaddam IV", "a Guild Navigator"};
    return (f >= 0 && f < FAC_COUNT) ? kNames[f] : "the enemy";
}

void spawn_pinnacle_boss(int f) {
    map::Zone& z = zone();
    const Entity& p = player_ent();
    for (int tries = 0; tries < 300; ++tries) {  // the named boss, placed opposite the entry
        const int x = g.rng.combat.range(map::W / 2, map::W - 4);
        const int y = g.rng.combat.range(4, map::H - 4);
        if (!z.walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
        const uint8_t traits = BT_BRUTE | BT_STALWART | (f == FAC_GUILD ? BT_SWIFT : 0);
        const int16_t name_pack = int16_t(0x100 + f);  // positive -> not a campaign-pinnacle tag
        if (Entity* e = alloc_entity())
            *e = Entity{EType::Boss, Ai::Hunt, g.cur_zone, int16_t(x), int16_t(y),
                        int16_t(80 + f * 6), 0, int16_t((6 & 0xFF) | (traits << 8)), name_pack};
        break;
    }
    for (int i = 0, tries = 0; i < 3 && tries < 400; ++tries) {  // the honor guard
        const int x = g.rng.combat.range(6, map::W - 4), y = g.rng.combat.range(4, map::H - 4);
        if (!z.walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
        const int ddx = x > p.x ? x - p.x : p.x - x, ddy = y > p.y ? y - p.y : p.y - y;
        if (ddx + ddy < 6) continue;
        if (Entity* e = alloc_entity())
            *e = Entity{EType::ShieldTrooper, Ai::Hunt, g.cur_zone, int16_t(x), int16_t(y),
                        int16_t(24), 0, 0};
        ++i;
    }
}

void enter_pinnacle(int f) {
    g.in_pinnacle = true;
    g.pinnacle_faction = uint8_t(f);
    for (int i = 1; i < kMaxEnts; ++i) g.ents[i] = Entity{};
    g.op = harvest::Op{};
    g.alarm_turns = 0; g.alarm_spawn_cd = 0;
    g.delta_count = 0;
    g.spawned_mask = uint8_t(1 << map::kZoneDeepPan);  // hand-placed arena, no auto-wildlife
    enter_zone(map::kZoneDeepPan, kPinnacleSeed[f], /*use_spawn=*/true, 0, 0, /*roll_events=*/false);
    spawn_pinnacle_boss(f);
    g.log.push("You breach the homeworld of %s. Their champion answers: %s.",
               faction_name(f), pinnacle_boss_name(f));
    refresh_fov_and_paths();
}

// The homeworld boss falls: take the Golden Path shard (clear_pinnacle) + a rich prize, then home.
void resolve_pinnacle() {
    const int f = g.pinnacle_faction;
    g.in_pinnacle = false;
    clear_pinnacle(f);  // shard + (Guild) guild_broken; announces the shard count / capstone
    items::Inst it = roll_item(10, 120);  // a pinnacle-tier prize
    if (pack_add(it) >= 0) {
        char nm[64];
        it.full_name(nm, sizeof nm);
        g.log.push("Pinnacle spoils: %s.", nm);
    }
    g.pl.spice += 40;
    raise_standing(10);
    enter_node(0);
    refresh_fov_and_paths();
}

void abandon_pinnacle() {
    g.in_pinnacle = false;
    g.log.push("The homeworld holds. You withdraw to fight another day.");
    enter_node(0);
    refresh_fov_and_paths();
}

// Assault a faction's homeworld pinnacle — only once its Jihad-pressure is full. False if not open.
bool assault_pinnacle(int f) {
    if (!pinnacle_available(f)) return false;
    enter_pinnacle(f);
    return true;
}

// B10.4: the Golden Path capstone. Deep in prescience Paul sees the Path — and its cost — and refuses
// it; the boss is that refusal made flesh, an alternate Paul who ACCEPTED it (a mirror of the player's
// own power). Fixed-seed arena; slaying it is the apex trophy (finish_capstone). Content-phase art +
// full mirror-of-your-kit AI are deferred; the MVP fields one very strong lone boss scaled to you.
inline constexpr uint64_t kCapstoneSeed = 0x60D0E9A7480000FFull;
void enter_capstone() {
    g.in_capstone = true;
    for (int i = 1; i < kMaxEnts; ++i) g.ents[i] = Entity{};
    g.op = harvest::Op{};
    g.alarm_turns = 0; g.alarm_spawn_cd = 0;
    g.delta_count = 0;
    g.spawned_mask = uint8_t(1 << map::kZoneDeepPan);
    enter_zone(map::kZoneDeepPan, kCapstoneSeed, /*use_spawn=*/true, 0, 0, /*roll_events=*/false);
    map::Zone& z = zone();
    const Entity& p = player_ent();
    for (int tries = 0; tries < 400; ++tries) {  // the mirror: HP scaled to the player's own power
        const int x = g.rng.combat.range(map::W / 2, map::W - 4);
        const int y = g.rng.combat.range(4, map::H - 4);
        if (!z.walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
        const int ddx = x > p.x ? x - p.x : p.x - x, ddy = y > p.y ? y - p.y : p.y - y;
        if (ddx + ddy < 10) continue;
        const uint8_t traits = BT_BRUTE | BT_STALWART | BT_SWIFT;
        const int hp = 120 + g.pl.st.level * 5;
        if (Entity* e = alloc_entity())
            *e = Entity{EType::Boss, Ai::Hunt, g.cur_zone, int16_t(x), int16_t(y),
                        int16_t(hp), 0, int16_t((8 & 0xFF) | (traits << 8)), int16_t(0x1FF)};
        break;
    }
    g.log.push("The vision resolves: yourself, crowned on a throne of bone — the Paul who WALKED");
    g.log.push("the Golden Path. He turns his prescient eyes on you. Refuse him.");
    refresh_fov_and_paths();
}
void resolve_capstone() {
    g.in_capstone = false;
    finish_capstone();  // refuse the Golden Path (the apex trophy)
    items::Inst it = roll_item(10, 200);  // the ultimate spoils
    if (pack_add(it) >= 0) {
        char nm[64];
        it.full_name(nm, sizeof nm);
        g.log.push("The other-Paul dissolves into sand, leaving: %s.", nm);
    }
    raise_standing(20);
    enter_node(0);
    refresh_fov_and_paths();
}
void abandon_capstone() {
    g.in_capstone = false;
    g.log.push("You are not ready to face what you might become. You withdraw.");
    enter_node(0);
    refresh_fov_and_paths();
}
// Confront the Golden Path — only once all five shards are gathered. False if not yet available.
bool confront_capstone() {
    if (!capstone_available()) return false;
    enter_capstone();
    return true;
}

// ---- The Long Ride: the deep-desert greed gauntlet (endgame; sibling to the Jihad Table) ----------
// Ride the Maker leg by leg; each site adds to an un-banked haul. Bank it (safe) or push on (greed).
// Past kRideBuckThreshold legs the Maker may BUCK & dive on the remount — the ride ends and the
// un-banked haul is lost. Jacurutu (the outcast hoard) is the SOLE Maker Hide source, its odds rising
// with depth; a Hide is secured on the spot (not part of the losable haul). Tithe-free; leg-scaled.

bool ride_begin() {
    if (!endgame_open() || g.in_ride) return false;
    g.in_ride = true;
    g.ride_leg = 0;
    g.ride_haul = 0;
    g.ride_restless = 0;
    g.log.push("You thump the deep sand. A great Maker rises beneath you. The Long Ride begins.");
    return true;
}

// Ride on to the next site. Returns 0 = rode on (arrived + resolved the site), 1 = the Maker bucked
// (ride over, un-banked haul lost). No-op-with-1 if not riding.
int ride_push() {
    if (!g.in_ride) return 1;
    if (g.ride_leg >= tune::kRideBuckThreshold) {  // the remount: the Maker may buck & dive
        int chance = (g.ride_leg - tune::kRideBuckThreshold + 1) * tune::kRideBuckPerLeg;
        if (chance > tune::kRideBuckMax) chance = tune::kRideBuckMax;
        if (int(g.rng.world.below(100)) < chance) {
            if (g.ride_leg > g_meta.best_ride_leg) g_meta.best_ride_leg = uint8_t(g.ride_leg > 255 ? 255 : g.ride_leg);
            g.log.push("The Maker BUCKS and sounds! The ride ends; the un-banked haul is lost to the deep.");
            g.in_ride = false;
            g.ride_haul = 0;
            meta_save();
            return 1;
        }
    }
    ++g.ride_leg;
    g.ride_restless += g.ride_leg;
    int jac = tune::kJacurutuBaseChance + g.ride_leg * tune::kJacurutuPerLeg;  // rises with depth
    if (jac > tune::kJacurutuMax) jac = tune::kJacurutuMax;
    if (int(g.rng.world.below(100)) < jac) {  // JACURUTU — the outcast hoard: a loot-vault burst
        const int alvl = area_level();  // deep on the Long Ride -> high-tier currency + gear
        for (int k = 0; k < 3; ++k) { int c = pick_currency(alvl); if (c) grant_currency_to_pack(uint8_t(c), 1); }
        const int gl = alvl > 10 ? 10 : alvl;  // def level caps ~10; affixes still roll at area level
        for (int k = 0; k < 3; ++k) grant_gear_to_pack(roll_item(gl, 60 + alvl * 3));
        g.log.push("JACURUTU. The outcast sietch's forbidden hoard bursts open (leg %d) — currency",
                   g.ride_leg);
        g.log.push("and mid-to-high gear spill across the sand. You fill your pack.");
    } else {
        g.ride_haul += tune::kRideHaulPerLeg * g.ride_leg;  // a spice site — deeper legs are richer
        g.log.push("You dismount at a spice blow (leg %d). Un-banked haul: %d.", g.ride_leg, g.ride_haul);
    }
    return 0;
}

// Bank the ride: secure the un-banked haul and walk home. Safe — the greed decision resolved.
void ride_bank() {
    if (!g.in_ride) return;
    g.pl.spice += g.ride_haul;
    if (g.ride_leg > g_meta.best_ride_leg) g_meta.best_ride_leg = uint8_t(g.ride_leg > 255 ? 255 : g.ride_leg);
    g.log.push("You bank the ride: %d spice secured after %d legs. You walk home.",
               g.ride_haul, g.ride_leg);
    g.in_ride = false;
    g.ride_haul = 0;
    meta_save();
}

// ---- The Landsraad / CHOAM: the Imperial politics layer (the economic face of the conquest) -------
// Not a boss — a standing. CHOAM control is DERIVED: each toppled homeworld is a seized directorship,
// each courted House adds its votes. A Great Power you are actively warring (high pressure, not yet
// toppled) OPPOSES you in the council; a courted one is an ALLY that pays dividends.

enum HouseStance : uint8_t { HOUSE_NEUTRAL = 0, HOUSE_ALLY, HOUSE_OPPONENT };

int house_stance(int f) {
    if (f < 0 || f >= FAC_COUNT) return HOUSE_NEUTRAL;
    if (g_meta.house_allied & (1 << f)) return HOUSE_ALLY;
    if (!(g_meta.pinnacle_cleared & (1 << f)) && g_meta.jihad_pressure[f] >= tune::kHouseOpposePressure)
        return HOUSE_OPPONENT;  // you're storming them: they fight you in the Landsraad
    return HOUSE_NEUTRAL;
}

// CHOAM control (0..100): directorships seized by conquest + votes from courted allies.
int choam_control() {
    int c = __builtin_popcount(g_meta.pinnacle_cleared) * tune::kChoamPerPinnacle +
            __builtin_popcount(g_meta.house_allied) * tune::kChoamPerAlly;
    return c < 0 ? 0 : c > 100 ? 100 : c;
}

// How many Great Powers currently oppose you in the council (their forces reinforce your strikes).
int opposing_houses() {
    int n = 0;
    for (int f = 0; f < FAC_COUNT; ++f)
        if (house_stance(f) == HOUSE_OPPONENT) ++n;
    return n;
}

// Court a House into a CHOAM alliance: costs spice. You cannot ally with a Power you are actively
// storming (an opponent), nor one already allied. Returns false (no charge) otherwise.
bool court_house(int f) {
    if (!endgame_open() || f < 0 || f >= FAC_COUNT) return false;
    if (house_stance(f) != HOUSE_NEUTRAL) return false;  // opponents can't be bought; allies already in
    if (g.pl.spice < tune::kCourtCost) return false;
    g.pl.spice -= tune::kCourtCost;
    g_meta.house_allied |= uint8_t(1 << f);
    g.log.push("%s joins your bloc in the Landsraad. CHOAM control: %d%%.",
               faction_name(f), choam_control());
    meta_save();
    return true;
}

// Collect the CHOAM dividend — the economic boon of your bloc, scaled by control. Returns the spice.
int collect_dividend() {
    if (!endgame_open()) return 0;
    const int div = choam_control() * tune::kDividendPerControl;
    g.pl.spice += div;
    g.log.push("The CHOAM dividend pays out: %d spice from your directorships.", div);
    return div;
}

void spawn_boss(bool baron) {
    map::Zone& z = zone();
    for (int tries = 0; tries < 300; ++tries) {
        const int edge = int(g.rng.combat.below(4));
        const int x = edge < 2 ? (edge == 0 ? 2 : map::W - 3)
                               : g.rng.combat.range(2, map::W - 3);
        const int y = edge < 2 ? g.rng.combat.range(2, map::H - 3)
                               : (edge == 2 ? 2 : map::H - 3);
        if (!z.walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
        Entity* e = nullptr;
        for (Entity& c : g.ents)
            if (c.type == EType::None) { e = &c; break; }
        if (!e) return;
        const int tier = g.season.denied;
        uint8_t traits;
        int16_t name_pack;
        int hp;
        if (baron) {
            traits = BT_BRUTE | BT_STALWART;
            name_pack = -1;
            hp = 40 + tier * 8;
        } else {
            traits = uint8_t(1 << g.rng.loot.below(5)) |
                     uint8_t(1 << g.rng.loot.below(5));
            name_pack = int16_t(g.rng.loot.below(8) | (g.rng.loot.below(8) << 4) |
                                (g.rng.loot.below(8) << 8));
            hp = 14 + tier * 5 + ((traits & BT_STALWART) ? 10 : 0);
        }
        *e = Entity{EType::Boss, Ai::Hunt, g.cur_zone, int16_t(x), int16_t(y),
                    int16_t(hp), 0, int16_t(tier | (traits << 8)), name_pack};
        if (baron) {
            g.log.push("A gilded thopter descends. BARON HARKONNEN COMES HIMSELF.");
            g.log.push("Kill him and the season is yours.");
        } else {
            char nm[80];
            std::snprintf(nm, sizeof nm, "%s %s %s",
                          kBossTitles[name_pack & 7], kBossNames[(name_pack >> 4) & 7],
                          kBossEpithets[(name_pack >> 8) & 7]);
            g.log.push("The Baron's champion answers: %s!", nm);
        }
        g.services.mixer->push({audio::Cmd::PlaySfx, 0, 220, 120});
        return;
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

    if (g.op.mutators & campaign::MUT_CLIMAX) {
        spawn_boss(true);  // the Baron himself — victory means killing HIM
        return;
    }
    spawn_boss(false);  // every fallen harvester sends a champion
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
            if (e.type == EType::Boss) {
                const bool pin = boss_is_pinnacle(e.data2);
                if (pin) award_pinnacle_clear(g.cur_node);
                if (e.data2 == -1 || (pin && boss_pinnacle_idx(e.data2) == 9)) {
                    g.log.push("SHAI-HULUD TAKES THE BARON. Shield and all.");
                    g.log.push("The desert itself has judged him.");
                    e.type = EType::None;
                    end_season(campaign::kVictory);
                    continue;
                }
                g.log.push(pin ? "Shai-Hulud takes your quarry whole." :
                                 "The champion's shield sang to the worm. It answered.");
            }
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
    map::Zone& z = g.zone_;
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
    if (floor <= 2 || floor == 5) n = 10;  // sand carries your rhythm
    else if (floor == 6) n = 30;           // drum / churned sand booms
    n -= affix_total(items::AK_NOISE) + 2 * g.pl.st.talents[3];  // Featherstep
    if (n < 0) n = 0;
    if (g.pl.sandwalk) n /= 3;             // the broken stride barely whispers
    return n;
}

// D1 damage types (all canon, non-magical). DT_PHYS = enemy blade/hunter-seeker (blockable);
// the elementals map to Arrakis hazards; DT_TRUE = internal (thirst) — bypasses mitigation.
enum DmgType : uint8_t { DT_PHYS, DT_HEAT, DT_COLD, DT_STORM, DT_TOX, DT_BLEED, DT_TRUE };
uint8_t res_kind_for(DmgType t) {
    switch (t) {
        case DT_HEAT: return items::AK_HEAT;
        case DT_COLD: return items::AK_RES_COLD;
        case DT_STORM: return items::AK_RES_STORM;
        case DT_TOX: return items::AK_RES_TOX;
        default: return 0xFF;
    }
}

// C3: Fedaykin (Knifehand tank ascendancy) — innate Block on top of any gear Block.
int ascendancy_block() { return g.pl.ascendancy == Ascendancy::Fedaykin ? 15 : 0; }

void damage_player(int dmg, const char* cause, DmgType type = DT_PHYS) {
    g.pl.shield_delay = tune::kShieldRechargeDelay;  // any hit pauses the shield recharge
    // Block: enemy physical only — a chance to turn the blow entirely. (No draw unless geared/
    // Fedaykin, so determinism baselines are unaffected for a plain character.)
    if (type == DT_PHYS) {
        const int blk = affix_total(items::AK_BLOCK) + ascendancy_block();
        if (blk > 0 && int(g.rng.combat.below(100)) < blk) { flash(180, 180, 60); return; }
    }
    // Resistance (typed hazards): reduce by the matching resist, 75% cap.
    const uint8_t rk = res_kind_for(type);
    if (rk != 0xFF && dmg > 0) {
        int res = affix_total(rk); if (res > 75) res = 75; if (res < 0) res = 0;
        dmg = dmg * (100 - res) / 100;
    }
    // Fortitude: flat % reduced incoming (all but true damage), 75% cap.
    if (type != DT_TRUE && dmg > 0) {
        int dr = affix_total(items::AK_DR); if (dr > 75) dr = 75;
        dmg = dmg * (100 - dr) / 100;
    }
    if ((rk != 0xFF || affix_total(items::AK_DR) > 0) && dmg < 1) dmg = 1;  // mitigation never fully negates
    // Energy shield (Focus) soaks before Life — except Bleed, which seeps under it (Fortitude only).
    if (g.pl.shield > 0 && dmg > 0 && type != DT_BLEED) {
        const int soaked = dmg < g.pl.shield ? dmg : g.pl.shield;
        g.pl.shield -= soaked;
        dmg -= soaked;
        if (dmg <= 0) { flash(60, 120, 230); return; }  // fully turned by the shield
    }
    g.pl.hp -= dmg;
    flash(220, 30, 20);
    if (g.pl.hp <= 0) die(cause);
}
// Deflect (thorns): a melee attacker takes returned damage. Called at melee-hit sites.
void reflect_thorns(Entity* attacker) {
    const int t = affix_total(items::AK_THORNS);
    if (t > 0 && attacker && attacker->hp > 0) attacker->hp = int16_t(attacker->hp - t);
}
// D1 clamped restores (on-hit/on-kill/leech).
void gain_hp(int amt) { if (amt <= 0) return; const int m = player_max_hp(); if ((g.pl.hp += amt) > m) g.pl.hp = m; }
void gain_shield(int amt) { if (amt <= 0) return; const int m = player_max_shield(); if ((g.pl.shield += amt) > m) g.pl.shield = m; }
void gain_will(int amt) { if (amt <= 0) return; const int m = player_max_will(); if ((g.pl.will += amt) > m) g.pl.will = m; }
// C3 DoT ailments — refresh to the stronger tick + the longer duration (stacks don't add).
void poison_entity(Entity& e, int dmg_per_turn, int turns) {
    if (dmg_per_turn > e.poison_dmg) e.poison_dmg = int16_t(dmg_per_turn);
    if (turns > e.poison_turns) e.poison_turns = int16_t(turns);
}
void scorch_entity(Entity& e, int dmg_per_turn, int turns) {
    if (dmg_per_turn > e.scorch_dmg) e.scorch_dmg = int16_t(dmg_per_turn);
    if (turns > e.scorch_turns) e.scorch_turns = int16_t(turns);
}
void bleed_entity(Entity& e, int dmg_per_turn, int turns) {
    if (dmg_per_turn > e.bleed_dmg) e.bleed_dmg = int16_t(dmg_per_turn);
    if (turns > e.bleed_turns) e.bleed_turns = int16_t(turns);
}
void poison_player(int dmg_per_turn, int turns) {
    if (dmg_per_turn > g.pl.ail_poison_dmg) g.pl.ail_poison_dmg = dmg_per_turn;
    if (turns > g.pl.ail_poison_turns) g.pl.ail_poison_turns = turns;
}
void scorch_player(int dmg_per_turn, int turns) {
    if (dmg_per_turn > g.pl.ail_scorch_dmg) g.pl.ail_scorch_dmg = dmg_per_turn;
    if (turns > g.pl.ail_scorch_turns) g.pl.ail_scorch_turns = turns;
}
void bleed_player(int dmg_per_turn, int turns) {
    if (dmg_per_turn > g.pl.ail_bleed_dmg) g.pl.ail_bleed_dmg = dmg_per_turn;
    if (turns > g.pl.ail_bleed_turns) g.pl.ail_bleed_turns = turns;
}
// C3 effect ailments (no damage): Blind (accuracy) + Freeze (turn control) — refresh to longer duration.
void blind_entity(Entity& e, int turns) { if (turns > e.blind_turns) e.blind_turns = int16_t(turns); }
void freeze_entity(Entity& e, int turns) { if (turns > e.frozen_turns) e.frozen_turns = int16_t(turns); }
void blind_player(int turns) { if (turns > g.pl.ail_blind_turns) g.pl.ail_blind_turns = turns; }
void stun_entity(Entity& e, int turns) { if (turns > e.stun_turns) e.stun_turns = int16_t(turns); }
void stun_player(int turns) { if (turns > g.pl.ail_stun_turns) g.pl.ail_stun_turns = turns; }
void freeze_player(int turns) { if (turns > g.pl.ail_frozen_turns) g.pl.ail_frozen_turns = turns; }

// ---- C7: Will-powered prescience abilities (universal active skills) --------------------------------
// One Will pool, one shared cooldown. Cast at a target tile; effects reuse the stun / teleport / Focus
// systems. Deterministic + player-initiated (never auto-fired, no RNG) — off every test's det path.
enum Ability : uint8_t { AB_NONE = 0, AB_VOICE, AB_BLINK, AB_PRESCIENCE, AB_COUNT };

bool ability_is_hostile(EType t) {
    switch (t) {
        case EType::Hound: case EType::Trooper: case EType::ShieldTrooper: case EType::Spotter:
        case EType::PanAdder: case EType::Hermit: case EType::HunterSeeker: case EType::Boss:
            return true;
        default: return false;
    }
}
int ability_cost(int ab) {
    switch (ab) {
        case AB_VOICE: return tune::kWillVoice;
        case AB_BLINK: return tune::kWillBlink;
        case AB_PRESCIENCE: return tune::kWillPrescience;
        default: return 0;
    }
}
bool ability_ready() { return g.pl.ability_cd <= 0 && !g.pl.dead; }

// the Voice: stun the nearest SEEN hostile within reach. Fails (no cast) if none is in range.
bool do_voice() {
    const Entity& p = player_ent();
    int best = -1, bestd = tune::kVoiceRange * tune::kVoiceRange + 1;
    for (int i = 1; i < kMaxEnts; ++i) {
        const Entity& e = g.ents[i];
        if (e.hp <= 0 || e.zone != g.cur_zone || !ability_is_hostile(e.type)) continue;
        if (!(zone().vis[e.y * map::W + e.x] & map::V_VISIBLE)) continue;
        const int dx = e.x - p.x, dy = e.y - p.y, d = dx * dx + dy * dy;
        if (d < bestd) { bestd = d; best = i; }
    }
    if (best < 0) return false;
    stun_entity(g.ents[best], tune::kVoiceStunTurns);
    g.log.push("The Voice cracks the air. The foe locks rigid, its will not its own.");
    return true;
}

// Prescient Step: blink toward the cursor, landing on the farthest clear tile within range.
bool do_blink(int tx, int ty) {
    Entity& p = player_ent();
    if (tx == p.x && ty == p.y) return false;
    int nx = p.x, ny = p.y, lx = p.x, ly = p.y;
    for (int s = 0; s < tune::kBlinkRange; ++s) {
        const int cx = tx > nx ? 1 : tx < nx ? -1 : 0, cy = ty > ny ? 1 : ty < ny ? -1 : 0;
        const int px = nx + cx, py = ny + cy;
        if (!zone().walkable(px, py) || entity_at(px, py, g.cur_zone)) break;
        nx = px; ny = py; lx = nx; ly = ny;
        if (nx == tx && ny == ty) break;
    }
    if (lx == p.x && ly == p.y) return false;  // couldn't move at all
    p.x = int16_t(lx); p.y = int16_t(ly);
    p.anim.facing = facing_from_tile_delta(tx - p.x, ty - p.y);
    g.log.push("Prescient step: the sand blurs and you are elsewhere.");
    return true;
}

// Cast an ability at (tx, ty). Checks the shared cooldown + Will; spends Will and starts the cooldown
// only if the effect lands. Returns false (no cost) otherwise.
bool cast_ability(int ab, int tx, int ty) {
    if (!ability_ready() || ab <= AB_NONE || ab >= AB_COUNT) return false;
    const int cost = ability_cost(ab);
    if (g.pl.will < cost) return false;
    bool ok = false;
    switch (ab) {
        case AB_VOICE: ok = do_voice(); break;
        case AB_BLINK: ok = do_blink(tx, ty); break;
        case AB_PRESCIENCE: gain_shield(tune::kPrescienceShield); ok = true; break;
        default: break;
    }
    if (!ok) return false;
    g.pl.will -= cost;
    g.pl.ability_cd = tune::kAbilityCdTurns;
    return true;
}

// ---- Flasks: the three-slot belt (Life / Will / Utility) --------------------------------------------
// Charges refill from kills (flask_gain on each kill) + a slow time trickle (upkeep). Not part of the
// state hash (deterministic + no RNG, like the ability cooldown) — keeps the cross-build baseline.
void flask_gain(int n) {
    if (n <= 0) return;
    for (int i = 0; i < tune::kFlaskSlots; ++i) {
        g.pl.flask_charges[i] += n;
        if (g.pl.flask_charges[i] > tune::kFlaskMaxCharges) g.pl.flask_charges[i] = tune::kFlaskMaxCharges;
    }
}
bool use_flask(int slot) {
    if (slot < 0 || slot >= tune::kFlaskSlots || g.pl.dead) return false;
    if (g.pl.flask_charges[slot] < tune::kFlaskUseCost) return false;
    g.pl.flask_charges[slot] -= tune::kFlaskUseCost;
    const int qmul = 100 + g.pl.flask_quality;  // D3: Distillate Microtubing boosts flask potency
    switch (slot) {
        case 0:  // Life
            gain_hp(tune::kFlaskLifeHeal * qmul / 100);
            g.log.push("You drink deep — a Life flask. The body knits.");
            break;
        case 1:  // Will
            gain_will(tune::kFlaskWillRestore * qmul / 100);
            g.log.push("A Will flask — prescience sharpens.");
            break;
        default:  // Utility: flush every ailment + raise a Focus ward
            g.pl.ail_poison_turns = g.pl.ail_scorch_turns = g.pl.ail_bleed_turns = 0;
            g.pl.ail_blind_turns = g.pl.ail_stun_turns = g.pl.ail_frozen_turns = 0;
            gain_shield(tune::kFlaskUtilShield * qmul / 100);
            g.log.push("A Utility flask — the venoms flush, the mind steadies.");
            break;
    }
    return true;
}

// RT2: Freeze duration (world-tick count) scaled by a cold hit vs the target's eff-max-Life,
// clamped to [0.3, 3.0] s. Below the 0.3 s floor -> 0 (no freeze). Pure integer math (firewall).
int freeze_ticks_for(int cold_dmg, int max_life) {
    if (max_life < 1) max_life = 1;
    int t = cold_dmg * tune::kFreezeMaxTicks / max_life;   // ratio * max ticks (floor)
    if (t < tune::kFreezeMinTicks) return 0;
    if (t > tune::kFreezeMaxTicks) t = tune::kFreezeMaxTicks;
    return t;
}

// RT2: the hit's damage as a % of the target's eff-max-Life, inflated by attacker Stun Buildup and
// saturated at the cap. The shared basis for both stun chance and stun duration.
int stun_ratio_pct(int hit_dmg, int max_life, int mult_pct) {
    if (max_life < 1) max_life = 1;
    int rp = hit_dmg * 100 / max_life;
    if (mult_pct > 0) rp = rp * (100 + mult_pct) / 100;
    if (rp > tune::kStunRatioCapPct) rp = tune::kStunRatioCapPct;
    return rp;
}
// Deterministic stun duration (world ticks, 0 = below threshold) — no RNG. Reduction shortens it.
int stun_duration_for(int hit_dmg, int max_life, int mult_pct, int reduce_pct) {
    if (reduce_pct < 0) reduce_pct = 0; if (reduce_pct > 90) reduce_pct = 90;
    const int rp = stun_ratio_pct(hit_dmg, max_life, mult_pct);
    if (rp < tune::kStunThresholdPct) return 0;
    int t = rp * tune::kStunMaxTicks / 100;
    t = t * (100 - reduce_pct) / 100;
    if (t < tune::kStunMinTicks) t = tune::kStunMinTicks;
    if (t > tune::kStunMaxTicks) t = tune::kStunMaxTicks;
    return t;
}
// Stun chance% (0 = can't stun) — ramps with the ratio above the threshold, less Stun Reduction.
int stun_chance_for(int hit_dmg, int max_life, int mult_pct, int reduce_pct) {
    if (reduce_pct < 0) reduce_pct = 0; if (reduce_pct > 90) reduce_pct = 90;
    const int rp = stun_ratio_pct(hit_dmg, max_life, mult_pct);
    if (rp < tune::kStunThresholdPct) return 0;
    int chance = (rp - tune::kStunThresholdPct) * tune::kStunChanceNum - reduce_pct;
    if (chance < 0) chance = 0;
    if (chance > 100) chance = 100;
    return chance;
}
// PoE-style Stun roll: RNG (combat stream) drawn ONLY when the hit crosses the threshold, so
// sub-threshold hits stay baseline. Returns the stun duration (world ticks, 0 = no stun).
int roll_stun_ticks(int hit_dmg, int max_life, int mult_pct, int reduce_pct) {
    const int chance = stun_chance_for(hit_dmg, max_life, mult_pct, reduce_pct);
    if (chance <= 0) return 0;
    if (int(g.rng.combat.below(100)) >= chance) return 0;  // failed the stun roll
    return stun_duration_for(hit_dmg, max_life, mult_pct, reduce_pct);
}

// RT2: an enemy melee/ranged hit on the player — applies damage, then rolls Stun (hit vs player
// eff-max-Life, lowered by the player's Stun Reduction). Environment/DoT damage does NOT route
// through here, so only real hits can stun. Two-way with the player-stuns-foe path in try_move.
void roll_enemy_element_on_player(int dmg);  // fwd (defined just below)
void foe_hit_player(int dmg, const char* cause, DmgType type = DT_PHYS) {
    damage_player(dmg, cause, type);
    if (g.pl.dead || dmg <= 0) return;
    if (int st = roll_stun_ticks(dmg, player_max_hp(), 0, affix_total(items::AK_STUN_REDUCE)); st > 0)
        stun_player(st);
    roll_enemy_element_on_player(dmg);  // C3: deep-run hits can also inflict an elemental ailment
}

// C3 (defensive half of the two-way ailment system): the chance a deep-run enemy hit turns
// elemental. Deterministic (depth in), 0 below the Awaken gate — so shallow/headless hits never
// draw the RNG and stay baseline.
int enemy_element_chance(int depth) {
    if (depth < tune::kElemEnemyMinAwaken) return 0;
    int c = (depth - tune::kElemEnemyMinAwaken + 1) * tune::kElemEnemyChancePerDepth;
    return c > 100 ? 100 : c;
}
// Apply one element's ailment to the player. Deterministic. DoTs (Scorch/Poison) route their tick
// damage through damage_player, which already applies the matching resist; the effect ailments
// (Blind/Freeze) have the matching resist reduce their DURATION here.
void inflict_element_on_player(int elem, int mag) {
    auto res = [](uint8_t k) { int r = affix_total(k); return r < 0 ? 0 : r > 75 ? 75 : r; };
    switch (elem) {
        case 0: scorch_player(mag, tune::secs_to_ticks(2.0)); break;                     // Heat  -> Heat Resist
        case 3: poison_player(mag, tune::secs_to_ticks(2.5)); break;                     // Toxin -> Toxin Resist
        case 2: {  // Storm -> Blind, duration cut by Storm Resist
            int t = tune::secs_to_ticks(1.5) * (100 - res(items::AK_RES_STORM)) / 100;
            if (t > 0) blind_player(t);
            break;
        }
        case 1: {  // Cold -> Freeze, scaled vs player Life, duration cut by Cold Resist
            int t = freeze_ticks_for(mag * 2, player_max_hp()) * (100 - res(items::AK_RES_COLD)) / 100;
            if (t > 0) freeze_player(t);
            break;
        }
        default: break;
    }
}
// A deep-run enemy hit rolls (combat RNG, only above the Awaken gate) to inflict a random element.
void roll_enemy_element_on_player(int dmg) {
    if (g.pl.dead || dmg <= 0) return;
    const int chance = enemy_element_chance(drop_ilvl());
    if (chance <= 0 || int(g.rng.combat.below(100)) >= chance) return;
    const int elem = int(g.rng.combat.below(4));   // 0 Heat / 1 Cold / 2 Storm / 3 Toxin
    inflict_element_on_player(elem, 1 + dmg / 3 + drop_ilvl() / 2);
}

// C3: the player's ascendancy stamps its signature ailment on a landed melee hit — the OFFENSE
// side of the two-way ailment system (gear resistances are the defense). Durations are seconds
// (secs_to_ticks). Fedaykin (tank) + Saboteur/Wayfinder/Spiceweaver (traps/ranged/abilities — C6/C7)
// have no on-hit ailment here.
void apply_ascendancy_on_hit(Entity& victim, int dealt, const Foe* foe) {
    if (victim.hp <= 0) return;
    const int lvl = g.pl.st.level;
    switch (g.pl.ascendancy) {
        case Ascendancy::Sunwalker:       // Scorch (Heat DoT)
            scorch_entity(victim, 1 + dealt / 3, tune::secs_to_ticks(2.0));
            break;
        case Ascendancy::WaterReaver:     // Bleed (bypasses armor)
            bleed_entity(victim, 1 + dealt / 2, tune::secs_to_ticks(2.0));
            break;
        case Ascendancy::Sayyadina:       // Poison mastery (no AK_POISON needed)
            poison_entity(victim, 1 + dealt / 3 + lvl / 4, tune::secs_to_ticks(2.5));
            break;
        case Ascendancy::DesertGhost:     // Storm -> Blind
            blind_entity(victim, tune::secs_to_ticks(1.5));
            break;
        case Ascendancy::ReverendMother:  // Cold -> Freeze, scaled by the hit vs the foe's Life
            if (foe) { const int ft = freeze_ticks_for(dealt, foe->hp); if (ft > 0) freeze_entity(victim, ft); }
            break;
        default: break;
    }
}
// C3: DoT-afflicted enemies lose hp each world turn; a DoT kill leaves a corpse (water, no loot/xp).
void tick_enemy_ailments() {
    for (int i = 1; i < kMaxEnts; ++i) {
        Entity& e = g.ents[i];
        if (e.hp <= 0) continue;
        if (e.blind_turns > 0) --e.blind_turns;  // C3: Blind wears off (Freeze ticks in the act loop)
        int dot = 0;
        if (e.poison_turns > 0) { --e.poison_turns; dot += e.poison_dmg; }
        if (e.scorch_turns > 0) { --e.scorch_turns; dot += e.scorch_dmg; }
        if (e.bleed_turns > 0) { --e.bleed_turns; dot += e.bleed_dmg; }
        if (dot <= 0) continue;
        e.hp = int16_t(e.hp - dot);
        if (e.hp <= 0)
            e = Entity{EType::Corpse, Ai::Idle, e.zone, e.x, e.y, 0, 0, int16_t(60)};
    }
}

// ---- world turn --------------------------------------------------------------

// B8: cardinal headings for patrol pacing. Ordered N,E,S,W so (h+2)&3 is the reverse.
constexpr int kDir4[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};

// B8: a Harkonnen search party paces a fixed beat around its spawn anchor (packed into
// e.data as (x<<8)|y; heading in the low 2 bits of e.data2), reversing at a wall or the
// leash edge. trooper_act() flips it to Ai::Hunt the instant it sees the player, so patrols
// "chase on sight" and fall back to their beat when they lose you. RNG-free (deterministic).
void patrol_step(Entity& e) {
    const map::Zone& z = zone();
    const Entity& p = player_ent();
    const int ax = e.data >> 8, ay = e.data & 0xFF;
    auto step = [&](int dx, int dy) -> bool {
        const int nx = e.x + dx, ny = e.y + dy;
        if (!z.walkable(nx, ny) || entity_at(nx, ny, e.zone)) return false;
        if (nx == p.x && ny == p.y) return false;
        e.anim.facing = facing_from_tile_delta(dx, dy);
        e.x = int16_t(nx);
        e.y = int16_t(ny);
        g.noise.add(e.x, e.y, 4);  // boots on gravel
        return true;
    };
    int adx = e.x - ax, ady = e.y - ay;
    if (adx < 0) adx = -adx;
    if (ady < 0) ady = -ady;
    if ((adx > ady ? adx : ady) > tune::kPatrolLeash) {  // strayed (usually after a chase): return
        const int dx = ax > e.x ? 1 : ax < e.x ? -1 : 0;
        const int dy = ay > e.y ? 1 : ay < e.y ? -1 : 0;
        if (!step(dx, dy) && !step(dx, 0)) step(0, dy);
        return;
    }
    int h = e.data2 & 3;
    for (int attempt = 0; attempt < 2; ++attempt) {  // pace; reverse at a wall or the leash edge
        int ndx = e.x + kDir4[h][0] - ax, ndy = e.y + kDir4[h][1] - ay;
        if (ndx < 0) ndx = -ndx;
        if (ndy < 0) ndy = -ndy;
        if ((ndx > ndy ? ndx : ndy) <= tune::kPatrolLeash && step(kDir4[h][0], kDir4[h][1])) {
            e.data2 = int16_t((e.data2 & ~3) | h);
            return;
        }
        h = (h + 2) & 3;  // reverse and try the other way
    }
    e.data2 = int16_t((e.data2 & ~3) | h);  // boxed in: remember the flipped heading for next turn
}

// B8: drop one patroller at (x,y) — a Trooper flagged Ai::Patrol with its beat anchored here.
bool place_patrol(int x, int y) {
    if (x < 1 || x >= map::W - 1 || y < 1 || y >= map::H - 1) return false;
    if (!zone().walkable(x, y) || entity_at(x, y, g.cur_zone)) return false;
    Entity* e = alloc_entity();
    if (!e) return false;
    *e = Entity{EType::Trooper, Ai::Patrol, g.cur_zone, int16_t(x), int16_t(y),
                int16_t(foe_hp(EType::Trooper)), 0, int16_t((x << 8) | y), 1};
    return true;
}

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
        // lost the trail: a patroller (anchored, e.data != 0) returns to its beat; a crawler
        // guard falls back to guarding the op.
        if (pdist < 0 || pdist > 14) { e.ai = e.data != 0 ? Ai::Patrol : Ai::Guard; return; }
        if (pdist == 1) {
            e.anim.facing = facing_from_tile_delta(p.x - e.x, p.y - e.y);
            e.anim.set(anim::State::Attack);
            const int dmg = foe_attack_roll(*foe_stats(e.type), e.blind_turns > 0 ? tune::kBlindAcc : 0);
            if (dmg == 0) {
                g.log.push("You slip the trooper's blade. A miss.");
                return;
            }
            g.log.push("The trooper's blade finds you for %d.", dmg);
            foe_hit_player(dmg, "Cut down by Harkonnen steel.");
            reflect_thorns(&e);  // D1: Deflect
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
    } else if (e.ai == Ai::Patrol) {
        patrol_step(e);  // B8: pace the beat (chase-on-sight handled by the sees->Hunt flip above)
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
            foe_hit_player(4, "A hunter-seeker found its mark.");
            poison_player(2, 4);  // C3: the needle is envenomed (Toxin — Toxin Resistance mitigates the ticks)
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
            damage_player(1, "Venom in the blood, sand in the mouth.", DT_TOX);
            g.poison_turns = 20;
        }
        return;  // ambusher: it does not chase
    }

    if (e.type == EType::Hermit) {
        if (pdist == 1) {
            const int dmg = foe_attack_roll(*foe_stats(EType::Hermit), e.blind_turns > 0 ? tune::kBlindAcc : 0);
            if (dmg == 0) { g.log.push("The hermit's claws rake past you."); return; }
            g.log.push("The spice-mad hermit claws at you, shrieking prophecy!");
            foe_hit_player(dmg, "Beaten down by a prophet of the sand.");
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
            const int dmg = foe_attack_roll(*foe_stats(EType::Hound), e.blind_turns > 0 ? tune::kBlindAcc : 0);
            if (dmg == 0) {
                g.log.push("The hound snaps at empty air.");
                return;
            }
            g.log.push("The dust hound bites you for %d.", dmg);
            foe_hit_player(dmg, "Torn apart by dust hounds.");
            reflect_thorns(&e);  // D1: Deflect
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
    int drain = base * tune::kThirstHeatMul4[amb] * player_suit_mul4() / 16;
    drain = drain * (8 - g.pl.st.talents[7]) / 8;  // Water Discipline
    if (int th = affix_total(items::AK_THRIFT); th > 0)  // D1: Reduced Water Use
        drain = drain * (100 - (th > 75 ? 75 : th)) / 100;
    if (base > 0 && drain < 1) drain = 1;
    g.pl.water -= drain;
    if (g.pl.water < 0) g.pl.water = 0;
    if (g.pl.water == 0) {
        damage_player(tune::kDehydrateDamage, "Died of thirst in the Tanzerouft.", DT_TRUE);
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

    // the Litany holds the body together
    if (g.pl.litany_turns > 0) --g.pl.litany_turns;
    if (g.pl.ability_cd > 0) --g.pl.ability_cd;  // C7: the shared ability cooldown ticks down
    if (g.pl.turn % tune::kFlaskRechargeTurns == 0) flask_gain(1);  // flasks: a slow time trickle

    // pan adder venom burns water
    if (g.poison_turns > 0) {
        --g.poison_turns;
        g.pl.water -= 1;
        if (g.pl.water < 0) g.pl.water = 0;
    }

    // storm scour: sand-blast exposure regardless of hour
    if (g.storm_turns > 0) {
        --g.storm_turns;
        if (g.storm_turns == 0) {
            g.log.push("The storm passes. The desert exhales.");
            mark_trial(TRIAL_STORM);  // Desert Trials: you rode out the Coriolis and lived
        }
        if (!(tf & (map::TF_INDOOR | map::TF_SHADE))) {
            g.pl.exposure += 2;
            if (g.pl.exposure > 100) g.pl.exposure = 100;
        }
        g.noise.decay();  // the roar swallows every other sound
    }

    // heat exposure — a stillsuit shields the body from the open sun (its
    // canonical purpose), so an equipped suit blunts the gain and better suits
    // negate it; the Litany halves whatever is left.
    const bool sheltered = (tf & (map::TF_INDOOR | map::TF_SHADE)) != 0;
    if (amb >= tune::kExposureHotThreshold && !sheltered) {
        int gain = tune::kExposureGain;
        gain -= affix_total(items::AK_HEAT);  // B2a: hood/cloak heat shielding blunts the sun
        if (g.pl.litany_turns > 0) gain /= 2;
        if (gain < 0) gain = 0;
        if (g.pl.exposure == 0 && gain > 0)
            g.log.push("The sun hammers down. Reach shade or a better stillsuit.");
        g.pl.exposure += gain;
    } else {
        g.pl.exposure -= tune::kExposureRecover;
    }
    if (g.pl.exposure < 0) g.pl.exposure = 0;
    if (g.pl.exposure > 100) g.pl.exposure = 100;
    if (g.pl.exposure >= tune::kExposureCollapse) {
        g.log.push("Heatstroke! Your vision swims.");
        damage_player(tune::kExposureDamage, "Cooked by the open sun.", DT_HEAT);
    }

    // D1: deep-desert typed hazards. Awaken-scaled (env/3), so shallow runs (all current tests,
    // which never go deep) take zero — only lategame demands Storm/Cold resistance.
    const int env = drop_ilvl();
    if (env >= 3 && !sheltered && !g.pl.dead) {
        if (g.storm_turns > 0)
            damage_player(env / 3, "Flayed by the Coriolis storm.", DT_STORM);
        else if (amb <= tune::kExposureHotThreshold / 2)
            damage_player(env / 3, "The desert night leaches your heat.", DT_COLD);
    }

    // C2: energy shield (Focus) recharges once no damage has landed for a short spell.
    // D1: AK_FOCUS_REGEN (Focus Recharge %) speeds the refill rate.
    const int shmax = player_max_shield();
    if (g.pl.shield > shmax) g.pl.shield = shmax;  // gear unequipped -> clamp down
    if (g.pl.shield_delay > 0) {
        --g.pl.shield_delay;
    } else if (g.pl.shield < shmax) {
        g.pl.shield += tune::kShieldRechargePerTurn * (100 + affix_total(items::AK_FOCUS_REGEN)) / 100;
        if (g.pl.shield > shmax) g.pl.shield = shmax;
    }

    // D1: Life regen (Recovery) per turn.
    const int hpmax = player_max_hp();
    const int regen = affix_total(items::AK_LIFE_REGEN);
    if (regen > 0 && g.pl.hp > 0 && g.pl.hp < hpmax) {
        g.pl.hp += regen;
        if (g.pl.hp > hpmax) g.pl.hp = hpmax;
    }

    // D1: the Will pool regenerates toward its max (spent by abilities in C7).
    const int wmax = player_max_will();
    if (g.pl.will > wmax) g.pl.will = wmax;
    else if (g.pl.will < wmax) {
        g.pl.will += tune::kWillRegenPerTurn;
        if (g.pl.will > wmax) g.pl.will = wmax;
    }

    // C3: the combat-poison ailment ticks as Toxin damage (Toxin Resistance mitigates it in
    // damage_player). This is the payoff hook for D1's AK_RES_TOX ("of the Antidote").
    if (g.pl.ail_poison_turns > 0 && !g.pl.dead) {
        --g.pl.ail_poison_turns;
        if (g.pl.ail_poison_dmg > 0) damage_player(g.pl.ail_poison_dmg, "The venom takes you.", DT_TOX);
    }
    // C3: Scorch (Heat Resistance mitigates) and Bleed (physical, bypasses armor + shield).
    if (g.pl.ail_scorch_turns > 0 && !g.pl.dead) {
        --g.pl.ail_scorch_turns;
        if (g.pl.ail_scorch_dmg > 0) damage_player(g.pl.ail_scorch_dmg, "You burn away to nothing.", DT_HEAT);
    }
    if (g.pl.ail_bleed_turns > 0 && !g.pl.dead) {
        --g.pl.ail_bleed_turns;
        if (g.pl.ail_bleed_dmg > 0) damage_player(g.pl.ail_bleed_dmg, "You bleed out into the sand.", DT_BLEED);
    }
    if (g.pl.ail_blind_turns > 0) --g.pl.ail_blind_turns;      // C3: Blind wears off
    if (g.pl.ail_frozen_turns > 0) --g.pl.ail_frozen_turns;    // C3/RT2: Freeze wears off (locks actions)
    if (g.pl.ail_stun_turns > 0) --g.pl.ail_stun_turns;        // RT2: Stun wears off (locks actions)
}

// The player spent a turn: world advances.
// RT1.2: real-time cadence. The player may take a grid action only once per cooldown
// (kPlayerActionTicks sim-ticks); consuming an action arms it. The WORLD no longer advances
// as a side effect of player actions — game_update fires world_tick() on its own timer.
bool player_ready() {
    // RT2: Freeze and Stun lock the player out entirely — the cooldown never readies while either
    // timer is running (they tick down on the world clock in upkeep).
    if (g.pl.ail_frozen_turns > 0 || g.pl.ail_stun_turns > 0) return false;
    return g.sim_clock >= g.pl.next_act_tick;
}
void consume_action() { g.pl.next_act_tick = g.sim_clock + uint32_t(tune::kPlayerActionTicks); }

// One world step: enemies act, upkeep runs, the worm/ops/seasons advance. Fired from
// game_update every kWorldStepTicks sim-ticks (real-time), NOT per player action.
void world_tick() {
    upkeep();
    tick_enemy_ailments();  // C3: DoTs (poison/scorch/bleed) on afflicted foes
    if (g.pl.dead) return;
    pickup_currency();  // D3d: collect any currency underfoot

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
        // C3/RT2: a Frozen (Cold control) or Stunned combatant loses its turn.
        if ((e.frozen_turns > 0 || e.stun_turns > 0) &&
            (e.type == EType::Boss || e.type == EType::Trooper || e.type == EType::ShieldTrooper ||
             e.type == EType::PanAdder || e.type == EType::Hermit || e.type == EType::HunterSeeker ||
             e.type == EType::Hound)) {
            if (e.frozen_turns > 0) --e.frozen_turns;
            if (e.stun_turns > 0) --e.stun_turns;
            continue;
        }
        if (e.type == EType::Boss) {
            const uint8_t traits = uint8_t(e.data >> 8);
            e.energy += int16_t((traits & BT_SWIFT) ? 150 : 100);
            while (e.energy >= 100 && !g.pl.dead) {
                e.energy -= 100;
                const int pdist = g.dmap[e.y * map::W + e.x];
                if (pdist == 1) {
                    e.anim.facing = facing_from_tile_delta(player_ent().x - e.x,
                                                           player_ent().y - e.y);
                    e.anim.set(anim::State::Attack);
                    const Foe f = boss_foe(e);
                    const int dmg = foe_attack_roll(f, e.blind_turns > 0 ? tune::kBlindAcc : 0);
                    if (dmg == 0) { g.log.push("You slip the champion's blade."); continue; }
                    if (traits & BT_TOXIC) {
                        g.poison_turns = 15;
                        g.log.push("The blade is POISONED - your water burns.");
                    }
                    g.log.push("The champion strikes you for %d.", dmg);
                    foe_hit_player(dmg, e.data2 == -1 ? "Slain by the Baron himself."
                                                      : "Cut down by the Baron's champion.");
                    reflect_thorns(&e);  // D1: Deflect
                } else {
                    int best_x = e.x, best_y = e.y;
                    int16_t best = pdist >= 0 ? pdist : INT16_MAX;
                    for (int dy2 = -1; dy2 <= 1; ++dy2)
                        for (int dx2 = -1; dx2 <= 1; ++dx2) {
                            if (!dx2 && !dy2) continue;
                            const int nx2 = e.x + dx2, ny2 = e.y + dy2;
                            if (!zone().walkable(nx2, ny2)) continue;
                            if (entity_at(nx2, ny2, e.zone)) continue;
                            if (nx2 == player_ent().x && ny2 == player_ent().y) continue;
                            const int16_t d = g.dmap[ny2 * map::W + nx2];
                            if (d >= 0 && d < best) { best = d; best_x = nx2; best_y = ny2; }
                        }
                    e.x = int16_t(best_x);
                    e.y = int16_t(best_y);
                }
            }
            continue;
        }
        if (e.type == EType::Trooper || e.type == EType::ShieldTrooper) {
            if (e.type == EType::ShieldTrooper)
                g.noise.add(e.x, e.y, 8);  // heavy armor clatters across the sand
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
    // Desert Trials: the Maker surfaced at your heels and has now gone back under — you crossed.
    if (before == worm::State::Surfaced && g.wrm.state != worm::State::Surfaced &&
        !g.pl.dead && !g.pl.riding)
        mark_trial(TRIAL_WORM);

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
    if (!g.pl.sandwalk && g.noise_meter > 35 && g.cur_zone != map::kZoneSietch &&
        (g.pl.turn % 40) == 17)
        g.log.push("Your rhythm carries through the sand. (C: sandwalk)");
    g.seismo[g.seismo_head] = g.noise_meter;
    g.seismo_head = (g.seismo_head + 1) & 63;

    refresh_fov_and_paths();
}

// ---- worm-riding / extraction --------------------------------------------------

void enter_node(int idx);  // fwd (defined below)

constexpr int kMaxRideTurns = 40;
constexpr int kExtractRideTurns = 6;  // B4: a few strides of spectacle, then the Maker bears you home

// B4: the ONE purpose of worm-riding is extraction. Ride home = end the vision and wake in the
// sietch; the Fremen forge/secure the vision's yield (gear + spice/water/materials). Gear and the
// character are never lost; only un-extracted run resources die with the vision (loss model = B5).
void extract_run() {
    g.pl.riding = false;
    g.pl.ride_turns = 0;
    g.wrm = worm::Worm{};
    g.noise.clear();
    g.services.mixer->push({audio::Cmd::Rumble, 0, 0, 0});
    g.log.push("You let Muad'Dib's vision fade. The desert releases you toward home.");
    g.log.push("The Fremen forge what the vision showed. Your haul is made real.");
    g_meta.banked_spice = g.pl.spice;  // B5: the whole run's spice is now safe (banked)
    if (g.pl.reclaimed > 0) {  // C5: the Deathstill's reclaimed water renders into Litrejons for the tribe
        const int gained = g.pl.reclaimed / tune::kLitrejonPerWater;
        if (gained > 0) {
            g_meta.litrejons += gained;
            g.log.push("The tribe's deathstills render %d litrejons from your reclaimed water.", gained);
        }
        g.pl.reclaimed = 0;
    }
    guild_tithe_accrue();              // B6: the Guild expects its cut of every haul (debt creeps up)
    persist_character();               // gear + progress made real (saves meta, incl. the tithe)
    enter_node(0);      // wake in the sietch hub
    refresh_fov_and_paths();  // RT1.2: settle FOV/paths after the transition (world runs on its timer)
}

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

// Step 1a: the Ride key now "fades the vision" = EXTRACTION (worm-riding is parked, reserved for the
// endgame "Long Ride"). Choosing to end Paul's vision carries you home with your haul — no Maker
// needed; you begin each vision near your objective, so there is nothing to ride to. Works anywhere
// in a run (the extract-or-push early-bail); completing the vision's graph also extracts (use_door).
void return_from_rendezvous();  // fwd (defined below, with enter_rendezvous)
void try_ride() {
    if (g.at_rendezvous) {  // B7: leave the Ninth Rock (a plain trip home, not an extraction)
        return_from_rendezvous();
        return;
    }
    if (g.cur_node == 0) {
        g.log.push("You are already awake in the sietch.");
        return;
    }
    g.log.push("You choose to end the vision here.");
    extract_run();
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

    // Worm-riding is parked (reserved for the endgame Long Ride). ride_move is currently unreachable
    // (try_ride no longer mounts); if it revives, a tired Maker dives and ends the ride — it does NOT
    // extract (extraction is fading the vision, not riding).
    if (++g.pl.ride_turns >= kExtractRideTurns) {
        dismount("The Maker tires and dives beneath the sand. The ride ends.");
        (void)cb;
        return;
    }
    consume_action();
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
void enter_zone(uint8_t id, uint64_t gen_seed, bool use_spawn, int at_x = 0, int at_y = 0,
                bool roll_events = true) {
    g.cur_zone = id;
    int sx, sy;
    map::generate(zone(), gen_seed, map::ZoneId(id), sx, sy);
    apply_deltas(zone(), id);
    spawn_zone_wildlife(zone(), id);
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

    // one weighted desert-event roll per entry (outdoors only). Suppressed (roll_events=false) for
    // indoor cave entries and when resuming a field zone after a cave excursion (B9), so a detour
    // neither rolls a storm underground nor re-rolls the field's entry event on the way back.
    g.storm_turns = 0;
    if (roll_events && id != map::kZoneSietch) {
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
        } else if (roll < 44 && drop_ilvl() >= tune::kPatrolMinDepth) {
            // B8: a deep-desert Harkonnen search party quarters the sand and hunts on sight.
            // Depth-gated (like the D1 hazards), so shallow/headless runs never roll it.
            int spawned = 0;
            for (int tries = 0; tries < 80 && spawned < 2; ++tries) {
                const int rx = g.rng.world.range(-15, 15), ry = g.rng.world.range(-15, 15);
                if (rx * rx + ry * ry < 49) continue;  // don't drop the beat on top of the player
                if (place_patrol(p.x + rx, p.y + ry)) ++spawned;
            }
            if (spawned) g.log.push("A Harkonnen patrol quarters the sand. Keep to the rocks.");
        } else if (roll < 50 && drop_ilvl() >= tune::kPatrolMinDepth) {
            // B8: the deep bled is His country — wormsign on the wind (also depth-gated).
            g.wrm.state = worm::State::Stirring;
            g.log.push("Wormsign on the wind. The deep bled belongs to Shai-Hulud.");
        } else if (roll < 54 && drop_ilvl() >= tune::kPatrolMinDepth) {
            // B9: a hidden cavern mouth — a rare, optional risk/reward detour (depth-gated).
            for (int tries = 0; tries < 60; ++tries) {
                const int x = p.x + g.rng.world.range(-12, 12), y = p.y + g.rng.world.range(-12, 12);
                if (x < 2 || x >= map::W - 2 || y < 2 || y >= map::H - 2) continue;
                if (!zone().walkable(x, y) || entity_at(x, y, id)) continue;
                if (Entity* e = alloc_entity()) {
                    *e = Entity{EType::CaveMouth, Ai::Idle, id, int16_t(x), int16_t(y), 1, 0, 0};
                    g.log.push("A dark fissure gapes in the rock — a cavern mouth. (Step in to enter.)");
                }
                break;
            }
        }
        // D3c: the old deep-desert Worm-Tooth find is retired — Worm Tooth now drops via the unified
        // area-level currency table (maybe_drop_loot), like every other reagent.
    }

    refresh_fov_and_paths();
    flash(10, 10, 10);
}

// B1.3: traverse the world graph. Each node is a fresh instance of the resident zone,
// regenerated from the node's own seed + its Stage's biome. Consecutive nodes can share a
// map::ZoneId, so we must clear the previous node's entities + per-node state so each node
// spawns fresh (the membership filters key off e.zone == cur_zone).
void enter_node(int idx) {
    if (idx < 0 || idx >= g.graph.count) return;
    world::Node& n = g.graph.nodes[idx];
    g.cur_node = idx;
    n.flags |= world::NF_VISITED;

    // wipe the outgoing node's transient occupants (keep the player, ents[0])
    for (int i = 1; i < kMaxEnts; ++i) g.ents[i] = Entity{};
    g.op = harvest::Op{};        // the harvester op is per-node (retired in B3)
    g.alarm_turns = 0;
    g.alarm_spawn_cd = 0;
    g.in_cave = false;           // B9: entering a real node ends any cave excursion
    g.in_strike = false;         // B10.2: entering a real node ends any strike excursion
    g.in_pinnacle = false;       // B10.3: entering a real node ends any pinnacle assault
    g.in_capstone = false;       // B10.4: entering a real node ends any capstone confrontation
    g.delta_count = 0;           // tile edits (churn, breaches) reset per node
    g.spawned_mask = 0;          // let spawn_zone_wildlife repopulate this fresh node

    const uint8_t zid = uint8_t(world::zone_id_of(n.stage));
    enter_zone(zid, n.seed, /*use_spawn=*/true);

    // B3: the vision's content spawns on entry. Field nodes field an elite Harkonnen escort;
    // Pinnacle nodes field the Awaken level's named boss. (Cleared nodes stay quiet on re-entry.)
    if (!(n.flags & world::NF_CLEARED)) {
        if (n.type == world::NodeType::Field) spawn_escort(n.awaken);
        else if (n.type == world::NodeType::Pinnacle) spawn_pinnacle(n.awaken);
    }
}

// The Water-of-Life ritual (B2) launches a campaign run at node 1; here it's a bare trigger.
// B5: launch the vision at the chosen Awaken level (its first field node on the spine).
void begin_run() {
    // Step 1b: linear (Hades-style) progression — a vision-run always launches at your CURRENT
    // frontier step (no difficulty picker). Headless tests always start at step 1 for isolation.
    int lvl = g.services.interactive ? int(g_meta.awaken_unlocked) : 1;
    if (lvl < 1) lvl = 1;
    if (lvl > world::kAwakenLevels) lvl = world::kAwakenLevels;
    const int first = 1 + (lvl - 1) * (world::kRoomsPerLevel + 1);  // first field node of the level
    if (first >= 1 && first < g.graph.count) enter_node(first);
    else { const int f = g.graph.forward(0); if (f >= 0) enter_node(f); }
}

// B7: the Guild rendezvous at the Ninth Rock — a short remote-desert excursion (NOT a run) to
// exchange spice with the Guild agent. Canon: the Fremen never let the Guild near a sietch, so the
// tithe is paid at a designated point in the open desert. Reuses the sideobj Envoy (footprint trail
// + bump-to-pay both apply); the fixed seed gives a stable "Ninth Rock" layout.
inline constexpr uint64_t kNinthRockSeed = 0x9E9E9E9E9E9E9E9Eull;
void enter_rendezvous() {
    g.at_rendezvous = true;
    for (int i = 1; i < kMaxEnts; ++i) g.ents[i] = Entity{};  // wipe the sietch's occupants
    g.op = harvest::Op{};
    g.alarm_turns = 0; g.alarm_spawn_cd = 0;
    g.delta_count = 0; g.spawned_mask = 0;
    enter_zone(map::kZoneDesert, kNinthRockSeed, /*use_spawn=*/true);
    // the Guild agent waits at a fixed rock, opposite the player's landing (a short walk).
    const Entity& p = player_ent();
    const int ax = p.x < map::W / 2 ? map::W - 6 : 5;
    place_npc_near(EType::Envoy, zone(), map::kZoneDesert, ax, p.y);
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::Envoy) {  // register the agent as the sideobj (trail + bump)
            g.sideobj_kind = 2; g.sideobj_zone = g.cur_zone;
            g.sideobj_x = g.ents[i].x; g.sideobj_y = g.ents[i].y;
            g.sideobj_spawned = true; g.sideobj_expires = g.pl.turn + 100000;  // won't expire mid-trip
            break;
        }
    g.log.push("The Guild waits at the Ninth Rock. They never ask where we came from.");
    g.log.push("We never ask where they go. Spice changes hands, and the desert keeps its secrets.");
    refresh_fov_and_paths();
}
void return_from_rendezvous() {
    g.at_rendezvous = false;
    g.sideobj_kind = 0; g.sideobj_spawned = false;
    g.log.push("You slip back across the sand toward home.");
    enter_node(0);  // wake in the sietch hub (wipes the rendezvous agent)
    refresh_fov_and_paths();
}

// B9: stock a freshly-entered cavern — the payoff (a rolled item + raw spice) with a lurking
// guardian for the risk. Drawn from the global world stream, so a given playthrough re-simulates
// the same cave; contents then live in g.ents (serialized), so save/load needs no re-population.
void populate_cave() {
    int16_t tiles[map::N];
    int nt = 0;
    const Entity& p = player_ent();
    for (int y = 2; y < map::H - 2; ++y)
        for (int x = 2; x < map::W - 2; ++x) {
            if (!zone().walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
            const int dd = (x - p.x) * (x - p.x) + (y - p.y) * (y - p.y);
            if (dd < 16) continue;  // keep the entry clear
            if (nt < map::N) tiles[nt++] = int16_t(y * map::W + x);
        }
    if (nt == 0) return;
    auto pick = [&]() -> int {
        const int k = int(g.rng.world.below(uint32_t(nt)));
        const int t = tiles[k];
        tiles[k] = tiles[--nt];  // sample without replacement
        return t;
    };
    // the payoff: one rolled item, biased a tier up and toward magic/rare (the reason to detour)
    if (nt > 0)
        for (int i = 1; i < kMaxEnts; ++i)
            if (g.ents[i].type == EType::None) {
                const int t = pick();
                items::Inst it = roll_item(drop_ilvl() + 2, 50 + run_difficulty() * 6);
                g.ents[i] = Entity{EType::Loot, Ai::Idle, g.cur_zone,
                                   int16_t(t % map::W), int16_t(t / map::W), 1, 0,
                                   int16_t(it.def), 0};
                g.loot_inst[i] = it;
                break;
            }
    for (int c = 0; c < 3 && nt > 0; ++c) {
        const int t = pick();
        if (Entity* e = alloc_entity())
            *e = Entity{EType::SpiceCache, Ai::Idle, g.cur_zone,
                        int16_t(t % map::W), int16_t(t / map::W), 1, 0, 2};
    }
    if (nt > 0) {  // the guardian
        const int t = pick();
        if (Entity* e = alloc_entity())
            *e = Entity{EType::PanAdder, Ai::Idle, g.cur_zone,
                        int16_t(t % map::W), int16_t(t / map::W),
                        int16_t(foe_hp(EType::PanAdder)), 0, 0};
    }
}

// B9: duck into a hidden cavern found in a deep field node. A transient excursion (cur_node is
// unchanged) — the field zone's entities stay parked in g.ents (filtered out by zone) and resume
// when you climb back out at cave_ret_*. The cave layout is a pure function of cave_seed.
void enter_cave() {
    const Entity& p = player_ent();
    g.cave_ret_zone = g.cur_zone;
    g.cave_ret_x = p.x;
    g.cave_ret_y = p.y;
    g.cave_seed = world::node_hash(g.graph.nodes[g.cur_node].seed,
                                   uint64_t(p.x * map::W + p.y) + 0xCA1Eull);
    g.in_cave = true;
    // remove the cave-mouth we entered by (it lived in the field zone); do NOT wipe field mobs
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::CaveMouth) g.ents[i] = Entity{};
    enter_zone(map::kZoneCave, g.cave_seed, /*use_spawn=*/true, 0, 0, /*roll_events=*/false);
    populate_cave();
    g.log.push("You squeeze through a fissure into a hidden cavern. The air lies still and cool.");
    refresh_fov_and_paths();
}
void exit_cave() {
    g.in_cave = false;
    for (int i = 1; i < kMaxEnts; ++i)  // clear the cave's own occupants; field mobs remain
        if (g.ents[i].zone == map::kZoneCave) g.ents[i] = Entity{};
    const uint64_t fseed = g.graph.nodes[g.cur_node].seed;
    enter_zone(g.cave_ret_zone, fseed, /*use_spawn=*/false, g.cave_ret_x, g.cave_ret_y,
               /*roll_events=*/false);  // resume the field node where we left it
    g.log.push("You climb back into the open, richer for the detour.");
    refresh_fov_and_paths();
}

// B2b: open the Revelation at a forward door — Muad'Dib's vision reveals gear for the road ahead.
// Offers 3 (preferably distinct) slots; the pick rolls 3 items and advances to `dest` after choosing.
void open_revelation(int dest) {
    g.rev_dest = dest;
    g.rev_sel = 0;
    const int kN = int(items::Slot::Count);
    for (int i = 0; i < 3; ++i) {
        uint8_t s = 0;
        for (int tries = 0; tries < 12; ++tries) {
            s = uint8_t(g.rng.loot.below(uint32_t(kN)));
            const bool dup = (i > 0 && s == g.rev_slot_offer[0]) ||
                             (i > 1 && s == g.rev_slot_offer[1]);
            if (!dup) break;
        }
        g.rev_slot_offer[i] = s;
    }
    g.revelation_stage = 1;
    g.log.push("Muad'Dib's sight settles on what you will need...");
}

// B2c: human-readable name of a journey stage (for the forward-door preview).
const char* stage_name(world::Stage s) {
    switch (s) {
        case world::Stage::Sietch:     return "the sietch";
        case world::Stage::DeepDesert: return "the deep desert";
        case world::Stage::Rimwall:    return "the rimwall";
        case world::Stage::ShieldWall: return "the Shield Wall";
        case world::Stage::Outskirts:  return "the Arrakeen outskirts";
        default:                       return "Arrakeen";
    }
}

// Resolve the current Revelation selection (shared by the modal input + the debug hook).
void revelation_confirm() {
    if (g.revelation_stage == 1) {  // slot chosen -> roll 3 items of that slot
        const items::Slot s = items::Slot(g.rev_slot_offer[g.rev_sel]);
        for (int i = 0; i < 3; ++i)
            g.rev_item_offer[i] =
                roll_item_for_slot(2 + run_difficulty(), 30 + run_difficulty() * 6, s);
        g.revelation_stage = 2;
        g.rev_sel = 0;
    } else if (g.revelation_stage == 2) {  // item chosen -> take it, then advance
        const items::Inst chosen = g.rev_item_offer[g.rev_sel];
        const bool placed = !chosen.empty() && pack_add(chosen) >= 0;
        char nm[64];
        chosen.full_name(nm, sizeof nm);
        g.log.push(placed ? "Muad'Dib's sight was true. You will bear: %s"
                          : "Your pack has no room; the vision of %s fades.", nm);
        const int dest = g.rev_dest;
        g.revelation_stage = 0;
        enter_node(dest);
        refresh_fov_and_paths();  // RT1.2: settle FOV/paths after the transition
    }
}

// C6: shared enemy-death resolution (boss/strike/pinnacle/capstone/loot/xp/corpse/quest) —
// used by both the melee bump (try_move) and the maula shot. The caller consume_action()s after.
void resolve_enemy_death(Entity* victim, const Foe* foe) {
            g.hitstop = 3;  // the world holds its breath
            // D1 on-kill: Life on Kill + Focus on Kill.
            gain_hp(affix_total(items::AK_LIFE_ON_KILL));
            gain_shield(affix_total(items::AK_FOCUS_ON_KILL));
            flask_gain(tune::kFlaskChargePerKill);  // flasks charge from the kill
            if (foe) {
                int xp = foe->xp;
                if (g.pl.st.level > foe->level + 2) xp /= 2;
                grant_xp(xp);
            }
            // B3: a named pinnacle clears its node; the Awaken-10 Baron ends the vision (victory).
            const bool pinnacle = victim->type == EType::Boss && boss_is_pinnacle(victim->data2);
            const bool was_baron = victim->type == EType::Boss &&
                                   (victim->data2 == -1 ||
                                    (pinnacle && boss_pinnacle_idx(victim->data2) == 9));
            if (pinnacle) {
                award_pinnacle_clear(g.cur_node);
                raise_standing(8);
            }
            maybe_drop_loot(victim->type, victim->x, victim->y);
            if (was_baron) {
                *victim = Entity{EType::Corpse, Ai::Idle, uint8_t(g.cur_zone),
                                 victim->x, victim->y, 0, 0, 400};
                end_season(campaign::kVictory);
                return;
            }
            if (g.in_strike && victim->type == EType::Boss) {  // B10.2: the strike objective falls
                *victim = Entity{EType::Corpse, Ai::Idle, uint8_t(g.cur_zone),
                                 victim->x, victim->y, 0, 0, 200};
                resolve_strike();  // press the target + melt home (wipes this zone)
                return;
            }
            if (g.in_pinnacle && victim->type == EType::Boss) {  // B10.3: the homeworld boss falls
                *victim = Entity{EType::Corpse, Ai::Idle, uint8_t(g.cur_zone),
                                 victim->x, victim->y, 0, 0, 300};
                resolve_pinnacle();  // take the Golden Path shard + return home
                return;
            }
            if (g.in_capstone && victim->type == EType::Boss) {  // B10.4: the alternate Paul falls
                *victim = Entity{EType::Corpse, Ai::Idle, uint8_t(g.cur_zone),
                                 victim->x, victim->y, 0, 0, 400};
                resolve_capstone();  // refuse the Golden Path (apex trophy) + return home
                return;
            }
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
                    g.log.push("The bulwark's guard breaks at last. He falls.");
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
                case EType::Boss:
                    if (victim->data2 == -1) {
                        g.log.push("The Baron falls, disbelieving, into the sand.");
                        g.log.push("His water belongs to the tribe. ALL of it.");
                    } else if (boss_is_pinnacle(victim->data2)) {
                        g.log.push("%s dies. Muad'Dib's sight was true.",
                                   kPinnacles[boss_pinnacle_idx(victim->data2)].name);
                    } else {
                        g.log.push("The champion dies hard. His gear is yours.");
                        raise_standing(5);
                    }
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
}

// C6: the first hostile entity on a tile (the maula's line-of-fire target set).
Entity* hostile_at(int x, int y) {
    static const EType kHostiles[] = {EType::Hound, EType::Trooper, EType::Spotter, EType::ShieldTrooper,
                                      EType::PanAdder, EType::Hermit, EType::HunterSeeker, EType::Boss};
    for (EType t : kHostiles) if (Entity* e = entity_at(x, y, g.cur_zone, t)) return e;
    return nullptr;
}
// C6: fire the maula pistol toward (tx,ty) — a Bresenham line from the player; a wall stops the dart,
// else it strikes the FIRST hostile on the line for lethality-scaled damage. LOUD (the report carries).
void fire_maula(int tx, int ty) {
    if (g.pl.dead || g.pl.riding) return;
    Entity& p = player_ent();
    if (tx < 0 || ty < 0 || (tx == p.x && ty == p.y)) return;  // no valid aim
    int x0 = p.x, y0 = p.y;
    const int dx = (tx > x0 ? tx - x0 : x0 - tx), dy = -(ty > y0 ? ty - y0 : y0 - ty);
    const int sx = x0 < tx ? 1 : -1, sy = y0 < ty ? 1 : -1;
    int err = dx + dy;
    Entity* victim = nullptr; int hx = x0, hy = y0;
    for (int step = 0; step < tune::kMaulaRange; ++step) {
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        hx = x0; hy = y0;
        if (!zone().walkable(x0, y0)) break;         // a wall (or the edge) stops the dart
        if ((victim = hostile_at(x0, y0)) != nullptr) break;  // first hostile on the line
        if (x0 == tx && y0 == ty) break;             // reached the aimed tile, nothing hit
    }
    p.anim.facing = facing_from_tile_delta(tx - p.x, ty - p.y);
    p.anim.set(anim::State::Attack);
    g.noise.add(p.x, p.y, tune::kMaulaNoise);   // the crack draws every ear on the sand
    g.impact_x = int16_t(hx); g.impact_y = int16_t(hy);
    g.services.mixer->push({audio::Cmd::PlaySfx, 1, 180, 120});
    if (!victim) {
        g.log.push("The maula cracks. The dart spends itself on stone.");
        consume_action();
        return;
    }
    g.impact_kind = victim->type == EType::ShieldTrooper ? 1 : 0;
    Foe boss_f{};
    if (victim->type == EType::Boss) boss_f = boss_foe(*victim);
    const Foe* foe = victim->type == EType::Boss ? &boss_f : foe_stats(victim->type);
    bool crit = false;
    const int dealt = foe ? player_attack_roll(*foe, &crit) : g.pl.atk;  // lethality-scaled
    if (dealt == 0) { g.log.push("The dart glances off. A miss."); consume_action(); return; }
    victim->hp -= int16_t(dealt);
    if (int lch = affix_total(items::AK_LEECH); lch > 0) gain_hp(dealt * lch / 100);
    if (int loh = affix_total(items::AK_LIFE_ON_HIT); loh > 0) gain_hp(loh);
    if (int ws = affix_total(items::AK_WILL_SIPHON); ws > 0) gain_will(ws);
    if (crit) g.log.push("CRIT - %d! The dart takes him clean.", dealt);
    else g.log.push("The maula hits for %d.", dealt);
    if (victim->hp <= 0) resolve_enemy_death(victim, foe);
    consume_action();
}

void try_move(int dx, int dy) {
    Entity& p = player_ent();
    const int nx = p.x + dx, ny = p.y + dy;
    map::Zone& z = zone();

    // B1.3: zone-to-zone travel is via the graph (doors/nodes), not the map edge.
    if (nx < 0 || nx >= map::W || ny < 0 || ny >= map::H) return;
    const uint8_t tf = z.at(nx, ny);

    // the surfaced worm is a wall of flesh — bump = strike
    if (worm_occupies(nx, ny)) {
        const items::Inst& w0 = g.pl.equip[int(items::Slot::Weapon)];
        const int wavg = w0.empty() ? 2 : (w0.d().a + w0.d().b) / 2;
        const int dealt = worm::hit(g.wrm, nx, ny, wavg + player_lethality(),
                                    worm_callbacks());
        if (dealt > 0) g.log.push("Your blade bites the soft flesh of the maw!");
        g.noise.add(p.x, p.y, 12);
        consume_action();
        return;
    }

    if (tf & map::TF_CISTERN) {
        if (g.pl.water < player_max_water()) {
            g.pl.water = player_max_water();
            g.log.push("You drink deep from the cistern. The tribe provides.");
            g.services.mixer->push({audio::Cmd::PlaySfx, 1, 150, 100});
        } else {
            g.log.push("Your water is full.");
        }
        consume_action();
        return;
    }

    if (Entity* car = entity_at(nx, ny, g.cur_zone, EType::Caravan)) {
        const items::Inst gift = roll_item(2 + run_difficulty(), 30 + run_difficulty() * 6);
        const bool taken = pack_add(gift) >= 0;
        if (taken) {
            char nm[64];
            gift.full_name(nm, sizeof nm);
            g.log.push("The caravan master presses contraband on you: %s.", nm);
        } else g.log.push("The caravan has wares, but your pack has no room.");
        g.pl.spice += 2;
        g.log.push("They share two grains for the road. The caravan breaks camp.");
        car->type = EType::None;
        g.sideobj_kind = 0;
        consume_action();
        return;
    }
    if (Entity* env = entity_at(nx, ny, g.cur_zone, EType::Envoy)) {
        const int bribe = 6 + run_difficulty();
        if (g.pl.spice >= bribe) {
            g.pl.spice -= bribe;
            g.season.alert = g.season.alert > 25 ? g.season.alert - 25 : 0;
            g.season.discovery = 0;
            guild_tithe_pay(tune::kGuildBribeReduce);  // B6: paying the Guild also cools the tithe
            g.log.push("The Guildsman weighs %d grains. 'The surveys will... err.'", bribe);
            g.log.push("The Guild's blindness is bought. The tithe eases; discovery resets.");
            env->type = EType::None;
            g.sideobj_kind = 0;
        } else {
            g.log.push("'%d grains buys blindness. You carry %d. Return heavier.'",
                       bribe, g.pl.spice);
        }
        consume_action();
        return;
    }
    if (entity_at(nx, ny, g.cur_zone, EType::CaveMouth)) {  // B9: step into the fissure = enter
        enter_cave();
        consume_action();
        return;
    }
    // NPCs are click-to-talk now: a bump just blocks (no menu). Interaction is
    // driven from game_update when the player clicks the NPC (see interact_kind).
    if (entity_at(nx, ny, g.cur_zone, EType::Naib) ||
        entity_at(nx, ny, g.cur_zone, EType::Sayyadina) ||
        entity_at(nx, ny, g.cur_zone, EType::Smuggler) ||
        entity_at(nx, ny, g.cur_zone, EType::Paul) ||
        entity_at(nx, ny, g.cur_zone, EType::Fremen))
        return;

    Entity* victim = entity_at(nx, ny, g.cur_zone, EType::Hound);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::Trooper);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::Spotter);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::ShieldTrooper);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::PanAdder);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::Hermit);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::HunterSeeker);
    if (!victim) victim = entity_at(nx, ny, g.cur_zone, EType::Boss);
    if (victim) {
        player_ent().anim.facing = facing_from_tile_delta(dx, dy);
        player_ent().anim.set(anim::State::Attack);
        g.impact_x = int16_t(nx);
        g.impact_y = int16_t(ny);
        // C2: the bulwark's heavy armor sparks (metal impact fx); no Holtzman shimmer/slow-blade.
        g.impact_kind = victim->type == EType::ShieldTrooper ? 1 : 0;
        Foe boss_f{};
        if (victim->type == EType::Boss) boss_f = boss_foe(*victim);
        const Foe* foe = victim->type == EType::Boss ? &boss_f : foe_stats(victim->type);
        bool crit = false;
        int dealt = foe ? player_attack_roll(*foe, &crit) : g.pl.atk;
        g.noise.add(nx, ny, victim->type == EType::Spotter ? 4 : 12);  // blades are quiet
        if (dealt == 0) {
            g.log.push("Your blade whispers past. A miss.");
            consume_action();
            return;
        }
        victim->hp -= int16_t(dealt);
        // D1 on-hit sustain: Siphon (% of damage dealt), flat Life on Hit, Will Siphon.
        if (int lch = affix_total(items::AK_LEECH); lch > 0)
            gain_hp(dealt * lch / 100);
        if (int loh = affix_total(items::AK_LIFE_ON_HIT); loh > 0) gain_hp(loh);
        if (int ws = affix_total(items::AK_WILL_SIPHON); ws > 0) gain_will(ws);
        // C3: AK_POISON (of Venom / Poison Damage) applies a Poison DoT — activates the D1 stat.
        if (int ap = affix_total(items::AK_POISON); ap > 0 && victim->hp > 0)
            poison_entity(*victim, 1 + ap / 20, 5);
        // RT2: a heavy blow can Stun (damage vs the foe's max Life; Stun Buildup inflates it).
        if (victim->hp > 0 && foe) {
            if (int st = roll_stun_ticks(dealt, foe->hp, affix_total(items::AK_STUN_MULT), 0); st > 0) {
                stun_entity(*victim, st);
                g.log.push("Staggered!");
            }
        }
        apply_ascendancy_on_hit(*victim, dealt, foe);  // C3: the ascendancy's signature ailment
        if (crit) g.log.push("CRIT - %d! The blade finds the seam.", dealt);
        else g.log.push("You hit for %d.", dealt);
        if (victim->hp <= 0) { resolve_enemy_death(victim, foe); } else {
            g.log.push("You strike %s.",
                       victim->type == EType::Hound ? "the dust hound" :
                       victim->type == EType::Trooper ? "the trooper" : "the spotter");
        }
        consume_action();
        return;
    }

    if (tf & map::TF_BLOCK) return;  // bump silently
    // Doors are click-to-use now (water seals the sietch): a bump just blocks, so
    // you never leave by walking onto the pad. The exit fires from game_update when
    // the door is clicked and the player is adjacent (see use_door / interact_kind).
    if (tf & map::TF_DOOR) return;

    p.x = int16_t(nx);
    p.y = int16_t(ny);

    const uint8_t floor = z.floor[ny * map::W + nx];
    const int noise = step_noise(floor);
    if (noise > 0) g.noise.add(nx, ny, noise);
    if (floor == 6 && noise >= 20) g.log.push("BOOM. Drum sand resounds underfoot.");

    if (entity_at(nx, ny, g.cur_zone, EType::Corpse))
        g.log.push("A corpse lies here. G harvests its water.");

    if (g.pl.free_moves > 0) {  // Threadwalk: two fast strides outside the cooldown
        --g.pl.free_moves;
        refresh_fov_and_paths();
        return;  // no cooldown consumed — the free stride is instant
    }
    consume_action();
    // RT1.2: sandwalk is now a slower glide (kGlideStepSandwalk), not a double world-tick.
}

void place_glowglobe() {
    Entity& p = player_ent();
    // standing on one? pick it back up
    if (Entity* globe = entity_at(p.x, p.y, g.cur_zone, EType::Glowglobe)) {
        globe->type = EType::None;
        ++g.pl.glowglobes;
        g.log.push("You pocket the glowglobe.");
        consume_action();
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
    consume_action();
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
    consume_action();
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
    const int plant_turns_needed =
        harvest::kPlantTurns - g.pl.st.talents[4] < 1 ? 1
            : harvest::kPlantTurns - g.pl.st.talents[4];  // Demolitions
    if (g.op.plant_progress < plant_turns_needed) {
        g.log.push("Working the charge into the hull seam... (%d/%d)",
                   int(g.op.plant_progress), plant_turns_needed);
    } else {
        --g.pl.charges;
        g.op.state = harvest::OpState::Destroyed;
        g.noise.add(g.op.x, g.op.y, 90);
        g.services.mixer->push({audio::Cmd::PlaySfx, 0, 255, 140});
        flash(255, 180, 60);
        add_flash(g.op.x, g.op.y, 10, 31, 3);  // fireball light
        op_denied("The crawler blossoms into fire and shrapnel.");
    }
    consume_action();
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
    consume_action();
}

void try_harvest() {
    Entity& p = player_ent();
    // stealing from the crawler hold takes priority when adjacent to it
    if (player_adjacent_op() && g.op.hold >= 15) {
        g.op.hold = int16_t(g.op.hold - 15);
        g.pl.spice += (((g.op.mutators & campaign::MUT_SLOPPY) ? 8 : 5)
                       * (100 + affix_total(items::AK_SPICE_FIND)) / 100);  // D1: Spice Find
        g.op.stolen = true;
        g.noise.add(p.x, p.y, 8);
        g.log.push("You scoop raw spice from the hold. +5 grains.");
        consume_action();
        return;
    }
    if (Entity* drop = entity_at(p.x, p.y, g.cur_zone, EType::Loot)) {
        const items::Inst it = loot_of(*drop);
        if (pack_add(it) >= 0) {  // G: place into the first-fit grid cell that fits its footprint
            char nm[64];
            it.full_name(nm, sizeof nm);
            g.log.push("Taken: %s (I to equip).", nm);
            drop->type = EType::None;
            g.services.mixer->push({audio::Cmd::PlaySfx, 1, 150, 140});
            consume_action();
            return;
        }
        g.log.push("Your pack has no room for that. Bank gear at the sietch vault.");
        return;
    }
    if (Entity* cache = entity_at(p.x, p.y, g.cur_zone, EType::SpiceCache)) {
        p.anim.set(anim::State::Use);
        g.pl.spice += cache->data * (100 + affix_total(items::AK_SPICE_FIND)) / 100;  // D1: Spice Find
        g.log.push("You scoop %d grains of raw spice from the sand.", int(cache->data));
        cache->type = EType::None;
        g.noise.add(p.x, p.y, 6);
        g.services.mixer->push({audio::Cmd::PlaySfx, 1, 150, 140});
        consume_action();
        return;
    }
    if (Entity* c = entity_at(p.x, p.y, g.cur_zone, EType::Corpse)) {
        p.anim.set(anim::State::Use);
        const int yield = c->data * (100 + affix_total(items::AK_WATER_FIND)) / 100;  // D1: Water Find
        g.pl.water += yield;                     // survival water for the run
        if (g.pl.water > player_max_water()) g.pl.water = player_max_water();
        g.pl.reclaimed += yield;                 // C5: also banks toward Litrejons on extraction
        g.log.push("The deathstill takes its due. +%d water (reclaimed for the tribe).", yield);
        c->type = EType::None;
        g.services.mixer->push({audio::Cmd::PlaySfx, 1, 150, 140});
        consume_action();
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
    g.graph.build(seed);  // B1: build the run's node graph (deterministic from seed)
    g.cur_node = 0;       // B1.3: start in the sietch hub (node 0); generate_sietch ignores seed
    meta_load();

    int sx, sy;
    map::generate(g.zone_, seed, map::kZoneSietch, sx, sy);
    g.ents[0] = Entity{EType::Player, Ai::Idle, map::kZoneSietch,
                       int16_t(sx), int16_t(sy), int16_t(tune::kPlayerHpMax), 0, 0};
    // Populate the sietch now that the player entity exists (the Naib + Sayyadina
    // are placed relative to it). This also sets spawned_mask for the zone; the
    // old code set the mask directly, which silently skipped the NPC spawn.
    spawn_zone_wildlife(g.zone_, map::kZoneSietch);

    // B5: the character is PERSISTENT. If one exists in meta, restore it (level/attributes/gear +
    // the banked spice wallet) and skip the chooser; otherwise start a fresh tier-1 Fremen.
    // Restore only for real (interactive) play so headless tests stay isolated on the shared
    // meta.dat (the vault still loads for rpgtest; persistence WRITES still happen for a metatest).
    if (g_meta.has_char && services.interactive) {
        g.pl.st = g_meta.st;
        g.pl.base_class = BaseClass(g_meta.base_class);   // C3: restore class identity
        g.pl.ascendancy = Ascendancy(g_meta.ascendancy);
        g.pl.passives = g_meta.passives;                  // C4: restore the passive tree
        std::memcpy(g.pl.equip, g_meta.equip, sizeof g.pl.equip);
        std::memcpy(g.pl.inv, g_meta.inv, sizeof g.pl.inv);
        std::memcpy(g.pl.inv_cell, g_meta.inv_cell, sizeof g.pl.inv_cell);  // G: restore the grid layout
        std::memcpy(g.pl.cur_tab, g_meta.cur_tab, sizeof g.pl.cur_tab);     // G2: restore the currency table
        g.pl.spice = g_meta.banked_spice;   // spendable wallet = the safe banked amount
        g.choosing = false;
    } else {
        // B2a: a fresh character in a basic tier-1 stillsuit set (Cloak/Catchpockets/Token are loot).
        g.pl.equip[int(items::Slot::Weapon)].def = 0;     // Dust Knife
        g.pl.equip[int(items::Slot::Hood)].def = 5;       // Cloth Headwrap
        g.pl.equip[int(items::Slot::Mask)].def = 8;       // Cracked Mask
        g.pl.equip[int(items::Slot::NosePlugs)].def = 11; // Rough Filt-plugs
        g.pl.equip[int(items::Slot::WaterTube)].def = 14; // Frayed Catchtube
        g.pl.equip[int(items::Slot::Body)].def = 17;      // Patched Suit-body
        g.pl.equip[int(items::Slot::Pumps)].def = 22;     // Worn Heel-pumps
        g.choosing = services.interactive;
        if (!g.choosing) apply_base_class(g.pl, BaseClass::Knifehand);
    }
    passive_ensure_start();  // C4: the class-start anchor is always allocated (fresh + pre-DMEJ saves)
    g.pl.hp = player_max_hp();
    g.pl.shield = player_max_shield();  // C2: begin a run with a full Focus shield
    g.pl.will = player_max_will();
    g.pl.maker_hooks = true;  // reserved for the endgame Long Ride (worm-riding is parked in-campaign)
    // Interactive launches open on the title menu; headless tests jump into play.
    g.screen = services.interactive ? GameState::Screen::Menu : GameState::Screen::Play;

    g.log.push("Muad'Dib waits with the Water of Life. Drink, and walk the vision.");
    g.log.push("Click Muad'Dib to begin. Arrows move. G harvest. C sandwalk.");
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
            return g.services.anims->get(kBaseClasses[int(g.pl.base_class)].sprite);
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
    // pre-buffer: the next step queues before touchdown, so held walking flows
    // continuously instead of stalling once per tile (A3).
    return dx * dx + dy * dy <= tune::kStepBufferPx * tune::kStepBufferPx;
}

// A4: take one step toward a click destination. A goal-rooted Dijkstra gives the
// distance-to-goal from every tile; we descend to the lowest-cost walkable
// neighbour. The final bump onto a blocked goal (enemy / cistern / door / wall)
// is delegated to try_move, so click-to-attack and click-to-interact fall out for
// free. Returns false when there is no legal step (stuck / unreachable).
bool click_step_toward(int gx, int gy) {
    const Entity& p = player_ent();
    if (p.x == gx && p.y == gy) return false;
    static int16_t pd[map::N];
    map::compute_dijkstra(zone(), gx, gy, pd);
    const int kNone = 1 << 30;
    int best = kNone, bdx = 0, bdy = 0;
    for (int ddy = -1; ddy <= 1; ++ddy)
        for (int ddx = -1; ddx <= 1; ++ddx) {
            if (!ddx && !ddy) continue;
            const int nx = p.x + ddx, ny = p.y + ddy;
            if (nx < 0 || nx >= map::W || ny < 0 || ny >= map::H) continue;
            int d;
            if (nx == gx && ny == gy) {
                d = 0;  // adjacent to the goal itself: bump it (attack / interact)
            } else {
                if (!zone().walkable(nx, ny)) continue;
                d = pd[ny * map::W + nx];
                if (d < 0) continue;  // unreachable from the goal
            }
            if (d < best) { best = d; bdx = ddx; bdy = ddy; }
        }
    if (best == kNone) return false;
    try_move(bdx, bdy);
    return true;
}

// Click-only sietch exit: fired from game_update when the player has walked
// adjacent to a door they clicked (water seals the sietch — you leave by the door).
void open_revelation(int dest);  // fwd (defined above)
void use_door() {
    if (g.in_strike) { abandon_strike(); return; }  // B10.2: leaving a strike unfinished = abandon it
    if (g.in_pinnacle) { abandon_pinnacle(); return; }  // B10.3: withdraw from the homeworld
    if (g.in_capstone) { abandon_capstone(); return; }  // B10.4: withdraw from the Golden Path
    if (g.in_cave) { exit_cave(); return; }  // B9: the cavern door climbs back out to the field
    if (g.cur_node == 0) {
        g.log.push("You drink deep of the Water. Muad'Dib's vision takes you.");
        begin_run();
        refresh_fov_and_paths();  // RT1.2: settle FOV/paths after entering the run
        return;
    }
    const int nxt = g.graph.forward(g.cur_node);
    if (nxt < 0) {
        g.log.push("The vision runs its course; you wake in the sietch.");
        extract_run();  // completing the vision forges the haul (extraction; also wakes at node 0)
        return;
    }
    // Desert Trials: reaching a forward door in sandwalk = crossing the leg unheard.
    if (g.pl.sandwalk) mark_trial(TRIAL_SANDWALK);
    // B2b: at a forward door the vision reveals gear; the Revelation pick then advances to `nxt`.
    open_revelation(nxt);
}

// Per render tick (60Hz): every visible entity glides toward its logical
// tile, walks while gliding, breathes while still. Purely presentational.
void worm_visual_tick() {
    // index 0 = head, then segments; lag grows down the body so it flows
    for (int i = 0; i <= worm::kSegments; ++i) {
        const int tx = i == 0 ? g.wrm.x : g.wrm.seg_x[i - 1];
        const int ty = i == 0 ? g.wrm.y : g.wrm.seg_y[i - 1];
        int wx, wy;
        world_px_of(tx, ty, wx, wy);
        int16_t& vx = g.worm_vis[i * 2];
        int16_t& vy = g.worm_vis[i * 2 + 1];
        const int dx = wx - vx, dy = wy - vy;
        if (dx * dx + dy * dy > 320 * 320) {  // teleport/reset: snap (2x world space)
            vx = int16_t(wx);
            vy = int16_t(wy);
            continue;
        }
        const int step = (i == 0 ? 8 : (6 - (i > 4 ? 2 : i / 2))) * iso::kView;
        vx = int16_t(vx + (dx > step ? step : dx < -step ? -step : dx));
        vy = int16_t(vy + (dy > step ? step : dy < -step ? -step : dy));
    }
}

void visual_tick() {
    worm_visual_tick();
    for (Entity& e : g.ents) {
        if (e.type == EType::None || e.zone != g.cur_zone) continue;
        int wx, wy;
        world_px_of(e.x, e.y, wx, wy);
        int dx = wx - e.vpx, dy = wy - e.vpy;
        if (dx * dx + dy * dy > 192 * 192) {  // teleport/spawn: snap (2x world space)
            e.vpx = int16_t(wx);
            e.vpy = int16_t(wy);
            dx = dy = 0;
        }
        const int step = (e.type == EType::Player && g.pl.sandwalk)
                             ? tune::kGlideStepSandwalk : tune::kGlideStep;
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
            // dust sized by terrain; drum sand booms visually too
            const int sx = e.vpx - g.cam.x + fb::W / 2;
            const int sy = e.vpy - g.cam.y + fb::H / 2;
            if (sx >= 0 && sx < fb::W && sy >= 0 && sy < fb::H) {
                const uint8_t fl = zone().floor[e.y * map::W + e.x];
                int count = fl == 6 ? 4 : (fl == 3 || fl == 4 || fl == 8) ? 0 : 1;
                if (e.type == EType::Player && g.pl.sandwalk) count = 0;  // featherstep
                if (count) g.particles.burst_dust(g.rng.fx, sx, sy + 10, count, 66);
                if (e.type == EType::Player && count)
                    g.services.mixer->push({audio::Cmd::PlaySfx, 1, 26,
                                            uint8_t((g.anim_tick & 32) ? 118 : 130)});
            }
        } else if (ev == anim::EV_IMPACT) {
            g.services.mixer->push({audio::Cmd::PlaySfx, 1, 200, 60});
            if (e.type == EType::Player && g.impact_x >= 0) {
                const int isx = 0;
                for (Entity& v : g.ents)
                    if (v.type != EType::None && v.zone == g.cur_zone &&
                        v.x == g.impact_x && v.y == g.impact_y) {
                        v.hurt_flash = 8;
                        // impact particles: sparks off shields, blood off flesh
                        const int vx = v.vpx - g.cam.x + fb::W / 2;
                        const int vy = v.vpy - g.cam.y + fb::H / 2;
                        const uint8_t color = g.impact_kind == 1 ? 30 : 204;
                        g.particles.burst_dust(g.rng.fx, vx, vy - 14, 4, color);
                    }
                g.impact_x = -1;
                g.microshake = 4;
                (void)isx;
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
            case EType::Paul: return "Muad'Dib (drink the Water: begin the vision)";
            case EType::Naib: return "the Naib";
            case EType::Sayyadina: return "the Sayyadina";
            case EType::Smuggler: return "Greybeard the smuggler";
            case EType::PanAdder: return "pan adder - give it room";
            case EType::Hermit: return "spice-mad hermit";
            case EType::HunterSeeker: return "HUNTER-SEEKER";
            case EType::SpiceCache: return "raw spice (G: gather)";
            case EType::CaveMouth: return "hidden cavern (step in to enter)";
            case EType::WarRoom: return "the War Council (enact the Jihad)";
            case EType::Vault: return "the sietch vault (stash tabs)";
            case EType::Currency: {
                static char cbuf[48];
                std::snprintf(cbuf, sizeof cbuf, "%s (walk over to collect)",
                              items::currency_name(uint8_t(e.data)));
                return cbuf;
            }
            case EType::Boss: {
                if (e.data2 == -1) return "BARON HARKONNEN";
                static char bbuf[96];
                if (boss_is_pinnacle(e.data2)) {
                    const Pinnacle& p = kPinnacles[boss_pinnacle_idx(e.data2)];
                    std::snprintf(bbuf, sizeof bbuf, "%s -- %s", p.name, p.note);
                    return bbuf;
                }
                std::snprintf(bbuf, sizeof bbuf, "%s %s %s",
                              kBossTitles[e.data2 & 7], kBossNames[(e.data2 >> 4) & 7],
                              kBossEpithets[(e.data2 >> 8) & 7]);
                return bbuf;
            }
            case EType::Caravan: return "smuggler caravan (bump: trade)";
            case EType::Envoy: return "Spacing Guild envoy (bump: pay the bribe)";
            case EType::Loot: {
                static char buf[72];
                char nm[64];
                loot_of(e).full_name(nm, sizeof nm);
                std::snprintf(buf, sizeof buf, "%s (G: take)", nm);
                return buf;
            }
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

// A6: which menu row is under the mouse (matching the pass_ui draw geometry), or
// -1. Lets menus be navigated by hover + click instead of only the arrow keys.
int menu_row_under_mouse(const InputState& in) {
    if (in.mouse_x < 0) return -1;
    const int mx = in.mouse_x, my = in.mouse_y;
    if (g.menu_open >= 1 && g.menu_open <= 4) {           // vendor lists (R2: 2x)
        const int x0 = fb::W / 2 - 300, y0 = 280;
        if (mx < x0 + 16 || mx > x0 + 16 + 568) return -1;
        const int counts[5] = {0, 6, 3, 5, 4};  // C5b: Naib gained the Sietch-tree row
        for (int i = 0; i < counts[g.menu_open]; ++i) {
            const int ry = y0 + 80 + i * 32 - 4;
            if (my >= ry && my < ry + 32) return i;
        }
        return -1;
    }
    const int x0 = fb::W / 2 - 380, y0 = 140;             // char / pack / vault (R2: 2x)
    if (mx < x0 + 16 || mx > x0 + 16 + 728) return -1;
    auto band = [&](int ry) { return my >= ry - 2 && my < ry + 22; };
    if (g.menu_open == 5) {
        for (int i = 0; i < 3; ++i) if (band(y0 + 88 + i * 24)) return i;
        for (int i = 0; i < kTalentCount; ++i) if (band(y0 + 176 + i * 24)) return 3 + i;
        if (band(y0 + 460)) return 12;
    }  // menu 6/7 (pack/vault) are grid screens now — driven by ui_grid_input, not row-hover.
    return -1;
}

}  // namespace

// Front-end handlers (defined below with the other UI passes).
namespace {
bool update_menu(const InputState&);
void update_options(const InputState&);
void update_manual(const InputState&);
}  // namespace

bool game_update(const InputState& in) {
    if (g.hitstop > 0) {  // killing-blow freeze: presentation only
        --g.hitstop;
        return true;
    }
    ++g.anim_tick;
    ++g.sim_clock;  // RT1: the fixed-timestep sim clock advances every game_update tick
    // Front-end screens: the world is paused; they run their own input + render.
    if (g.screen == GameState::Screen::Menu) return update_menu(in);
    if (g.screen == GameState::Screen::Options) { update_options(in); return true; }
    if (g.screen == GameState::Screen::Manual) { update_manual(in); return true; }
    if (g.microshake > 0) --g.microshake;
    if (!g.choosing) visual_tick();

    // B2b: the Revelation modal — a prescient gear pick at a forward door; pauses the world.
    if (g.revelation_stage) {
        if (in.was_pressed(Key::Up)) g.rev_sel = (g.rev_sel + 2) % 3;
        else if (in.was_pressed(Key::Down)) g.rev_sel = (g.rev_sel + 1) % 3;
        bool confirm = in.was_pressed(Key::Confirm);
        if (in.mouse_pressed[0] && in.mouse_x >= 0) {
            const int y0 = 280, my = in.mouse_y;
            for (int i = 0; i < 3; ++i)
                if (my >= y0 + 76 + i * 40 && my < y0 + 76 + i * 40 + 34) { g.rev_sel = i; confirm = true; }
        }
        if (confirm) revelation_confirm();
        return true;
    }

    g.hover_tx = -1;
    if (in.mouse_x >= 0 && !g.choosing && !g.menu_open) {
        int tx, ty;
        iso::screen_to_tile(in.mouse_x, in.mouse_y, g.cam, tx, ty);
        g.hover_tx = int16_t(tx);
        g.hover_ty = int16_t(ty);
    }
    if (g.fade > 0) g.fade -= 6;

    if (g.choosing) {
        const int n = int(BaseClass::Count);
        if (in.was_pressed(Key::Left)) g.choose_idx = (g.choose_idx + n - 1) % n;
        if (in.was_pressed(Key::Right)) g.choose_idx = (g.choose_idx + 1) % n;
        if (in.was_pressed(Key::Confirm)) {
            apply_base_class(g.pl, BaseClass(g.choose_idx));
            passive_ensure_start();  // C4: anchor this class's passive-tree corner
            g.choosing = false;
            g.log.push("You are %s. The Tanzerouft waits.", kBaseClasses[g.choose_idx].name);
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
        // 5 = character sheet, 6 = inventory, 7 = the vault
        int counts[14] = {0, 6, 3, 5, 4, 3 + kTalentCount + 1, 13, 13, 4, 5 + 1 + 1, 5 + 1 + 1, 1,
                          kSietchNodeCount + 1, kPassiveNodeCount};  // C5b menu 12 tree, C4 menu 13 tree
        int n = counts[g.menu_open];
        if (g.menu_open == 11) {  // D3d bench: rows are owned currencies, or gear once a currency is picked
            uint8_t cb[items::CUR_COUNT]; int gi[12];
            n = 1 + (g.bench_cur == 0 ? list_owned_currency(cb) : list_gear(gi));  // + a Back row
        }
        g.ui_mx = in.mouse_x; g.ui_my = in.mouse_y;  // G4: remember the mouse for the grid render
        // A6: mouse — hovering a row selects it, left-click confirms it, right-click closes.
        const int mrow = menu_row_under_mouse(in);
        if (mrow >= 0) g.menu_sel = mrow;
        const bool mclick = mrow >= 0 && in.mouse_pressed[0];
        if (!in.mouse_down[0] && g.drag_kind && !in.mouse_released[0]) { g.drag_kind = 0; g.drag_idx = -1; }  // G4: heal a stale drag
        if (in.mouse_pressed[2] || in.mouse_pressed[1]) {
            if (g.drag_kind) { g.drag_kind = 0; g.drag_idx = -1; return true; }  // right-click cancels a drag
            g.menu_open = 0; return true;
        }
        // G4: on the grid screens (pack/vault/bench), mouse drag-drop + Ctrl-click drive item movement.
        if (ui_grid_input(in)) return true;
        if (g.menu_open == 13) {  // C4: the passive skill tree (a node graph; click a node to allocate)
            if (in.was_pressed(Key::Cancel) || in.was_pressed(Key::Character)) { g.menu_open = 0; return true; }
            if (in.was_pressed(Key::Up)) g.menu_sel = (g.menu_sel + n - 1) % n;
            if (in.was_pressed(Key::Down)) g.menu_sel = (g.menu_sel + 1) % n;
            const int hit = in.mouse_pressed[0] ? passiveui::node_at(in.mouse_x, in.mouse_y) : -1;
            if (hit >= 0) g.menu_sel = hit;
            if (in.was_pressed(Key::Confirm) || hit >= 0) {
                if (passive_alloc(g.menu_sel))
                    g.log.push("The vision sharpens: %s.", kPassiveNodes[g.menu_sel].name);
                else if (!passive_has(g.menu_sel))
                    g.log.push("%s is out of reach (no point, or not connected).", kPassiveNodes[g.menu_sel].name);
            }
            return true;
        }
        if (g.menu_open >= 8 && g.menu_open <= 12) {  // endgame hub menus + the sietch tree (keyboard)
            if (in.was_pressed(Key::Up)) g.menu_sel = (g.menu_sel + n - 1) % n;
            if (in.was_pressed(Key::Down)) g.menu_sel = (g.menu_sel + 1) % n;
            if (in.was_pressed(Key::Cancel)) { g.menu_open = 0; g.bench_cur = 0; return true; }
            if (g.menu_open == 9) {  // Left/Right: commission tier
                if (in.was_pressed(Key::Left) && g.jihad_tier > 1) --g.jihad_tier;
                if (in.was_pressed(Key::Right) && g.jihad_tier < tune::kStrikeTiers) ++g.jihad_tier;
            }
            if (in.was_pressed(Key::Confirm) || mclick) {
                const int sel = g.menu_sel;
                if (g.menu_open == 8) {  // the War Council hub
                    if (sel <= 2) { g.menu_open = 9 + sel; g.menu_sel = 0; g.bench_cur = 0; }
                    else g.menu_open = 0;
                } else if (g.menu_open == 9) {  // the Jihad Table
                    if (sel < FAC_COUNT) {
                        if (pinnacle_available(sel)) { g.menu_open = 0; assault_pinnacle(sel); }
                        else if (commission_strike(sel, g.jihad_tier)) g.menu_open = 0;
                        else g.log.push("Not enough spice to commission that strike.");
                    } else if (sel == FAC_COUNT) {
                        if (capstone_available()) { g.menu_open = 0; confront_capstone(); }
                        else g.log.push("The Golden Path needs all five shards first.");
                    } else { g.menu_open = 8; g.menu_sel = 0; }
                } else if (g.menu_open == 10) {  // the Landsraad
                    if (sel < FAC_COUNT) {
                        if (!court_house(sel)) g.log.push("That House cannot be courted now.");
                    } else if (sel == FAC_COUNT) { collect_dividend(); }
                    else { g.menu_open = 8; g.menu_sel = 0; }
                } else if (g.menu_open == 11) {  // the crafting bench — pick a currency, then target gear
                    if (g.bench_cur == 0) {  // choosing a currency
                        uint8_t cb[items::CUR_COUNT];
                        const int nc = list_owned_currency(cb);
                        if (sel < nc) {
                            const int c = cb[sel];
                            if (c == items::CUR_FLASKQUAL) {  // targets the belt, not a gear item
                                if (!bench_apply(c, 0)) g.log.push("Flask quality is already at its peak.");
                            } else { g.bench_cur = c; g.menu_sel = 0; }
                        } else { g.menu_open = 8; g.menu_sel = 0; }  // Back to the council
                    } else {  // choosing a target gear item
                        int gi[12];
                        const int ng = list_gear(gi);
                        if (sel < ng) {
                            if (!bench_apply(g.bench_cur, gi[sel]))
                                g.log.push("That currency can't be applied to that item.");
                            g.bench_cur = 0; g.menu_sel = 0;
                        } else { g.bench_cur = 0; g.menu_sel = 0; }  // Back to the currency list
                    }
                } else {  // 12: the Sietch talent tree — spend Litrejons to rank up a node
                    if (sel < kSietchNodeCount) {
                        const int cost = sietch_node_cost(sel);
                        if (cost <= 0) g.log.push("'%s is at its peak.'", kSietchNodes[sel].name);
                        else if (buy_sietch_node(sel))
                            g.log.push("The tribe drinks your water. %s deepens. -%d litrejons.",
                                       kSietchNodes[sel].name, cost);
                        else g.log.push("Not enough litrejons (%d needed). Reclaim more water in the field.", cost);
                    } else { g.menu_open = 1; g.menu_sel = 0; }  // Back to the Naib
                }
            }
            return true;
        }
        if (g.menu_open == 5) {
            // 0..2 stats, 3..11 talents, 12 close
            if (in.was_pressed(Key::Character)) { g.menu_open = 13; g.menu_sel = 0; return true; }  // C4: to the tree
            if (in.was_pressed(Key::Up)) g.menu_sel = (g.menu_sel + n - 1) % n;
            if (in.was_pressed(Key::Down)) g.menu_sel = (g.menu_sel + 1) % n;
            if (in.was_pressed(Key::Cancel)) g.menu_open = 0;
            if (in.was_pressed(Key::Confirm) || mclick) {
                if (g.menu_sel < 3 && g.pl.st.statpts > 0) {
                    (g.menu_sel == 0 ? g.pl.st.cond
                     : g.menu_sel == 1 ? g.pl.st.vers : g.pl.st.awar) += 1;
                    --g.pl.st.statpts;
                    // Conditioning raises Life; top a little of it up so the point feels good.
                    if (g.menu_sel == 0) g.pl.hp = g.pl.hp < player_max_hp()
                                                       ? g.pl.hp + 2 : player_max_hp();
                } else if (g.menu_sel >= 3 && g.menu_sel < 3 + kTalentCount &&
                           g.pl.st.talentpts > 0 &&
                           g.pl.st.talents[g.menu_sel - 3] < 3) {
                    ++g.pl.st.talents[g.menu_sel - 3];
                    --g.pl.st.talentpts;
                    g.log.push("%s rank %d.", kTalentNames[g.menu_sel - 3],
                               int(g.pl.st.talents[g.menu_sel - 3]));
                } else if (g.menu_sel == n - 1) {
                    g.menu_open = 0;
                }
            }
            return true;
        }
        if (g.menu_open == 6) {
            // 12 inventory slots + close: Confirm equips (swaps by slot)
            if (in.was_pressed(Key::Up)) g.menu_sel = (g.menu_sel + n - 1) % n;
            if (in.was_pressed(Key::Down)) g.menu_sel = (g.menu_sel + 1) % n;
            if (in.was_pressed(Key::Cancel)) g.menu_open = 0;
            if (in.was_pressed(Key::Confirm) || mclick) {
                if (g.menu_sel == 12) {
                    g.menu_open = 0;
                } else if (g.pl.inv[g.menu_sel].is_currency()) {
                    g.log.push("Currency is spent at the crafting bench, not equipped.");  // D3 guard
                } else if (!g.pl.inv[g.menu_sel].empty() && !g.pl.inv[g.menu_sel].unidentified()) {
                    items::Inst& it = g.pl.inv[g.menu_sel];
                    items::Inst& eq = g.pl.equip[int(it.d().slot)];
                    const items::Inst tmp = eq;
                    eq = it;
                    it = tmp;
                    char nm[64];
                    eq.full_name(nm, sizeof nm);
                    g.log.push("Equipped: %s", nm);
                    if (g.pl.hp > player_max_hp()) g.pl.hp = player_max_hp();
                    if (g.pl.water > player_max_water()) g.pl.water = player_max_water();
                }
            }
            return true;
        }
        if (g.menu_open == 7) {
            // G3 Vault (interim keyboard UI; G4 makes it a spatial multi-tab grid): Left/Right toggles
            // deposit/withdraw, the current stash tab is g.stash_tab. Row 12 = buy the next tab.
            if (in.was_pressed(Key::Left) || in.was_pressed(Key::Right)) {
                g.vault_page ^= 1;
                g.menu_sel = 0;
            }
            if (in.was_pressed(Key::Up)) g.menu_sel = (g.menu_sel + n - 1) % n;
            if (in.was_pressed(Key::Down)) g.menu_sel = (g.menu_sel + 1) % n;
            if (in.was_pressed(Key::Cancel)) g.menu_open = 0;
            if (in.was_pressed(Key::Confirm) || mclick) {
                if (g.menu_sel == 12) {  // buy the next stash tab (spice, escalating)
                    const int cost = stash_tab_cost();
                    if (cost <= 0) g.log.push("The vault is at its full span (%d tabs).", int(g_meta.stash_tabs));
                    else if (buy_stash_tab()) g.log.push("A new stash tab is carved. -%d spice.", cost);
                    else g.log.push("Not enough spice for a new tab (%d needed).", cost);
                } else if (g.vault_page == 0) {  // deposit inv[sel] -> the current tab
                    if (g.season.denied < 1)
                        g.log.push("'The vault opens for the blooded. Deny a harvest first.'");
                    else if (!g.pl.inv[g.menu_sel].empty()) {
                        char nm[64]; g.pl.inv[g.menu_sel].full_name(nm, sizeof nm);
                        if (stash_deposit(g.menu_sel, g.stash_tab))
                            g.log.push("Banked: %s. It is the tribe's now - and yours.", nm);
                        else g.log.push("This stash tab has no room for that.");
                    }
                } else {  // withdraw the current tab's entry sel -> the pack
                    if (!g_meta.stash[g.stash_tab][g.menu_sel].empty()) {
                        char nm[64]; g_meta.stash[g.stash_tab][g.menu_sel].full_name(nm, sizeof nm);
                        if (stash_withdraw(g.stash_tab, g.menu_sel)) g.log.push("Withdrawn: %s", nm);
                        else g.log.push("Your pack has no room for that.");
                    }
                }
            }
            return true;
        }
        if (in.was_pressed(Key::Up)) g.menu_sel = (g.menu_sel + n - 1) % n;
        if (in.was_pressed(Key::Down)) g.menu_sel = (g.menu_sel + 1) % n;
        if (in.was_pressed(Key::Cancel)) g.menu_open = 0;
        if (in.was_pressed(Key::Confirm) || mclick) {
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
                case 13:  // Naib: ride out to the Ninth Rock to meet the Guild (B7 rendezvous)
                    g.menu_open = 0;
                    enter_rendezvous();
                    break;
                case 14: g.menu_open = 12; g.menu_sel = 0; break;  // C5b: the Sietch talent tree
                case 15: g.menu_open = 0; break;  // leave
                case 20:  // Sayyadina: spice tribute
                    if (g.pl.spice >= 5) {
                        g.pl.spice -= 5;
                        raise_standing(12);
                        g.log.push("Blue eyes hold yours. 'The tribe sees you.' +12 standing.");
                    } else {
                        g.log.push("'Bring spice when you have it to give.'");
                    }
                    break;
                case 21:  // Sayyadina: healing trance
                    if (g.pl.spice >= 6 && g.pl.hp < player_max_hp()) {
                        g.pl.spice -= 6;
                        g.pl.hp = player_max_hp();
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
                case 40: {  // Farsight: the spice unfolds the map
                    const int cost = g.pl.st.talents[6] >= 2 ? 1 : 2;
                    if (g.pl.spice >= cost) {
                        g.pl.spice -= cost;
                        for (int i = 0; i < map::N; ++i)
                            zone().vis[i] |= map::V_REMEMBERED;
                        g.log.push("The spice unfolds. You see the whole quarter at once.");
                        flash(120, 80, 200);
                        g.menu_open = 0;
                    } else g.log.push("Not enough grains for the long sight.");
                    break;
                }
                case 41: {  // Threadwalk: two free strides outside time
                    const int cost = g.pl.st.talents[6] >= 2 ? 2 : 3;
                    if (g.pl.spice >= cost) {
                        g.pl.spice -= cost;
                        g.pl.free_moves = 2;
                        g.log.push("Time goes thick as syrup. You move BETWEEN moments.");
                        flash(150, 100, 220);
                        g.menu_open = 0;
                    } else g.log.push("Not enough grains to walk the thread.");
                    break;
                }
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

    // RT1.2: the world advances on its own real-time cadence, independent of player input.
    // Menus / dead / choosing / revelation all returned above, so we only reach here in live
    // play. A world step fires every kWorldStepTicks sim-ticks — enemies act and time passes
    // even while the player stands still. This is what makes the game real-time.
    if (++g.world_accum >= tune::kWorldStepTicks) {
        g.world_accum -= tune::kWorldStepTicks;
        world_tick();
    }
    if (g.pl.dead || g.season.ending != campaign::kNone) return true;  // world_tick may have ended the run

    if (in.was_pressed(Key::Map)) g.show_map = !g.show_map;  // A5: toggle the minimap

    // RT1.2: player grid-actions are rate-limited by the action cooldown (sim-clock based), NOT by
    // the glide animation. Fast input can't speed the world (it's on a timer); standing still can't
    // freeze it. `ready` gates every action below; glide (vpx/vpy) stays purely cosmetic.
    const bool ready = player_ready();
    auto move_input = [&](Key k) { return ready && (in.was_pressed(k) || in.is_down(k)); };
    int dx = 0, dy = 0;

    // PoE2 screen-relative WASD: compose an 8-way tile step from the four keys so
    // W walks *up the screen* (an iso diagonal). Inverse of the iso projection:
    // dx = sign(Sx+Sy), dy = sign(Sy-Sx), for screen intent (Sx, Sy).
    const int wsx = (in.is_down(Key::WalkRight) ? 1 : 0) - (in.is_down(Key::WalkLeft) ? 1 : 0);
    const int wsy = (in.is_down(Key::WalkDown) ? 1 : 0) - (in.is_down(Key::WalkUp) ? 1 : 0);
    if ((wsx || wsy) && ready) {  // RT1.2: cooldown-gated, not glide-gated
        const int tdx = wsx + wsy, tdy = wsy - wsx;
        dx = tdx > 0 ? 1 : tdx < 0 ? -1 : 0;
        dy = tdy > 0 ? 1 : tdy < 0 ? -1 : 0;
    }
    if (!dx && !dy) {  // classic tile-cardinal keys (arrows / numpad / headless tests)
        if (move_input(Key::Up)) { dy = -1; }
        else if (move_input(Key::Down)) { dy = 1; }
        else if (move_input(Key::Left)) { dx = -1; }
        else if (move_input(Key::Right)) { dx = 1; }
        else if (move_input(Key::UpLeft)) { dx = -1; dy = -1; }
        else if (move_input(Key::UpRight)) { dx = 1; dy = -1; }
        else if (move_input(Key::DownLeft)) { dx = -1; dy = 1; }
        else if (move_input(Key::DownRight)) { dx = 1; dy = 1; }
    }

    // Left-click aims: set a move goal at the cursor tile (hold-drag re-steers).
    // A FRESH click also classifies what to interact with on arrival (click-only
    // NPCs/doors). Right-click or Cancel aborts it; any keyboard move overrides it.
    if (in.mouse_pressed[1] || in.was_pressed(Key::Cancel)) {
        g.move_goal_x = -1;
        g.interact_kind = 0;
    }
    const bool cursor_in_world = in.mouse_x >= 0 && in.mouse_y >= 30 &&
                                 in.mouse_y < ui::kPanelY && g.hover_tx >= 0 && !g.pl.riding;
    if (cursor_in_world && in.mouse_pressed[0]) {  // fresh press: aim + classify target
        g.move_goal_x = g.hover_tx;
        g.move_goal_y = g.hover_ty;
        if (entity_at(g.hover_tx, g.hover_ty, g.cur_zone, EType::Naib)) g.interact_kind = 1;
        else if (entity_at(g.hover_tx, g.hover_ty, g.cur_zone, EType::Sayyadina)) g.interact_kind = 2;
        else if (entity_at(g.hover_tx, g.hover_ty, g.cur_zone, EType::Smuggler)) g.interact_kind = 3;
        else if (zone().at(g.hover_tx, g.hover_ty) & map::TF_DOOR) g.interact_kind = 4;
        else if (entity_at(g.hover_tx, g.hover_ty, g.cur_zone, EType::Fremen)) g.interact_kind = 5;
        else if (entity_at(g.hover_tx, g.hover_ty, g.cur_zone, EType::Paul)) g.interact_kind = 6;
        else if (entity_at(g.hover_tx, g.hover_ty, g.cur_zone, EType::WarRoom)) g.interact_kind = 7;
        else if (entity_at(g.hover_tx, g.hover_ty, g.cur_zone, EType::Vault)) g.interact_kind = 8;  // G3
        else g.interact_kind = 0;
    } else if (cursor_in_world && in.mouse_down[0] &&
               (g.hover_tx != g.move_goal_x || g.hover_ty != g.move_goal_y)) {
        g.move_goal_x = g.hover_tx;  // hold-drag to a new tile = pure movement
        g.move_goal_y = g.hover_ty;
        g.interact_kind = 0;
    }

    if (dx || dy) {
        g.move_goal_x = -1;  // keyboard movement overrides click-to-move
        g.interact_kind = 0;
        if (g.pl.riding) ride_move(dx, dy);
        else try_move(dx, dy);
    }
    else if (g.move_goal_x >= 0) {
        if (ready) {
            const Entity& p = player_ent();
            if ((p.x == g.move_goal_x && p.y == g.move_goal_y) ||
                !click_step_toward(g.move_goal_x, g.move_goal_y))
                g.move_goal_x = -1;  // arrived or stuck
        }
    }
    // Click-only interaction: once adjacent to the clicked NPC/door, act on it.
    if (g.interact_kind) {
        if (g.move_goal_x < 0) {
            g.interact_kind = 0;  // path was lost/cleared; drop the pending interaction
        } else {
            const Entity& p = player_ent();
            const int adx = p.x - g.move_goal_x, ady = p.y - g.move_goal_y;
            if (adx >= -1 && adx <= 1 && ady >= -1 && ady <= 1) {
                if (g.interact_kind == 4) {
                    use_door();
                } else if (g.interact_kind == 6) {  // Muad'Dib: the Water of Life ritual
                    if (g.cur_node == 0) {
                        g.log.push("Muad'Dib presses the Water of Life to your lips.");
                        g.log.push("The vision takes you: the road to Arrakeen opens.");
                        begin_run();
                    } else {
                        g.log.push("Muad'Dib walks only in the sietch of the waking world.");
                    }
                } else if (g.interact_kind == 5) {  // Fremen flavor bark
                    if (Entity* f = entity_at(g.move_goal_x, g.move_goal_y,
                                              g.cur_zone, EType::Fremen)) {
                        constexpr int kN = int(sizeof kFremenBarks / sizeof *kFremenBarks);
                        g.log.push("%s", kFremenBarks[(f->data % kN + kN) % kN]);
                    }
                } else if (g.interact_kind == 7) {  // the War Council table -> the Jihad hub menu (8)
                    g.menu_open = 8;
                    g.menu_sel = 0;
                } else if (g.interact_kind == 8) {  // G3: the sietch Vault -> the stash screen (menu 7)
                    g.menu_open = 7;
                    g.menu_sel = 0;
                    g.vault_page = 0;
                    g.stash_tab = 0;
                } else {
                    g.menu_open = g.interact_kind;  // 1 Naib / 2 Sayyadina / 3 Smuggler
                    g.menu_sel = 0;
                }
                g.move_goal_x = -1;
                g.interact_kind = 0;
            }
        }
    }
    else if (in.pressed_or_repeat(Key::Wait)) { /* RT1.2: hold position; the world advances on its own */ }
    else if (ready && in.was_pressed(Key::Ride)) try_ride();
    else if (in.was_pressed(Key::Powers)) { g.menu_open = 4; g.menu_sel = 0; }
    else if (in.was_pressed(Key::Inventory)) { g.menu_open = 6; g.menu_sel = 0; }
    else if (in.was_pressed(Key::Character)) { g.menu_open = 5; g.menu_sel = 0; }
    else if (ready && in.was_pressed(Key::Harvest)) try_harvest();
    else if (ready && in.was_pressed(Key::Thumper)) place_thumper();
    else if (ready && in.was_pressed(Key::Glowglobe)) place_glowglobe();
    else if (ready && in.was_pressed(Key::Plant)) try_plant();
    else if (ready && in.was_pressed(Key::Poison)) try_poison();
    else if (in.was_pressed(Key::Sandwalk)) {
        g.pl.sandwalk = !g.pl.sandwalk;
        g.log.push(g.pl.sandwalk ? "You break your rhythm. Walk without wormsign."
                                 : "You stride freely again.");
    }
    // C7: cast a prescience ability at the cursor tile (Voice ignores it, Blink aims at it).
    else if (ready && in.was_pressed(Key::Ability1)) {
        if (cast_ability(AB_VOICE, g.hover_tx, g.hover_ty)) consume_action();
    }
    else if (ready && in.was_pressed(Key::Ability2)) {
        if (cast_ability(AB_BLINK, g.hover_tx, g.hover_ty)) consume_action();
    }
    else if (ready && in.was_pressed(Key::Ability3)) {
        if (cast_ability(AB_PRESCIENCE, g.hover_tx, g.hover_ty)) consume_action();
    }
    // C6: right-click fires the maula pistol at the cursor tile (a loud, lethality-scaled ranged shot).
    else if (ready && cursor_in_world && in.mouse_pressed[1]) fire_maula(g.hover_tx, g.hover_ty);

    // Flasks quaff instantly (not gated by the action cooldown) — independent of the move/act chain.
    if (in.was_pressed(Key::Flask1)) use_flask(0);
    if (in.was_pressed(Key::Flask2)) use_flask(1);
    if (in.was_pressed(Key::Flask3)) use_flask(2);

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

    // camera follows the player's gliding visual position (A3: gentler lerp)
    const int tx = player_ent().vpx;
    const int ty = player_ent().vpy + fb::H / 2 - (fb::H - ui::kPanelH) / 2;
    g.cam.x += (tx - g.cam.x) / 8;
    g.cam.y += (ty - g.cam.y) / 8;

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

// Sprite by key, falling back to the loud magenta placeholder rather than
// dereferencing null — so a missing atlas key is a visible marker, not a crash.
const SpriteSheet& sheet_or_ph(RenderCtx& r, const char* name) {
    const SpriteSheet* s = r.assets->get(name);
    return s ? *s : *r.assets->placeholder();
}

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

// B7: a wandering field vendor leaves footprints leading to it — a ground overlay (like the worm
// ripples) drawn only on tiles the player has seen. Placeholder sprite until real art (E2).
void pass_footprints(RenderCtx& r) {
    if (g.footprint_count <= 0) return;
    const SpriteSheet* fp = r.assets->get("footprint");
    if (!fp) fp = r.assets->get("ripple");  // reuse the ripple decal as a placeholder
    if (!fp) return;
    int sx, sy;
    for (int i = 0; i < g.footprint_count; ++i) {
        const int tx = g.footprints_x[i], ty = g.footprints_y[i];
        if (tx < 0 || tx >= map::W || ty < 0 || ty >= map::H) continue;
        const int idx = ty * map::W + tx;
        if (!(r.z->vis[idx] & (map::V_VISIBLE | map::V_REMEMBERED))) continue;
        iso::tile_to_screen(tx, ty, g.cam, sx, sy);
        draw_sprite(r.ctx, *fp, 0, sx, sy, tile_light(r, idx));
    }
}

// A4: a pulsing iso-diamond reticle on the tile under the mouse cursor.
void pass_cursor(RenderCtx& r) {
    if (g.hover_tx < 0 || g.choosing || g.menu_open) return;
    int sx, sy;
    iso::tile_to_screen(g.hover_tx, g.hover_ty, g.cam, sx, sy);
    const uint8_t col = ((g.anim_tick / 8) & 1) ? 246 : 232;  // green/amber pulse
    // Tiles are centre-anchored (ax=32, ay=16), so tile_to_screen gives the tile
    // CENTRE. Plot the diamond centred there (offset by -half) so the reticle sits
    // exactly on the tile under the cursor, not half a tile down-right.
    for (int yy = 0; yy < 32; ++yy)
        for (int xx = 0; xx < 64; ++xx) {
            const int e = (xx < 32 ? 32 - xx : xx - 32) + 2 * (yy < 16 ? 16 - yy : yy - 16);
            if (e < 28 || e > 32) continue;  // the diamond edge band
            const int px = sx - iso::kHalfW + xx, py = sy - iso::kHalfH + yy;
            if (px >= 0 && px < fb::W && py >= 0 && py < fb::H)
                r.ctx.frame->at(px, py) = 0xff000000u | r.ctx.pal->color[col];
        }
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

// Sietch furniture/decor + the exit door, into the y-sorted scene list.
void pass_decor(RenderCtx& r) {
    if (g.cur_zone == map::kZoneSietch) {
        for (const Decor& d : g.decor) {
            const int idx = d.y * map::W + d.x;
            if (!(r.z->vis[idx] & (map::V_VISIBLE | map::V_REMEMBERED))) continue;
            int sx, sy;
            iso::tile_to_screen(d.x, d.y, g.cam, sx, sy);
            if (sx < -96 || sx > fb::W + 96 || sy < -128 || sy > fb::H + 128) continue;
            const DecorDef& def = kDecorProps[d.prop];
            const SpriteSheet& psh = sheet_or_ph(r, def.key);
            if (d.prop != 0)  // ground standing props (the rug is already flat)
                ground_shadow(r.ctx.frame, sx, sy + 14, psh.fw / 4 + 6, psh.fw / 8 + 3);
            g.draw_list.add(&psh, 0, sx, sy + def.seat, tile_light(r, idx), def.layer);
        }
    }
    // exit door as a real prop, wherever a TF_DOOR pad sits
    for (int i = 0; i < map::N; ++i) {
        if (!(r.z->flags[i] & map::TF_DOOR)) continue;
        if (!(r.z->vis[i] & (map::V_VISIBLE | map::V_REMEMBERED))) continue;
        int sx, sy;
        iso::tile_to_screen(i % map::W, i / map::W, g.cam, sx, sy);
        g.draw_list.add(&sheet_or_ph(r, "door_seal"), 0, sx, sy + 4, tile_light(r, i), 1);
    }
}

// Entities: actors (and the surfaced worm), into the scene list.
void pass_entities(RenderCtx& r) {
    const SpriteSheet* player_spr =
        r.assets->get(kBaseClasses[int(g.pl.base_class)].sprite);
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
                if (light2 < light::kEntityMinLight) light2 = light::kEntityMinLight;  // mobs stay visible
                if (e.hurt_flash > 0) {
                    light2 = 31;
                    sx += int(g.rng.fx.below(3)) - 1;
                }
                const bool riding = e.type == EType::Player && g.pl.riding;
                if (!riding) ground_shadow(r.ctx.frame, sx, sy + 16, 18, 8);
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
        int light = g.lightmap[idx];
        if (light < light::kEntityMinLight) light = light::kEntityMinLight;  // mobs stay visible
        // contact shadow to ground standing actors (skip flat/flying/pickup types)
        switch (e.type) {
            case EType::Glowglobe: case EType::HunterSeeker: case EType::Corpse:
            case EType::SpiceCache: case EType::Loot: case EType::Thumper:
            case EType::CaveMouth: case EType::WarRoom: case EType::Currency: case EType::Vault: break;
            default:
                if (!(e.type == EType::Player && g.pl.riding))
                    ground_shadow(r.ctx.frame, sx, sy + 16, 18, 8);
        }
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
            case EType::ShieldTrooper:  // C2: a heavy-armor bulwark (no Holtzman shimmer)
                g.draw_list.add(trooper_spr, (g.anim_tick / 8) % 2, sx, sy + 12, light, 1);
                break;
            case EType::Paul:  // placeholder sprite until E2 art; distinct via a lit tint
                g.draw_list.add(r.assets->get("naib"), 0, sx, sy + 12,
                                light < 12 ? 14 : light, 1);
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
            case EType::Fremen: {  // dialog-only tribesfolk; varied sprite
                constexpr int kNS = int(sizeof kFremenSprites / sizeof *kFremenSprites);
                const char* spr = kFremenSprites[(e.data % kNS + kNS) % kNS];
                g.draw_list.add(&sheet_or_ph(r, spr), 0, sx, sy + 12, light, 1);
                break;
            }
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
            case EType::CaveMouth:  // B9: a dark fissure in the rock (placeholder sprite until E2)
                g.draw_list.add(&sheet_or_ph(r, "cavemouth"), 0, sx, sy + 12, light, 1);
                break;
            case EType::WarRoom:  // the War Council table (placeholder sprite until E2)
                g.draw_list.add(&sheet_or_ph(r, "wartable"), 0, sx, sy + 12, light, 1);
                break;
            case EType::Vault:  // G3: the sietch stash (placeholder sprite until art exists)
                g.draw_list.add(&sheet_or_ph(r, "vault"), 0, sx, sy + 12, light, 1);
                break;
            case EType::Currency:  // D3d: dropped crafting currency (placeholder sprite until E2)
                g.draw_list.add(&sheet_or_ph(r, "currency"), 0, sx, sy + 8, 31, 0);
                break;
            case EType::Boss: {
                const int pidx = boss_is_pinnacle(e.data2) ? boss_pinnacle_idx(e.data2) : -1;
                const bool baron = e.data2 == -1 || pidx == 9;  // the Baron himself
                g.draw_list.add(r.assets->get(baron ? "baron" : "champion"),
                                (g.anim_tick / 8) % 2, sx, sy + 12, light, 1);
                if (baron && g.anim_tick % 6 == 0)
                    g.particles.spawn(sx - 8 + int(g.rng.fx.below(17)), sy - 40, 0, -15,
                                      14, 230);
                break;
            }
            case EType::Caravan:
                g.draw_list.add(r.assets->get("smuggler"), 0, sx, sy + 12, light, 1);
                break;
            case EType::Envoy:
                g.draw_list.add(r.assets->get("guildsman"), 0, sx, sy + 12, light, 1);
                break;
            case EType::Loot: {
                g.draw_list.add(r.assets->get("thumper"), 0, sx, sy + 10, light, 0);
                if (g.anim_tick % 9 == 0)  // gleam so drops read at a glance
                    g.particles.spawn(sx - 4 + int(g.rng.fx.below(9)), sy - 8, 0, -12,
                                      12, uint8_t((e.data2 >> 8) == 2 ? 230 : 30));
                break;
            }
            default: break;
        }
    }

    if (g.wrm.state == worm::State::Surfaced) {
        const SpriteSheet* seg = r.assets->get("wormseg");
        for (int i = worm::kSegments; i >= 1; --i) {
            const int sx = g.worm_vis[i * 2] - g.cam.x + fb::W / 2;
            const int sy = g.worm_vis[i * 2 + 1] - g.cam.y + fb::H / 2;
            g.draw_list.add(seg, 0, sx, sy + 20,
                            light_at(g.wrm.seg_x[i - 1], g.wrm.seg_y[i - 1]), 1);
        }
        const int hx = g.worm_vis[0] - g.cam.x + fb::W / 2;
        const int hy = g.worm_vis[1] - g.cam.y + fb::H / 2;
        g.draw_list.add(r.assets->get("wormhead"), (g.anim_tick / 10) % 2, hx, hy + 24,
                        light_at(g.wrm.x, g.wrm.y), 2);
    }
}

// Effects: above-scene sprites — the carryall flies in over the countdown.
void pass_effects(RenderCtx& r) {
    if (g.op.state == harvest::OpState::Evac && g.op.zone == g.cur_zone) {
        const int idx = g.op.y * map::W + g.op.x;
        if (r.z->vis[idx] & map::V_VISIBLE) {
            int sx, sy;
            iso::tile_to_screen(g.op.x, g.op.y, g.cam, sx, sy);
            // approach: from the north-east sky toward the crawler as eta ticks
            const int total = harvest::kEvacTurns;
            const int t256 = (total - g.op.eta) * 256 / total;  // 0..256
            const int off = (256 - t256);  // distance remaining
            const int ax = sx - 20 + off * 2;
            const int ay = sy - 80 - off;
            const int bob = (g.anim_tick / 12) % 3 - 1;
            draw_sprite(r.ctx, sheet_or_ph(r, "carryall"), 0, ax, ay + bob,
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
            draw_sprite(r.ctx, sheet_or_ph(r, "carryall"), 0, sx, sy - 60, 31);
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

// Weather: heat shimmer, lazy wind, and the Coriolis storm.
void pass_weather(RenderCtx& r) {
    const bool outdoors = !(r.z->at(player_ent().x, player_ent().y) & map::TF_INDOOR);
    if (g.storm_turns <= 0 && outdoors) {
        // scorching-hour heat shimmer: sparse vertical pixel swaps
        if (ambient_now() >= tune::kExposureHotThreshold) {
            for (int i = 0; i < 220; ++i) {
                const int x = int(g.rng.fx.below(fb::W));
                const int y = 20 + int(g.rng.fx.below(fb::H - ui::kPanelH - 40));
                uint32_t* px = r.ctx.frame->px;
                const uint32_t tmp = px[y * fb::W + x];
                px[y * fb::W + x] = px[(y + 1) * fb::W + x];
                px[(y + 1) * fb::W + x] = tmp;
            }
        }
        // a lazy grain of wind now and then
        if (g.anim_tick % 7 == 0)
            g.particles.spawn(fb::W - 1, int(g.rng.fx.below(fb::H - ui::kPanelH)),
                              -(200 + int(g.rng.fx.below(200))),
                              int(g.rng.fx.below(40)) - 20,
                              50, uint8_t(66 + g.rng.fx.below(8)));
    }
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
        uint32_t* row = r.ctx.frame->px + y * fb::W;
        for (int x = (y + phase) & 3; x < fb::W; x += 4)
            row[x] = 0xff000000u | r.ctx.pal->color[60 + ((x + y) % 5)];
    }
}

// ---- front-end: title menu, options, field manual -----------------------------

struct ManualPage { const char* title; const char* lines[13]; };
const ManualPage kManual[] = {
    {"WALKER OF VISIONS", {
        "You are a Fremen of Sietch Tabr. Muad'Dib has drunk the Water of",
        "Life; the futures lie open to him. He cannot walk them all -- so he",
        "sends YOU. Drink at his hand and enter a prescient vision: the road",
        "across Arrakis to Arrakeen, and the fall of Baron Harkonnen.",
        "",
        "Click MUAD'DIB in the sietch to begin the vision. Each vision walks one",
        "step of the path toward the Baron. Nothing in a vision is truly real --",
        "so your gear and your life are never lost. But the spice, water, and",
        "materials you gather are only MADE REAL if you END THE VISION and wake",
        "(the Ride key, or complete its path); die, and the spoils vanish.",
        "",
        "'Dreams are messages from the deep.'",
        nullptr}},
    {"CONTROLS", {
        "WASD ........ move (screen-relative; the cursor aims)",
        "Left mouse .. primary action / attack toward the cursor",
        "R/Q/E, RMB .. skills (aimed at the cursor); 1-4 use flasks",
        "Click Muad'Dib to begin a vision  |  click the east door to press on",
        "G ........... work a harvester    T ... drop a thumper (worm bait)",
        "C ........... sandwalk (slow, quiet)   R ... mount a surfaced worm",
        "M ........... map   I ... pack   P/Character ... sheet",
        "Alt+Enter ... fullscreen   Esc ... back / quit",
        nullptr}},
    {"WATER & HEAT", {
        "Water is life. Every step spends water -- more on open sand and",
        "under the midday sun, less on rock or at night. At zero water you",
        "dehydrate and lose blood (life) each turn.",
        "",
        "Direct sun at scorching hours builds EXPOSURE; too much and you",
        "collapse. Move to shade, travel at night, or duck indoors to recover.",
        "A better stillsuit reclaims more of your water. Drink at the sietch",
        "cistern, and harvest the water from the fallen.",
        nullptr}},
    {"COMBAT & CLASSES", {
        "Strike a foe by moving into it or clicking it; ranged classes fire",
        "at the cursor down a clear line. Abilities cost WILL (a shared pool).",
        "",
        "Fedaykin ... Conditioning: a tanky melee bruiser (crysknife, sandwalk)",
        "Saboteur ... Versatility: ranged maula + deployable tech, evasive",
        "Sayyadina .. Awareness: a Focus shield + spice/poison mysticism",
        "",
        "Firing and fighting are LOUD -- noise draws Harkonnen... and worms.",
        nullptr}},
    {"HARVESTERS & THE WORM", {
        "Each field section holds a Harkonnen spice harvester. Deny it --",
        "spoil or destroy its load -- to earn loot and raise the heat (more",
        "escorts, richer rewards). Denials commit you to the run.",
        "",
        "The seismograph reads WORMSIGN. Shai-Hulud comes for rhythmic noise:",
        "a planted thumper calls him; sandwalk to stay silent. A worm is death",
        "on open sand -- but a Fremen can mount and ride the Maker.",
        nullptr}},
    {"EXTRACTION (RIDE HOME)", {
        "You leave a run one of two ways:",
        "",
        "  1. Ride the worm home. After a denial, bait a worm with a thumper,",
        "     mount it with your maker hooks (R), and steer for the sietch.",
        "     Everything you carried is BANKED -- safe forever.",
        "",
        "  2. Die. The sand takes all carried spice, gear, and materials.",
        "",
        "Only what you extract and bank funds your growth. Greed kills.",
        nullptr}},
    {"THE SIETCH (YOUR HUB)", {
        "Between raids you rest in the sietch. It is safe: no worms, no death.",
        "",
        "The Naib ...... tribe tasks and the ride to the Ninth Rock",
        "The Vault ..... stash tabs (banked gear survives death; buy more with spice)",
        "The Sayyadina . spice-rites and healing",
        "The Smuggler .. gear, water, and consumables for spice",
        "",
        "Spend banked SPICE on the Sietch tree (permanent tribe upgrades). Level",
        "up to earn attribute points and passive-tree points. Leave by the door.",
        nullptr}},
    {"GEAR, FLASKS & CRAFTING", {
        "Loot drops with affixes and rarities (normal / magic / rare). Equip it",
        "from your pack; better gear rolls higher tiers in deeper runs.",
        "",
        "Flasks -- Life, Will, Utility -- ride a belt and are used on the number",
        "keys, recharging from kills and time.",
        "",
        "Crafting: materials found in runs are spent at the sietch bench to",
        "reroll, add, remove, or upgrade an item's affixes (PoE-style).",
        "  (Note: affixes, flasks, and the crafting bench are in active",
        "   development -- some are still coming online.)",
        nullptr}},
};
constexpr int kManualPages = int(sizeof kManual / sizeof *kManual);

const char* const kMenuItems[]   = {"Enter the Sietch", "Field Manual", "Options",
                                     "Leave the Desert"};
constexpr int kMenuCount = 4;
const char* const kOptionItems[] = {"Sound", "Toggle Fullscreen",
                                    "Wipe Character + Vault", "Back"};
constexpr int kOptionCount = 4;

// hit-test a vertically-stacked, horizontally-centered menu; row under the mouse or -1
int frontend_row_at(const InputState& in, int start_y, int row_h, int count) {
    if (in.mouse_x < 0 || in.mouse_x < fb::W / 2 - 360 || in.mouse_x > fb::W / 2 + 360)
        return -1;
    for (int i = 0; i < count; ++i)
        if (in.mouse_y >= start_y + i * row_h - 20 && in.mouse_y < start_y + i * row_h + 34)
            return i;
    return -1;
}

void pass_frontend(RenderCtx& r) {
    DrawCtx& ctx = r.ctx;
    set_text_scale(2);
    constexpr int kCh = 8;   // half a 16px glyph (centering); scale-4 title uses 16
    fill_rect(ctx, 0, 0, fb::W, fb::H, 1);
    // a warm horizon band so the title screen isn't flat black
    for (int y = 0; y < 360; ++y)
        fill_rect(ctx, 0, y, fb::W, 1, uint8_t(32 + (360 - y) * 30 / 360));

    if (g.screen == GameState::Screen::Menu) {
        draw_text_shadow(ctx, "D U S T F A L L", fb::W / 2 - 15 * 16, 150, 230, 1, 4);
        draw_text_shadow(ctx, "a Fremen extraction roguelike",
                         fb::W / 2 - 29 * kCh, 250, 24, 1);
        for (int i = 0; i < kMenuCount; ++i) {
            const bool sel = i == g.menu_idx;
            const int y = 430 + i * 80;
            if (sel) draw_text_shadow(ctx, ">", fb::W / 2 - 260, y, 245, 1);
            draw_text_shadow(ctx, kMenuItems[i],
                             fb::W / 2 - int(std::strlen(kMenuItems[i])) * kCh, y,
                             uint8_t(sel ? 245 : 20), 1);
        }
        draw_text_shadow(ctx, "up / down + ENTER   (mouse works too)",
                         fb::W / 2 - 37 * kCh, 860, 12, 1);
        return;
    }
    if (g.screen == GameState::Screen::Options) {
        draw_text_shadow(ctx, "OPTIONS", fb::W / 2 - 7 * kCh, 220, 230, 1);
        for (int i = 0; i < kOptionCount; ++i) {
            const bool sel = i == g.options_idx;
            const int y = 380 + i * 80;
            char buf[48];
            if (i == 0) std::snprintf(buf, sizeof buf, "Sound: %s", g.opt_muted ? "OFF" : "ON");
            else std::snprintf(buf, sizeof buf, "%s", kOptionItems[i]);
            if (sel) draw_text_shadow(ctx, ">", fb::W / 2 - 260, y, 245, 1);
            draw_text_shadow(ctx, buf, fb::W / 2 - int(std::strlen(buf)) * kCh, y,
                             uint8_t(sel ? 245 : 20), 1);
        }
        draw_text_shadow(ctx, "ENTER select    ESC back", fb::W / 2 - 24 * kCh, 860, 12, 1);
        return;
    }
    // Manual
    const ManualPage& p = kManual[g.manual_page];
    fill_rect(ctx, fb::W / 2 - 580, 80, 1160, 880, 2);
    fill_rect(ctx, fb::W / 2 - 580, 80, 1160, 3, 228);
    fill_rect(ctx, fb::W / 2 - 580, 957, 1160, 3, 228);
    draw_text_shadow(ctx, p.title, fb::W / 2 - int(std::strlen(p.title)) * kCh, 110, 230, 1);
    int y = 190;
    for (const char* line : p.lines) {
        if (!line) break;
        if (line[0]) draw_text(ctx, line, fb::W / 2 - 550, y, 20);
        y += 34;
    }
    char pg[40];
    std::snprintf(pg, sizeof pg, "page %d / %d", g.manual_page + 1, kManualPages);
    draw_text_shadow(ctx, pg, fb::W / 2 - int(std::strlen(pg)) * kCh, 900, 14, 1);
    draw_text_shadow(ctx, "<- / -> pages     ESC back", fb::W / 2 - 26 * kCh, 930, 12, 1);
}

// Returns false only when the player picks "Leave the Desert" (quits the game).
bool update_menu(const InputState& in) {
    const int hov = frontend_row_at(in, 430, 80, kMenuCount);
    if (hov >= 0) g.menu_idx = hov;
    if (in.was_pressed(Key::Up)) g.menu_idx = (g.menu_idx + kMenuCount - 1) % kMenuCount;
    if (in.was_pressed(Key::Down)) g.menu_idx = (g.menu_idx + 1) % kMenuCount;
    const bool go = in.was_pressed(Key::Confirm) || (hov >= 0 && in.mouse_pressed[0]);
    if (!go) return true;
    switch (g.menu_idx) {
        case 0: g.screen = GameState::Screen::Play; break;     // enter the sietch
        case 1: g.screen = GameState::Screen::Manual; g.manual_page = 0; break;
        case 2: g.screen = GameState::Screen::Options; g.options_idx = 0; break;
        case 3: return false;                                   // leave the desert (quit)
    }
    return true;
}

void update_options(const InputState& in) {
    if (in.was_pressed(Key::Cancel)) { g.screen = GameState::Screen::Menu; return; }
    const int hov = frontend_row_at(in, 380, 80, kOptionCount);
    if (hov >= 0) g.options_idx = hov;
    if (in.was_pressed(Key::Up)) g.options_idx = (g.options_idx + kOptionCount - 1) % kOptionCount;
    if (in.was_pressed(Key::Down)) g.options_idx = (g.options_idx + 1) % kOptionCount;
    if (!(in.was_pressed(Key::Confirm) || (hov >= 0 && in.mouse_pressed[0]))) return;
    switch (g.options_idx) {
        case 0: g.opt_muted = !g.opt_muted; break;
        case 1: g.fullscreen_req = true; break;
        case 2:  // wipe the persistent save + vault (a fresh start next launch)
            std::remove("meta.dat");
            g_meta = Meta{};
            break;
        case 3: g.screen = GameState::Screen::Menu; break;
    }
}

void update_manual(const InputState& in) {
    if (in.was_pressed(Key::Cancel)) { g.screen = GameState::Screen::Menu; return; }
    const bool clickL = in.mouse_pressed[0] && in.mouse_x >= 0 && in.mouse_x < fb::W / 2;
    const bool clickR = in.mouse_pressed[0] && in.mouse_x >= fb::W / 2;
    if (in.was_pressed(Key::Right) || in.was_pressed(Key::Down) || clickR)
        if (g.manual_page < kManualPages - 1) ++g.manual_page;
    if (in.was_pressed(Key::Left) || in.was_pressed(Key::Up) || clickL)
        if (g.manual_page > 0) --g.manual_page;
}

// G4: draw a W x H item grid (empty-cell lattice + variable-size item rects) at (ox,oy). `hover` is
// the grid cell under the mouse (-1 = none); `drag_pool` is the pool index being dragged (hidden here,
// drawn at the cursor instead). Items get a rarity/currency tint, a short label, and stack counts.
void draw_item_grid(DrawCtx& ctx, int ox, int oy, const items::Inst* pool, const uint8_t* cellarr,
                    int cap, int W, int H, int hover, int drag_pool) {
    const int C = gridui::CELL;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int px = ox + x * C, py = oy + y * C;
            fill_rect(ctx, px, py, C - 1, C - 1, 2);
            fill_rect(ctx, px, py, C - 1, 1, 3);
        }
    if (hover >= 0 && hover < W * H)
        fill_rect(ctx, ox + (hover % W) * C, oy + (hover / W) * C, C - 1, C - 1, 4);
    for (int i = 0; i < cap; ++i) {
        if (pool[i].empty() || i == drag_pool) continue;
        const items::GridSize s = pool[i].size();
        const int px = ox + (cellarr[i] % W) * C, py = oy + (cellarr[i] / W) * C;
        const int w = s.w * C - 2, hgt = s.h * C - 2;
        const uint8_t col = pool[i].is_currency() ? 137
                            : pool[i].unidentified() ? 8
                            : pool[i].rarity == 2 ? 245 : pool[i].rarity == 1 ? 230 : 20;
        fill_rect(ctx, px + 1, py + 1, w, hgt, uint8_t(pool[i].corrupted() ? 176 : 5));
        fill_rect(ctx, px + 1, py + 1, w, 2, col);
        fill_rect(ctx, px + 1, py + 1, 2, hgt, col);
        char nm[48]; pool[i].full_name(nm, sizeof nm);
        char lbl[8]; std::snprintf(lbl, sizeof lbl, "%.6s", nm);
        draw_text(ctx, lbl, px + 4, py + 4, col);
        if (pool[i].is_currency()) {
            char cnt[8]; std::snprintf(cnt, sizeof cnt, "%d", int(pool[i].stack));
            draw_text(ctx, cnt, px + 4, py + hgt - 12, 231);
        }
        if (pool[i].mirrored()) draw_text(ctx, "M", px + w - 12, py + 4, 245);
        else if (pool[i].unidentified()) draw_text(ctx, "?", px + w - 12, py + 4, 231);
    }
}
// G4: the gear paperdoll — one clickable row box per equip slot (name + equipped item).
void draw_paperdoll(DrawCtx& ctx, int ox, int oy, int hover_slot) {
    static const char* slots[int(items::Slot::Count)] =
        {"Weapon", "Hood", "Mask", "Plugs", "Tube", "Body", "Belt", "Boots", "Cloak", "Token"};
    for (int i = 0; i < int(items::Slot::Count); ++i) {
        const int y = oy + i * (gridui::EQ_H + 4);
        fill_rect(ctx, ox, y, gridui::EQ_W, gridui::EQ_H, uint8_t(hover_slot == i ? 4 : 2));
        fill_rect(ctx, ox, y, gridui::EQ_W, 1, 3);
        char nm[48]; g.pl.equip[i].full_name(nm, sizeof nm);
        char buf[80]; std::snprintf(buf, sizeof buf, "%-6s %s", slots[i], nm);
        draw_text(ctx, buf, ox + 8, y + 13, uint8_t(g.pl.equip[i].empty() ? 8 : 136));
    }
}
// C4: a thin line (Bresenham, plotted as 3x3 dots) — for the passive-tree edges. No engine primitive.
void draw_line(DrawCtx& ctx, int x0, int y0, int x1, int y1, uint8_t col) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;  // dy negative
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int guard = 0; guard < 4000; ++guard) {
        fill_rect(ctx, x0 - 1, y0 - 1, 3, 3, col);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
// C4: the passive skill tree — the node graph (edges + discs, coloured by allocated/allocatable/locked).
void draw_passive_tree(DrawCtx& ctx) {
    fill_rect(ctx, 0, 0, fb::W, fb::H, 1);
    draw_text_shadow(ctx, "THE PASSIVE TREE", fb::W / 2 - 15 * 8, 56, 230, 1);
    char buf[96];
    std::snprintf(buf, sizeof buf, "passive points: %d      (click a lit node to allocate  -  C / ESC to close)",
                  passive_points_available());
    draw_text(ctx, buf, fb::W / 2 - 300, 92, 14);
    for (int e = 0; e < kPassiveEdgeCount; ++e) {  // edges under the nodes
        int ax, ay, bx, by;
        passiveui::node_screen(kPassiveEdges[e][0], ax, ay);
        passiveui::node_screen(kPassiveEdges[e][1], bx, by);
        const bool lit = passive_has(kPassiveEdges[e][0]) && passive_has(kPassiveEdges[e][1]);
        draw_line(ctx, ax, ay, bx, by, uint8_t(lit ? 231 : 3));
    }
    const int hov = passiveui::node_at(g.ui_mx, g.ui_my);
    for (int i = 0; i < kPassiveNodeCount; ++i) {
        int sx, sy; passiveui::node_screen(i, sx, sy);
        const bool alloc = passive_has(i), can = passive_allocatable(i);
        const uint8_t col = alloc ? 245 : can ? 231 : 8;  // gold / lit / dim
        const int r = passiveui::R;
        fill_rect(ctx, sx - r, sy - r, 2 * r, 2 * r, uint8_t(i == hov ? 4 : 2));
        fill_rect(ctx, sx - r + 2, sy - r + 2, 2 * r - 4, 2 * r - 4, col);
        char lbl[10]; std::snprintf(lbl, sizeof lbl, "%.7s", kPassiveNodes[i].name);
        draw_text(ctx, lbl, sx - r, sy + r + 2, uint8_t(alloc ? 245 : can ? 200 : 14));
    }
    if (hov >= 0) {  // detail for the hovered node
        const PassiveNode& nd = kPassiveNodes[hov];
        if (nd.value > 0) std::snprintf(buf, sizeof buf, "%s   +%d %s", nd.name, nd.value, items::stat_name(nd.kind));
        else std::snprintf(buf, sizeof buf, "%s   (class anchor)", nd.name);
        draw_text_shadow(ctx, buf, fb::W / 2 - 200, fb::H - 70, 231, 1);
    }
}
// G4: draw the item currently being dragged, floating at the cursor.
void draw_drag_ghost(DrawCtx& ctx) {
    if (!g.drag_kind || g.ui_mx < 0) return;
    const items::Inst* it = nullptr;
    if (g.drag_kind == 1 && g.drag_idx >= 0) it = &g.pl.inv[g.drag_idx];
    else if (g.drag_kind == 2 && g.drag_idx >= 0) it = &g.pl.equip[g.drag_idx];
    else if (g.drag_kind == 3 && g.drag_idx >= 0) it = &g_meta.stash[g.stash_tab][g.drag_idx];
    char nm[48];
    if (it && !it->empty()) it->full_name(nm, sizeof nm);
    else if (g.drag_kind == 4) std::snprintf(nm, sizeof nm, "%s", items::currency_name(uint8_t(g.drag_idx)));
    else return;
    const int w = 140, h = 20;
    fill_rect(ctx, g.ui_mx + 6, g.ui_my + 6, w, h, 5);
    fill_rect(ctx, g.ui_mx + 6, g.ui_my + 6, w, 2, 245);
    draw_text(ctx, nm, g.ui_mx + 12, g.ui_my + 12, 231);
}

// UI: HUD panel, banners, death screen.
void pass_ui(RenderCtx& r) {
    DrawCtx& ctx = r.ctx;
    const map::Zone& z = *r.z;

    // R2: the UI pass draws its 8x8 font at 2x (16px) for 1080p. Text-centering
    // uses half a glyph = 8px; row spacing and panel sizes are doubled to match.
    set_text_scale(2);
    constexpr int kCh = 8;  // half a 16px glyph, for centering: x = cx - strlen*kCh

    if (g.choosing) {
        fill_rect(ctx, 0, 0, fb::W, fb::H, 1);
        draw_text_shadow(ctx, "D U S T F A L L", fb::W / 2 - 15 * kCh, 120, 230, 1);
        draw_text_shadow(ctx, "Choose your path", fb::W / 2 - 16 * kCh, 170, 24, 1);

        const int n = int(BaseClass::Count);
        set_draw_scale(1);  // R3: class portraits authored at native size, blit 1:1
        for (int i = 0; i < n; ++i) {
            const int cx = fb::W / 2 + (i - 1) * 320;
            const bool sel = i == g.choose_idx;
            if (const SpriteSheet* spr = r.assets->get(kBaseClasses[i].sprite)) {
                if (sel) fill_rect(ctx, cx - 90, 300, 180, 220, 3);
                draw_sprite(ctx, *spr, (g.anim_tick / 9) % spr->frames, cx, 500,
                            sel ? 31 : 24);
            }
            const int name_x = cx - int(std::strlen(kBaseClasses[i].name)) * kCh;
            draw_text_shadow(ctx, kBaseClasses[i].name, name_x, 540, sel ? 245 : 12, 1);
        }
        set_draw_scale(1);
        const BaseClassDef& b = kBaseClasses[g.choose_idx];
        draw_text_shadow(ctx, b.blurb1, fb::W / 2 - int(std::strlen(b.blurb1)) * kCh, 640, 28, 1);
        draw_text_shadow(ctx, b.blurb2, fb::W / 2 - int(std::strlen(b.blurb2)) * kCh, 672, 230, 1);
        draw_text_shadow(ctx, "left/right choose   ENTER begin",
                         fb::W / 2 - 31 * kCh, 760, 14, 1);
        if (g_meta.runs > 0) {
            char mbuf[64];
            std::snprintf(mbuf, sizeof mbuf, "seasons ended %u   endings seen:%s%s%s",
                          unsigned(g_meta.runs),
                          (g_meta.endings_seen & (1 << campaign::kVictory)) ? " VICTORY" : "",
                          (g_meta.endings_seen & (1 << campaign::kQuotaLoss)) ? " QUOTA" : "",
                          (g_meta.endings_seen & (1 << campaign::kRaidLoss)) ? " RAID" : "");
            draw_text(ctx, mbuf, fb::W / 2 - int(std::strlen(mbuf)) * kCh, 860, 12);
        }
        return;
    }

    if (g.disp_hp < 0) { g.disp_hp = g.pl.hp * 16; g.disp_water = g.pl.water * 16; }
    g.disp_hp += (g.pl.hp * 16 - g.disp_hp) / 8;
    g.disp_water += (g.pl.water * 16 - g.disp_water) / 8;
    ui::HudState hud{(g.disp_hp + 8) / 16, player_max_hp(),
                     (g.disp_water + 8) / 16, player_max_water(),
                     g.pl.exposure, hour_now(), ambient_now(),
                     g.pl.turn / tune::kTurnsPerDay + 1,
                     (z.at(player_ent().x, player_ent().y) & map::TF_INDOOR) != 0,
                     g.noise_meter, uint8_t(g.wrm.state), g.seismo, g.seismo_head,
                     g.pl.thumpers, g.pl.sandwalk};
    ui::draw_hud(ctx, hud, g.log, g.anim_tick, *r.assets);

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
        draw_text_shadow(ctx, buf, 12, 8,
                         g.op.state == harvest::OpState::Destroyed ? 245 :
                         g.op.state == harvest::OpState::Evac ? 210 : 230, 1);
        if (g.alarm_turns > 0) {
            std::snprintf(buf, sizeof buf, "ALARM %d", g.alarm_turns);
            draw_text_shadow(ctx, buf, 12, 32, 208, 1);
        }
    }

    // hover description + personal gauges, just above the panel
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "LVL %d  xp %d/%d  spice %d%s%s",
                      int(g.pl.st.level), int(g.pl.st.xp), xp_to_level(g.pl.st.level),
                      g.pl.spice,
                      g.pl.riding ? "  [RIDING]" : "",
                      g.pl.st.statpts + g.pl.st.talentpts > 0 ? "  [P: spend pts]" : "");
        draw_text_shadow(ctx, buf, 4, ui::kPanelY - 22, 136, 1);
        // C7 + flasks readout: the Will pool, the three flask charges, and ability readiness.
        char abuf[96];
        std::snprintf(abuf, sizeof abuf, "Will %d/%d   flasks [L%d W%d U%d]   abilities %s",
                      g.pl.will, player_max_will(),
                      g.pl.flask_charges[0], g.pl.flask_charges[1], g.pl.flask_charges[2],
                      g.pl.ability_cd > 0 ? "(cooldown)" : "ready Q/E/F");
        draw_text_shadow(ctx, abuf, 4, ui::kPanelY - 40, 208, 1);
        if (g.hover_tx >= 0)
            if (const char* what = describe_at(g.hover_tx, g.hover_ty))
                draw_text_shadow(ctx, what, fb::W / 2 - int(std::strlen(what)) * kCh,
                                 ui::kPanelY - 62, 24, 1);  // lifted off the level/task row
        if (g.sideobj_kind != 0) {
            std::snprintf(buf, sizeof buf, "%s in the %s (%d)",
                          g.sideobj_kind == 1 ? "CARAVAN" : "GUILD ENVOY",
                          g.sideobj_zone == map::kZoneDesert ? "Rimwall" : "Deep Pan",
                          int(g.sideobj_expires - g.pl.turn));
            draw_text_shadow(ctx, buf, fb::W - 8 - int(std::strlen(buf)) * 16, 76, 170, 1);
        }
        if (g.quest_kind != 0) {
            std::snprintf(buf, sizeof buf, "TASK: %s (%d/%d)",
                          g.quest_kind == 1 ? "cull the hounds"
                          : g.quest_kind == 2 ? "gather spice" : "quiet denial",
                          g.quest_kind == 2 ? g.quest_progress + g.pl.spice
                                            : g.quest_progress,
                          g.quest_need);
            draw_text_shadow(ctx, buf, fb::W - 8 - int(std::strlen(buf)) * 16,
                             ui::kPanelY - 22, 240, 1);
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
        draw_text_shadow(ctx, dbuf, 4, 96, 31, 1);
    }

    // CHOAM ledger + season gauges, top-right.
    {
        char buf[48];
        std::snprintf(buf, sizeof buf, "SHIPPED %d/%d  DENIED %d", int(g.season.shipped),
                      campaign::kQuotaLossAt, int(g.season.denied));
        draw_text_shadow(ctx, buf, fb::W - 8 - int(std::strlen(buf)) * 16, 8, 224, 1);
        std::snprintf(buf, sizeof buf, "ALERT %d  STANDING %d", int(g.season.alert),
                      int(g.season.standing));
        draw_text_shadow(ctx, buf, fb::W - 8 - int(std::strlen(buf)) * 16, 30,
                         uint8_t(g.season.alert >= campaign::kAlertDiscoveryAt ? 208 : 14), 1);
        if (g.season.alert >= campaign::kAlertDiscoveryAt) {
            std::snprintf(buf, sizeof buf, "DISCOVERY %d%%",
                          g.season.discovery * 100 / campaign::kDiscoveryRaidAt);
            draw_text_shadow(ctx, buf, fb::W - 8 - int(std::strlen(buf)) * 16, 52, 208, 1);
        }
    }

    // A5: minimap of everywhere seen this run — a gold-framed top-right panel,
    // 2px/tile, coloured by feature. Toggled with M; hidden under menus.
    if (g.show_map && !g.menu_open) {
        const map::Zone& mz = zone();
        const int scale = 3;
        const int mw = map::W * scale, mh = map::H * scale;  // 192 x 192
        const int mx = fb::W - mw - 8, my = 90;  // below the CHOAM/sideobj stack
        fill_rect(ctx, mx - 2, my - 2, mw + 4, mh + 4, 230);  // gold frame
        fill_rect(ctx, mx - 1, my - 1, mw + 2, mh + 2, 4);    // dark backdrop
        for (int ty = 0; ty < map::H; ++ty)
            for (int tx = 0; tx < map::W; ++tx) {
                const int idx = ty * map::W + tx;
                if (!(mz.vis[idx] & (map::V_VISIBLE | map::V_REMEMBERED))) continue;
                const uint8_t f = mz.flags[idx];
                uint8_t c;
                if (f & map::TF_DOOR) c = 246;             // green exit pad
                else if (f & map::TF_CISTERN) c = 172;     // blue cistern
                else if (f & map::TF_WALL) c = 24;         // wall
                else if (f & map::TF_ROCKPROP) c = 20;     // rock prop
                else if (mz.floor[idx] == 5) c = 232;      // spice-veined pan
                else c = 14;                               // open floor
                fill_rect(ctx, mx + tx * scale, my + ty * scale, scale, scale, c);
            }
        if ((g.anim_tick / 15) & 1) {  // blinking player dot
            const Entity& p = player_ent();
            fill_rect(ctx, mx + p.x * scale - 1, my + p.y * scale - 1, scale + 2, scale + 2, 245);
        }
    }

    if (g.menu_open == 13) draw_passive_tree(ctx);  // C4: the passive-tree screen

    // Endgame hub menus (War Council / Jihad Table / Landsraad / Bench) — dynamic, keyboard-driven.
    if (g.menu_open >= 8 && g.menu_open <= 12) {
        const int x0 = fb::W / 2 - 340, y0 = 200;
        if (g.menu_open <= 10 || g.menu_open == 12) {  // 8-10 + 12 keyboard lists; 11 is a grid screen (G4)
        fill_rect(ctx, x0, y0, 680, 460, 2);
        fill_rect(ctx, x0, y0, 680, 2, 228);
        fill_rect(ctx, x0, y0 + 458, 680, 2, 228);
        }
        static const char* kFac[] = {"Bene Gesserit (Wallach IX)", "Tleilaxu (Bandalong)",
                                     "Ixians (Ix)", "House Corrino (Kaitain)", "Spacing Guild (Junction)"};
        char buf[96];
        auto row = [&](int i, const char* text, bool sel) {
            if (sel) fill_rect(ctx, x0 + 16, y0 + 78 + i * 30 - 4, 648, 26, 4);
            draw_text(ctx, text, x0 + 28, y0 + 78 + i * 30, uint8_t(sel ? 245 : 20));
        };
        if (g.menu_open == 8) {
            draw_text_shadow(ctx, "THE WAR COUNCIL", x0 + 24, y0 + 16, 230, 1);
            draw_text(ctx, "Enact the Jihad. Muad'Dib's legions await your word.", x0 + 24, y0 + 46, 14);
            const char* rows[4] = {"The Jihad Table  (strikes & pinnacles)",
                                   "The Landsraad  (CHOAM politics)",
                                   "The Crafting Bench", "Leave"};
            for (int i = 0; i < 4; ++i) row(i, rows[i], g.menu_sel == i);
        } else if (g.menu_open == 9) {
            draw_text_shadow(ctx, "THE JIHAD TABLE", x0 + 24, y0 + 16, 230, 1);
            std::snprintf(buf, sizeof buf, "spice %d   commission tier %d  (<-/-> change)   shards %d/5",
                          g.pl.spice, g.jihad_tier, __builtin_popcount(g_meta.golden_shards));
            draw_text(ctx, buf, x0 + 24, y0 + 46, 14);
            for (int f = 0; f < FAC_COUNT; ++f) {
                if (pinnacle_available(f))
                    std::snprintf(buf, sizeof buf, "ASSAULT %s  [PINNACLE OPEN]", kFac[f]);
                else
                    std::snprintf(buf, sizeof buf, "Strike %s  (%d sp)  [%d%%]", kFac[f],
                                  g.jihad_tier * tune::kStrikeSpiceBase, int(g_meta.jihad_pressure[f]));
                row(f, buf, g.menu_sel == f);
            }
            row(FAC_COUNT, capstone_available() ? "CONFRONT THE GOLDEN PATH" : "(the Golden Path: 5 shards)",
                g.menu_sel == FAC_COUNT);
            row(FAC_COUNT + 1, "Back", g.menu_sel == FAC_COUNT + 1);
        } else if (g.menu_open == 10) {
            draw_text_shadow(ctx, "THE LANDSRAAD", x0 + 24, y0 + 16, 230, 1);
            std::snprintf(buf, sizeof buf, "CHOAM control %d%%   spice %d", choam_control(), g.pl.spice);
            draw_text(ctx, buf, x0 + 24, y0 + 46, 14);
            for (int f = 0; f < FAC_COUNT; ++f) {
                const int st = house_stance(f);
                std::snprintf(buf, sizeof buf, "Court %s  (%d sp)  [%s]", kFac[f], tune::kCourtCost,
                              st == HOUSE_ALLY ? "ALLY" : st == HOUSE_OPPONENT ? "OPPOSED" : "neutral");
                row(f, buf, g.menu_sel == f);
            }
            std::snprintf(buf, sizeof buf, "Collect CHOAM dividend  (~%d sp)", choam_control());
            row(FAC_COUNT, buf, g.menu_sel == FAC_COUNT);
            row(FAC_COUNT + 1, "Back", g.menu_sel == FAC_COUNT + 1);
        } else if (g.menu_open == 12) {  // C5b: the Sietch talent tree (spend Litrejons)
            draw_text_shadow(ctx, "THE SIETCH TREE", x0 + 24, y0 + 16, 230, 1);
            std::snprintf(buf, sizeof buf, "litrejons %d   (tribal water reclaimed by the Deathstill)",
                          int(g_meta.litrejons));
            draw_text(ctx, buf, x0 + 24, y0 + 46, 14);
            for (int i = 0; i < kSietchNodeCount; ++i) {
                const int rank = g_meta.sietch_rank[i], cost = sietch_node_cost(i);
                const char* trk = kSietchNodes[i].track == 0 ? "Sandcraft" : "Blessing";
                if (cost > 0)
                    std::snprintf(buf, sizeof buf, "[%-9s] %-16s %d/%d  (+%d)  -  %d litrejons", trk,
                                  kSietchNodes[i].name, rank, kSietchNodes[i].max_rank,
                                  kSietchNodes[i].per_rank, cost);
                else
                    std::snprintf(buf, sizeof buf, "[%-9s] %-16s %d/%d  (+%d)  -  MAX", trk,
                                  kSietchNodes[i].name, rank, kSietchNodes[i].max_rank, kSietchNodes[i].per_rank);
                row(i, buf, g.menu_sel == i);
            }
            row(kSietchNodeCount, "Back to the Naib", g.menu_sel == kSietchNodeCount);
        } else {  // G4: the crafting bench — the pack grid (left) + the currency-tab slots (right)
            const int bx = fb::W / 2 - 620, by = 190;
            fill_rect(ctx, bx, by, 1240, 620, 2);
            fill_rect(ctx, bx, by, 1240, 2, 228);
            fill_rect(ctx, bx, by + 618, 1240, 2, 228);
            const UIHitResult bh = ui_hit_test(g.ui_mx, g.ui_my);
            draw_text_shadow(ctx, "THE CRAFTING BENCH   Ctrl+click currency to bank it - drag an orb onto gear to apply - ESC to the council",
                             bx + 24, by + 16, 230, 1);
            draw_text(ctx, "PACK", gridui::bench_pack_x(), gridui::bench_pack_y() - 20, 14);
            draw_text(ctx, "CURRENCY TABLE", gridui::bench_tab_x(), gridui::bench_tab_y() - 20, 14);
            draw_item_grid(ctx, gridui::bench_pack_x(), gridui::bench_pack_y(), g.pl.inv, g.pl.inv_cell,
                           tune::kInvCap, tune::kPackW, tune::kPackH,
                           bh.what == UIHit::PackCell ? bh.idx : -1, g.drag_kind == 1 ? g.drag_idx : -1);
            // currency-tab slots: one cell per currency kind, showing its count + name
            const int C = gridui::CELL, cols = gridui::TAB_COLS;
            for (int c = 1; c < items::CUR_COUNT; ++c) {
                const int slot = c - 1, px = gridui::bench_tab_x() + (slot % cols) * C;
                const int py = gridui::bench_tab_y() + (slot / cols) * C;
                const bool hov = bh.what == UIHit::CurTabSlot && bh.idx == c;
                fill_rect(ctx, px, py, C - 1, C - 1, uint8_t(hov ? 4 : 2));
                fill_rect(ctx, px, py, C - 1, 1, 3);
                if (g.pl.cur_tab[c] > 0) {
                    fill_rect(ctx, px + 1, py + 1, C - 3, C - 3, 5);
                    char nm[16]; std::snprintf(nm, sizeof nm, "%.5s", items::currency_name(uint8_t(c)));
                    draw_text(ctx, nm, px + 3, py + 4, 137);
                    char cnt[8]; std::snprintf(cnt, sizeof cnt, "%d", int(g.pl.cur_tab[c]));
                    draw_text(ctx, cnt, px + 3, py + C - 14, 231);
                }
            }
            const int over = ui_item_under(bh);  // tooltip on the pack item under the cursor
            if (over >= 0) {
                char det[256]; g.pl.inv[over].describe(det, sizeof det);
                int dy = gridui::bench_pack_y() + tune::kPackH * C + 14;
                for (char* line = det; *line;) {
                    char* nl = std::strchr(line, '\n'); if (nl) *nl = 0;
                    draw_text(ctx, line, gridui::bench_pack_x(), dy, 200);
                    dy += 16; if (!nl) break; line = nl + 1;
                }
            }
        }
    } else
    // Character sheet / inventory / vault overlays (dynamic content).
    if (g.menu_open >= 5 && g.menu_open <= 7) {
        char buf[80];
        const UIHitResult gh = ui_hit_test(g.ui_mx, g.ui_my);
        if (g.menu_open == 5) {
            const int x0 = fb::W / 2 - 380, y0 = 140;
            fill_rect(ctx, x0, y0, 760, 640, 2);
            fill_rect(ctx, x0, y0, 760, 2, 228);
            fill_rect(ctx, x0, y0 + 638, 760, 2, 228);
            draw_text_shadow(ctx, "CHARACTER", x0 + 24, y0 + 16, 230, 1);
            std::snprintf(buf, sizeof buf, "level %d   xp %d/%d   stat pts %d   talent pts %d",
                          int(g.pl.st.level), int(g.pl.st.xp), xp_to_level(g.pl.st.level),
                          int(g.pl.st.statpts), int(g.pl.st.talentpts));
            draw_text(ctx, buf, x0 + 24, y0 + 44, 14);
            const char* rows[3] = {"Conditioning", "Versatility", "Awareness"};
            const int vals[3] = {g.pl.st.cond, g.pl.st.vers, g.pl.st.awar};
            for (int i = 0; i < 3; ++i) {
                const bool sel = g.menu_sel == i;
                if (sel) fill_rect(ctx, x0 + 16, y0 + 88 + i * 24 - 2, 728, 22, 4);
                std::snprintf(buf, sizeof buf, "%-10s %3d", rows[i], vals[i]);
                draw_text(ctx, buf, x0 + 32, y0 + 88 + i * 24, uint8_t(sel ? 245 : 20));
            }
            for (int i = 0; i < kTalentCount; ++i) {
                const bool sel = g.menu_sel == 3 + i;
                const int y = y0 + 176 + i * 24;
                if (sel) fill_rect(ctx, x0 + 16, y - 2, 728, 22, 4);
                std::snprintf(buf, sizeof buf, "%-22s %d/3", kTalentNames[i],
                              int(g.pl.st.talents[i]));
                draw_text(ctx, buf, x0 + 32, y, uint8_t(sel ? 245 : 20));
            }
            std::snprintf(buf, sizeof buf,
                          "Life %d  Lethality+%d  Precision+%d  Flow(dodge) %d  armor %d",
                          player_max_hp(), player_lethality(),
                          affix_total(items::AK_TOHIT) + g.pl.st.vers * 2, player_dodge(),
                          player_armor());
            draw_text(ctx, buf, x0 + 24, y0 + 420, 136);
            std::snprintf(buf, sizeof buf, "Focus (shield) %d/%d   Will %d   crit %d%%",
                          g.pl.shield, player_max_shield(), player_max_will(),
                          affix_total(items::AK_CRIT) + 4 * g.pl.st.talents[2]);
            draw_text(ctx, buf, x0 + 24, y0 + 440, 136);
            std::snprintf(buf, sizeof buf, "Press C for the PASSIVE TREE  (%d points to spend)",
                          passive_points_available());
            draw_text(ctx, buf, x0 + 24, y0 + 484, 230);
            draw_text(ctx, g.menu_sel == 12 ? "> Close" : "  Close", x0 + 32, y0 + 460,
                      uint8_t(g.menu_sel == 12 ? 245 : 20));
        } else if (g.menu_open == 6) {  // G4: gear paperdoll (left) + the 8x12 pack grid (right)
            const int bx = fb::W / 2 - 480, by = 170;
            fill_rect(ctx, bx, by, 980, 660, 2);
            fill_rect(ctx, bx, by, 980, 2, 228);
            fill_rect(ctx, bx, by + 658, 980, 2, 228);
            draw_text_shadow(ctx, "PACK   drag to move / equip - Ctrl+click to equip - right-click closes",
                             bx + 24, by + 16, 230, 1);
            draw_paperdoll(ctx, gridui::inv_eq_x(), gridui::inv_eq_y(),
                           gh.what == UIHit::EquipSlot ? gh.idx : -1);
            draw_item_grid(ctx, gridui::inv_pack_x(), gridui::inv_pack_y(), g.pl.inv, g.pl.inv_cell,
                           tune::kInvCap, tune::kPackW, tune::kPackH,
                           gh.what == UIHit::PackCell ? gh.idx : -1, g.drag_kind == 1 ? g.drag_idx : -1);
            const int over = ui_item_under(gh);
            if (over >= 0) {  // tooltip: the item under the cursor
                char det[256]; g.pl.inv[over].describe(det, sizeof det);
                int dy = gridui::inv_pack_y() + tune::kPackH * gridui::CELL + 14;
                for (char* line = det; *line;) {
                    char* nl = std::strchr(line, '\n'); if (nl) *nl = 0;
                    draw_text(ctx, line, gridui::inv_pack_x(), dy, 200);
                    dy += 16; if (!nl) break; line = nl + 1;
                }
            }
        } else {  // G4: the Vault — a stash tab (12x12) + the pack grid + a tab strip + Buy
            const int bx = fb::W / 2 - 640, by = 170;
            fill_rect(ctx, bx, by, 1280, 680, 2);
            fill_rect(ctx, bx, by, 1280, 2, 228);
            fill_rect(ctx, bx, by + 678, 1280, 2, 228);
            draw_text_shadow(ctx, "THE SIETCH VAULT   drag between stash and pack - Ctrl+click moves - right-click closes",
                             bx + 24, by + 16, 230, 1);
            for (int t = 0; t < g_meta.stash_tabs; ++t) {  // tab strip
                const int tx = gridui::vault_stash_x() + t * 44, ty = gridui::vault_tab_y();
                fill_rect(ctx, tx, ty, 40, 40, uint8_t(t == g.stash_tab ? 245 : 2));
                fill_rect(ctx, tx, ty, 40, 2, 228);
                char tb[4]; std::snprintf(tb, sizeof tb, "%d", t + 1);
                draw_text(ctx, tb, tx + 14, ty + 14, uint8_t(t == g.stash_tab ? 5 : 231));
            }
            const int cost = stash_tab_cost(), buyx = gridui::vault_stash_x() + tune::kMaxStashTabs * 44 + 20;
            if (cost > 0) {
                fill_rect(ctx, buyx, gridui::vault_tab_y(), 220, 40, uint8_t(gh.what == UIHit::BuyTab ? 4 : 2));
                fill_rect(ctx, buyx, gridui::vault_tab_y(), 220, 2, 228);
                std::snprintf(buf, sizeof buf, "+ Buy tab (%d spice)", cost);
                draw_text(ctx, buf, buyx + 12, gridui::vault_tab_y() + 14, 231);
            }
            draw_text(ctx, "STASH", gridui::vault_stash_x(), gridui::vault_stash_y() - 20, 14);
            draw_text(ctx, "PACK", gridui::vault_pack_x(), gridui::vault_pack_y() - 20, 14);
            draw_item_grid(ctx, gridui::vault_stash_x(), gridui::vault_stash_y(), g_meta.stash[g.stash_tab],
                           g_meta.stash_cell[g.stash_tab], tune::kStashCap, tune::kStashW, tune::kStashH,
                           gh.what == UIHit::StashCell ? gh.idx : -1, g.drag_kind == 3 ? g.drag_idx : -1);
            draw_item_grid(ctx, gridui::vault_pack_x(), gridui::vault_pack_y(), g.pl.inv, g.pl.inv_cell,
                           tune::kInvCap, tune::kPackW, tune::kPackH,
                           gh.what == UIHit::PackCell ? gh.idx : -1, g.drag_kind == 1 ? g.drag_idx : -1);
        }
    } else
    // Vendor menu overlay.
    if (g.menu_open) {
        static const char* kMenus[5][6] = {
            {},
            {"Tribute 200 water  (+10 standing)", "Ask about the season",
             "A task for the tribe", "Ride out to the Ninth Rock (Guild tithe)",
             "Contribute to the tribe  (the Sietch tree)", "Leave"},
            {"Offer 5 spice  (+12 standing)", "Healing trance  (6 spice)", "Leave", nullptr},
            {"Demolition charge  (5 spice)", "Two thumpers  (3 spice)",
             "Toxin vial  (2 spice)", "Glowglobe  (3 spice)", "Leave"},
            {"FARSIGHT - unfold the map  (2 spice)", "THREADWALK - two free strides  (3 spice)",
             "The Litany against fear  (free, once a day)", "Close"},
        };
        static const char* kTitles[5] = {"", "THE NAIB", "THE SAYYADINA",
                                         "GREYBEARD THE SMUGGLER", "PRESCIENCE"};
        const int x0 = fb::W / 2 - 300, y0 = 280;
        fill_rect(ctx, x0, y0, 600, 260, 2);
        fill_rect(ctx, x0, y0, 600, 2, 228);
        fill_rect(ctx, x0, y0 + 258, 600, 2, 228);
        draw_text_shadow(ctx, kTitles[g.menu_open], x0 + 24, y0 + 16, 230, 1);
        char res[48];
        std::snprintf(res, sizeof res, "water %d  spice %d", g.pl.water, g.pl.spice);
        draw_text(ctx, res, x0 + 24, y0 + 44, 14);
        for (int i = 0; i < 6 && kMenus[g.menu_open][i]; ++i) {
            const bool sel = i == g.menu_sel;
            if (sel) fill_rect(ctx, x0 + 16, y0 + 80 + i * 32 - 4, 568, 28, 4);
            draw_text(ctx, kMenus[g.menu_open][i], x0 + 24, y0 + 80 + i * 32,
                      uint8_t(sel ? 245 : 20));
        }
    }
    draw_drag_ghost(ctx);  // G4: a dragged item floats at the cursor on the grid screens (no-op otherwise)

    // B2b: the Revelation overlay — Paul's prescient gear suggestion (pick a slot, then 1 of 3).
    if (g.revelation_stage) {
        static const char* kSlotName[int(items::Slot::Count)] = {
            "Weapon", "Hood", "Filter Mask", "Filt-plugs", "Catchtube",
            "Stillsuit Body", "Catchpockets", "Heel-pumps", "Jubba Cloak", "Token"};
        const int x0 = fb::W / 2 - 300, y0 = 280;
        fill_rect(ctx, x0, y0, 600, 240, 2);
        fill_rect(ctx, x0, y0, 600, 2, 228);
        fill_rect(ctx, x0, y0 + 238, 600, 2, 228);
        draw_text_shadow(ctx, "A REVELATION OF MUAD'DIB", x0 + 24, y0 + 16, 230, 1);
        char lead[80];
        std::snprintf(lead, sizeof lead, "The road leads on to %s.",
                      stage_name(g.graph.nodes[g.rev_dest].stage));
        draw_text(ctx, lead, x0 + 300, y0 + 16, 14);
        draw_text(ctx, g.revelation_stage == 1 ? "The vision reveals what you must bear -- choose:"
                                               : "Three shapes the vision might take -- choose one:",
                  x0 + 24, y0 + 44, 14);
        char row[80];
        for (int i = 0; i < 3; ++i) {
            const bool sel = i == g.rev_sel;
            if (sel) fill_rect(ctx, x0 + 16, y0 + 76 + i * 40, 568, 34, 4);
            if (g.revelation_stage == 1) {
                std::snprintf(row, sizeof row, "%s", kSlotName[g.rev_slot_offer[i] % int(items::Slot::Count)]);
            } else {
                char nm[64];
                g.rev_item_offer[i].full_name(nm, sizeof nm);
                std::snprintf(row, sizeof row, "%s", nm);
            }
            const uint8_t col = g.revelation_stage == 2 && g.rev_item_offer[i].rarity == 2 ? 245
                                : g.revelation_stage == 2 && g.rev_item_offer[i].rarity == 1 ? 230 : 20;
            draw_text(ctx, row, x0 + 32, y0 + 80 + i * 40, uint8_t(sel ? 245 : col));
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
        fill_rect(ctx, fb::W / 2 - 480, 400, 960, 200, 2);
        draw_text_shadow(ctx, kTitle[e], fb::W / 2 - int(std::strlen(kTitle[e])) * kCh, 430,
                         uint8_t(e == campaign::kVictory ? 245 : 210), 1);
        draw_text_shadow(ctx, kLine1[e], fb::W / 2 - int(std::strlen(kLine1[e])) * kCh, 480, 24, 1);
        draw_text_shadow(ctx, kLine2[e], fb::W / 2 - int(std::strlen(kLine2[e])) * kCh, 520, 14, 1);
        draw_text_shadow(ctx, "ENTER: another season", fb::W / 2 - 21 * kCh, 570, 230, 1);
    } else if (g.pl.dead) {
        // count what the desert takes
        int lost = 0;
        for (const items::Inst& it : g.pl.inv)
            if (!it.empty()) ++lost;
        for (const items::Inst& it : g.pl.equip)
            if (!it.empty()) ++lost;
        const int box_h = 200 + (lost < 6 ? lost : 6) * 24;
        fill_rect(ctx, fb::W / 2 - 340, 260, 680, box_h, 2);
        draw_text_shadow(ctx, "You are one with the sand now.", fb::W / 2 - 30 * kCh, 280, 210, 1);
        draw_text_shadow(ctx, "Fear is the mind-killer.", fb::W / 2 - 24 * kCh, 320, 24, 1);
        char buf[80];
        std::snprintf(buf, sizeof buf, "the sand keeps %d pieces of gear:", lost);
        draw_text_shadow(ctx, buf, fb::W / 2 - int(std::strlen(buf)) * kCh, 360,
                         uint8_t(lost ? 208 : 14), 1);
        int row = 0;
        for (int pass_ = 0; pass_ < 2 && row < 6; ++pass_) {
            const items::Inst* list = pass_ ? g.pl.equip : g.pl.inv;
            const int count = pass_ ? 4 : 12;
            for (int i = 0; i < count && row < 6; ++i)
                if (!list[i].empty()) {
                    char nm[64];
                    list[i].full_name(nm, sizeof nm);
                    draw_text(ctx, nm, fb::W / 2 - int(std::strlen(nm)) * kCh,
                              400 + row * 24, uint8_t(list[i].rarity == 2 ? 245 : 20));
                    ++row;
                }
        }
        std::snprintf(buf, sizeof buf, "seed %llu   ENTER: rise again",
                      (unsigned long long)g.seed);
        draw_text_shadow(ctx, buf, fb::W / 2 - int(std::strlen(buf)) * kCh,
                         410 + row * 24, 230, 1);
    }
}

}  // namespace

void game_render(Framebuffer& frame) {
    frame.clear(0xff000000u | g.services.pal->color[1]);  // near-black background
    set_draw_scale(1);  // R3: art is authored at native on-screen size, blit 1:1
                        // (iso projection still uses kView; only the blit scale is 1)

    RenderCtx r{DrawCtx{&frame, g.services.cmaps, g.services.pal}, &zone(), is_night(),
                is_night() ? 18 : 31, g.services.assets};

    // Front-end screens (title menu / options / manual) replace the world view.
    if (g.screen != GameState::Screen::Play) { pass_frontend(r); return; }

    // screen shake (visual only; fx stream never touches gameplay)
    const iso::Camera saved_cam = g.cam;
    int shake = 0;
    switch (g.wrm.state) {
        case worm::State::Hunting: shake = 1; break;
        case worm::State::Breaching: shake = 3; break;
        case worm::State::Surfaced: shake = 2; break;
        default: break;
    }
    if (g.microshake > 0) shake = shake > 1 ? shake : 1;
    if (shake > 0) {
        g.cam.x += int(g.rng.fx.below(uint32_t(shake * 2 + 1))) - shake;
        g.cam.y += int(g.rng.fx.below(uint32_t(shake * 2 + 1))) - shake;
    }

    pass_ground(r);
    pass_ground_decals(r);
    pass_footprints(r);  // B7: wandering-vendor trail overlay
    pass_walls(r);      // \
    pass_objects(r);    //  > populate the y-sorted scene list
    pass_decor(r);      //  > sietch furniture + exit door
    pass_entities(r);   // /
    g.draw_list.flush(r.ctx);
    pass_effects(r);
    pass_particles(r);
    pass_lighting(r);
    pass_weather(r);
    pass_cursor(r);  // A4: aim reticle, over the lit world, under the HUD

    g.cam = saved_cam;
    set_draw_scale(1);  // UI draws at native 1080p (world scale doesn't apply)
    pass_ui(r);
}

PaletteFade game_fade() {
    if (g.fade <= 0) return PaletteFade{};
    return PaletteFade{g.fade_r, g.fade_g, g.fade_b, uint16_t(g.fade)};
}

bool game_muted() { return g.opt_muted; }
bool game_consume_fullscreen_request() {
    const bool req = g.fullscreen_req;
    g.fullscreen_req = false;
    return req;
}

void game_serialize(SaveWriter& w) {
    w.u64(g.seed);
    w.u8(g.cur_zone);
    w.i32(g.cur_node);                                  // B1.3: current graph node
    w.u32(g.sim_clock);                                 // RT1: real-time sim clock
    w.i32(g.world_accum);                               // RT1: world-step accumulator
    w.i32(g.graph.count);                               // + per-node flags (graph rebuilds from seed)
    for (int i = 0; i < g.graph.count; ++i) w.u8(g.graph.nodes[i].flags);
    w.i32(g.pl.hp);
    w.i32(g.pl.water);
    w.i32(g.pl.reclaimed);  // C5: the Deathstill stake (run-scoped)
    w.u64(g.pl.passives);   // C4: the passive-tree allocation mask
    w.i32(g.pl.exposure);
    w.i32(g.pl.turn);
    w.bytes(&g.pl.st, sizeof g.pl.st);
    w.bytes(g.pl.inv, sizeof g.pl.inv);
    w.bytes(g.pl.inv_cell, sizeof g.pl.inv_cell);  // G: pack grid layout
    w.bytes(g.pl.cur_tab, sizeof g.pl.cur_tab);    // G2: crafting-table currency slots
    w.bytes(g.pl.equip, sizeof g.pl.equip);
    w.i32(g.pl.thumpers);
    w.i32(g.pl.charges);
    w.i32(g.pl.poisons);
    w.i32(g.pl.spice);
    w.i32(g.pl.glowglobes);
    w.i32(g.pl.atk);
    w.i32(g.pl.shield);        // C2
    w.i32(g.pl.shield_delay);  // C2
    w.i32(g.pl.will);          // D1
    w.i32(g.pl.ability_cd);    // C7: shared ability cooldown
    for (int i = 0; i < tune::kFlaskSlots; ++i) w.i32(g.pl.flask_charges[i]);  // flask belt
    w.i32(g.pl.flask_quality);  // D3: flask quality
    w.i32(g.pl.ail_poison_dmg);    // C3
    w.i32(g.pl.ail_poison_turns);  // C3
    w.i32(g.pl.ail_scorch_dmg);    // C3
    w.i32(g.pl.ail_scorch_turns);  // C3
    w.i32(g.pl.ail_bleed_dmg);     // C3
    w.i32(g.pl.ail_bleed_turns);   // C3
    w.i32(g.pl.ail_blind_turns);   // C3
    w.i32(g.pl.ail_frozen_turns);  // C3
    w.i32(g.pl.ail_stun_turns);    // RT2
    w.u8(g.pl.maker_hooks ? 1 : 0);
    w.u8(g.pl.riding ? 1 : 0);
    w.i32(g.pl.ride_turns);
    w.i32(g.pl.litany_turns);
    w.i32(g.pl.litany_day);
    w.i32(g.pl.free_moves);
    w.u32(g.pl.next_act_tick);  // RT1.2: player action cooldown
    w.i32(g.quest_kind);
    w.i32(g.quest_need);
    w.i32(g.quest_progress);
    w.u8(uint8_t(g.pl.base_class));   // C3
    w.u8(uint8_t(g.pl.ascendancy));   // C3
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
    w.i32(g.sideobj_kind);
    w.i32(g.sideobj_zone);
    w.i32(g.sideobj_x);
    w.i32(g.sideobj_y);
    w.i32(g.sideobj_expires);
    w.u8(g.sideobj_spawned ? 1 : 0);
    w.u8(g.at_rendezvous ? 1 : 0);  // B7: out at the Ninth Rock
    w.u8(g.in_cave ? 1 : 0);        // B9: inside a hidden cavern
    w.u64(g.cave_seed);
    w.i32(g.cave_ret_x);
    w.i32(g.cave_ret_y);
    w.u8(g.cave_ret_zone);
    w.u8(g.in_strike ? 1 : 0);      // B10.2: inside a Jihad strike excursion
    w.u8(g.strike_faction);
    w.u8(g.strike_tier);
    w.u64(g.strike_seed);
    w.u8(g.in_pinnacle ? 1 : 0);    // B10.3: inside a homeworld pinnacle assault
    w.u8(g.pinnacle_faction);
    w.u8(g.in_capstone ? 1 : 0);    // B10.4: inside the Golden Path capstone
    w.u8(g.in_ride ? 1 : 0);        // The Long Ride: greed-gauntlet state (abstract, no owned zone)
    w.i32(g.ride_leg);
    w.i32(g.ride_haul);
    w.i32(g.ride_restless);
    w.i32(g.storm_turns);
    w.i32(g.poison_turns);
    w.i32(g.thopter_x);
    w.i32(g.thopter_y);
    w.i32(g.thopter_dx);
    w.bytes(g.ents, sizeof g.ents);
    w.bytes(g.loot_inst, sizeof g.loot_inst);  // D1: on-map loot pool (parallel to ents)
    w.bytes(&g.rng.world, sizeof g.rng.world);
    w.bytes(&g.rng.combat, sizeof g.rng.combat);
    w.bytes(&g.rng.loot, sizeof g.rng.loot);
    w.bytes(&g.wrm, sizeof g.wrm);
    w.bytes(g.noise.at, sizeof g.noise.at);
    w.i32(g.delta_count);
    w.bytes(g.deltas, sizeof g.deltas);
    w.bytes(g.zone_.vis, sizeof g.zone_.vis);
}

bool game_deserialize(SaveReader& r) {
    const uint64_t seed = r.u64();
    const uint8_t cur = r.u8();
    const int cur_node = r.i32();                       // B1.3: current graph node
    const uint32_t sim_clock = r.u32();                 // RT1: real-time sim clock
    const int world_accum = r.i32();                    // RT1: world-step accumulator
    const int gcount = r.i32();                         // + per-node flags
    static uint8_t node_flags[world::kMaxNodes];
    const int gclamp = (gcount >= 0 && gcount <= world::kMaxNodes) ? gcount : 0;
    for (int i = 0; i < gclamp; ++i) node_flags[i] = r.u8();
    Player pl;
    pl.hp = r.i32();
    pl.water = r.i32();
    pl.reclaimed = r.i32();  // C5: the Deathstill stake (run-scoped)
    pl.passives = r.u64();   // C4: the passive-tree allocation mask
    pl.exposure = r.i32();
    pl.turn = r.i32();
    r.bytes(&pl.st, sizeof pl.st);
    r.bytes(pl.inv, sizeof pl.inv);
    r.bytes(pl.inv_cell, sizeof pl.inv_cell);  // G: pack grid layout
    r.bytes(pl.cur_tab, sizeof pl.cur_tab);    // G2: crafting-table currency slots
    r.bytes(pl.equip, sizeof pl.equip);
    pl.thumpers = r.i32();
    pl.charges = r.i32();
    pl.poisons = r.i32();
    pl.spice = r.i32();
    pl.glowglobes = r.i32();
    pl.atk = r.i32();
    pl.shield = r.i32();        // C2
    pl.shield_delay = r.i32();  // C2
    pl.will = r.i32();          // D1
    pl.ability_cd = r.i32();    // C7: shared ability cooldown
    for (int i = 0; i < tune::kFlaskSlots; ++i) pl.flask_charges[i] = r.i32();  // flask belt
    pl.flask_quality = r.i32();  // D3: flask quality
    pl.ail_poison_dmg = r.i32();    // C3
    pl.ail_poison_turns = r.i32();  // C3
    pl.ail_scorch_dmg = r.i32();    // C3
    pl.ail_scorch_turns = r.i32();  // C3
    pl.ail_bleed_dmg = r.i32();     // C3
    pl.ail_bleed_turns = r.i32();   // C3
    pl.ail_blind_turns = r.i32();   // C3
    pl.ail_frozen_turns = r.i32();  // C3
    pl.ail_stun_turns = r.i32();    // RT2
    pl.maker_hooks = r.u8() != 0;
    pl.riding = r.u8() != 0;
    pl.ride_turns = r.i32();
    pl.litany_turns = r.i32();
    pl.litany_day = r.i32();
    pl.free_moves = r.i32();
    pl.next_act_tick = r.u32();  // RT1.2: player action cooldown
    const int quest_kind = r.i32();
    const int quest_need = r.i32();
    const int quest_progress = r.i32();
    pl.base_class = BaseClass(r.u8());   // C3
    pl.ascendancy = Ascendancy(r.u8());  // C3
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
    const int so_kind = r.i32();
    const int so_zone = r.i32();
    const int so_x = r.i32();
    const int so_y = r.i32();
    const int so_exp = r.i32();
    const bool so_spawned = r.u8() != 0;
    const bool at_rendezvous = r.u8() != 0;  // B7
    const bool in_cave = r.u8() != 0;        // B9
    const uint64_t cave_seed = r.u64();
    const int cave_ret_x = r.i32();
    const int cave_ret_y = r.i32();
    const uint8_t cave_ret_zone = r.u8();
    const bool in_strike = r.u8() != 0;      // B10.2
    const uint8_t strike_faction = r.u8();
    const uint8_t strike_tier = r.u8();
    const uint64_t strike_seed = r.u64();
    const bool in_pinnacle = r.u8() != 0;    // B10.3
    const uint8_t pinnacle_faction = r.u8();
    const bool in_capstone = r.u8() != 0;    // B10.4
    const bool in_ride = r.u8() != 0;        // The Long Ride
    const int ride_leg = r.i32();
    const int ride_haul = r.i32();
    const int ride_restless = r.i32();
    const int storm_turns = r.i32();
    const int poison_turns = r.i32();
    const int thopter_x = r.i32();
    const int thopter_y = r.i32();
    const int thopter_dx = r.i32();
    Entity ents[kMaxEnts];
    r.bytes(ents, sizeof ents);
    static items::Inst loot_inst[kMaxEnts];
    r.bytes(loot_inst, sizeof loot_inst);  // D1: on-map loot pool
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
    static uint8_t vis[map::N];
    r.bytes(vis, sizeof vis);
    if (!r.ok() || cur >= map::kZoneCount || delta_count > kMaxDeltas ||
        cur_node < 0 || cur_node >= world::kMaxNodes) return false;

    g.seed = seed;
    g.graph.build(seed);                               // B1.3: rebuild the run graph from the seed
    for (int i = 0; i < gclamp && i < g.graph.count; ++i) g.graph.nodes[i].flags = node_flags[i];
    g.cur_node = (cur_node < g.graph.count) ? cur_node : 0;
    g.sim_clock = sim_clock;    // RT1
    g.world_accum = world_accum;  // RT1
    g.revelation_stage = 0;  // B2b: transient modal — never mid-pick after a load
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
    g.sideobj_kind = so_kind;
    g.sideobj_zone = uint8_t(so_zone);
    g.sideobj_x = int16_t(so_x);
    g.sideobj_y = int16_t(so_y);
    g.sideobj_expires = so_exp;
    g.sideobj_spawned = so_spawned;
    g.at_rendezvous = at_rendezvous;  // B7
    g.in_cave = in_cave;              // B9
    g.cave_seed = cave_seed;
    g.cave_ret_x = int16_t(cave_ret_x);
    g.cave_ret_y = int16_t(cave_ret_y);
    g.cave_ret_zone = cave_ret_zone;
    g.in_strike = in_strike;          // B10.2
    g.strike_faction = strike_faction;
    g.strike_tier = strike_tier;
    g.strike_seed = strike_seed;
    g.in_pinnacle = in_pinnacle;      // B10.3
    g.pinnacle_faction = pinnacle_faction;
    g.in_capstone = in_capstone;      // B10.4
    g.in_ride = in_ride;              // The Long Ride
    g.ride_leg = ride_leg;
    g.ride_haul = ride_haul;
    g.ride_restless = ride_restless;
    g.storm_turns = storm_turns;
    g.poison_turns = poison_turns;
    g.thopter_x = int16_t(thopter_x);
    g.thopter_y = int16_t(thopter_y);
    g.thopter_dx = int8_t(thopter_dx);
    std::memcpy(g.ents, ents, sizeof g.ents);
    std::memcpy(g.loot_inst, loot_inst, sizeof g.loot_inst);
    g.rng.world = world;
    g.rng.combat = combat;
    g.rng.loot = loot;
    g.wrm = wrm;
    std::memcpy(g.noise.at, noise_buf, sizeof noise_buf);
    g.delta_count = delta_count;
    std::memcpy(g.deltas, deltas, sizeof g.deltas);
    int sx_, sy_;
    if (in_strike) {  // B10.2: a strike excursion regenerates its raid zone from the strike seed
        map::generate(g.zone_, strike_seed, map::ZoneId(map::kZoneDeepPan), sx_, sy_);
    } else if (in_pinnacle) {  // B10.3: a pinnacle arena regenerates from its fixed per-faction seed
        map::generate(g.zone_, kPinnacleSeed[pinnacle_faction < FAC_COUNT ? pinnacle_faction : 0],
                      map::ZoneId(map::kZoneDeepPan), sx_, sy_);
    } else if (in_capstone) {  // B10.4: the Golden Path arena regenerates from its fixed seed
        map::generate(g.zone_, kCapstoneSeed, map::ZoneId(map::kZoneDeepPan), sx_, sy_);
    } else if (in_cave) {  // B9: a hidden cavern is a transient sub-zone regenerated from its own seed
        map::generate(g.zone_, cave_seed, map::ZoneId(map::kZoneCave), sx_, sy_);
    } else if (at_rendezvous) {  // B7: the Ninth Rock is a fixed-seed desert zone, not a graph node
        map::generate(g.zone_, kNinthRockSeed, map::ZoneId(map::kZoneDesert), sx_, sy_);
    } else {
        const world::Node& cn = g.graph.nodes[g.cur_node];  // regen the resident zone from its node
        map::generate(g.zone_, cn.seed, map::ZoneId(cur), sx_, sy_);
    }
    apply_deltas(g.zone_, cur);
    std::memcpy(g.zone_.vis, vis, sizeof vis);
    if (g.op.live() && g.op.zone == g.cur_zone)
        for (int dy = -1; dy <= 0; ++dy)
            for (int dx = -1; dx <= 0; ++dx)
                g.zone_.flags[(g.op.y + dy) * map::W + g.op.x + dx] |= map::TF_BLOCK;
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
                           int16_t(p.x + dx), int16_t(p.y + dy), int16_t(foe_hp(EType::ShieldTrooper)), 0, 0};
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

void game_debug_player_pos(int* x, int* y) {
    if (x) *x = g.ents[0].x;
    if (y) *y = g.ents[0].y;
}

void game_debug_click(int tx, int ty) {  // A4: set a click-to-move goal (headless)
    g.move_goal_x = int16_t(tx);
    g.move_goal_y = int16_t(ty);
}

void game_debug_use_door() { use_door(); }  // headless: leave the sietch (doors are click-only)

// B6: Guild tithe hooks (guildtest).
int game_debug_guild_debt() { return g_meta.guild_debt; }
int game_debug_guild_difficulty() { return guild_difficulty(); }
void game_debug_add_guild_debt(int n) {  // simulate n unpaid runs, honoring the active cap
    for (int i = 0; i < n; ++i) guild_tithe_accrue();
    meta_save();
}
void game_debug_pay_guild(int reduce) { guild_tithe_pay(reduce); }  // pay the tithe down
void game_debug_break_guild() { g_meta.guild_broken = 1; meta_save(); }  // Guild Navigator falls (B10)
int game_debug_foe_hp(int etype) { return foe_hp(EType(etype)); }  // scaled HP at the current depth+tithe

// B7: wandering-vendor / footprint-trail hooks (vendortest).
void game_debug_spawn_sideobj(int kind, int x, int y) {  // place + materialize a vendor near (x,y)
    g.sideobj_kind = kind;
    g.sideobj_zone = g.cur_zone;
    g.sideobj_expires = g.pl.turn + 220;
    g.sideobj_spawned = false;
    const EType et = kind == 1 ? EType::Caravan : EType::Envoy;
    for (int rr = 0; rr <= 12 && !g.sideobj_spawned; ++rr)
        for (int dy = -rr; dy <= rr && !g.sideobj_spawned; ++dy)
            for (int dx = -rr; dx <= rr && !g.sideobj_spawned; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx < 1 || nx >= map::W - 1 || ny < 1 || ny >= map::H - 1) continue;
                if (!zone().walkable(nx, ny) || entity_at(nx, ny, g.cur_zone)) continue;
                for (int i = 1; i < kMaxEnts; ++i)
                    if (g.ents[i].type == EType::None) {
                        g.ents[i] = Entity{et, Ai::Idle, uint8_t(g.cur_zone),
                                           int16_t(nx), int16_t(ny), 10, 0, 0};
                        g.sideobj_x = int16_t(nx); g.sideobj_y = int16_t(ny);
                        g.sideobj_spawned = true;
                        break;
                    }
            }
    refresh_fov_and_paths();  // lays the footprint trail
}
int game_debug_footprint_count() { return g.footprint_count; }
int game_debug_sideobj_active() { return g.sideobj_kind; }
int game_debug_footprint_ok() {  // trail connects vendor <-> player (first tile by the vendor, last by the player)
    if (g.footprint_count <= 0) return 0;
    auto adj = [](int ax, int ay, int bx, int by) {
        const int dx = ax > bx ? ax - bx : bx - ax, dy = ay > by ? ay - by : by - ay;
        return dx <= 1 && dy <= 1;
    };
    const Entity& p = player_ent();
    const int last = g.footprint_count - 1;
    return adj(g.footprints_x[0], g.footprints_y[0], g.sideobj_x, g.sideobj_y) &&
           adj(g.footprints_x[last], g.footprints_y[last], p.x, p.y) ? 1 : 0;
}
void game_debug_expire_sideobj() {  // simulate the expiry despawn
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::Caravan || g.ents[i].type == EType::Envoy)
            g.ents[i].type = EType::None;
    g.sideobj_kind = 0; g.sideobj_spawned = false;
    refresh_fov_and_paths();
}
void game_debug_add_spice(int n) { g.pl.spice += n; }        // test helper
int game_debug_signal_guild() { return signal_guild() ? 1 : 0; }  // B7: (direct) tithe payment
void game_debug_enter_rendezvous() { enter_rendezvous(); }   // B7: ride out to the Ninth Rock
void game_debug_return_rendezvous() { return_from_rendezvous(); }
int game_debug_at_rendezvous() { return g.at_rendezvous ? 1 : 0; }

void game_debug_spawn_patrol(int x, int y) { place_patrol(x, y); }  // B8: place one patroller
void game_debug_world_tick(int n) { for (int i = 0; i < n && !g.pl.dead; ++i) world_tick(); }  // B8
int game_debug_patrol_info(int* x, int* y, int* ai, int* ax, int* ay) {  // B8: first patroller
    int count = 0;
    const Entity* first = nullptr;
    for (int i = 1; i < kMaxEnts; ++i) {
        const Entity& e = g.ents[i];
        if (e.hp <= 0 || e.zone != g.cur_zone) continue;
        if (e.type != EType::Trooper && e.type != EType::ShieldTrooper) continue;
        if (e.ai != Ai::Patrol && !(e.ai == Ai::Hunt && e.data != 0)) continue;  // patroller in either state
        if (!count) first = &e;
        ++count;
    }
    if (first) {
        if (x) *x = first->x;
        if (y) *y = first->y;
        if (ai) *ai = int(first->ai);
        if (ax) *ax = first->data >> 8;
        if (ay) *ay = first->data & 0xFF;
    }
    return count;
}

int game_debug_cur_zone() { return g.cur_zone; }  // B9
void game_debug_enter_cave() { enter_cave(); }     // B9: duck into a hidden cavern
void game_debug_exit_cave() { exit_cave(); }       // B9: climb back out to the field
int game_debug_in_cave() { return g.in_cave ? 1 : 0; }
void game_debug_spawn_cave_mouth(int x, int y) {   // B9: place a cavern mouth in the current zone
    if (Entity* e = alloc_entity())
        *e = Entity{EType::CaveMouth, Ai::Idle, g.cur_zone, int16_t(x), int16_t(y), 1, 0, 0};
}
int game_debug_cave_reward_count() {  // B9: loot + spice piles present in the current zone
    int n = 0;
    for (int i = 1; i < kMaxEnts; ++i) {
        const Entity& e = g.ents[i];
        if (e.zone == g.cur_zone && (e.type == EType::Loot || e.type == EType::SpiceCache)) ++n;
    }
    return n;
}

int game_debug_trials() { return g_meta.trials_done & TRIAL_ALL; }  // Trials: bitmask passed
int game_debug_can_ascend() { return can_ascend() ? 1 : 0; }        // Trials: is the Awakening open?
void game_debug_trigger_trial(int which) {  // Trials: pass one ordeal via the real (guarded) path
    mark_trial(which == 0 ? TRIAL_SANDWALK : which == 1 ? TRIAL_STORM : TRIAL_WORM);
}
void game_debug_complete_trials() { g_meta.trials_done = TRIAL_ALL; }  // Trials: open the gate outright
int game_debug_spice_essence() { return g_meta.spice_essence; }        // D3e: banked Awakening resource
void game_debug_grant_spice_essence(int n) { g_meta.spice_essence = uint8_t(n); meta_save(); }

// --- Endgame (Jihad) hooks ---
int game_debug_endgame_open() { return endgame_open() ? 1 : 0; }
void game_debug_open_chosen_path() { g_meta.chosen_path = 1; meta_save(); }  // simulate the Baron's fall
int game_debug_jihad_pressure(int f) {
    return (f >= 0 && f < FAC_COUNT) ? g_meta.jihad_pressure[f] : -1;
}
int game_debug_commission_strike(int f, int tier) { return commission_strike(f, tier) ? 1 : 0; }
int game_debug_pinnacle_available(int f) { return pinnacle_available(f) ? 1 : 0; }
void game_debug_clear_pinnacle(int f) { clear_pinnacle(f); }
int game_debug_golden_shards() { return g_meta.golden_shards; }
int game_debug_capstone_available() { return capstone_available() ? 1 : 0; }
void game_debug_finish_capstone() { finish_capstone(); }
int game_debug_guild_broken_flag() { return g_meta.guild_broken; }
void game_debug_press_faction(int f, int tier) { press_faction(f, tier); }  // data-core press (no raid)
int game_debug_in_strike() { return g.in_strike ? 1 : 0; }
int game_debug_strike_boss_count() {  // objectives present in the current strike zone
    int n = 0;
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].hp > 0 && g.ents[i].zone == g.cur_zone && g.ents[i].type == EType::Boss) ++n;
    return n;
}
void game_debug_resolve_strike() { resolve_strike(); }   // simulate clearing the objective
void game_debug_abandon_strike() { abandon_strike(); }
int game_debug_assault_pinnacle(int f) { return assault_pinnacle(f) ? 1 : 0; }  // enter if available
int game_debug_in_pinnacle() { return g.in_pinnacle ? 1 : 0; }
int game_debug_pinnacle_boss_count() {
    int n = 0;
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].hp > 0 && g.ents[i].zone == g.cur_zone && g.ents[i].type == EType::Boss) ++n;
    return n;
}
void game_debug_resolve_pinnacle() { resolve_pinnacle(); }   // simulate slaying the homeworld boss
void game_debug_abandon_pinnacle() { abandon_pinnacle(); }
int game_debug_confront_capstone() { return confront_capstone() ? 1 : 0; }  // enter if all shards gathered
int game_debug_in_capstone() { return g.in_capstone ? 1 : 0; }
void game_debug_resolve_capstone() { resolve_capstone(); }   // simulate defeating the alternate Paul
int game_debug_capstone_done() { return g_meta.capstone_done; }
// --- The Long Ride hooks ---
int game_debug_ride_begin() { return ride_begin() ? 1 : 0; }
int game_debug_ride_push() { return ride_push(); }        // 0 = rode on, 1 = bucked
void game_debug_ride_bank() { ride_bank(); }
int game_debug_in_ride() { return g.in_ride ? 1 : 0; }
int game_debug_ride_leg() { return g.ride_leg; }
int game_debug_ride_haul() { return g.ride_haul; }
int game_debug_maker_hides() { return g_meta.maker_hides; }
int game_debug_best_ride_leg() { return g_meta.best_ride_leg; }
// --- Landsraad / CHOAM hooks ---
int game_debug_choam_control() { return choam_control(); }
int game_debug_house_stance(int f) { return house_stance(f); }   // 0 neutral, 1 ally, 2 opponent
int game_debug_court_house(int f) { return court_house(f) ? 1 : 0; }
int game_debug_collect_dividend() { return collect_dividend(); }
int game_debug_opposing_houses() { return opposing_houses(); }
// --- D3 currency / crafting hooks ---
void game_debug_grant_currency(int kind, int n) {  // add n to the crafting-table slots (the bench store)
    if (kind <= 0 || kind >= items::CUR_COUNT || n <= 0) return;
    g.pl.cur_tab[kind] = uint16_t(g.pl.cur_tab[kind] + n);
}
void game_debug_grant_currency_grid(int kind, int n) {  // G2: add carried currency into the pack grid
    if (kind <= 0 || kind >= items::CUR_COUNT || n <= 0) return;
    grant_currency_to_pack(uint8_t(kind), n);
}
int game_debug_currency_count(int kind) { return currency_count(uint8_t(kind)); }          // table slots
int game_debug_currency_count_grid(int kind) { return currency_count_grid(uint8_t(kind)); }  // carried
int game_debug_tab_deposit(int idx) { return tab_deposit(idx) ? 1 : 0; }                   // grid stack -> table
int game_debug_tab_withdraw(int kind, int amount) { return tab_withdraw(uint8_t(kind), amount) ? 1 : 0; }
int game_debug_quickmove(int idx) { return quickmove(idx) ? 1 : 0; }  // Ctrl-click: currency->table, gear->equip
void game_debug_setup_gear(int idx, int rarity, int ilvl) {  // place a known ARMOUR item in inv[idx]
    if (idx < 0 || idx >= tune::kInvCap) return;
    items::Inst it;
    for (int i = 0; i < items::kDefCount; ++i)
        if (items::kDefs[i].slot != items::Slot::Weapon) { it.def = uint8_t(i); break; }
    it.ilvl = uint8_t(ilvl < 1 ? 1 : ilvl > 255 ? 255 : ilvl);
    it.rarity = uint8_t(rarity);
    it.nprefix = it.nsuffix = 0;
    if (rarity == items::R_MAGIC) { roll_one_side(it, ilvl, true, 1); roll_one_side(it, ilvl, false, 1); }
    else if (rarity == items::R_RARE) { roll_one_side(it, ilvl, true, 2); roll_one_side(it, ilvl, false, 1); }
    pack_place_at(idx, it);
}
int game_debug_bench(int cur, int idx) { return bench_apply(cur, idx) ? 1 : 0; }
int game_debug_item_sig(int idx, int* rarity, int* naffix, int* frac) {  // inspect inv[idx]; returns slot
    if (idx < 0 || idx >= tune::kInvCap) return -3;
    const items::Inst& it = g.pl.inv[idx];
    if (it.is_currency()) return -2;
    if (it.empty()) return -1;
    if (rarity) *rarity = it.rarity;
    if (naffix) *naffix = it.nprefix + it.nsuffix;
    if (frac) *frac = it.fractured;
    return int(it.d().slot);
}
int game_debug_item_valuesum(int idx) {  // sum of affix values (detects a Divine reroll)
    if (idx < 0 || idx >= tune::kInvCap) return 0;
    const items::Inst& it = g.pl.inv[idx];
    if (it.empty() || it.is_currency()) return 0;
    int v = 0;
    for (int i = 0; i < it.nprefix; ++i) v += it.pre[i].value;
    for (int i = 0; i < it.nsuffix; ++i) v += it.suf[i].value;
    return v;
}
// --- D3b hooks ---
void game_debug_set_unidentified(int idx, int on) {  // set/clear the unID bit on inv[idx]
    if (idx < 0 || idx >= tune::kInvCap || g.pl.inv[idx].empty()) return;
    if (on) g.pl.inv[idx].istate |= 1; else g.pl.inv[idx].istate &= uint8_t(~1);
}
int game_debug_item_state(int idx) {  // istate bits (1 unID, 2 corrupt, 4 mirror), -1 empty/currency
    if (idx < 0 || idx >= tune::kInvCap || g.pl.inv[idx].empty() || g.pl.inv[idx].is_currency()) return -1;
    return g.pl.inv[idx].istate;
}
int game_debug_item_quality(int idx) {  // item quality %; -1 for empty/currency
    if (idx < 0 || idx >= tune::kInvCap || g.pl.inv[idx].empty() || g.pl.inv[idx].is_currency()) return -1;
    return g.pl.inv[idx].quality;
}
int game_debug_item_implicit(int idx) {  // implicit % (enchant); 0 = default
    if (idx < 0 || idx >= tune::kInvCap || g.pl.inv[idx].empty() || g.pl.inv[idx].is_currency()) return -1;
    return g.pl.inv[idx].enchant;
}
void game_debug_setup_weapon(int idx, int ilvl) {  // place a common weapon in inv[idx]
    if (idx < 0 || idx >= tune::kInvCap) return;
    items::Inst it;
    for (int i = 0; i < items::kDefCount; ++i)
        if (items::kDefs[i].slot == items::Slot::Weapon) { it.def = uint8_t(i); break; }
    it.ilvl = uint8_t(ilvl < 1 ? 1 : ilvl > 255 ? 255 : ilvl);
    it.rarity = items::R_COMMON;
    pack_place_at(idx, it);
}
int game_debug_flask_quality() { return g.pl.flask_quality; }
int game_debug_inv_used() {  // count of non-empty pack cells (gear + currency)
    int n = 0; for (const items::Inst& s : g.pl.inv) if (!s.empty()) ++n; return n;
}
// --- G grid hooks ---
void game_debug_clear_pack() {  // empty the whole pack pool
    for (int i = 0; i < tune::kInvCap; ++i) { g.pl.inv[i] = items::Inst{}; g.pl.inv_cell[i] = 0; }
}
int game_debug_pack_free_cells() { return pack_free_cells(); }  // free grid cells (of kPackW*kPackH)
int game_debug_grid_cell_of(int idx) {  // top-left grid cell of pool entry idx (-1 if empty)
    if (idx < 0 || idx >= tune::kInvCap || g.pl.inv[idx].empty()) return -1;
    return g.pl.inv_cell[idx];
}
int game_debug_grid_size_of(int idx, int* w, int* h) {  // footprint of pool entry idx (cells; -1 if empty)
    if (idx < 0 || idx >= tune::kInvCap || g.pl.inv[idx].empty()) return -1;
    const items::GridSize s = g.pl.inv[idx].size();
    if (w) *w = s.w; if (h) *h = s.h;
    return s.w * s.h;
}
int game_debug_grid_item_at(int cell) {  // pool index whose footprint covers grid cell (-1 = empty)
    return grid_item_at(g.pl.inv, g.pl.inv_cell, tune::kInvCap, tune::kPackW, cell);
}
// Build an item of slot `slot` and place it via first-fit; returns its assigned cell (-1 if no room).
int game_debug_pack_add_slot(int slot) {
    items::Inst it;
    for (int i = 0; i < items::kDefCount; ++i)
        if (int(items::kDefs[i].slot) == slot) { it.def = uint8_t(i); break; }
    if (it.empty()) return -1;
    const int idx = pack_add(it);
    return idx < 0 ? -1 : g.pl.inv_cell[idx];
}
// Try to move pool entry idx so its top-left sits at `cell` (collision-checked). Returns 1 on success.
int game_debug_grid_move(int idx, int cell) {
    if (idx < 0 || idx >= tune::kInvCap || g.pl.inv[idx].empty()) return 0;
    if (!grid_can_place(g.pl.inv, g.pl.inv_cell, tune::kInvCap, tune::kPackW, tune::kPackH,
                        idx, cell, g.pl.inv[idx].size())) return 0;
    g.pl.inv_cell[idx] = uint8_t(cell);
    return 1;
}
// --- D3c drop hooks ---
int game_debug_force_currency_drop() {  // drop a currency at the player, return its kind, then clear it
    const Entity& p = player_ent();
    drop_currency(p.x, p.y);
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::Currency) { int k = g.ents[i].data; g.ents[i] = Entity{}; return k; }
    return 0;
}
void game_debug_spawn_currency(int kind) {  // D3d: place a currency entity on the player's tile
    const Entity& p = player_ent();
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::None) {
            g.ents[i] = Entity{EType::Currency, Ai::Idle, g.cur_zone, p.x, p.y, 1, 0, int16_t(kind), 1};
            return;
        }
}
int game_debug_kill_drops(int n) {  // n Trooper loot rolls at the player; return currency drops (auto-cleared)
    const Entity& p = player_ent();
    int c = 0;
    for (int i = 0; i < n; ++i) {
        maybe_drop_loot(EType::Trooper, p.x, p.y);
        for (int j = 1; j < kMaxEnts; ++j) {
            if (g.ents[j].type == EType::Currency) { ++c; g.ents[j] = Entity{}; }
            else if (g.ents[j].type == EType::Loot) g.ents[j] = Entity{};
        }
    }
    return c;
}
// --- C7 ability hooks ---
int game_debug_cast_ability(int ab, int tx, int ty) { return cast_ability(ab, tx, ty) ? 1 : 0; }
int game_debug_will() { return g.pl.will; }
void game_debug_set_will(int w) { g.pl.will = w; }
int game_debug_ability_cd() { return g.pl.ability_cd; }
// --- hub-menu hooks ---
void game_debug_open_menu(int m, int sel) { g.menu_open = m; g.menu_sel = sel; }
int game_debug_menu_open() { return g.menu_open; }
// --- flask hooks ---
int game_debug_use_flask(int slot) { return use_flask(slot) ? 1 : 0; }
int game_debug_flask_charges(int slot) {
    return (slot >= 0 && slot < tune::kFlaskSlots) ? g.pl.flask_charges[slot] : -1;
}
void game_debug_set_flask_charges(int slot, int c) {
    if (slot >= 0 && slot < tune::kFlaskSlots) g.pl.flask_charges[slot] = c;
}
void game_debug_flask_gain(int n) { flask_gain(n); }
// --- C5 Litrejon economy hooks ---
int game_debug_litrejons() { return g_meta.litrejons; }        // banked tribal water (the sietch currency)
int game_debug_reclaimed() { return g.pl.reclaimed; }          // this run's un-banked Deathstill stake
void game_debug_grant_litrejons(int n) { g_meta.litrejons += n; meta_save(); }
// --- C5b sietch-tree hooks ---
int game_debug_sietch_node_count() { return kSietchNodeCount; }
int game_debug_sietch_rank(int idx) { return (idx >= 0 && idx < kSietchNodeCount) ? g_meta.sietch_rank[idx] : -1; }
int game_debug_sietch_cost(int idx) { return sietch_node_cost(idx); }
int game_debug_buy_sietch_node(int idx) { return buy_sietch_node(idx) ? 1 : 0; }
int game_debug_sietch_bonus(int kind) { return sietch_affix_bonus(uint8_t(kind)); }
int game_debug_max_hp() { return player_max_hp(); }   // for verifying a tree node moves a derived stat
// --- C4 passive-tree hooks ---
int game_debug_passive_node_count() { return kPassiveNodeCount; }
int game_debug_passive_has(int n) { return passive_has(n) ? 1 : 0; }
int game_debug_passive_allocatable(int n) { return passive_allocatable(n) ? 1 : 0; }
int game_debug_passive_points() { return passive_points_available(); }
int game_debug_passive_alloc(int n) { return passive_alloc(n) ? 1 : 0; }
int game_debug_passive_bonus(int kind) { return passive_affix_bonus(uint8_t(kind)); }
int game_debug_meta_passive_has(int n) { return (n >= 0 && n < 64 && (g_meta.passives & (1ull << n))) ? 1 : 0; }
void game_debug_set_level(int lvl) { g.pl.st.level = int16_t(lvl < 1 ? 1 : lvl > 30 ? 30 : lvl); }
int game_debug_deathstill(int water) {  // place a corpse worth `water` on the player + harvest it
    const Entity& p = player_ent();
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::None) {
            g.ents[i] = Entity{EType::Corpse, Ai::Idle, g.cur_zone, p.x, p.y, 1, 0, int16_t(water), 0};
            break;
        }
    const int before = g.pl.reclaimed;
    try_harvest();
    return g.pl.reclaimed - before;
}
// --- C6 maula hooks ---
int game_debug_spawn_hostile(int dx, int dy) {  // Trooper at player+(dx,dy); clears the line; returns ents idx
    const Entity p = g.ents[0];
    const int tx = p.x + dx, ty = p.y + dy;
    if (tx < 0 || tx >= map::W || ty < 0 || ty >= map::H) return -1;
    int x0 = p.x, y0 = p.y;  // clear TF_BLOCK along the line so the shot has a clean path in tests
    const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy, sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = adx - ady;
    for (int guard = 0; guard < 128; ++guard) {
        g.zone_.flags[y0 * map::W + x0] &= uint8_t(~map::TF_BLOCK);
        if (x0 == tx && y0 == ty) break;
        const int e2 = 2 * err;
        if (e2 > -ady) { err -= ady; x0 += sx; }
        if (e2 < adx)  { err += adx; y0 += sy; }
    }
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::None) {
            g.ents[i] = Entity{EType::Trooper, Ai::Idle, g.cur_zone, int16_t(tx), int16_t(ty),
                               int16_t(foe_hp(EType::Trooper)), 0, 0};
            return i;
        }
    return -1;
}
int game_debug_entity_hp(int idx) {
    return (idx >= 0 && idx < kMaxEnts && g.ents[idx].type != EType::None) ? g.ents[idx].hp : -1;
}
void game_debug_block_tile(int dx, int dy) {  // raise a wall at player+(dx,dy) (blocks the maula line)
    const Entity p = g.ents[0];
    const int tx = p.x + dx, ty = p.y + dy;
    if (tx >= 0 && tx < map::W && ty >= 0 && ty < map::H) g.zone_.flags[ty * map::W + tx] |= map::TF_BLOCK;
}
void game_debug_fire_maula(int dx, int dy) {  // fire the maula toward player+(dx,dy)
    const Entity& p = player_ent();
    fire_maula(p.x + dx, p.y + dy);
}
void game_debug_fire_maula_at(int idx) {  // fire at entity idx's live tile (re-aims as it moves)
    if (idx > 0 && idx < kMaxEnts && g.ents[idx].type != EType::None)
        fire_maula(g.ents[idx].x, g.ents[idx].y);
}

void game_debug_revelation(int* stage, int* inv_count) {
    if (stage) *stage = g.revelation_stage;
    if (inv_count) {
        int n = 0;
        for (const items::Inst& it : g.pl.inv) if (!it.empty()) ++n;
        *inv_count = n;
    }
}
void game_debug_revelation_pick(int idx) {  // headless: choose row idx + confirm the Revelation
    if (!g.revelation_stage) return;
    g.rev_sel = (idx % 3 + 3) % 3;
    revelation_confirm();
}
int game_debug_cur_node() { return g.cur_node; }
void game_debug_extract() { extract_run(); }  // Step 1a: fade-the-vision extraction (no worm)
void game_debug_warp_to_node(int idx) { enter_node(idx); }  // B3: jump straight to a graph node
void game_debug_awaken(int* unlocked, int* cleared, int* banked_spice) {  // B5: meta ladder + wallet
    if (unlocked) *unlocked = g_meta.awaken_unlocked;
    if (cleared) *cleared = g_meta.awaken_cleared;
    if (banked_spice) *banked_spice = g_meta.banked_spice;
}
int game_debug_char_level() { return g.pl.st.level; }

// G2: read-only UI query surface for native Godot HUD/inventory (determinism-safe).
void game_debug_player_stats(int* hp, int* max_hp, int* water, int* max_water, int* level, int* spice) {
    if (hp) *hp = g.pl.hp;
    if (max_hp) *max_hp = player_max_hp();
    if (water) *water = g.pl.water;
    if (max_water) *max_water = player_max_water();
    if (level) *level = g.pl.st.level;
    if (spice) *spice = g.pl.spice;
}
void game_debug_hurt(int dmg) { damage_player(dmg, "Struck down in the harness."); }
void game_debug_poison_player(int dmg, int turns) { poison_player(dmg, turns); }  // C3
void game_debug_scorch_player(int dmg, int turns) { scorch_player(dmg, turns); }  // C3
void game_debug_bleed_player(int dmg, int turns) { bleed_player(dmg, turns); }    // C3
void game_debug_blind_player(int turns) { blind_player(turns); }                  // C3
// C3: the player's clamped to-hit vs a foe of ac_dodge `ac` — deterministic (no roll), for blindtest.
int game_debug_player_tohit(int ac) {
    int acc = with_inc(55 + eff_vers() * 2 + affix_total(items::AK_TOHIT), items::AK_INC_TOHIT);
    if (g.pl.ail_blind_turns > 0) acc -= tune::kBlindAcc;
    const int t = acc - ac;
    return t < 15 ? 15 : t > 95 ? 95 : t;
}
void game_debug_freeze_first_enemy(int turns) {  // C3: freeze the first ShieldTrooper in the zone
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::ShieldTrooper && g.ents[i].zone == g.cur_zone) {
            freeze_entity(g.ents[i], turns);
            return;
        }
}
int game_debug_enemy_frozen_turns() {  // C3: frozen turns on the first ShieldTrooper
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::ShieldTrooper && g.ents[i].zone == g.cur_zone)
            return g.ents[i].frozen_turns;
    return -1;
}
int game_debug_enemy_poison_turns() {  // C3: poison turns on the first ShieldTrooper in the zone
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::ShieldTrooper && g.ents[i].zone == g.cur_zone)
            return g.ents[i].poison_turns;
    return -1;
}
int game_debug_enemy_stun_turns() {  // RT2: stun turns on the first ShieldTrooper in the zone
    for (int i = 1; i < kMaxEnts; ++i)
        if (g.ents[i].type == EType::ShieldTrooper && g.ents[i].zone == g.cur_zone)
            return g.ents[i].stun_turns;
    return -1;
}
// RT2 (deterministic, no RNG): the Freeze/Stun duration formulas, for asserting the seconds-based
// scaling + clamps directly. secs = ticks * kWorldStepTicks / kTickHz.
int game_debug_freeze_ticks(int cold_dmg, int max_life) { return freeze_ticks_for(cold_dmg, max_life); }
int game_debug_stun_ticks(int hit_dmg, int max_life, int mult, int reduce) {
    return stun_duration_for(hit_dmg, max_life, mult, reduce);
}
int game_debug_stun_chance(int hit_dmg, int max_life, int mult, int reduce) {
    return stun_chance_for(hit_dmg, max_life, mult, reduce);
}
void game_debug_stun_player(int ticks) { stun_player(ticks); }  // RT2: force a player Stun
int game_debug_player_stun_turns() { return g.pl.ail_stun_turns; }
int game_debug_player_ready() { return player_ready() ? 1 : 0; }  // RT2: is the player able to act?
// C3: base class / ascendancy hooks (classtest).
void game_debug_apply_base(int bc) { apply_base_class(g.pl, BaseClass(bc)); passive_ensure_start(); }
void game_debug_class(int* base, int* asc) {
    if (base) *base = int(g.pl.base_class);
    if (asc) *asc = int(g.pl.ascendancy);
}
void game_debug_unlock_awaken(int lvl) { g_meta.awaken_unlocked = uint8_t(lvl); }  // gate the rite
int game_debug_ascend(int asc) { return ascend(Ascendancy(asc)) ? 1 : 0; }
// C3 enemy-element (defensive half) hooks.
int game_debug_enemy_element_chance(int depth) { return enemy_element_chance(depth); }
void game_debug_inflict_element(int elem, int mag) { inflict_element_on_player(elem, mag); }
int game_debug_player_frozen_turns() { return g.pl.ail_frozen_turns; }
void game_debug_meta_reload_class(int* base, int* asc) {  // prove the DME5 write->read round-trip
    g_meta = Meta{};
    meta_load();
    if (base) *base = g_meta.base_class;
    if (asc) *asc = g_meta.ascendancy;
}
void game_debug_shield_state(int* shield, int* max_shield, int* hp) {
    if (shield) *shield = g.pl.shield;
    if (max_shield) *max_shield = player_max_shield();
    if (hp) *hp = g.pl.hp;
}
// D1 harness: clear equipment, then equip a synthetic Dust Knife carrying one affix {kind,value}
// (a Weapon has no implicit, so affix_total isolates the mod). Lets statmodtest probe each stat.
void game_debug_equip_affix(int kind, int value) {
    for (auto& e : g.pl.equip) e = items::Inst{};
    items::Inst it;
    it.def = 0;  // Dust Knife (Weapon, no implicit affix)
    it.rarity = items::R_MAGIC;
    it.pre[0] = {uint8_t(kind), 1, int16_t(value)};
    it.nprefix = 1;
    g.pl.equip[int(items::Slot::Weapon)] = it;
    g.pl.hp = player_max_hp();
    g.pl.shield = player_max_shield();
    g.pl.will = player_max_will();
}
void game_debug_derived(int* max_hp, int* leth, int* max_shield, int* max_will,
                        int* armor, int* dodge, int* crit_mult) {
    if (max_hp) *max_hp = player_max_hp();
    if (leth) *leth = player_lethality();
    if (max_shield) *max_shield = player_max_shield();
    if (max_will) *max_will = player_max_will();
    if (armor) *armor = player_armor();
    if (dodge) *dodge = player_dodge();
    if (crit_mult) *crit_mult = 200 + affix_total(items::AK_CRIT_MULT);
}
// D1 harness: write a legacy DME3 meta.dat, load it, and verify the DME3->DME4 conversion kept the
// banked items (with ilvl + affixes) and the wallet/ladder. Returns 1 on success.
int game_debug_meta_migrate_selftest() {
    {
        FILE* f = std::fopen("meta.dat", "wb");
        if (!f) return 0;
        std::fwrite(&kMetaV3, 4, 1, f);
        uint32_t runs = 3; std::fwrite(&runs, 4, 1, f);
        uint8_t endings = 1, sc = 2; std::fwrite(&endings, 1, 1, f); std::fwrite(&sc, 1, 1, f);
        LegacyInst ls[24]; std::memset(ls, 0xFF, sizeof ls);
        ls[0] = {17, 0, 0, 2};       // Patched Suit-body, legacy prefix#0 + suffix#0, rare
        ls[1] = {4, 0xFF, 0xFF, 0};  // Crysknife, common
        std::fwrite(ls, sizeof ls, 1, f);
        uint8_t au = 5, ac = 4, hc = 1; std::fwrite(&au, 1, 1, f); std::fwrite(&ac, 1, 1, f); std::fwrite(&hc, 1, 1, f);
        int32_t bs = 1234; std::fwrite(&bs, 4, 1, f);
        CharStats st{}; std::fwrite(&st, sizeof st, 1, f);
        LegacyInst le[int(items::Slot::Count)]; std::memset(le, 0xFF, sizeof le);
        LegacyInst li[12]; std::memset(li, 0xFF, sizeof li);
        std::fwrite(le, sizeof le, 1, f); std::fwrite(li, sizeof li, 1, f);
        std::fclose(f);
    }
    g_meta = Meta{};
    meta_load();
    return (stash_used(0) == 2 && g_meta.stash_tabs == 1 && g_meta.banked_spice == 1234 &&
            g_meta.awaken_unlocked == 5 && !g_meta.stash[0][0].empty() && g_meta.stash[0][0].ilvl >= 1 &&
            (g_meta.stash[0][0].nprefix + g_meta.stash[0][0].nsuffix) >= 2 &&
            g_meta.stash[0][1].rarity == items::R_COMMON)
               ? 1 : 0;
}
// D1 harness: roll an item of `slot` at a forced item level; report rarity, affix count, top tier.
void game_debug_roll_item(int ilvl, int rarity_bonus, int slot,
                          int* rarity, int* naffix, int* max_tier) {
    items::Inst it;
    for (int i = 0; i < items::kDefCount; ++i)
        if (int(items::kDefs[i].slot) == slot) { it.def = uint8_t(i); break; }
    if (it.empty()) it.def = 0;
    roll_affixes(it, ilvl, rarity_bonus);
    if (rarity) *rarity = it.rarity;
    if (naffix) *naffix = it.nprefix + it.nsuffix;
    int mt = 0;
    for (int i = 0; i < it.nprefix; ++i) if (it.pre[i].tier > mt) mt = it.pre[i].tier;
    for (int i = 0; i < it.nsuffix; ++i) if (it.suf[i].tier > mt) mt = it.suf[i].tier;
    if (max_tier) *max_tier = mt;
}
// where: 0 = inventory[idx] (0..11), 1 = equipment[slot] (0..Slot::Count). Fills the item's
// full display name into `out`; returns 1 if the slot holds an item, 0 if empty/out of range.
int game_debug_item_name(int where, int idx, char* out, int cap) {
    const items::Inst* it = nullptr;
    if (where == 0 && idx >= 0 && idx < tune::kInvCap) it = &g.pl.inv[idx];
    else if (where == 1 && idx >= 0 && idx < int(items::Slot::Count)) it = &g.pl.equip[idx];
    if (!it) { if (cap > 0) out[0] = 0; return 0; }
    it->full_name(out, cap);
    return it->empty() ? 0 : 1;
}
int game_debug_hostile_count() {
    int n = 0;
    for (const Entity& e : g.ents)
        if (e.zone == g.cur_zone &&
            (e.type == EType::Trooper || e.type == EType::ShieldTrooper ||
             e.type == EType::Hound || e.type == EType::Boss)) ++n;
    return n;
}

void game_debug_rpg(int* level, int* inv_count, int* stash_count) {
    if (level) *level = g.pl.st.level;
    if (inv_count) {
        int n = 0;
        for (const items::Inst& it : g.pl.inv)
            if (!it.empty()) ++n;
        *inv_count = n;
    }
    if (stash_count) {  // G3: total banked items across all owned tabs
        int n = 0; for (int t = 0; t < g_meta.stash_tabs; ++t) n += stash_used(t);
        *stash_count = n;
    }
}

void game_debug_grab_loot() {
    for (Entity& e : g.ents) {
        if (e.type != EType::Loot || e.zone != g.cur_zone) continue;
        if (pack_add(loot_of(e)) >= 0) e.type = EType::None;
    }
}

void game_debug_vault_deposit_all() {  // G3: bank every pack item into tab 0 (first-fit)
    for (int i = 0; i < tune::kInvCap; ++i)
        if (!g.pl.inv[i].empty()) stash_deposit(i, 0);
    meta_save();
}

int game_debug_vault_withdraw_one() {  // G3: pull the first banked item (tab 0) back into the pack
    for (int i = 0; i < tune::kStashCap; ++i)
        if (!g_meta.stash[0][i].empty()) return stash_withdraw(0, i) ? 1 : 0;
    return 0;
}
// --- G3 vault hooks ---
int game_debug_stash_tabs() { return g_meta.stash_tabs; }
int game_debug_stash_tab_cost() { return stash_tab_cost(); }
int game_debug_buy_stash_tab() { return buy_stash_tab() ? 1 : 0; }
int game_debug_stash_used(int t) { return stash_used(t); }
int game_debug_stash_deposit(int pidx, int t) { return stash_deposit(pidx, t) ? 1 : 0; }
int game_debug_stash_withdraw(int t, int sidx) { return stash_withdraw(t, sidx) ? 1 : 0; }
// The tab pool index whose footprint covers stash-grid cell `gc` in tab `t` (-1 = empty).
int game_debug_stash_item_at(int t, int gc) {
    if (t < 0 || t >= tune::kMaxStashTabs) return -1;
    return grid_item_at(g_meta.stash[t], g_meta.stash_cell[t], tune::kStashCap, tune::kStashW, gc);
}
int game_debug_vault_present() {  // count of EType::Vault interactables in the current zone
    int n = 0;
    for (const Entity& e : g.ents) if (e.type == EType::Vault && e.zone == g.cur_zone) ++n;
    return n;
}
// G4: framebuffer center of pack grid cell `cell` on the open grid screen (for the mouse-UI test).
void game_debug_pack_cell_center(int cell, int* sx, int* sy) {
    int ox = gridui::inv_pack_x(), oy = gridui::inv_pack_y();
    if (g.menu_open == 7) { ox = gridui::vault_pack_x(); oy = gridui::vault_pack_y(); }
    else if (g.menu_open == 11) { ox = gridui::bench_pack_x(); oy = gridui::bench_pack_y(); }
    if (sx) *sx = ox + (cell % tune::kPackW) * gridui::CELL + gridui::CELL / 2;
    if (sy) *sy = oy + (cell / tune::kPackW) * gridui::CELL + gridui::CELL / 2;
}

void game_debug_clear_hostiles() {
    for (Entity& e : g.ents)
        if (e.type == EType::Trooper || e.type == EType::ShieldTrooper ||
            e.type == EType::Hound || e.type == EType::Boss)
            e.type = EType::None;
}

void game_debug_warp_to_boss(int* atk_dx, int* atk_dy) {
    if (atk_dx) *atk_dx = 0;
    if (atk_dy) *atk_dy = 0;
    for (const Entity& e : g.ents) {
        if (e.type != EType::Boss || e.zone != g.cur_zone) continue;
        static const int kAdj[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1},
                                       {-1, -1}, {1, 1}, {-1, 1}, {1, -1}};
        for (const auto& d : kAdj) {
            const int x = e.x + d[0], y = e.y + d[1];
            if (!zone().walkable(x, y) || entity_at(x, y, g.cur_zone)) continue;
            g.ents[0].x = int16_t(x);
            g.ents[0].y = int16_t(y);
            if (atk_dx) *atk_dx = -d[0];
            if (atk_dy) *atk_dy = -d[1];
            refresh_fov_and_paths();
            return;
        }
    }
}

const char* game_debug_death_cause() { return g_last_death; }

void game_debug_buff() {  // harness only: end fights before the worm arrives
    g.pl.st.cond = 60;
    g.pl.st.vers = 40;
    g.pl.st.awar = 30;
    g.pl.hp = player_max_hp();
    g.pl.shield = player_max_shield();
    g.pl.will = player_max_will();
}

bool game_debug_boss_is_baron() {
    for (const Entity& e : g.ents)
        if (e.type == EType::Boss && e.zone == g.cur_zone &&
            (e.data2 == -1 || (boss_is_pinnacle(e.data2) && boss_pinnacle_idx(e.data2) == 9)))
            return true;
    return false;
}

bool game_debug_boss_alive() {
    for (const Entity& e : g.ents)
        if (e.type == EType::Boss && e.zone == g.cur_zone) return true;
    return false;
}

int game_debug_shield_hp() {
    for (const Entity& e : g.ents)
        if (e.type == EType::ShieldTrooper && e.zone == g.cur_zone) return e.hp;
    return -1;
}

int game_worm_state() { return int(g.wrm.state); }

void game_debug_warp(int zone_id, int x, int y) {
    // keep cur_node consistent with the warped-to biome (first node of that Stage)
    for (int i = 0; i < g.graph.count; ++i)
        if (world::zone_id_of(g.graph.nodes[i].stage) == zone_id) { g.cur_node = i; break; }
    enter_zone(uint8_t(zone_id), g.seed, false, x, y);  // g.seed keeps the old warp map layout
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
    g.pl.water = player_max_water();
    g.pl.hp = player_max_hp();
    g.pl.shield = player_max_shield();
    g.pl.will = player_max_will();
    g.pl.exposure = 0;
}

uint64_t game_state_hash() {
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001b3ULL; };
    mix(g.seed);
    mix(uint64_t(g.cur_zone));
    mix(uint64_t(uint32_t(g.cur_node)));  // B1.3: graph position is part of run state
    mix(uint64_t(uint32_t(g.pl.hp)));
    mix(uint64_t(uint32_t(g.pl.water)));
    mix(uint64_t(uint32_t(g.pl.exposure)));
    mix(uint64_t(uint32_t(g.pl.turn)));
    // RT1.2: the real-time clock, world-step accumulator, and player action cooldown now vary
    // independently of the turn count, so they are part of the hashed sim state.
    mix(uint64_t(g.sim_clock) | (uint64_t(uint32_t(g.world_accum)) << 32));
    mix(uint64_t(uint32_t(g.pl.next_act_tick)));
    mix(uint64_t(uint32_t(g.pl.shield)) | (uint64_t(uint32_t(g.pl.shield_delay)) << 32));  // C2
    mix(uint64_t(uint32_t(g.pl.will)));  // D1
    mix(uint64_t(uint32_t(g.pl.ail_poison_dmg)) | (uint64_t(uint32_t(g.pl.ail_poison_turns)) << 32));  // C3
    mix(uint64_t(uint32_t(g.pl.ail_scorch_dmg)) | (uint64_t(uint32_t(g.pl.ail_scorch_turns)) << 32));  // C3
    mix(uint64_t(uint32_t(g.pl.ail_bleed_dmg)) | (uint64_t(uint32_t(g.pl.ail_bleed_turns)) << 32));    // C3
    mix(uint64_t(uint32_t(g.pl.ail_blind_turns)) | (uint64_t(uint32_t(g.pl.ail_frozen_turns)) << 32));  // C3
    mix(uint64_t(uint32_t(g.pl.ail_stun_turns)));  // RT2: Stun locks the player out
    mix(uint64_t(uint8_t(g.pl.base_class)) | (uint64_t(uint8_t(g.pl.ascendancy)) << 8) |
        (uint64_t(uint32_t(g.pl.atk)) << 16));  // C3: base class + ascendancy
    mix(uint64_t(uint16_t(g.pl.st.level)) | (uint64_t(uint32_t(g.pl.st.xp)) << 16) |
        (uint64_t(uint16_t(g.pl.st.cond)) << 48));  // C1: Conditioning (was str)
    mix(uint64_t(uint32_t(g.season.shipped)) | (uint64_t(uint32_t(g.season.denied)) << 16) |
        (uint64_t(uint32_t(g.season.alert)) << 32) |
        (uint64_t(uint32_t(g.season.standing)) << 48));
    mix(uint64_t(g.season.ending));
    for (const Entity& e : g.ents) {
        mix(uint64_t(uint8_t(e.type)) | (uint64_t(e.zone) << 8) |
            (uint64_t(uint16_t(e.x)) << 16) | (uint64_t(uint16_t(e.y)) << 32) |
            (uint64_t(uint16_t(e.hp)) << 48));
        mix(uint64_t(uint16_t(e.energy)));  // RT1.2: energy advances on the world clock, now hashed
    }
    mix(g.rng.world.state);
    mix(g.rng.combat.state);
    mix(g.rng.loot.state);
    return h;
}
