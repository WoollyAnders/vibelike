#pragma once
// Runtime asset registry. The game loads one packed file — data/atlas.bin,
// produced by the asset pipeline (genart/make_assets -> quantize -> atlas) —
// containing indexed-pixel pages and named sprite entries. No image decoding
// happens in the shipped game.
#include <cstdint>
#include <vector>

struct SpriteSheet {
    const uint8_t* px = nullptr;  // frame 0's top-left inside an atlas page
    int pitch = 0;                // atlas page width
    int fw = 0, fh = 0;           // frame size
    int frames = 0;
    int ax = 0, ay = 0;           // anchor within a frame

    const uint8_t* frame_ptr(int f) const { return px + f * fw; }
};

class Assets {
public:
    // Loads <data_dir>/atlas.bin. Returns false (with stderr detail) on failure.
    bool load_all(const char* data_dir);
    const SpriteSheet* get(const char* name) const;

private:
    struct Entry {
        char name[24];
        SpriteSheet sheet;
    };
    std::vector<std::vector<uint8_t>> pages_;
    std::vector<Entry> entries_;
};
