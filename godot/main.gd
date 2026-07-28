extends Control
# G1: run the DUSTFALL sim (GDExtension) at 60 fps and blit its software framebuffer to a
# TextureRect every frame, so per-frame animation (glide, particles, worm) shows and the game
# is fully controllable. Keyboard maps to the sim's Key ids; mouse plumbing lands in G1.2.
var sim
var tr: TextureRect
var img: Image
var tex: ImageTexture

# Godot keycode -> DUSTFALL Key id (see src/platform/input.h enum order).
const KEYMAP := {
    KEY_UP: 0, KEY_DOWN: 1, KEY_LEFT: 2, KEY_RIGHT: 3,
    KEY_KP_7: 4, KEY_KP_9: 5, KEY_KP_1: 6, KEY_KP_3: 7,
    KEY_PERIOD: 8, KEY_KP_5: 8,                       # Wait
    KEY_ENTER: 9, KEY_KP_ENTER: 9, KEY_ESCAPE: 10,    # Confirm / Cancel
    KEY_I: 11, KEY_X: 12, KEY_Z: 13, KEY_C: 14, KEY_M: 15,
    KEY_G: 16, KEY_T: 17, KEY_B: 18, KEY_V: 19,
    KEY_L: 20, KEY_R: 21, KEY_P: 22,
    KEY_W: 25, KEY_S: 26, KEY_A: 27, KEY_D: 28,       # PoE2 WASD
}

func _ready():
    sim = DustfallSim.new()
    add_child(sim)
    sim.load_data(ProjectSettings.globalize_path("res://data"))
    sim.init_run(20260721)
    tr = TextureRect.new()
    tr.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
    tr.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
    tr.set_anchors_preset(Control.PRESET_FULL_RECT)
    add_child(tr)
    var w: int = sim.fb_width()
    var h: int = sim.fb_height()
    img = Image.create(w, h, false, Image.FORMAT_RGBA8)
    tex = ImageTexture.create_from_image(img)
    tr.texture = tex

func _process(_dt):
    var down := PackedInt32Array()
    for kc in KEYMAP:
        if Input.is_key_pressed(kc):
            down.append(KEYMAP[kc])
    sim.tick(down)                       # one game_update this frame (turn advances only on an action)
    img.set_data(sim.fb_width(), sim.fb_height(), false, Image.FORMAT_RGBA8, sim.frame_rgba())
    tex.update(img)                      # reuse the texture (no per-frame alloc)
