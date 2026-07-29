// Godot-spike step 0: prove the DUSTFALL sim runs deterministically as a plain, SDL-free
// C++ library (no platform/app.cpp, no SDL). Replicates run_dettest's sequence and prints the
// hash; it must equal the SDL build's baseline 5dc9443f6cf23331. If it does, the sim ports to a
// GDExtension cleanly (this harness IS the GDExtension's core, minus godot-cpp).
#include <cstdio>
#include <string>

#include "engine/animator.h"
#include "engine/assets.h"
#include "engine/audio.h"
#include "engine/palette.h"
#include "game/game.h"
#include "platform/input.h"

int main(int argc, char** argv) {
    const char* data_dir = argc > 1 ? argv[1] : "data";

    Palette master;
    if (!palette_load((std::string(data_dir) + "/palette.pal").c_str(), master))
        master = palette_make_default();
    Colormaps cmaps;
    colormaps_build(master, cmaps);
    Assets assets;
    if (!assets.load_all(data_dir)) { std::printf("assets.load_all failed\n"); return 2; }
    static anim::Registry anims;
    anims.load(data_dir, assets);
    audio::Mixer mixer;  // pure FM synth object; no device needed (only app.cpp opened one)

    GameServices services{&mixer, &assets, &cmaps, &anims, /*interactive=*/false, false, &master};

    uint64_t hashes[2];
    for (int run = 0; run < 2; ++run) {
        game_init(20260721, services);
        InputState in;
        auto tick_with = [&](Key k, int n) {
            for (int i = 0; i < n; ++i) {
                in.pressed[int(k)] = true;
                game_update(in);
                in.end_tick();
            }
        };
        tick_with(Key::Down, 13);
        tick_with(Key::Right, 8);
        tick_with(Key::Thumper, 1);
        tick_with(Key::Left, 7);
        tick_with(Key::Wait, 120);
        tick_with(Key::Right, 10);
        tick_with(Key::Wait, 60);
        hashes[run] = game_state_hash();
    }
    const bool det = hashes[0] == hashes[1];
    std::printf("sim_dettest %s  hash=%016llx  (baseline 5dc9443f6cf23331)\n",
                det ? "DETERMINISTIC" : "NONDETERMINISTIC", (unsigned long long)hashes[0]);
    return det ? 0 : 1;
}
