#pragma once
// Runtime asset registry. The game loads one packed file — data/atlas.bin,
// produced by the asset pipeline (genart/make_assets -> atlas) — containing
// native RGBA (0xAARRGGBB) pages and named sprite entries. No image decoding
// happens in the shipped game. (R3: pages are 32-bit RGBA, not 8-bit indexed.)
#include <cstdint>
#include <vector>

struct SpriteSheet {
    const uint32_t* px = nullptr;  // frame 0's top-left inside an atlas page (ARGB8888)
    int pitch = 0;                 // atlas page width (in pixels)
    int fw = 0, fh = 0;            // frame size
    int frames = 0;
    int ax = 0, ay = 0;            // anchor within a frame

    const uint32_t* frame_ptr(int f) const { return px + f * fw; }
};

class Assets {
public:
    // Loads <data_dir>/atlas.bin. Returns false (with stderr detail) on failure.
    bool load_all(const char* data_dir);
    const SpriteSheet* get(const char* name) const;
    // A loud magenta checkerboard sheet, so a missing key is a visible marker
    // rather than a null deref. Available after load_all().
    const SpriteSheet* placeholder() const { return &placeholder_; }

private:
    struct Entry {
        char name[32];
        SpriteSheet sheet;
    };
    std::vector<std::vector<uint32_t>> pages_;
    std::vector<Entry> entries_;
    std::vector<uint32_t> placeholder_px_;  // owns the placeholder page
    SpriteSheet placeholder_;
};
