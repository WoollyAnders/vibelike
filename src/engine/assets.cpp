#include "engine/assets.h"

#include <cstdio>
#include <cstring>

#include "stb_image.h"

namespace {

// 0xRRGGBB -> palette index. Small open-addressing table; palette has at most
// 256 distinct colors.
struct ColorIndex {
    uint32_t rgb[512];
    uint8_t idx[512];
    bool used[512] = {};

    void insert(uint32_t c, uint8_t i) {
        uint32_t h = (c * 2654435761u) & 511;
        while (used[h]) {
            if (rgb[h] == c) { idx[h] = i; return; }  // later insert wins
            h = (h + 1) & 511;
        }
        used[h] = true;
        rgb[h] = c;
        idx[h] = i;
    }
    int find(uint32_t c) const {
        uint32_t h = (c * 2654435761u) & 511;
        while (used[h]) {
            if (rgb[h] == c) return idx[h];
            h = (h + 1) & 511;
        }
        return -1;
    }
};

}  // namespace

bool Assets::load_all(const char* data_dir, const Palette& master) {
    ColorIndex lookup;
    for (int i = 255; i >= 1; --i)  // low indices win duplicate colors
        lookup.insert(master.color[i] & 0xFFFFFF, uint8_t(i));

    char path[512];
    std::snprintf(path, sizeof path, "%s/manifest.txt", data_dir);
    FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "assets: cannot open %s\n", path);
        return false;
    }

    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        Entry e{};
        char file[128];
        if (std::sscanf(line, "%31s %127s %d %d %d %d %d", e.name, file,
                        &e.sheet.fw, &e.sheet.fh, &e.sheet.frames,
                        &e.sheet.ax, &e.sheet.ay) != 7) {
            std::fprintf(stderr, "assets: bad manifest line: %s", line);
            std::fclose(f);
            return false;
        }

        std::snprintf(path, sizeof path, "%s/%s", data_dir, file);
        int w = 0, h = 0, comp = 0;
        unsigned char* rgba = stbi_load(path, &w, &h, &comp, 4);
        if (!rgba) {
            std::fprintf(stderr, "assets: failed to load %s: %s\n", path, stbi_failure_reason());
            std::fclose(f);
            return false;
        }
        if (w != e.sheet.fw * e.sheet.frames || h != e.sheet.fh) {
            std::fprintf(stderr, "assets: %s is %dx%d, expected %dx%d\n", path, w, h,
                         e.sheet.fw * e.sheet.frames, e.sheet.fh);
            stbi_image_free(rgba);
            std::fclose(f);
            return false;
        }

        e.sheet.sheet_w = w;
        e.sheet.px.resize(size_t(w) * h);
        for (int i = 0; i < w * h; ++i) {
            const unsigned char* p = rgba + i * 4;
            if (p[3] < 128) {
                e.sheet.px[i] = 0;
                continue;
            }
            const uint32_t c = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
            const int idx = lookup.find(c);
            if (idx < 0) {
                std::fprintf(stderr,
                             "assets: %s pixel %d,%d color #%06X is not in the palette\n",
                             path, i % w, i / w, c);
                stbi_image_free(rgba);
                std::fclose(f);
                return false;
            }
            e.sheet.px[i] = uint8_t(idx);
        }
        stbi_image_free(rgba);
        entries_.push_back(std::move(e));
    }
    std::fclose(f);
    return true;
}

const SpriteSheet* Assets::get(const char* name) const {
    for (const Entry& e : entries_)
        if (std::strcmp(e.name, name) == 0) return &e.sheet;
    return nullptr;
}
