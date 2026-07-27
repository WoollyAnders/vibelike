extends Control
# Godot-spike presentation: run the DUSTFALL sim (GDExtension) and blit its software
# framebuffer to a TextureRect. Proves the whole existing game runs inside Godot.
var sim
var tr: TextureRect
const K_UP=0; const K_DOWN=1; const K_LEFT=2; const K_RIGHT=3; const K_WAIT=8

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
    _blit()

func _blit():
    var w: int = sim.fb_width()
    var h: int = sim.fb_height()
    var img := Image.create_from_data(w, h, false, Image.FORMAT_RGBA8, sim.frame_rgba())
    tr.texture = ImageTexture.create_from_image(img)

func _unhandled_key_input(e):
    if not e.pressed: return
    var k := -1
    match e.keycode:
        KEY_UP: k = K_UP
        KEY_DOWN: k = K_DOWN
        KEY_LEFT: k = K_LEFT
        KEY_RIGHT: k = K_RIGHT
        KEY_PERIOD, KEY_SPACE: k = K_WAIT
    if k >= 0:
        sim.tick_key(k, 1)
        _blit()
