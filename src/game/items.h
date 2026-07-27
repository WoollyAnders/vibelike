#pragma once
// X1: itemization. Defs are constexpr tables; a live item instance is 4 bytes
// (def, prefix, suffix, rarity) so it packs into entities, saves, and the
// cross-run vault untouched.
#include <cstdint>
#include <cstdio>

namespace items {

// B2a: a granular Fremen stillsuit paperdoll. Weapon + 8 stillsuit/desert pieces + a Token
// (the ARPG amulet: the character's identity/belief/history).
enum class Slot : uint8_t {
    Weapon = 0, Hood, Mask, NosePlugs, WaterTube, Body, Catchpockets, Pumps, Cloak, Token, Count
};

// a/b meaning per slot: Weapon uses a=dmg_min, b=dmg_max. EVERY other piece carries one
// intrinsic affix (a = AffixKind, b = power) that sums through affix_total, exactly like the
// old Trinket. (Per-slot stat balance is provisional until the D1 affix pass.)
struct Def {
    const char* name;
    Slot slot;
    uint8_t level;
    uint8_t a, b;
    uint16_t value;  // spice
};

enum AffixKind : uint8_t {
    AK_DMG = 0, AK_TOHIT, AK_ARMOR, AK_HP, AK_WATER, AK_NOISE, AK_TOX, AK_DODGE,
    AK_RECLAIM,  // stillsuit water reclamation (reduces thirst drain)
    AK_HEAT,     // heat shielding (reduces sun exposure gain)
};

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
    // token — the amulet of identity (varied affixes)
    {"Water Ring", Slot::Token, 1, AK_WATER, 100, 6},
    {"Hawk-bone Token", Slot::Token, 2, AK_TOHIT, 6, 9},
    {"Bone Talisman", Slot::Token, 3, AK_HP, 8, 11},
    {"Vial of Ibad", Slot::Token, 4, AK_TOX, 25, 14},
};
inline constexpr int kDefCount = int(sizeof(kDefs) / sizeof(kDefs[0]));

struct Affix {
    const char* name;
    uint8_t kind;
    int16_t power;
};
inline constexpr Affix kPrefixes[] = {
    {"Keen", AK_DMG, 2},        {"Guarded", AK_ARMOR, 2},  {"Watered", AK_WATER, 150},
    {"Sure", AK_TOHIT, 8},      {"Still", AK_NOISE, 3},    {"Hale", AK_HP, 6},
    {"Sealed", AK_RECLAIM, 1},  {"Shaded", AK_HEAT, 1},
};
inline constexpr Affix kSuffixes[] = {
    {"of the Hawk", AK_TOHIT, 10},   {"of the Maker", AK_NOISE, 4},
    {"of Ibad", AK_TOX, 20},         {"of the Wind", AK_DODGE, 8},
    {"of the Deep Well", AK_WATER, 250}, {"of the Bull", AK_HP, 10},
    {"of the Dune", AK_RECLAIM, 1},  {"of Noon", AK_HEAT, 1},
};
inline constexpr int kPrefixCount = int(sizeof(kPrefixes) / sizeof(kPrefixes[0]));
inline constexpr int kSuffixCount = int(sizeof(kSuffixes) / sizeof(kSuffixes[0]));

// 4-byte item instance. rarity: 0 common, 1 fine (one affix), 2 rare (both).
struct Inst {
    uint8_t def = 0xFF;
    uint8_t prefix = 0xFF;
    uint8_t suffix = 0xFF;
    uint8_t rarity = 0;

    bool empty() const { return def == 0xFF; }
    const Def& d() const { return kDefs[def]; }

    // Sum of a given affix kind across prefix/suffix/trinket-builtin.
    int affix_power(uint8_t kind) const {
        if (empty()) return 0;
        int p = 0;
        if (prefix != 0xFF && kPrefixes[prefix].kind == kind) p += kPrefixes[prefix].power;
        if (suffix != 0xFF && kSuffixes[suffix].kind == kind) p += kSuffixes[suffix].power;
        if (d().slot != Slot::Weapon && d().a == kind) p += d().b;  // intrinsic piece affix
        return p;
    }

    void full_name(char* out, int n) const {
        if (empty()) { std::snprintf(out, size_t(n), "-"); return; }
        std::snprintf(out, size_t(n), "%s%s%s%s%s",
                      prefix != 0xFF ? kPrefixes[prefix].name : "",
                      prefix != 0xFF ? " " : "", d().name,
                      suffix != 0xFF ? " " : "",
                      suffix != 0xFF ? kSuffixes[suffix].name : "");
    }
};

}  // namespace items
