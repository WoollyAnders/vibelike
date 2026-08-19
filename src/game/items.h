#pragma once
// D1: PoE-style itemization. A live item (`Inst`) carries an item level, a rarity, and up to
// 6 tiered explicit affixes (<=3 prefixes + <=3 suffixes) rolled from `kFamilies`. The Def's
// intrinsic a/b remains the item's IMPLICIT mod. `quality`/`enchant` are reserved for D2.
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace items {

// B2a: a granular Fremen stillsuit paperdoll. Weapon + 8 stillsuit/desert pieces + a Token
// (the ARPG amulet: the character's identity/belief/history).
enum class Slot : uint8_t {
    Weapon = 0, Hood, Mask, NosePlugs, WaterTube, Body, Catchpockets, Pumps, Cloak, Token, Count
};
constexpr uint16_t sm(Slot s) { return uint16_t(1u << uint8_t(s)); }  // slot bit for family masks

// G (grid inventory): a gear item's footprint (W x H cells) in the pack/stash grid. User-specified:
// chest/cloak/2H-weapon = 6 (2x3); 1H-weapon/shield/offhand = 3 (1x3); helmet/mask/boots = 4 (2x2);
// tube/belt = 2; plugs/tokens/rings = 1. The game has ONE Weapon slot today (all 1H blades), so weapons
// are 3 cells; the 6-cell (2H) and shield/offhand sizes wait here for when those item classes exist.
struct GridSize { uint8_t w, h; };
inline constexpr GridSize slot_size(Slot s) {
    switch (s) {
        case Slot::Body:         return {2, 3};  // chest = 6
        case Slot::Cloak:        return {2, 3};  // 6
        case Slot::Weapon:       return {1, 3};  // 1H blade = 3 (a 2H would be 2x3)
        case Slot::Hood:         return {2, 2};  // helmet = 4
        case Slot::Mask:         return {2, 2};  // 4
        case Slot::Pumps:        return {2, 2};  // boots = 4
        case Slot::WaterTube:    return {1, 2};  // tube = 2
        case Slot::Catchpockets: return {2, 1};  // belt = 2
        case Slot::NosePlugs:    return {1, 1};
        case Slot::Token:        return {1, 1};
        default:                 return {1, 1};
    }
}

// a/b meaning per slot: Weapon uses a=dmg_min, b=dmg_max. EVERY other piece carries one
// intrinsic (implicit) affix: a = AffixKind, b = power, summed through affix_power.
// C6r: `range` classifies a weapon — 0 = melee (the default; also for all non-weapons), >0 = a ranged
// weapon whose shot travels that many tiles. Appended last so existing 6-field defs value-init it to 0.
struct Def {
    const char* name;
    Slot slot;
    uint8_t level;
    uint8_t a, b;
    uint16_t value;  // spice
    uint8_t range = 0;  // C6r: ranged weapon tile range (0 = melee / non-weapon)
};

// D1: the affix-kind vocabulary. The first block (AK_DMG..AK_SHIELD) predates D1; the rest are
// new. AK_HEAT doubles as Heat Resistance (it already reduced sun exposure). Symbols stay terse in
// code; user-facing strings come from stat_name().
enum AffixKind : uint8_t {
    AK_DMG = 0, AK_TOHIT, AK_ARMOR, AK_HP, AK_WATER, AK_NOISE, AK_TOX, AK_DODGE,
    AK_RECLAIM, AK_HEAT /* = Heat Resistance */, AK_CRIT, AK_SHIELD,
    // --- D1 new ---
    AK_INC_DMG, AK_INC_TOHIT, AK_CRIT_MULT,          // offense %
    AK_LEECH, AK_LIFE_ON_HIT, AK_POISON,             // offense sustain / poison dealt
    AK_LIFE_ON_KILL, AK_FOCUS_ON_KILL,               // on-kill
    AK_INC_ARMOR, AK_INC_LIFE, AK_INC_FOCUS,         // defense %
    AK_LIFE_REGEN, AK_FOCUS_REGEN,                   // regen
    AK_DR, AK_BLOCK, AK_THORNS, AK_INC_DODGE,        // mitigation / evasion
    AK_ADD_COND, AK_ADD_VERS, AK_ADD_AWAR, AK_ADD_ALL,  // attributes
    AK_WILL, AK_WILL_SIPHON,                          // resource
    AK_RES_COLD, AK_RES_STORM, AK_RES_TOX,           // resistances (heat = AK_HEAT)
    AK_THRIFT, AK_WATER_FIND, AK_SPICE_FIND, AK_RARITY, AK_XP,  // desert / find
    AK_STUN_MULT, AK_STUN_REDUCE,                    // RT2: stun buildup (offense) / stun reduction (defense)
    AK_COUNT
};

// Fremen display name for a stat (glossary). Core stats are flavored; situational mechanics stay
// plain. Governs tooltip/char-sheet text only.
inline const char* stat_name(uint8_t k) {
    switch (k) {
        case AK_DMG: return "Lethality";
        case AK_INC_DMG: return "increased Lethality";
        case AK_TOHIT: return "Precision";
        case AK_INC_TOHIT: return "increased Precision";
        case AK_CRIT: return "Vital Strike Chance";
        case AK_CRIT_MULT: return "Vital Strike Damage";
        case AK_LEECH: return "Siphon";
        case AK_LIFE_ON_HIT: return "Life on Hit";
        case AK_POISON: return "Poison Damage";
        case AK_LIFE_ON_KILL: return "Life on Kill";
        case AK_FOCUS_ON_KILL: return "Focus on Kill";
        case AK_ARMOR: return "Toughness";
        case AK_INC_ARMOR: return "increased Toughness";
        case AK_HP: return "Life";
        case AK_INC_LIFE: return "increased Life";
        case AK_SHIELD: return "Focus";
        case AK_INC_FOCUS: return "increased Focus";
        case AK_LIFE_REGEN: return "Recovery";
        case AK_FOCUS_REGEN: return "Focus Recharge";
        case AK_DR: return "Fortitude";
        case AK_BLOCK: return "Block";
        case AK_THORNS: return "Deflect";
        case AK_DODGE: return "Flow";
        case AK_INC_DODGE: return "increased Flow";
        case AK_ADD_COND: return "Conditioning";
        case AK_ADD_VERS: return "Versatility";
        case AK_ADD_AWAR: return "Awareness";
        case AK_ADD_ALL: return "all Attributes";
        case AK_WILL: return "Will";
        case AK_WILL_SIPHON: return "Will Siphon";
        case AK_HEAT: return "Heat Resistance";
        case AK_RES_COLD: return "Cold Resistance";
        case AK_RES_STORM: return "Storm Resistance";
        case AK_RES_TOX: return "Toxin Resistance";
        case AK_RECLAIM: return "Reclamation";
        case AK_THRIFT: return "Reduced Water Use";
        case AK_WATER_FIND: return "Water Find";
        case AK_WATER: return "Water Capacity";
        case AK_NOISE: return "Stealth";
        case AK_TOX: return "Spice Tolerance";
        case AK_SPICE_FIND: return "Spice Find";
        case AK_RARITY: return "Rarity Find";
        case AK_XP: return "Experience Gain";
        case AK_STUN_MULT: return "Stun Buildup";
        case AK_STUN_REDUCE: return "Stun Reduction";
        default: return "?";
    }
}

inline constexpr Def kDefs[] = {
    // weapons (a=dmg_min, b=dmg_max)
    {"Dust Knife", Slot::Weapon, 1, 2, 4, 3},
    {"Maula Blade", Slot::Weapon, 2, 3, 6, 6},
    {"Trench Pick", Slot::Weapon, 3, 4, 8, 10},
    {"Fedaykin Edge", Slot::Weapon, 4, 5, 10, 16},
    {"Crysknife", Slot::Weapon, 6, 7, 13, 30},
    // hood (a=AK_HEAT)
    {"Cloth Headwrap", Slot::Hood, 1, AK_HEAT, 1, 3},
    {"Stillsuit Hood", Slot::Hood, 3, AK_HEAT, 2, 10},
    {"Sietch-woven Hood", Slot::Hood, 5, AK_HEAT, 3, 22},
    // mask (a=AK_RECLAIM, big)
    {"Cracked Mask", Slot::Mask, 1, AK_RECLAIM, 2, 4},
    {"Filter Mask", Slot::Mask, 3, AK_RECLAIM, 3, 12},
    {"Sealed Filtermask", Slot::Mask, 5, AK_RECLAIM, 4, 26},
    // nose-plugs (a=AK_RECLAIM, small)
    {"Rough Filt-plugs", Slot::NosePlugs, 1, AK_RECLAIM, 1, 2},
    {"Fitted Filt-plugs", Slot::NosePlugs, 3, AK_RECLAIM, 1, 7},
    {"Sietch Filt-plugs", Slot::NosePlugs, 5, AK_RECLAIM, 2, 16},
    // water-tube (a=AK_WATER)
    {"Frayed Catchtube", Slot::WaterTube, 1, AK_WATER, 60, 4},
    {"Catchtube", Slot::WaterTube, 3, AK_WATER, 120, 12},
    {"Sealed Catchtube", Slot::WaterTube, 5, AK_WATER, 200, 24},
    // body (a=AK_ARMOR, the main piece)
    {"Patched Suit-body", Slot::Body, 1, AK_ARMOR, 2, 6},
    {"Stillsuit Body", Slot::Body, 3, AK_ARMOR, 4, 16},
    {"Master-woven Body", Slot::Body, 5, AK_ARMOR, 6, 34},
    // catchpockets (a=AK_WATER, capacity)
    {"Loose Pockets", Slot::Catchpockets, 1, AK_WATER, 80, 4},
    {"Deep Catchpockets", Slot::Catchpockets, 3, AK_WATER, 160, 14},
    // heel-pumps (a=AK_DODGE, mobility)
    {"Worn Heel-pumps", Slot::Pumps, 1, AK_DODGE, 3, 5},
    {"Temag Boots", Slot::Pumps, 3, AK_DODGE, 6, 15},
    // jubba cloak (a=AK_NOISE, stealth)
    {"Tattered Cloak", Slot::Cloak, 1, AK_NOISE, 2, 6},
    {"Jubba Cloak", Slot::Cloak, 3, AK_NOISE, 3, 18},
    // token — the amulet of identity (varied implicits)
    {"Water Ring", Slot::Token, 1, AK_WATER, 100, 6},
    {"Hawk-bone Token", Slot::Token, 2, AK_TOHIT, 6, 9},
    {"Bone Talisman", Slot::Token, 3, AK_HP, 8, 11},
    {"Vial of Ibad", Slot::Token, 4, AK_TOX, 25, 14},
    {"Bloodstone Charm", Slot::Token, 5, AK_CRIT, 10, 20},     // gear source of crit (renamed off "Kindjal")
    {"Sayyadina's Eye", Slot::Token, 6, AK_SHIELD, 14, 30},    // +Focus (energy shield)
    // C6r: ranged weapons (a=dmg_min, b=dmg_max, range>0). APPENDED at the end — def indices are stored
    // in saves + hard-referenced by the starting-gear equips, so never reorder. They join the weapon loot
    // pool naturally (roll_item filters by level, not by a contiguous weapon block).
    {"Maula Pistol", Slot::Weapon, 2, 2, 5, 9, 8},
    {"Ixian Dartgun", Slot::Weapon, 4, 4, 8, 22, 10},
    {"Lasgun", Slot::Weapon, 6, 7, 14, 40, 12},
};
inline constexpr int kDefCount = int(sizeof(kDefs) / sizeof(kDefs[0]));

// ---- D1 tiered affix families -------------------------------------------------------------
inline constexpr int kMaxTier = 5, kMaxPrefix = 3, kMaxSuffix = 3, kMaxAffix = kMaxPrefix + kMaxSuffix;
inline constexpr int kMaxSockets = 3;  // D5: max stat-gem sockets on a piece of gear
// Global tier gates + spawn-weight curve: T1 rolls at Awaken 1+, T5 at 9+; higher tiers are rarer.
inline constexpr uint8_t kTierMinIlvl[kMaxTier] = {1, 3, 5, 7, 9};
inline constexpr uint16_t kTierWeight[kMaxTier] = {100, 70, 45, 25, 10};

struct AffixFamily {
    uint8_t kind;
    bool prefix;            // true = prefix, false = suffix
    uint16_t slots;         // OR of sm(Slot)
    const char* word;       // magic-item name fragment: "Keen" / "of the Hawk"
    uint16_t weight;        // family spawn weight (scarce mods low)
    int16_t lo[kMaxTier];
    int16_t hi[kMaxTier];
};

// ~40 families across OFFENSE / DEFENSE / RESISTANCE / ATTRIBUTE / DESERT. Values provisional.
inline constexpr AffixFamily kFamilies[] = {
    // ---- OFFENSE ----
    {AK_DMG,      true,  sm(Slot::Weapon)|sm(Slot::Token), "Keen", 1000,
        {1,2,4,6,9}, {2,4,6,9,13}},
    {AK_INC_DMG,  true,  sm(Slot::Weapon)|sm(Slot::Token), "Savage", 700,
        {5,10,16,24,32}, {10,16,24,32,45}},
    {AK_TOHIT,    true,  sm(Slot::Weapon)|sm(Slot::Token), "Sure", 900,
        {4,6,9,13,18}, {6,9,13,18,24}},
    {AK_INC_TOHIT,false, sm(Slot::Weapon)|sm(Slot::Token), "of the Hawk", 700,
        {6,10,16,24,32}, {10,16,24,32,42}},
    {AK_CRIT,     false, sm(Slot::Weapon)|sm(Slot::Token), "of the Cull", 500,
        {3,5,7,9,12}, {4,6,8,11,15}},
    {AK_CRIT_MULT,false, sm(Slot::Weapon)|sm(Slot::Token), "of Slaughter", 500,
        {8,14,22,32,44}, {14,22,32,44,58}},
    {AK_LEECH,    true,  sm(Slot::Weapon)|sm(Slot::Token), "Vicious", 400,
        {1,2,3,4,6}, {2,3,4,6,8}},
    {AK_LIFE_ON_HIT,false, sm(Slot::Weapon)|sm(Slot::Token)|sm(Slot::Body), "of Sustenance", 400,
        {1,2,3,5,7}, {2,3,5,7,10}},
    {AK_POISON,   false, sm(Slot::Weapon)|sm(Slot::Mask)|sm(Slot::Token), "of Venom", 400,
        {10,15,25,40,60}, {15,25,40,60,85}},
    {AK_LIFE_ON_KILL,true, sm(Slot::Weapon)|sm(Slot::Body)|sm(Slot::Token), "Bloodthirsty", 500,
        {1,2,4,6,9}, {2,4,6,9,13}},
    {AK_FOCUS_ON_KILL,true, sm(Slot::Hood)|sm(Slot::Token), "Rapt", 400,
        {2,4,7,11,16}, {4,7,11,16,22}},
    // ---- DEFENSE ----
    {AK_ARMOR,    true,  sm(Slot::Body)|sm(Slot::Cloak)|sm(Slot::Hood), "Guarded", 1000,
        {1,3,5,8,12}, {2,4,7,11,16}},
    {AK_INC_ARMOR,true,  sm(Slot::Body)|sm(Slot::Cloak), "Plated", 700,
        {8,14,22,32,44}, {14,22,32,44,58}},
    {AK_HP,       true,  sm(Slot::Body)|sm(Slot::Token)|sm(Slot::Hood)|sm(Slot::Cloak), "Hale", 1000,
        {4,7,11,16,23}, {6,10,15,22,30}},
    {AK_INC_LIFE, true,  sm(Slot::Body)|sm(Slot::Token), "Vital", 700,
        {5,9,14,20,28}, {9,14,20,28,38}},
    {AK_SHIELD,   true,  sm(Slot::Hood)|sm(Slot::Body)|sm(Slot::Token), "Warded", 800,
        {4,9,15,23,33}, {8,14,22,32,45}},
    {AK_INC_FOCUS,true,  sm(Slot::Hood)|sm(Slot::Token), "Shielding", 600,
        {8,14,22,32,44}, {14,22,32,44,58}},
    {AK_LIFE_REGEN,false, sm(Slot::Body)|sm(Slot::Token), "of Recovery", 500,
        {1,1,2,3,4}, {1,2,3,4,6}},
    {AK_FOCUS_REGEN,false, sm(Slot::Hood)|sm(Slot::Token), "of Serenity", 500,
        {10,20,35,55,80}, {20,35,55,80,110}},
    {AK_DR,       false, sm(Slot::Body)|sm(Slot::Cloak), "of Stone", 350,
        {2,3,5,7,10}, {3,5,7,10,14}},
    {AK_BLOCK,    false, sm(Slot::Body)|sm(Slot::Cloak)|sm(Slot::Weapon), "of the Wall", 400,
        {3,5,8,12,16}, {5,8,12,16,22}},
    {AK_THORNS,   true,  sm(Slot::Body)|sm(Slot::Pumps), "Barbed", 350,
        {1,2,4,7,11}, {2,4,7,11,16}},
    {AK_DODGE,    true,  sm(Slot::Pumps)|sm(Slot::Cloak), "Runner's", 900,
        {3,5,8,12,17}, {5,8,12,17,23}},
    {AK_INC_DODGE,false, sm(Slot::Pumps)|sm(Slot::Cloak)|sm(Slot::Token), "of the Wind", 700,
        {6,10,16,24,32}, {10,16,24,32,42}},
    // ---- RESISTANCES ---- (heat/cold/storm cap 75%; toxin low weight+values = hard to cap)
    {AK_HEAT,     false, sm(Slot::Hood)|sm(Slot::Cloak), "of Noon", 700,
        {6,10,15,22,30}, {10,15,22,30,40}},
    {AK_RES_COLD, false, sm(Slot::Body)|sm(Slot::Cloak)|sm(Slot::Hood), "of the Long Night", 700,
        {6,10,15,22,30}, {10,15,22,30,40}},
    {AK_RES_STORM,false, sm(Slot::Cloak)|sm(Slot::Pumps)|sm(Slot::Body), "of the Coriolis", 700,
        {6,10,15,22,30}, {10,15,22,30,40}},
    {AK_RES_TOX,  false, sm(Slot::Mask)|sm(Slot::NosePlugs)|sm(Slot::Token), "of the Antidote", 220,
        {3,5,8,11,15}, {5,8,11,15,20}},
    // ---- ATTRIBUTES ----
    {AK_ADD_COND, false, sm(Slot::Body)|sm(Slot::Token)|sm(Slot::Weapon), "of the Sietch", 500,
        {1,2,4,6,9}, {2,4,6,9,12}},
    {AK_ADD_VERS, false, sm(Slot::Pumps)|sm(Slot::Token)|sm(Slot::Weapon), "of Cunning", 500,
        {1,2,4,6,9}, {2,4,6,9,12}},
    {AK_ADD_AWAR, false, sm(Slot::Hood)|sm(Slot::Token)|sm(Slot::Mask), "of Muad'Dib", 500,
        {1,2,4,6,9}, {2,4,6,9,12}},
    {AK_ADD_ALL,  false, sm(Slot::Token), "of the Naib", 150,
        {1,1,2,3,4}, {1,2,3,4,6}},
    {AK_WILL,     false, sm(Slot::Hood)|sm(Slot::Token), "of Will", 400,
        {4,6,10,15,22}, {6,10,15,22,30}},
    {AK_WILL_SIPHON,false, sm(Slot::Weapon)|sm(Slot::Token), "of the Trance", 350,
        {1,2,3,5,7}, {2,3,5,7,10}},
    // ---- DESERT / CHASE ----
    {AK_WATER,    true,  sm(Slot::WaterTube)|sm(Slot::Catchpockets)|sm(Slot::Token), "Capacious", 800,
        {60,100,160,240,340}, {100,160,240,340,460}},
    {AK_RECLAIM,  false, sm(Slot::Mask)|sm(Slot::NosePlugs)|sm(Slot::Body), "of the Dune", 600,
        {1,1,1,2,2}, {1,1,2,2,3}},
    {AK_THRIFT,   false, sm(Slot::Mask)|sm(Slot::NosePlugs)|sm(Slot::WaterTube), "of Thrift", 500,
        {5,8,12,18,25}, {8,12,18,25,35}},
    {AK_WATER_FIND,false, sm(Slot::Catchpockets)|sm(Slot::Token), "of the Deathstill", 400,
        {10,20,35,55,80}, {20,35,55,80,110}},
    {AK_NOISE,    false, sm(Slot::Cloak)|sm(Slot::Pumps), "of the Maker", 500,
        {2,3,4,6,8}, {3,4,6,8,11}},
    {AK_TOX,      false, sm(Slot::Token)|sm(Slot::Mask), "of Ibad", 400,
        {15,20,30,45,65}, {20,30,45,65,90}},
    {AK_SPICE_FIND,false, sm(Slot::Token)|sm(Slot::Catchpockets), "of Plenty", 300,
        {8,14,22,32,44}, {14,22,32,44,58}},
    {AK_RARITY,   false, sm(Slot::Token)|sm(Slot::Cloak), "of Fortune", 250,
        {5,9,14,20,28}, {9,14,20,28,38}},
    {AK_XP,       false, sm(Slot::Token)|sm(Slot::Hood), "of Insight", 300,
        {5,9,14,20,28}, {9,14,20,28,38}},
    // ---- RT2: STUN ---- (offense buildup on weapons; defensive reduction on armor)
    {AK_STUN_MULT,  true,  sm(Slot::Weapon)|sm(Slot::Token), "Staggering", 350,
        {10,18,28,40,55}, {18,28,40,55,75}},
    {AK_STUN_REDUCE,false, sm(Slot::Body)|sm(Slot::Hood)|sm(Slot::Token), "of Poise", 350,
        {8,14,22,32,44}, {14,22,32,44,58}},
};
inline constexpr int kFamilyCount = int(sizeof(kFamilies) / sizeof(kFamilies[0]));

inline const AffixFamily* family_of(uint8_t kind) {
    for (const AffixFamily& f : kFamilies)
        if (f.kind == kind) return &f;
    return nullptr;
}

// Generated rare titles (avoid weapon-type + ocean/water-abundance words).
inline constexpr const char* kRareAdj[8] = {
    "Sietch-forged", "Sunbaked", "Dust-worn", "Sacred", "Ancient", "Fremen", "Storm-worn", "Blessed"};
inline constexpr const char* kRareNoun[8] = {
    "Vow", "Whisper", "Litany", "Rite", "Vision", "Oath", "Requiem", "Passage"};

enum Rarity : uint8_t { R_COMMON = 0, R_MAGIC = 1, R_RARE = 2 };

struct RolledAffix {                 // one explicit mod actually on an item
    uint8_t kind = 0xFF;             // AffixKind; 0xFF = empty
    uint8_t tier = 0;                // 1..kMaxTier
    int16_t value = 0;               // rolled magnitude
    bool empty() const { return kind == 0xFF; }
};

// D3: PoE-style crafting currency. Each currency is a stacking inventory item (stored in Inst.currency
// + Inst.stack) that applies one PoE orb operation at the sietch bench. `min_alvl` gates drops (PoE
// area-level logic); `weight` sets drop rarity (higher = more common). Names are Fremen stillsuit /
// fabrication materials (see the plan).
enum Currency : uint8_t {
    CUR_NONE = 0,
    CUR_WISDOM,      // Ancient Scroll        — Scroll of Wisdom (identify)
    CUR_TRANSMUTE,   // Melange Residue        — Transmutation
    CUR_AUGMENT,     // Fibrous Biopolymer     — Augmentation
    CUR_ALTERATION,  // Filterweave            — Alteration
    CUR_FLASKQUAL,   // Distillate Microtubing — Glassblower (flask quality)
    CUR_ALCHEMY,     // Thermal Filament       — Alchemy
    CUR_SCOURING,    // Scouring Sand          — Scouring
    CUR_REGAL,       // Plasteel Scrap         — Regal
    CUR_BLESSED,     // Osmotic Membrane       — Blessed (implicit reroll)
    CUR_ARMORQUAL,   // Maker Hide             — Armourer's Scrap (armor quality)
    CUR_WEAPONQUAL,  // Worm Tooth             — Blacksmith's Whetstone (weapon quality)
    CUR_CHAOS,       // Shigawire Coil         — Chaos
    CUR_ANNUL,       // Electrolyte Solvent    — Annulment
    CUR_EXALT,       // Precipitator Cell      — Exalted
    CUR_FRACTURE,    // Ixian Mechanism        — Fracturing
    CUR_VAAL,        // Unstable Catalyst      — Vaal (corrupt)
    CUR_DIVINE,      // Refined Duraluminum    — Divine (value reroll)
    CUR_MIRROR,      // Echo of Shai'Hulud     — Mirror (duplicate)
    CUR_COUNT
};
struct CurrencyDef {
    const char* name;
    uint8_t min_alvl;   // minimum area level at which this currency can drop
    uint8_t weight;     // drop weight (relative rarity; higher = more common)
    const char* effect; // short bench-screen description
};
inline constexpr CurrencyDef kCurrency[CUR_COUNT] = {
    {"", 0, 0, ""},
    {"Ancient Scroll", 1, 100, "Identify an unidentified item"},
    {"Melange Residue", 1, 100, "Upgrade a Normal item to Magic"},
    {"Fibrous Biopolymer", 1, 90, "Add a mod to a Magic item"},
    {"Filterweave", 2, 80, "Reforge a Magic item's mods"},
    {"Distillate Microtubing", 2, 40, "Improve a flask's quality"},
    {"Thermal Filament", 3, 55, "Upgrade a Normal item to Rare"},
    {"Scouring Sand", 3, 45, "Strip an item back to Normal"},
    {"Plasteel Scrap", 4, 45, "Upgrade a Magic item to Rare"},
    {"Osmotic Membrane", 4, 35, "Reroll an item's implicit values"},
    {"Maker Hide", 5, 40, "Improve armour quality"},
    {"Worm Tooth", 5, 40, "Improve weapon quality"},
    {"Shigawire Coil", 5, 30, "Reforge a Rare item's mods"},
    {"Electrolyte Solvent", 6, 22, "Remove a random mod"},
    {"Precipitator Cell", 7, 14, "Add a mod to a Rare item"},
    {"Ixian Mechanism", 8, 8, "Fracture: lock one random mod"},
    {"Unstable Catalyst", 8, 6, "Corrupt: a risky random outcome"},
    {"Refined Duraluminum", 9, 4, "Reroll an item's mod values"},
    {"Echo of Shai'Hulud", 12, 1, "Duplicate an item"},
};
inline const char* currency_name(uint8_t c) { return c > 0 && c < CUR_COUNT ? kCurrency[c].name : "?"; }

// D5: stat-gems (the Dune equivalent of D4/PoE2 gems — Ixian cores + cut spice-gems). A gem drops as a
// loose 1x1 item; socketed into gear at the bench it grants a flat AffixKind bonus (folded into
// affix_power, so affix_total picks it up). Pure stats — skills live in the class trees. index 0 = none.
enum GemKind : uint8_t {
    GEM_NONE = 0,
    GEM_SAPHO, GEM_IXCORE, GEM_BLOODOPAL, GEM_SUNSTONE, GEM_WATERPEARL,
    GEM_STORMGLASS, GEM_SPICERUBY, GEM_SANDEMERALD, GEM_COUNT
};
struct GemDef { const char* name; uint8_t kind; int16_t value; uint8_t min_alvl; const char* effect; };
inline constexpr GemDef kGems[GEM_COUNT] = {
    {"", 0, 0, 0, ""},
    {"Cut Sapho",    AK_INC_DMG,     8, 3, "+8% Lethality"},
    {"Ixian Core",   AK_INC_TOHIT,   8, 3, "+8% Precision"},
    {"Blood Opal",   AK_CRIT,        5, 4, "+5% Vital Strike chance"},
    {"Sun-Stone",    AK_INC_LIFE,    8, 4, "+8% Life"},
    {"Water Pearl",  AK_HP,         18, 4, "+18 Life"},
    {"Storm Glass",  AK_RES_STORM,  15, 5, "+15 Storm Resistance"},
    {"Spice Ruby",   AK_SPICE_FIND, 20, 5, "+20% Spice Find"},
    {"Sand Emerald", AK_INC_DODGE,   8, 5, "+8% Flow (evasion)"},
};
inline const char* gem_name(uint8_t g) { return g > 0 && g < GEM_COUNT ? kGems[g].name : "?"; }

// D1/D3 item instance. rarity 0 common / 1 magic / 2 rare. A currency stack sets `currency` (a
// Currency kind) + `stack`, leaving `def` = 0xFF; gear leaves `currency` = 0.
struct Inst {
    uint8_t def = 0xFF;
    uint8_t rarity = R_COMMON;
    uint8_t ilvl = 1;
    uint8_t nprefix = 0, nsuffix = 0;
    uint8_t rare_name = 0;           // adj | (noun<<4)
    uint8_t quality = 0;             // D3: item quality % (0..20); armour/weapon quality raise it
    uint8_t enchant = 0;             // D3: implicit value as % of the def default (0 = default 100%); Blessed rerolls it
    uint8_t currency = 0;            // D3: 0 = gear; else a Currency kind (this slot is a currency stack)
    uint16_t stack = 0;              // D3: currency stack count (currency slots only)
    uint8_t fractured = 0;           // D3: per-affix lock bitmask (bits 0-2 prefix, 3-5 suffix)
    uint8_t istate = 0;              // D3: bit0 unidentified, bit1 corrupted, bit2 mirrored
    RolledAffix pre[kMaxPrefix];
    RolledAffix suf[kMaxSuffix];
    uint8_t gem = 0;                     // D5: a loose stat-gem (GemKind); 0 = not a gem (this is gear/currency)
    uint8_t sockets = 0;                 // D5: socket count on this gear (0..kMaxSockets)
    uint8_t socketed[kMaxSockets] = {};  // D5: the gem in each socket (GemKind; 0 = empty)

    bool is_currency() const { return currency != 0; }
    bool is_gem() const { return gem != 0; }  // D5: a loose (unsocketed) stat-gem item
    bool empty() const { return def == 0xFF && currency == 0 && gem == 0; }
    const Def& d() const { return kDefs[def]; }
    // G: this item's grid footprint (currency / gem / empty = 1x1; gear by its slot).
    GridSize size() const { return (is_currency() || is_gem() || empty()) ? GridSize{1, 1} : slot_size(d().slot); }
    // C6r: a ranged weapon (its shot travels d().range tiles). Melee weapons + non-weapons are false.
    bool is_ranged_weapon() const {
        return !empty() && !is_currency() && d().slot == Slot::Weapon && d().range > 0;
    }
    bool unidentified() const { return (istate & 1) != 0; }  // D3
    bool corrupted() const { return (istate & 2) != 0; }     // D3
    bool mirrored() const { return (istate & 4) != 0; }      // D3
    // A fractured affix (bit set in `fractured`) is locked: reroll/strip ops must preserve it.
    bool frac_pre(int i) const { return (fractured & (1 << i)) != 0; }
    bool frac_suf(int i) const { return (fractured & (1 << (3 + i))) != 0; }

    // implicit (Def a/b for non-weapon) + every rolled affix of this kind.
    int affix_power(uint8_t kind) const {
        if (empty() || is_currency() || is_gem()) return 0;
        int p = 0;
        if (d().slot != Slot::Weapon && d().a == kind) {  // implicit (D3: Blessed % + quality boost the local mod)
            int imp = d().b * (enchant == 0 ? 100 : enchant) / 100;
            imp = imp * (100 + quality) / 100;
            p += imp;
        }
        for (int i = 0; i < nprefix; ++i) if (pre[i].kind == kind) p += pre[i].value;
        for (int i = 0; i < nsuffix; ++i) if (suf[i].kind == kind) p += suf[i].value;
        for (int i = 0; i < sockets && i < kMaxSockets; ++i)  // D5: socketed stat-gems
            if (socketed[i] && kGems[socketed[i]].kind == kind) p += kGems[socketed[i]].value;
        return p;
    }

    void full_name(char* out, int n) const {
        if (is_currency()) {  // D3: "Filterweave x24"
            if (stack > 1) std::snprintf(out, size_t(n), "%s x%u", currency_name(currency), unsigned(stack));
            else std::snprintf(out, size_t(n), "%s", currency_name(currency));
            return;
        }
        if (is_gem()) { std::snprintf(out, size_t(n), "%s", gem_name(gem)); return; }  // D5
        if (empty()) { std::snprintf(out, size_t(n), "-"); return; }
        if (unidentified()) { std::snprintf(out, size_t(n), "Unidentified %s", d().name); return; }
        if (rarity == R_RARE) {
            std::snprintf(out, size_t(n), "%s %s",
                          kRareAdj[rare_name & 7], kRareNoun[(rare_name >> 4) & 7]);
            return;
        }
        const char* pfx = (nprefix > 0) ? family_of(pre[0].kind)->word : "";
        const char* sfx = (nsuffix > 0) ? family_of(suf[0].kind)->word : "";
        std::snprintf(out, size_t(n), "%s%s%s%s%s",
                      *pfx ? pfx : "", *pfx ? " " : "", d().name,
                      *sfx ? " " : "", *sfx ? sfx : "");
    }

    // Multi-line detail for tooltips: "ilvl N" then one line per rolled mod. Returns line count.
    int describe(char* out, int cap) const {
        if (is_currency()) {  // D3: currency stack — name + count + what it does
            std::snprintf(out, size_t(cap), "%s (x%u)\n%s\n", currency_name(currency),
                          unsigned(stack), kCurrency[currency].effect);
            return 2;
        }
        if (is_gem()) {  // D5: a loose stat-gem — name + what it grants when socketed
            std::snprintf(out, size_t(cap), "%s (gem)\n%s\n", gem_name(gem), kGems[gem].effect);
            return 2;
        }
        if (empty()) { if (cap) out[0] = 0; return 0; }
        if (unidentified()) { std::snprintf(out, size_t(cap), "Unidentified %s\n", d().name); return 1; }
        int off = 0, lines = 0;
        auto add = [&](const char* fmt, int v, const char* nm, int t) {
            if (off >= cap) return;
            off += std::snprintf(out + off, size_t(cap - off), fmt, v, nm, t);
            ++lines;
        };
        if (off < cap) { off += std::snprintf(out + off, size_t(cap - off),
                                              "item level %d\n", int(ilvl)); ++lines; }
        for (int i = 0; i < nprefix; ++i)
            add("+%d %s (T%d)\n", pre[i].value, stat_name(pre[i].kind), pre[i].tier);
        for (int i = 0; i < nsuffix; ++i)
            add("+%d %s (T%d)\n", suf[i].value, stat_name(suf[i].kind), suf[i].tier);
        for (int i = 0; i < sockets && i < kMaxSockets && off < cap; ++i) {  // D5: socket lines
            off += std::snprintf(out + off, size_t(cap - off), "socket: %s\n",
                                 socketed[i] ? gem_name(socketed[i]) : "(empty)");
            ++lines;
        }
        return lines;
    }
};

}  // namespace items
