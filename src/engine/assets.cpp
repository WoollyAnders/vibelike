#include "engine/assets.h"

#include <cstdio>
#include <cstring>

namespace {
struct AtlasHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t page_count;
    uint16_t entry_count;
    uint16_t page_w;
    uint16_t page_h;
};
#pragma pack(push, 1)
struct AtlasEntry {
    char name[24];
    uint16_t fw, fh, frames;
    int16_t ax, ay;
    uint16_t page, x, y;
};
#pragma pack(pop)
static_assert(sizeof(AtlasEntry) == 40, "atlas entry layout");
}  // namespace

bool Assets::load_all(const char* data_dir) {
    char path[512];
    std::snprintf(path, sizeof path, "%s/atlas.bin", data_dir);
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "assets: cannot open %s\n", path);
        return false;
    }

    AtlasHeader hdr{};
    if (std::fread(&hdr.magic, 4, 1, f) != 1 || std::fread(&hdr.version, 2, 1, f) != 1 ||
        std::fread(&hdr.page_count, 2, 1, f) != 1 ||
        std::fread(&hdr.entry_count, 2, 1, f) != 1 ||
        std::fread(&hdr.page_w, 2, 1, f) != 1 || std::fread(&hdr.page_h, 2, 1, f) != 1 ||
        hdr.magic != 0x4C544144u || hdr.version != 1) {
        std::fprintf(stderr, "assets: %s is not a v1 atlas\n", path);
        std::fclose(f);
        return false;
    }

    pages_.resize(hdr.page_count);
    for (auto& page : pages_) {
        page.resize(size_t(hdr.page_w) * hdr.page_h);
        if (std::fread(page.data(), 1, page.size(), f) != page.size()) {
            std::fprintf(stderr, "assets: truncated atlas pages\n");
            std::fclose(f);
            return false;
        }
    }

    entries_.resize(hdr.entry_count);
    for (Entry& e : entries_) {
        AtlasEntry ae{};
        if (std::fread(&ae, sizeof ae, 1, f) != 1 || ae.page >= hdr.page_count) {
            std::fprintf(stderr, "assets: truncated/invalid atlas entries\n");
            std::fclose(f);
            return false;
        }
        std::memcpy(e.name, ae.name, sizeof e.name);
        e.sheet.px = pages_[ae.page].data() + size_t(ae.y) * hdr.page_w + ae.x;
        e.sheet.pitch = hdr.page_w;
        e.sheet.fw = ae.fw;
        e.sheet.fh = ae.fh;
        e.sheet.frames = ae.frames;
        e.sheet.ax = ae.ax;
        e.sheet.ay = ae.ay;
    }
    std::fclose(f);
    return true;
}

const SpriteSheet* Assets::get(const char* name) const {
    for (const Entry& e : entries_)
        if (std::strcmp(e.name, name) == 0) return &e.sheet;
    return nullptr;
}
