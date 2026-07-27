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
                             Caravan, Envoy, Fremen };
enum class Ai : uint8_t { Idle = 0, Wander, Hunt, Guard };

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

struct CharStats {
    int16_t str = 12, dex = 12, vit = 12;
    int16_t level = 1, statpts = 0, talentpts = 0;
    int32_t xp = 0;
    uint8_t talents[kTalentCount] = {};
};

struct Player {
    CharStats st;
    items::Inst inv[12];
    items::Inst equip[4];  // by items::Slot
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
            pl.st.str += 4;
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

    map::Zone zone_;      // B1.2: the single resident zone (regenerated on node entry)
    world::Graph graph;   // B1: per-run node graph (built from seed; not yet driving zones)
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
    int menu_open = 0;  // 1 naib / 2 sayyadina / 3 smuggler / 5 char / 6 inv / 7 vault
    int menu_sel = 0;
    int vault_page = 0;
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

worm::Tuning worm_tuning() {
    worm::Tuning t;
    t.active = g.cur_zone != map::kZoneSietch;
    if (g.cur_zone == map::kZoneDeepPan) { t.stir_threshold = 25; t.hunt_threshold = 50; }
    return t;
}

// Y3: champions — randomly generated bosses. Entity.data packs
// (tier | traits<<8); data2 packs (name_a | name_b<<8). Traits are bits:
inline constexpr uint8_t BT_SHIELDED = 1, BT_SWIFT = 2, BT_BRUTE = 4,
                         BT_STALWART = 8, BT_TOXIC = 16;
inline constexpr const char* kBossTitles[8] = {
    "Warmaster", "Executor", "Beast-Handler", "Count", "Whipmaster",
    "Provost", "Blade-Tutor", "Overseer"};
inline constexpr const char* kBossNames[8] = {
    "Dreth", "Gorou", "Vask", "Ilbam", "Krayt", "Feyd-Rausha", "Mordo", "Skarn"};
inline constexpr const char* kBossEpithets[8] = {
    "the Unbowed", "the Flenser", "of the Tithe", "the Whip-Hand",
    "the Waterless", "the Gilded", "the Patient", "the Cruel"};

// X2: Diablo-style enemy statlines.
struct Foe { uint8_t level, tohit, dmin, dmax, ac_dodge; uint16_t xp; };
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
    static constexpr Foe kHound{1, 60, 1, 3, 8, 12};
    static constexpr Foe kAdder{2, 72, 1, 2, 4, 15};
    static constexpr Foe kHermit{3, 55, 2, 5, 6, 28};
    static constexpr Foe kTrooper{3, 62, 2, 5, 8, 35};
    static constexpr Foe kShield{5, 68, 3, 7, 8, 70};
    static constexpr Foe kSeeker{4, 88, 4, 4, 22, 40};
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

Entity& player_ent() { return g.ents[0]; }

int affix_total(uint8_t kind) {
    int p = 0;
    for (const items::Inst& it : g.pl.equip) p += it.affix_power(kind);
    return p;
}

int player_max_hp() {
    return 10 + g.pl.st.vit * 2 + (g.pl.st.level - 1) * 3 + affix_total(items::AK_HP);
}
int player_max_water() { return tune::kWaterMax + affix_total(items::AK_WATER); }
int player_armor() {
    const items::Inst& a = g.pl.equip[int(items::Slot::Armor)];
    return (a.empty() ? 0 : a.d().a) + affix_total(items::AK_ARMOR);
}
int player_suit_mul4() {
    const items::Inst& su = g.pl.equip[int(items::Slot::Suit)];
    int m = su.empty() ? 4 : su.d().a;
    return m;
}
int player_dodge() {
    return g.pl.st.dex / 2 + affix_total(items::AK_DODGE) + 4 * g.pl.st.talents[8];
}

// Player melee swing vs a foe. Returns damage dealt (0 = miss); fills crit.
int player_attack_roll(const Foe& f, bool* crit_out) {
    const int tohit = 55 + g.pl.st.dex * 2 - f.ac_dodge + affix_total(items::AK_TOHIT);
    const int chance = tohit < 15 ? 15 : tohit > 95 ? 95 : tohit;
    if (int(g.rng.combat.below(100)) >= chance) return 0;
    const items::Inst& w = g.pl.equip[int(items::Slot::Weapon)];
    const int wmin = w.empty() ? 1 : w.d().a;
    const int wmax = w.empty() ? 2 : w.d().b;
    int dmg = g.rng.combat.range(wmin, wmax) + g.pl.st.str / 4 +
              affix_total(items::AK_DMG) + 2 * g.pl.st.talents[0];
    if (g.pl.sandwalk) dmg += 3 * g.pl.st.talents[1];
    const int critc = g.pl.st.dex / 2 + 4 * g.pl.st.talents[2];
    const bool crit = int(g.rng.combat.below(100)) < critc;
    if (crit) dmg *= 2;
    if (crit_out) *crit_out = crit;
    return dmg;
}

// Enemy swing at the player. Returns damage taken (0 = miss/dodge).
int foe_attack_roll(const Foe& f) {
    const int chance0 = int(f.tohit) - player_dodge();
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
        place_npc_near(EType::Naib, z, zone_id, 30, 30);       // great hall
        place_npc_near(EType::Sayyadina, z, zone_id, 32, 16);  // cistern chamber
        place_npc_near(EType::Smuggler, z, zone_id, 50, 28);   // storeroom
        // dialog-only Fremen — one per chamber, each in open floor well away from
        // the vendors and the wall-hugging props (so no NPC+prop "huddle" reads as
        // a cluster), each a distinct tribesperson (data = sprite/bark index).
        place_npc_near(EType::Fremen, z, zone_id, 13, 30, 0);  // living quarters
        place_npc_near(EType::Fremen, z, zone_id, 20, 47, 1);  // armory
        place_npc_near(EType::Fremen, z, zone_id, 46, 47, 2);  // sleeping chamber
        place_npc_near(EType::Fremen, z, zone_id, 53, 32, 3);  // storeroom
        place_npc_near(EType::Fremen, z, zone_id, 36, 28, 4);  // great hall (east)
        place_npc_near(EType::Fremen, z, zone_id, 26, 32, 5);  // great hall (west)
        return;
    }
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

void refresh_fov_and_paths() {
    map::compute_fov(zone(), player_ent().x, player_ent().y, fov_radius());
    // An enclosed indoor room is lit whole (no black no-man's-land in the sietch).
    map::reveal_room(zone(), player_ent().x, player_ent().y);
    map::compute_dijkstra(zone(), player_ent().x, player_ent().y, g.dmap);
    recompute_light();
}

// meta.dat: tiny cross-run memory — seasons played, endings witnessed.
struct Meta {
    uint32_t runs = 0;
    uint8_t endings_seen = 0;      // bit per campaign::Ending
    uint8_t stash_count = 0;       // X4: the tribe vault — gear that survived
    items::Inst stash[24];
};
Meta g_meta;

void meta_load() {
    FILE* f = std::fopen("meta.dat", "rb");
    if (!f) return;
    uint32_t magic = 0;
    if (std::fread(&magic, 4, 1, f) == 1) {
        if (magic == 0x32454D44u) {  // 'DME2'
            (void)!std::fread(&g_meta.runs, 4, 1, f);
            (void)!std::fread(&g_meta.endings_seen, 1, 1, f);
            (void)!std::fread(&g_meta.stash_count, 1, 1, f);
            (void)!std::fread(g_meta.stash, sizeof g_meta.stash, 1, f);
        } else if (magic == 0x54454D44u) {  // v1: carry runs/endings forward
            (void)!std::fread(&g_meta.runs, 4, 1, f);
            (void)!std::fread(&g_meta.endings_seen, 1, 1, f);
        }
    }
    std::fclose(f);
}

void meta_save() {
    FILE* f = std::fopen("meta.dat", "wb");
    if (!f) return;
    const uint32_t magic = 0x32454D44u;  // 'DME2'
    std::fwrite(&magic, 4, 1, f);
    std::fwrite(&g_meta.runs, 4, 1, f);
    std::fwrite(&g_meta.endings_seen, 1, 1, f);
    std::fwrite(&g_meta.stash_count, 1, 1, f);
    std::fwrite(g_meta.stash, sizeof g_meta.stash, 1, f);
    std::fclose(f);
}

void grant_xp(int amount) {
    if (g.pl.st.level >= 30) return;
    g.pl.st.xp += amount;
    while (g.pl.st.xp >= xp_to_level(g.pl.st.level)) {
        g.pl.st.xp -= xp_to_level(g.pl.st.level);
        ++g.pl.st.level;
        g.pl.st.statpts += 2;
        ++g.pl.st.talentpts;
        g.pl.hp = player_max_hp();
        g.log.push("LEVEL %d. The desert tempers you. (+2 stats, +1 talent: P)",
                   int(g.pl.st.level));
        flash(240, 220, 120);
        g.services.mixer->push({audio::Cmd::PlaySfx, 1, 220, 150});
    }
}

// Y2: run difficulty — each denied harvester and every tier of alert deepens
// the loot tables. Risk feeds reward.
int run_difficulty() { return g.season.denied + g.season.alert / 25; }

items::Inst roll_item(int item_level, int rarity_bonus) {
    int candidates[items::kDefCount];
    int n = 0;
    for (int i = 0; i < items::kDefCount; ++i)
        if (items::kDefs[i].level <= item_level + 1 &&
            items::kDefs[i].level + 3 >= item_level)
            candidates[n++] = i;
    items::Inst it;
    if (!n) return it;
    it.def = uint8_t(candidates[g.rng.loot.below(uint32_t(n))]);
    const int r = int(g.rng.loot.below(100)) - rarity_bonus;
    it.rarity = r < 10 ? 2 : r < 40 ? 1 : 0;
    if (it.rarity >= 1) {
        if (g.rng.loot.chance(1, 2) || it.rarity == 2)
            it.prefix = uint8_t(g.rng.loot.below(items::kPrefixCount));
        if (it.prefix == 0xFF || it.rarity == 2)
            it.suffix = uint8_t(g.rng.loot.below(items::kSuffixCount));
    }
    return it;
}

void maybe_drop_loot(EType t, int x, int y) {
    const Foe* f = foe_stats(t);
    if (!f) return;
    const bool human = t == EType::Trooper || t == EType::ShieldTrooper ||
                       t == EType::Spotter || t == EType::Hermit;
    if (!human && t != EType::Boss) return;
    const int chance = (t == EType::Boss ? 100 : 40) + 8 * g.pl.st.talents[5];
    if (int(g.rng.loot.below(100)) >= chance) return;

    const int diff = run_difficulty();
    items::Inst it = roll_item(f->level + diff / 2,
                               diff * 6 + (t == EType::Boss ? 60 : 0));
    for (Entity& e : g.ents)
        if (e.type == EType::None) {
            e = Entity{EType::Loot, Ai::Idle, g.cur_zone, int16_t(x), int16_t(y), 1, 0,
                       int16_t(it.def | (it.prefix << 8)),
                       int16_t(it.suffix | (it.rarity << 8))};
            char nm[64];
            it.full_name(nm, sizeof nm);
            g.log.push("%s dropped: %s", it.rarity == 2 ? "RARE" : it.rarity ? "Fine" : "Gear", nm);
            return;
        }
}

items::Inst loot_of(const Entity& e) {
    items::Inst it;
    it.def = uint8_t(e.data & 0xFF);
    it.prefix = uint8_t((e.data >> 8) & 0xFF);
    it.suffix = uint8_t(e.data2 & 0xFF);
    it.rarity = uint8_t((e.data2 >> 8) & 0xFF);
    return it;
}

const char* g_last_death = "";
void die(const char* cause) {
    g_last_death = cause;
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
        for (items::Inst& slot : g.pl.inv)
            if (slot.empty()) { slot.def = 4; break; }  // the Crysknife itself
        g.pl.maker_hooks = true;
        g.log.push("The Naib unwraps a pale blade. 'Honored one. It is yours.'");
        g.log.push("Crysknife and MAKER HOOKS taken. R mounts a surfaced worm.");
    }
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
            traits = BT_SHIELDED | BT_BRUTE | BT_STALWART;
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
            g.log.push("Kill him and the season is yours. (Shielded: slow blade!)");
        } else {
            char nm[80];
            std::snprintf(nm, sizeof nm, "%s %s %s",
                          kBossTitles[name_pack & 7], kBossNames[(name_pack >> 4) & 7],
                          kBossEpithets[(name_pack >> 8) & 7]);
            g.log.push("The Baron's champion answers: %s!", nm);
            if (traits & BT_SHIELDED) g.log.push("A shield shimmer rides with him.");
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

    if (g.pl.eyes_ibad) alert_gain += alert_gain > 0 ? 1 : 0;  // a marked face
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
                if (e.data2 == -1) {
                    g.log.push("SHAI-HULUD TAKES THE BARON. Shield and all.");
                    g.log.push("The desert itself has judged him.");
                    e.type = EType::None;
                    end_season(campaign::kVictory);
                    continue;
                }
                g.log.push("The champion's shield sang to the worm. It answered.");
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
            const int dmg = foe_attack_roll(*foe_stats(e.type));
            if (dmg == 0) {
                g.log.push("You slip the trooper's blade. A miss.");
                return;
            }
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
            const int dmg = foe_attack_roll(*foe_stats(EType::Hermit));
            if (dmg == 0) { g.log.push("The hermit's claws rake past you."); return; }
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
            const int dmg = foe_attack_roll(*foe_stats(EType::Hound));
            if (dmg == 0) {
                g.log.push("The hound snaps at empty air.");
                return;
            }
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
    int drain = base * tune::kThirstHeatMul4[amb] * player_suit_mul4() / 16;
    drain = drain * (8 - g.pl.st.talents[7]) / 8;  // Water Discipline
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

    // heat exposure — a stillsuit shields the body from the open sun (its
    // canonical purpose), so an equipped suit blunts the gain and better suits
    // negate it; the Litany halves whatever is left.
    const bool sheltered = (tf & (map::TF_INDOOR | map::TF_SHADE)) != 0;
    if (amb >= tune::kExposureHotThreshold && !sheltered) {
        int gain = tune::kExposureGain;
        const items::Inst& suit = g.pl.equip[int(items::Slot::Suit)];
        if (!suit.empty()) gain -= (4 - suit.d().a) + 1;  // Mk I halves, Mk III negates
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
        if (e.type == EType::Boss) {
            const uint8_t traits = uint8_t(e.data >> 8);
            if (traits & BT_SHIELDED) g.noise.add(e.x, e.y, 8);
            e.energy += int16_t((traits & BT_SWIFT) ? 150 : 100);
            while (e.energy >= 100 && !g.pl.dead) {
                e.energy -= 100;
                const int pdist = g.dmap[e.y * map::W + e.x];
                if (pdist == 1) {
                    e.anim.facing = facing_from_tile_delta(player_ent().x - e.x,
                                                           player_ent().y - e.y);
                    e.anim.set(anim::State::Attack);
                    const Foe f = boss_foe(e);
                    const int dmg = foe_attack_roll(f);
                    if (dmg == 0) { g.log.push("You slip the champion's blade."); continue; }
                    if (traits & BT_TOXIC) {
                        g.poison_turns = 15;
                        g.log.push("The blade is POISONED - your water burns.");
                    }
                    g.log.push("The champion strikes you for %d.", dmg);
                    damage_player(dmg, e.data2 == -1 ? "Slain by the Baron himself."
                                                     : "Cut down by the Baron's champion.");
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
    if (!g.pl.sandwalk && g.noise_meter > 35 && g.cur_zone != map::kZoneSietch &&
        (g.pl.turn % 40) == 17)
        g.log.push("Your rhythm carries through the sand. (C: sandwalk)");
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
        const items::Inst& w0 = g.pl.equip[int(items::Slot::Weapon)];
        const int wavg = w0.empty() ? 2 : (w0.d().a + w0.d().b) / 2;
        const int dealt = worm::hit(g.wrm, nx, ny, wavg + g.pl.st.str / 4,
                                    worm_callbacks());
        if (dealt > 0) g.log.push("Your blade bites the soft flesh of the maw!");
        g.noise.add(p.x, p.y, 12);
        advance_world();
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
        advance_world();
        return;
    }

    if (Entity* car = entity_at(nx, ny, g.cur_zone, EType::Caravan)) {
        const items::Inst gift = roll_item(2 + run_difficulty(), 30 + run_difficulty() * 6);
        bool taken = false;
        for (items::Inst& slot : g.pl.inv)
            if (slot.empty() && !gift.empty()) {
                slot = gift;
                char nm[64];
                gift.full_name(nm, sizeof nm);
                g.log.push("The caravan master presses contraband on you: %s.", nm);
                taken = true;
                break;
            }
        if (!taken) g.log.push("The caravan has wares, but your pack is full.");
        g.pl.spice += 2;
        g.log.push("They share two grains for the road. The caravan breaks camp.");
        car->type = EType::None;
        g.sideobj_kind = 0;
        advance_world();
        return;
    }
    if (Entity* env = entity_at(nx, ny, g.cur_zone, EType::Envoy)) {
        const int bribe = 6 + run_difficulty();
        if (g.pl.spice >= bribe) {
            g.pl.spice -= bribe;
            g.season.alert = g.season.alert > 25 ? g.season.alert - 25 : 0;
            g.season.discovery = 0;
            g.log.push("The Guildsman weighs %d grains. 'The surveys will... err.'", bribe);
            g.log.push("The Guild's blindness is bought. Discovery resets.");
            env->type = EType::None;
            g.sideobj_kind = 0;
        } else {
            g.log.push("'%d grains buys blindness. You carry %d. Return heavier.'",
                       bribe, g.pl.spice);
        }
        advance_world();
        return;
    }
    // NPCs are click-to-talk now: a bump just blocks (no menu). Interaction is
    // driven from game_update when the player clicks the NPC (see interact_kind).
    if (entity_at(nx, ny, g.cur_zone, EType::Naib) ||
        entity_at(nx, ny, g.cur_zone, EType::Sayyadina) ||
        entity_at(nx, ny, g.cur_zone, EType::Smuggler) ||
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
        const bool shielded_boss =
            victim->type == EType::Boss && ((victim->data >> 8) & BT_SHIELDED);
        player_ent().anim.facing = facing_from_tile_delta(dx, dy);
        player_ent().anim.set(anim::State::Attack);
        g.impact_x = int16_t(nx);
        g.impact_y = int16_t(ny);
        g.impact_kind =
            (victim->type == EType::ShieldTrooper || shielded_boss) ? 1 : 0;
        if ((victim->type == EType::ShieldTrooper || shielded_boss) && !g.pl.sandwalk) {
            g.log.push("Your quick blade GLANCES off the shield shimmer.");
            g.log.push("The slow blade penetrates. (C: sandwalk, then strike.)");
            g.noise.add(nx, ny, 10);
            advance_world();
            return;
        }
        Foe boss_f{};
        if (victim->type == EType::Boss) boss_f = boss_foe(*victim);
        const Foe* foe = victim->type == EType::Boss ? &boss_f : foe_stats(victim->type);
        bool crit = false;
        int dealt = foe ? player_attack_roll(*foe, &crit) : g.pl.atk;
        if (dealt == 0 && g.pl.sandwalk &&
            (victim->type == EType::ShieldTrooper || shielded_boss))
            dealt = 1 + g.pl.st.str / 4;  // slow and true: it always lands
        g.noise.add(nx, ny, victim->type == EType::Spotter ? 4 : 12);  // blades are quiet
        if (dealt == 0) {
            g.log.push("Your blade whispers past. A miss.");
            advance_world();
            return;
        }
        victim->hp -= int16_t(dealt);
        if (crit) g.log.push("CRIT - %d! The blade finds the seam.", dealt);
        else g.log.push("You hit for %d.", dealt);
        if (victim->hp <= 0) {
            g.hitstop = 3;  // the world holds its breath
            if (foe) {
                int xp = foe->xp;
                if (g.pl.st.level > foe->level + 2) xp /= 2;
                grant_xp(xp);
            }
            const bool was_baron = victim->type == EType::Boss && victim->data2 == -1;
            maybe_drop_loot(victim->type, victim->x, victim->y);
            if (was_baron) {
                *victim = Entity{EType::Corpse, Ai::Idle, uint8_t(g.cur_zone),
                                 victim->x, victim->y, 0, 0, 400};
                end_season(campaign::kVictory);
                advance_world();
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
                case EType::Boss:
                    if (victim->data2 == -1) {
                        g.log.push("The Baron falls, disbelieving, into the sand.");
                        g.log.push("His water belongs to the tribe. ALL of it.");
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
        } else {
            g.log.push("You strike %s.",
                       victim->type == EType::Hound ? "the dust hound" :
                       victim->type == EType::Trooper ? "the trooper" : "the spotter");
        }
        advance_world();
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
    if (Entity* drop = entity_at(p.x, p.y, g.cur_zone, EType::Loot)) {
        const items::Inst it = loot_of(*drop);
        for (items::Inst& slot : g.pl.inv)
            if (slot.empty()) {
                slot = it;
                char nm[64];
                it.full_name(nm, sizeof nm);
                g.log.push("Taken: %s (I to equip).", nm);
                drop->type = EType::None;
                g.services.mixer->push({audio::Cmd::PlaySfx, 1, 150, 140});
                advance_world();
                return;
            }
        g.log.push("Your pack is full. Bank gear at the Naib's vault.");
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
        if (g.pl.water > player_max_water()) g.pl.water = player_max_water();
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
    g.graph.build(seed);  // B1: build the run's node graph (deterministic from seed)
    meta_load();

    int sx, sy;
    map::generate(g.zone_, seed, map::kZoneSietch, sx, sy);
    g.ents[0] = Entity{EType::Player, Ai::Idle, map::kZoneSietch,
                       int16_t(sx), int16_t(sy), int16_t(tune::kPlayerHpMax), 0, 0};
    // Populate the sietch now that the player entity exists (the Naib + Sayyadina
    // are placed relative to it). This also sets spawned_mask for the zone; the
    // old code set the mask directly, which silently skipped the NPC spawn.
    spawn_zone_wildlife(g.zone_, map::kZoneSietch);

    g.pl.equip[int(items::Slot::Weapon)].def = 0;  // Dust Knife
    g.pl.equip[int(items::Slot::Suit)].def = 9;    // Stillsuit Mk I
    g.pl.hp = player_max_hp();
    g.choosing = services.interactive;
    // Interactive launches open on the title menu; headless tests jump into play.
    g.screen = services.interactive ? GameState::Screen::Menu : GameState::Screen::Play;
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
void use_door() {
    if (g.cur_zone == map::kZoneSietch) {
        g.log.push("You step out into the Tanzerouft.");
        enter_zone(map::kZoneDesert, true);
    } else {
        g.log.push("You slip back into the sietch's cool dark.");
        enter_zone(map::kZoneSietch, true);
    }
    advance_world();
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
            case EType::Naib: return "the Naib";
            case EType::Sayyadina: return "the Sayyadina";
            case EType::Smuggler: return "Greybeard the smuggler";
            case EType::PanAdder: return "pan adder - give it room";
            case EType::Hermit: return "spice-mad hermit";
            case EType::HunterSeeker: return "HUNTER-SEEKER";
            case EType::SpiceCache: return "raw spice (G: gather)";
            case EType::Boss: {
                if (e.data2 == -1) return "BARON HARKONNEN (shielded!)";
                static char bbuf[80];
                std::snprintf(bbuf, sizeof bbuf, "%s %s %s%s",
                              kBossTitles[e.data2 & 7], kBossNames[(e.data2 >> 4) & 7],
                              kBossEpithets[(e.data2 >> 8) & 7],
                              ((e.data >> 8) & BT_SHIELDED) ? " (shielded)" : "");
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
        const int counts[5] = {0, 5, 3, 5, 4};
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
    } else if (g.menu_open == 6) {
        for (int i = 0; i < 12; ++i) if (band(y0 + 140 + i * 24)) return i;
        if (band(y0 + 460)) return 12;
    } else if (g.menu_open == 7) {
        for (int i = 0; i < 12; ++i) if (band(y0 + 88 + i * 24)) return i;
        if (band(y0 + 420)) return 12;
    }
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
    // Front-end screens: the world is paused; they run their own input + render.
    if (g.screen == GameState::Screen::Menu) return update_menu(in);
    if (g.screen == GameState::Screen::Options) { update_options(in); return true; }
    if (g.screen == GameState::Screen::Manual) { update_manual(in); return true; }
    if (g.microshake > 0) --g.microshake;
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
        // 5 = character sheet, 6 = inventory, 7 = the vault
        int counts[8] = {0, 5, 3, 5, 4, 3 + kTalentCount + 1, 13, 13};
        const int n = counts[g.menu_open];
        // A6: mouse — hovering a row selects it, left-click confirms it, right-click closes.
        const int mrow = menu_row_under_mouse(in);
        if (mrow >= 0) g.menu_sel = mrow;
        const bool mclick = mrow >= 0 && in.mouse_pressed[0];
        if (in.mouse_pressed[2] || in.mouse_pressed[1]) { g.menu_open = 0; return true; }
        if (g.menu_open == 5) {
            // 0..2 stats, 3..11 talents, 12 close
            if (in.was_pressed(Key::Up)) g.menu_sel = (g.menu_sel + n - 1) % n;
            if (in.was_pressed(Key::Down)) g.menu_sel = (g.menu_sel + 1) % n;
            if (in.was_pressed(Key::Cancel)) g.menu_open = 0;
            if (in.was_pressed(Key::Confirm) || mclick) {
                if (g.menu_sel < 3 && g.pl.st.statpts > 0) {
                    (g.menu_sel == 0 ? g.pl.st.str
                     : g.menu_sel == 1 ? g.pl.st.dex : g.pl.st.vit) += 1;
                    --g.pl.st.statpts;
                    if (g.menu_sel == 2) g.pl.hp = g.pl.hp < player_max_hp()
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
                } else if (!g.pl.inv[g.menu_sel].empty()) {
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
            // vault: Left/Right pages (0 deposit inv, 1 withdraw stash)
            if (in.was_pressed(Key::Left) || in.was_pressed(Key::Right)) {
                g.vault_page ^= 1;
                g.menu_sel = 0;
            }
            if (in.was_pressed(Key::Up)) g.menu_sel = (g.menu_sel + n - 1) % n;
            if (in.was_pressed(Key::Down)) g.menu_sel = (g.menu_sel + 1) % n;
            if (in.was_pressed(Key::Cancel)) g.menu_open = 0;
            if (in.was_pressed(Key::Confirm) || mclick) {
                if (g.menu_sel == 12) {
                    g.menu_open = 0;
                } else if (g.vault_page == 0) {  // deposit
                    items::Inst& it = g.pl.inv[g.menu_sel];
                    if (g.season.denied < 1) {
                        g.log.push("'The vault opens for the blooded. Deny a harvest first.'");
                    } else if (!it.empty() && g_meta.stash_count < 24) {
                        g_meta.stash[g_meta.stash_count++] = it;
                        char nm[64];
                        it.full_name(nm, sizeof nm);
                        g.log.push("Banked: %s. It is the tribe's now - and yours.", nm);
                        it = items::Inst{};
                        meta_save();
                    }
                } else {  // withdraw
                    if (g.menu_sel < g_meta.stash_count) {
                        for (items::Inst& slot : g.pl.inv)
                            if (slot.empty()) {
                                slot = g_meta.stash[g.menu_sel];
                                for (int i = g.menu_sel; i + 1 < g_meta.stash_count; ++i)
                                    g_meta.stash[i] = g_meta.stash[i + 1];
                                --g_meta.stash_count;
                                char nm[64];
                                slot.full_name(nm, sizeof nm);
                                g.log.push("Withdrawn: %s", nm);
                                meta_save();
                                break;
                            }
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
                case 13:  // the tribe vault
                    g.menu_open = 7;
                    g.menu_sel = 0;
                    g.vault_page = 0;
                    break;
                case 14: g.menu_open = 0; break;  // leave
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
                        g.pl.tox += 10;
                        g.pl.tox_lifetime += 10;
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
                        g.pl.tox += 15;
                        g.pl.tox_lifetime += 15;
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

    // a fresh press moves at once; a held key re-steps as the glide lands
    if (in.was_pressed(Key::Map)) g.show_map = !g.show_map;  // A5: toggle the minimap

    const bool can_continue = glide_done(player_ent());
    auto move_input = [&](Key k) {
        return in.was_pressed(k) ||
               (in.is_down(k) && can_continue && in.held_ticks[int(k)] >= tune::kHeldStepFloor);
    };
    int dx = 0, dy = 0;

    // PoE2 screen-relative WASD: compose an 8-way tile step from the four keys so
    // W walks *up the screen* (an iso diagonal). Inverse of the iso projection:
    // dx = sign(Sx+Sy), dy = sign(Sy-Sx), for screen intent (Sx, Sy).
    const int wsx = (in.is_down(Key::WalkRight) ? 1 : 0) - (in.is_down(Key::WalkLeft) ? 1 : 0);
    const int wsy = (in.is_down(Key::WalkDown) ? 1 : 0) - (in.is_down(Key::WalkUp) ? 1 : 0);
    if (wsx || wsy) {
        int held = 0;
        for (Key k : {Key::WalkUp, Key::WalkDown, Key::WalkLeft, Key::WalkRight})
            if (in.is_down(k) && in.held_ticks[int(k)] > held) held = in.held_ticks[int(k)];
        const bool fresh = in.was_pressed(Key::WalkUp) || in.was_pressed(Key::WalkDown) ||
                           in.was_pressed(Key::WalkLeft) || in.was_pressed(Key::WalkRight);
        if (fresh || (can_continue && held >= tune::kHeldStepFloor)) {
            const int tdx = wsx + wsy, tdy = wsy - wsx;
            dx = tdx > 0 ? 1 : tdx < 0 ? -1 : 0;
            dy = tdy > 0 ? 1 : tdy < 0 ? -1 : 0;
        }
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
        if (can_continue) {
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
                } else if (g.interact_kind == 5) {  // Fremen flavor bark
                    if (Entity* f = entity_at(g.move_goal_x, g.move_goal_y,
                                              g.cur_zone, EType::Fremen)) {
                        constexpr int kN = int(sizeof kFremenBarks / sizeof *kFremenBarks);
                        g.log.push("%s", kFremenBarks[(f->data % kN + kN) % kN]);
                    }
                } else {
                    g.menu_open = g.interact_kind;
                    g.menu_sel = 0;
                }
                g.move_goal_x = -1;
                g.interact_kind = 0;
            }
        }
    }
    else if (in.pressed_or_repeat(Key::Wait)) advance_world();
    else if (in.was_pressed(Key::Ride)) try_ride();
    else if (in.was_pressed(Key::Powers)) { g.menu_open = 4; g.menu_sel = 0; }
    else if (in.was_pressed(Key::Inventory)) { g.menu_open = 6; g.menu_sel = 0; }
    else if (in.was_pressed(Key::Character)) { g.menu_open = 5; g.menu_sel = 0; }
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
            case EType::SpiceCache: case EType::Loot: case EType::Thumper: break;
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
            case EType::Boss: {
                const bool baron = e.data2 == -1;
                g.draw_list.add(r.assets->get(baron ? "baron" : "champion"),
                                (g.anim_tick / 8) % 2, sx, sy + 12, light, 1);
                if (((e.data >> 8) & BT_SHIELDED) && g.anim_tick % 4 == 0)
                    g.particles.spawn(sx - 12 + int(g.rng.fx.below(25)),
                                      sy - 26 + int(g.rng.fx.below(30)), 0, -20, 10, 182);
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
    {"THE FREMEN REBEL", {
        "You are a Fremen of the deep desert, waging a water-war on House",
        "Harkonnen. From your hidden sietch you strike out across the",
        "Tanzerouft to deny the Baron his spice, then vanish into the sand.",
        "",
        "A run is a raid: leave the sietch, sabotage spice harvesters, and",
        "ride a sandworm home to BANK what you carried. Die in the field and",
        "the desert keeps your spice and gear -- only banked loot is safe.",
        "",
        "'The spice must NOT flow.'",
        nullptr}},
    {"CONTROLS", {
        "WASD ........ move (screen-relative; the cursor aims)",
        "Left mouse .. primary action / attack toward the cursor",
        "R/Q/E, RMB .. skills (aimed at the cursor); 1-4 use flasks",
        "Click NPC ... talk / trade   |   Click the door ... leave the sietch",
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
        "The Naib ...... tribe tasks, and the vault (banked gear survives death)",
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

        const int n = int(Background::Count);
        set_draw_scale(1);  // R3: class portraits authored at native size, blit 1:1
        for (int i = 0; i < n; ++i) {
            const int cx = fb::W / 2 + (i - 1) * 320;
            const bool sel = i == g.choose_idx;
            if (const SpriteSheet* spr = r.assets->get(kBackgrounds[i].sprite)) {
                if (sel) fill_rect(ctx, cx - 90, 300, 180, 220, 3);
                draw_sprite(ctx, *spr, (g.anim_tick / 9) % spr->frames, cx, 500,
                            sel ? 31 : 24);
            }
            const int name_x = cx - int(std::strlen(kBackgrounds[i].name)) * kCh;
            draw_text_shadow(ctx, kBackgrounds[i].name, name_x, 540, sel ? 245 : 12, 1);
        }
        set_draw_scale(1);
        const BackgroundDef& b = kBackgrounds[g.choose_idx];
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
        std::snprintf(buf, sizeof buf, "LVL %d  xp %d/%d  spice %d  tox %d%s%s%s",
                      int(g.pl.st.level), int(g.pl.st.xp), xp_to_level(g.pl.st.level),
                      g.pl.spice, g.pl.tox,
                      g.pl.eyes_ibad ? "  [EYES OF IBAD]" : "",
                      g.pl.riding ? "  [RIDING]" : "",
                      g.pl.st.statpts + g.pl.st.talentpts > 0 ? "  [P: spend pts]" : "");
        draw_text_shadow(ctx, buf, 4, ui::kPanelY - 22, uint8_t(g.pl.tox > 60 ? 208 : 136), 1);
        if (g.hover_tx >= 0)
            if (const char* what = describe_at(g.hover_tx, g.hover_ty))
                draw_text_shadow(ctx, what, fb::W / 2 - int(std::strlen(what)) * kCh,
                                 ui::kPanelY - 46, 24, 1);  // lifted off the level/task row
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

    // Character sheet / inventory / vault overlays (dynamic content).
    if (g.menu_open >= 5) {
        const int x0 = fb::W / 2 - 380, y0 = 140;
        fill_rect(ctx, x0, y0, 760, 640, 2);
        fill_rect(ctx, x0, y0, 760, 2, 228);
        fill_rect(ctx, x0, y0 + 638, 760, 2, 228);
        char buf[80];
        if (g.menu_open == 5) {
            draw_text_shadow(ctx, "CHARACTER", x0 + 24, y0 + 16, 230, 1);
            std::snprintf(buf, sizeof buf, "level %d   xp %d/%d   stat pts %d   talent pts %d",
                          int(g.pl.st.level), int(g.pl.st.xp), xp_to_level(g.pl.st.level),
                          int(g.pl.st.statpts), int(g.pl.st.talentpts));
            draw_text(ctx, buf, x0 + 24, y0 + 44, 14);
            const char* rows[3] = {"Ferocity", "Weirding", "Endurance"};
            const int vals[3] = {g.pl.st.str, g.pl.st.dex, g.pl.st.vit};
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
            std::snprintf(buf, sizeof buf, "dmg+%d  hit+%d  armor %d  dodge %d",
                          g.pl.st.str / 4 + affix_total(items::AK_DMG),
                          affix_total(items::AK_TOHIT), player_armor(), player_dodge());
            draw_text(ctx, buf, x0 + 24, y0 + 420, 136);
            draw_text(ctx, g.menu_sel == 12 ? "> Close" : "  Close", x0 + 32, y0 + 460,
                      uint8_t(g.menu_sel == 12 ? 245 : 20));
        } else if (g.menu_open == 6) {
            draw_text_shadow(ctx, "PACK  (ENTER equips)", x0 + 24, y0 + 16, 230, 1);
            const char* slots[4] = {"wpn", "arm", "sut", "trk"};
            for (int i = 0; i < 4; ++i) {
                char nm[64];
                g.pl.equip[i].full_name(nm, sizeof nm);
                std::snprintf(buf, sizeof buf, "[%s] %s", slots[i], nm);
                draw_text(ctx, buf, x0 + 24, y0 + 44 + i * 22, 136);
            }
            for (int i = 0; i < 12; ++i) {
                const bool sel = g.menu_sel == i;
                const int y = y0 + 140 + i * 24;
                if (sel) fill_rect(ctx, x0 + 16, y - 2, 728, 22, 4);
                char nm[64];
                g.pl.inv[i].full_name(nm, sizeof nm);
                const uint8_t col = g.pl.inv[i].empty() ? 8
                                    : g.pl.inv[i].rarity == 2 ? 245
                                    : g.pl.inv[i].rarity == 1 ? 230 : 20;
                draw_text(ctx, nm, x0 + 32, y, uint8_t(sel ? 245 : col));
            }
            draw_text(ctx, g.menu_sel == 12 ? "> Close" : "  Close", x0 + 32, y0 + 460,
                      uint8_t(g.menu_sel == 12 ? 245 : 20));
        } else {
            std::snprintf(buf, sizeof buf, "THE TRIBE VAULT - %s (left/right)",
                          g.vault_page == 0 ? "DEPOSIT" : "WITHDRAW");
            draw_text_shadow(ctx, buf, x0 + 24, y0 + 16, 230, 1);
            std::snprintf(buf, sizeof buf, "banked %d/24 - the vault survives your death",
                          int(g_meta.stash_count));
            draw_text(ctx, buf, x0 + 24, y0 + 44, 14);
            for (int i = 0; i < 12; ++i) {
                const bool sel = g.menu_sel == i;
                const int y = y0 + 88 + i * 24;
                if (sel) fill_rect(ctx, x0 + 16, y - 2, 728, 22, 4);
                char nm[64] = "-";
                if (g.vault_page == 0) g.pl.inv[i].full_name(nm, sizeof nm);
                else if (i < g_meta.stash_count) g_meta.stash[i].full_name(nm, sizeof nm);
                draw_text(ctx, nm, x0 + 32, y, uint8_t(sel ? 245 : 20));
            }
            draw_text(ctx, g.menu_sel == 12 ? "> Close" : "  Close", x0 + 32, y0 + 420,
                      uint8_t(g.menu_sel == 12 ? 245 : 20));
        }
    } else
    // Vendor menu overlay.
    if (g.menu_open) {
        static const char* kMenus[5][5] = {
            {},
            {"Tribute 200 water  (+10 standing)", "Ask about the season",
             "A task for the tribe", "Open the tribe vault", "Leave"},
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
        for (int i = 0; i < 5 && kMenus[g.menu_open][i]; ++i) {
            const bool sel = i == g.menu_sel;
            if (sel) fill_rect(ctx, x0 + 16, y0 + 80 + i * 32 - 4, 568, 28, 4);
            draw_text(ctx, kMenus[g.menu_open][i], x0 + 24, y0 + 80 + i * 32,
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
    w.i32(g.pl.hp);
    w.i32(g.pl.water);
    w.i32(g.pl.exposure);
    w.i32(g.pl.turn);
    w.bytes(&g.pl.st, sizeof g.pl.st);
    w.bytes(g.pl.inv, sizeof g.pl.inv);
    w.bytes(g.pl.equip, sizeof g.pl.equip);
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
    w.i32(g.sideobj_kind);
    w.i32(g.sideobj_zone);
    w.i32(g.sideobj_x);
    w.i32(g.sideobj_y);
    w.i32(g.sideobj_expires);
    w.u8(g.sideobj_spawned ? 1 : 0);
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
    w.bytes(g.zone_.vis, sizeof g.zone_.vis);
}

bool game_deserialize(SaveReader& r) {
    const uint64_t seed = r.u64();
    const uint8_t cur = r.u8();
    Player pl;
    pl.hp = r.i32();
    pl.water = r.i32();
    pl.exposure = r.i32();
    pl.turn = r.i32();
    r.bytes(&pl.st, sizeof pl.st);
    r.bytes(pl.inv, sizeof pl.inv);
    r.bytes(pl.equip, sizeof pl.equip);
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
    const int so_kind = r.i32();
    const int so_zone = r.i32();
    const int so_x = r.i32();
    const int so_y = r.i32();
    const int so_exp = r.i32();
    const bool so_spawned = r.u8() != 0;
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
    static uint8_t vis[map::N];
    r.bytes(vis, sizeof vis);
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
    g.sideobj_kind = so_kind;
    g.sideobj_zone = uint8_t(so_zone);
    g.sideobj_x = int16_t(so_x);
    g.sideobj_y = int16_t(so_y);
    g.sideobj_expires = so_exp;
    g.sideobj_spawned = so_spawned;
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
    map::generate(g.zone_, seed, map::ZoneId(cur), sx_, sy_);
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

void game_debug_player_pos(int* x, int* y) {
    if (x) *x = g.ents[0].x;
    if (y) *y = g.ents[0].y;
}

void game_debug_click(int tx, int ty) {  // A4: set a click-to-move goal (headless)
    g.move_goal_x = int16_t(tx);
    g.move_goal_y = int16_t(ty);
}

void game_debug_use_door() { use_door(); }  // headless: leave the sietch (doors are click-only)

void game_debug_rpg(int* level, int* inv_count, int* stash_count) {
    if (level) *level = g.pl.st.level;
    if (inv_count) {
        int n = 0;
        for (const items::Inst& it : g.pl.inv)
            if (!it.empty()) ++n;
        *inv_count = n;
    }
    if (stash_count) *stash_count = g_meta.stash_count;
}

void game_debug_grab_loot() {
    for (Entity& e : g.ents) {
        if (e.type != EType::Loot || e.zone != g.cur_zone) continue;
        for (items::Inst& slot : g.pl.inv)
            if (slot.empty()) {
                slot = loot_of(e);
                e.type = EType::None;
                break;
            }
    }
}

void game_debug_vault_deposit_all() {
    for (items::Inst& it : g.pl.inv)
        if (!it.empty() && g_meta.stash_count < 24) {
            g_meta.stash[g_meta.stash_count++] = it;
            it = items::Inst{};
        }
    meta_save();
}

int game_debug_vault_withdraw_one() {
    if (g_meta.stash_count == 0) return 0;
    for (items::Inst& slot : g.pl.inv)
        if (slot.empty()) {
            slot = g_meta.stash[--g_meta.stash_count];
            meta_save();
            return 1;
        }
    return 0;
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
    g.pl.st.str = 60;
    g.pl.st.dex = 40;
    g.pl.st.vit = 30;
    g.pl.hp = player_max_hp();
}

bool game_debug_boss_is_baron() {
    for (const Entity& e : g.ents)
        if (e.type == EType::Boss && e.zone == g.cur_zone && e.data2 == -1) return true;
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
    g.pl.water = player_max_water();
    g.pl.hp = player_max_hp();
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
    mix(uint64_t(uint16_t(g.pl.st.level)) | (uint64_t(uint32_t(g.pl.st.xp)) << 16) |
        (uint64_t(uint16_t(g.pl.st.str)) << 48));
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
