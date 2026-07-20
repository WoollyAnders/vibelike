#include "engine/palette.h"

#include <cstdio>

bool palette_load(const char* path, Palette& out) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char magic[16] = {};
    int count = 0;
    if (std::fscanf(f, "%15s %d", magic, &count) != 2 || count != 256) {
        std::fclose(f);
        return false;
    }
    for (int i = 0; i < 256; ++i) {
        int r, g, b;
        if (std::fscanf(f, "%d %d %d", &r, &g, &b) != 3) {
            std::fclose(f);
            return false;
        }
        out.color[i] = 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
    }
    std::fclose(f);
    out.color[0] = 0xFF000000u;  // transparent key renders black if ever shown
    return true;
}

static int color_dist2(uint32_t a, int r, int g, int b) {
    const int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    return (ar - r) * (ar - r) + (ag - g) * (ag - g) + (ab - b) * (ab - b);
}

void colormaps_build(const Palette& master, Colormaps& out) {
    for (int level = 0; level < 32; ++level) {
        out.map[level][0] = 0;
        for (int i = 1; i < 256; ++i) {
            const uint32_t c = master.color[i];
            // darken: scale by (level+1)/32
            const int r = int((c >> 16) & 0xFF) * (level + 1) / 32;
            const int g = int((c >> 8) & 0xFF) * (level + 1) / 32;
            const int b = int(c & 0xFF) * (level + 1) / 32;
            if (level == 31) {  // identity by construction
                out.map[level][i] = uint8_t(i);
                continue;
            }
            int best = 1, best_d = color_dist2(master.color[1], r, g, b);
            for (int j = 2; j < 256; ++j) {
                const int d = color_dist2(master.color[j], r, g, b);
                if (d < best_d) { best = j; best_d = d; }
            }
            out.map[level][i] = uint8_t(best);
        }
    }
}

static uint32_t lerp_color(uint32_t c, uint8_t r, uint8_t g, uint8_t b, uint16_t t) {
    // t: 0..256
    const uint32_t cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
    const uint32_t or_ = cr + ((int(r) - int(cr)) * t >> 8);
    const uint32_t og = cg + ((int(g) - int(cg)) * t >> 8);
    const uint32_t ob = cb + ((int(b) - int(cb)) * t >> 8);
    return 0xFF000000u | (or_ << 16) | (og << 8) | ob;
}

void palette_build_lut(const Palette& master, const PaletteFade& fade,
                       int rotate_offset, int rotate_lo, int rotate_hi,
                       uint32_t out[256]) {
    const int span = rotate_hi - rotate_lo + 1;
    for (int i = 0; i < 256; ++i) {
        int src = i;
        if (span > 1 && i >= rotate_lo && i <= rotate_hi)
            src = rotate_lo + (i - rotate_lo + rotate_offset) % span;
        out[i] = master.color[src];
    }
    if (fade.amount > 0)
        for (int i = 0; i < 256; ++i)
            out[i] = lerp_color(out[i], fade.r, fade.g, fade.b, fade.amount);
}

Palette palette_make_default() {
    Palette p{};
    auto set = [&](int i, int r, int g, int b) {
        p.color[i] = 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
    };
    // 0: transparent-key black (index 0 is reserved as transparent for sprites later)
    set(0, 0, 0, 0);
    // 1..31: grayscale ramp
    for (int i = 1; i < 32; ++i) { int v = i * 255 / 31; set(i, v, v, v); }
    // 32..95: sand ramp, dark dune shadow -> blazing highlight
    for (int i = 0; i < 64; ++i) {
        int t = i * 255 / 63;
        set(32 + i, 60 + t * 195 / 255, 40 + t * 175 / 255, 20 + t * 120 / 255);
    }
    // 96..127: rock ramp (cool browns/grays)
    for (int i = 0; i < 32; ++i) {
        int t = i * 255 / 31;
        set(96 + i, 40 + t * 120 / 255, 35 + t * 110 / 255, 32 + t * 105 / 255);
    }
    // 128..159: spice ramp (rust -> cinnamon -> ember orange)
    for (int i = 0; i < 32; ++i) {
        int t = i * 255 / 31;
        set(128 + i, 70 + t * 185 / 255, 25 + t * 105 / 255, 10 + t * 30 / 255);
    }
    // 160..191: sky/night blues
    for (int i = 0; i < 32; ++i) {
        int t = i * 255 / 31;
        set(160 + i, 8 + t * 90 / 255, 12 + t * 120 / 255, 30 + t * 190 / 255);
    }
    // 192..223: blood/alert reds
    for (int i = 0; i < 32; ++i) {
        int t = i * 255 / 31;
        set(192 + i, 40 + t * 215 / 255, 5 + t * 40 / 255, 5 + t * 35 / 255);
    }
    // 224..255: UI golds/greens
    for (int i = 0; i < 16; ++i) {
        int t = i * 255 / 15;
        set(224 + i, 90 + t * 165 / 255, 75 + t * 150 / 255, 20 + t * 60 / 255);
    }
    for (int i = 0; i < 16; ++i) {
        int t = i * 255 / 15;
        set(240 + i, 15 + t * 85 / 255, 50 + t * 170 / 255, 25 + t * 95 / 255);
    }
    return p;
}
