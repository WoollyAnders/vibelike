extends Control
# G1: run the DUSTFALL sim (GDExtension) at 60fps, blit its software framebuffer to a TextureRect,
# forward keyboard+mouse. G2.2: a NATIVE Godot character/inventory panel (Control nodes) that reads
# the live sim state via the query surface (player_stats/equipment/inventory) and toggles on I --
# the first screen replaced by native UI (the reason for the migration). World still blits.
var sim
var tr: TextureRect
var img: Image
var tex: ImageTexture

# Native character/inventory panel.
var inv_panel: PanelContainer
var stats_label: Label
var equip_label: Label
var pack_label: Label
const SLOT_NAMES := ["weapon", "hood", "mask", "filt-plugs", "catchtube",
                     "suit-body", "catchpockets", "heel-pumps", "jubba cloak", "token"]

# Godot keycode -> DUSTFALL Key id (src/platform/input.h order). I (11) is handled natively below.
const KEYMAP := {
    KEY_UP: 0, KEY_DOWN: 1, KEY_LEFT: 2, KEY_RIGHT: 3,
    KEY_KP_7: 4, KEY_KP_9: 5, KEY_KP_1: 6, KEY_KP_3: 7,
    KEY_PERIOD: 8, KEY_KP_5: 8, KEY_ENTER: 9, KEY_KP_ENTER: 9, KEY_ESCAPE: 10,
    KEY_X: 12, KEY_Z: 13, KEY_C: 14, KEY_M: 15,
    KEY_G: 16, KEY_T: 17, KEY_B: 18, KEY_V: 19, KEY_L: 20, KEY_R: 21, KEY_P: 22,
    KEY_W: 25, KEY_S: 26, KEY_A: 27, KEY_D: 28,
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
    _build_inventory_panel()

func _build_inventory_panel():
    var layer := CanvasLayer.new()
    add_child(layer)
    inv_panel = PanelContainer.new()
    inv_panel.visible = false
    inv_panel.set_anchors_and_offsets_preset(Control.PRESET_CENTER)
    layer.add_child(inv_panel)
    var margin := MarginContainer.new()
    for side in ["left", "right", "top", "bottom"]:
        margin.add_theme_constant_override("margin_" + side, 24)
    inv_panel.add_child(margin)
    var vb := VBoxContainer.new()
    vb.custom_minimum_size = Vector2(560, 0)
    margin.add_child(vb)
    var title := Label.new()
    title.text = "MUAD'DIB'S FEDAYKIN — character"
    vb.add_child(title)
    stats_label = Label.new()
    vb.add_child(stats_label)
    vb.add_child(HSeparator.new())
    var eqh := Label.new()
    eqh.text = "STILLSUIT (equipped)"
    vb.add_child(eqh)
    equip_label = Label.new()
    vb.add_child(equip_label)
    vb.add_child(HSeparator.new())
    var ph := Label.new()
    ph.text = "PACK"
    vb.add_child(ph)
    pack_label = Label.new()
    vb.add_child(pack_label)
    var hint := Label.new()
    hint.text = "(I to close)"
    vb.add_child(hint)

func _refresh_inventory():
    var s: Dictionary = sim.player_stats()
    stats_label.text = "Life %d/%d    Water %d/%d    Level %d    Spice %d" % [
        s["hp"], s["max_hp"], s["water"], s["max_water"], s["level"], s["spice"]]
    var eq: PackedStringArray = sim.equipment()
    var elines := PackedStringArray()
    for i in range(eq.size()):
        elines.append(SLOT_NAMES[i] + ":  " + eq[i])
    equip_label.text = "\n".join(elines)
    var inv: PackedStringArray = sim.inventory()
    var plines := PackedStringArray()
    for it in inv:
        if it != "-":
            plines.append(it)
    pack_label.text = "\n".join(plines) if plines.size() > 0 else "(empty)"

func _input(event):
    if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_I:
        inv_panel.visible = not inv_panel.visible
        if inv_panel.visible:
            _refresh_inventory()

func _process(_dt):
    var down := PackedInt32Array()
    if not inv_panel.visible:                      # panel open = world input paused
        for kc in KEYMAP:
            if Input.is_key_pressed(kc):
                down.append(KEYMAP[kc])
    # cursor -> framebuffer pixels (KEEP_ASPECT_CENTERED letterbox)
    var fw: int = sim.fb_width()
    var fh: int = sim.fb_height()
    var cs: Vector2 = tr.size
    var scale: float = min(cs.x / float(fw), cs.y / float(fh))
    if scale <= 0.0:
        scale = 1.0
    var off: Vector2 = (cs - Vector2(fw, fh) * scale) * 0.5
    var lp: Vector2 = tr.get_local_mouse_position()
    var mx: int = int((lp.x - off.x) / scale)
    var my: int = int((lp.y - off.y) / scale)
    if inv_panel.visible or mx < 0 or my < 0 or mx >= fw or my >= fh:
        mx = -1
        my = -1
    var btn: int = 0
    if not inv_panel.visible:
        if Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT): btn |= 1
        if Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT): btn |= 2
        if Input.is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE): btn |= 4
    sim.tick(down, mx, my, btn)
    img.set_data(fw, fh, false, Image.FORMAT_RGBA8, sim.frame_rgba())
    tex.update(img)
