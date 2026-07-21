#pragma once
// X1: itemization. Defs are constexpr tables; a live item instance is 4 bytes
// (def, prefix, suffix, rarity) so it packs into entities, saves, and the
// cross-run vault untouched.
#include <cstdint>
#include <cstdio>

namespace items {

enum class Slot : uint8_t { Weapon = 0, Armor, Suit, Trinket, Count };

// a/b meaning per slot — Weapon: dmg min/max. Armor: armor/0.
// Suit: thirst multiplier in quarters (4 = none, 1 = Stillsuit III)/0.
// Trinket: affix-kind/power (a built-in effect).
struct Def {
    const char* name;
    Slot slot;
    uint8_t level;
    uint8_t a, b;
    uint16_t value;  // spice
};

enum AffixKind : uint8_t {
    AK_DMG = 0, AK_TOHIT, AK_ARMOR, AK_HP, AK_WATER, AK_NOISE, AK_TOX, AK_DODGE,
};

inline constexpr Def kDefs[] = {
    // weapons
    {"Dust Knife", Slot::Weapon, 1, 2, 4, 3},
    {"Maula Blade", Slot::Weapon, 2, 3, 6, 6},
    {"Trench Pick", Slot::Weapon, 3, 4, 8, 10},
    {"Fedaykin Edge", Slot::Weapon, 4, 5, 10, 16},
    {"Crysknife", Slot::Weapon, 6, 7, 13, 30},
    // armor
    {"Rag Wraps", Slot::Armor, 1, 1, 0, 2},
    {"Chitin Vest", Slot::Armor, 2, 3, 0, 6},
    {"Scavenged Plate", Slot::Armor, 4, 5, 0, 14},
    {"Carapace Plate", Slot::Armor, 6, 8, 0, 28},
    // stillsuits (a = thirst multiplier x/4)
    {"Stillsuit Mk I", Slot::Suit, 1, 3, 0, 8},
    {"Stillsuit Mk II", Slot::Suit, 3, 2, 0, 20},
    {"Stillsuit Mk III", Slot::Suit, 5, 1, 0, 45},
    // trinkets (a = AffixKind, b = power)
    {"Water Ring", Slot::Trinket, 1, AK_WATER, 100, 6},
    {"Sand Charm", Slot::Trinket, 2, AK_NOISE, 3, 8},
    {"Hawk Fetish", Slot::Trinket, 2, AK_TOHIT, 6, 9},
    {"Bone Talisman", Slot::Trinket, 3, AK_HP, 8, 11},
    {"Vial of Ibad", Slot::Trinket, 4, AK_TOX, 25, 14},
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
};
inline constexpr Affix kSuffixes[] = {
    {"of the Hawk", AK_TOHIT, 10},   {"of the Maker", AK_NOISE, 4},
    {"of Ibad", AK_TOX, 20},         {"of the Wind", AK_DODGE, 8},
    {"of the Deep Well", AK_WATER, 250}, {"of the Bull", AK_HP, 10},
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
        if (d().slot == Slot::Trinket && d().a == kind) p += d().b;
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
