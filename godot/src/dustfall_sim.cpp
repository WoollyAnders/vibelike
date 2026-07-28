#include "dustfall_sim.h"

#include <cstdio>
#include <string>

#include <godot_cpp/core/class_db.hpp>

#include "game/game.h"

using namespace godot;

void DustfallSim::_bind_methods() {
    ClassDB::bind_method(D_METHOD("load_data", "data_dir"), &DustfallSim::load_data);
    ClassDB::bind_method(D_METHOD("init_run", "seed"), &DustfallSim::init_run);
    ClassDB::bind_method(D_METHOD("tick_key", "key", "count"), &DustfallSim::tick_key);
    ClassDB::bind_method(D_METHOD("tick", "down_keys"), &DustfallSim::tick);
    ClassDB::bind_method(D_METHOD("state_hash"), &DustfallSim::state_hash);
    ClassDB::bind_method(D_METHOD("state_hash_hex"), &DustfallSim::state_hash_hex);
    ClassDB::bind_method(D_METHOD("frame_rgba"), &DustfallSim::frame_rgba);
    ClassDB::bind_method(D_METHOD("fb_width"), &DustfallSim::fb_width);
    ClassDB::bind_method(D_METHOD("fb_height"), &DustfallSim::fb_height);
    ClassDB::bind_method(D_METHOD("cur_node"), &DustfallSim::cur_node);
    ClassDB::bind_method(D_METHOD("hostiles"), &DustfallSim::hostiles);
    ClassDB::bind_method(D_METHOD("use_door"), &DustfallSim::use_door);
    ClassDB::bind_method(D_METHOD("warp_node", "idx"), &DustfallSim::warp_node);
}

bool DustfallSim::load_data(const String& data_dir) {
    const std::string base = data_dir.utf8().get_data();  // godot String -> C string
    const std::string pal = base + "/palette.pal";
    if (!palette_load(pal.c_str(), master_)) master_ = palette_make_default();
    colormaps_build(master_, cmaps_);
    loaded_ = assets_.load_all(base.c_str());
    anims_.load(base.c_str(), assets_);
    return loaded_;
}

void DustfallSim::init_run(int64_t seed) {
    GameServices services{&mixer_, &assets_, &cmaps_, &anims_, /*interactive=*/false, false, &master_};
    game_init(uint64_t(seed), services);
    in_ = InputState{};
}

void DustfallSim::tick_key(int key, int count) {
    for (int i = 0; i < count; ++i) {
        if (key >= 0 && key < int(::Key::Count)) in_.pressed[key] = true;  // ::Key (ours) vs godot::Key
        game_update(in_);
        in_.end_tick();
    }
}

void DustfallSim::tick(const PackedInt32Array& down_keys) {
    bool now[int(::Key::Count)] = {};
    for (int i = 0; i < down_keys.size(); ++i) {
        const int k = down_keys[i];
        if (k >= 0 && k < int(::Key::Count)) now[k] = true;
    }
    for (int k = 0; k < int(::Key::Count); ++k) {
        in_.pressed[k] = now[k] && !in_.down[k];  // edge = down now, up last frame
        in_.down[k] = now[k];
    }
    game_update(in_);   // advances a turn only on an action key; always runs the per-frame visual tick
    in_.end_tick();     // advances held_ticks (drives key-repeat movement), clears pressed
}

int64_t DustfallSim::state_hash() const { return int64_t(game_state_hash()); }

String DustfallSim::state_hash_hex() const {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)game_state_hash());
    return String(buf);
}

PackedByteArray DustfallSim::frame_rgba() {
    static Framebuffer frame;  // ~8MB; keep off the Godot heap churn
    game_render(frame);
    PackedByteArray out;
    out.resize(fb::W * fb::H * 4);
    uint8_t* dst = out.ptrw();
    for (int i = 0; i < fb::W * fb::H; ++i) {
        const uint32_t c = frame.px[i];           // 0xAARRGGBB
        dst[i * 4 + 0] = uint8_t((c >> 16) & 0xFF);  // R
        dst[i * 4 + 1] = uint8_t((c >> 8) & 0xFF);   // G
        dst[i * 4 + 2] = uint8_t(c & 0xFF);          // B
        dst[i * 4 + 3] = uint8_t((c >> 24) & 0xFF);  // A
    }
    return out;
}

int DustfallSim::cur_node() const { return game_debug_cur_node(); }
int DustfallSim::hostiles() const { return game_debug_hostile_count(); }
void DustfallSim::use_door() { game_debug_use_door(); }
void DustfallSim::warp_node(int idx) { game_debug_warp_to_node(idx); }
