#pragma once
// Godot 4 GDExtension binding: wraps the (SDL-free, deterministic) DUSTFALL sim as a Node.
// Godot drives it (init/tick/queries) and presents its software framebuffer as a texture; the
// determinism-proven C++ logic is unchanged.
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "engine/animator.h"
#include "engine/assets.h"
#include "engine/audio.h"
#include "engine/framebuffer.h"
#include "engine/palette.h"
#include "platform/input.h"

class DustfallSim : public godot::Node {
    GDCLASS(DustfallSim, godot::Node)

    Palette master_;
    Colormaps cmaps_;
    Assets assets_;
    anim::Registry anims_;
    audio::Mixer mixer_;
    InputState in_;
    bool loaded_ = false;

protected:
    static void _bind_methods();

public:
    bool load_data(const godot::String& data_dir);  // load atlas/palette/anims from data/
    void init_run(int64_t seed);                     // game_init(seed) (headless services)
    void tick_key(int key, int count);               // press Key `key` for `count` sim ticks (tests)
    // Continuous play: `down_keys` = Key ids held THIS frame. The binding derives edges/held,
    // runs one game_update (advances a turn only on an action; always ticks visuals), and returns
    // the freshly-rendered frame so per-frame glide/particle animation shows. Call every _process.
    void tick(const godot::PackedInt32Array& down_keys);
    int64_t state_hash() const;                      // game_state_hash()
    godot::String state_hash_hex() const;            // 16-char hex (matches the C++ dettest baseline)
    godot::PackedByteArray frame_rgba();             // game_render -> RGBA8 pixels (fb::W x fb::H)
    int fb_width() const { return fb::W; }
    int fb_height() const { return fb::H; }
    int cur_node() const;
    int hostiles() const;
    void use_door();                                 // headless door/ritual trigger
    void warp_node(int idx);                         // jump to a graph node
};
