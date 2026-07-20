#pragma once
// Runtime asset registry. Loads data/manifest.txt, which names every sprite
// sheet: PNGs are converted to palette indices at load (exact RGB match
// against the master palette; off-palette pixels are a hard error so art
// stays honest). Index 0 / transparent alpha = color key.
#include <cstdint>
#include <vector>

#include "engine/palette.h"

struct SpriteSheet {
    std::vector<uint8_t> px;  // indexed pixels, frames laid out horizontally
    int fw = 0, fh = 0;       // frame size
    int frames = 0;
    int ax = 0, ay = 0;       // anchor within a frame (the point placed at draw x,y)
    int sheet_w = 0;          // fw * frames

    const uint8_t* frame_ptr(int f) const { return px.data() + f * fw; }
    int pitch() const { return sheet_w; }
};

class Assets {
public:
    // data_dir e.g. "data". Returns false (with stderr detail) on any failure.
    bool load_all(const char* data_dir, const Palette& master);
    const SpriteSheet* get(const char* name) const;

private:
    struct Entry {
        char name[32];
        SpriteSheet sheet;
    };
    std::vector<Entry> entries_;
};
