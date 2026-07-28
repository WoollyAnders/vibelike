extends SceneTree
func _init():
    var sim = DustfallSim.new()
    sim.load_data(ProjectSettings.globalize_path("res://data"))
    sim.init_run(20260721)
    sim.tick(PackedInt32Array(), -1, -1, 0)                 # settle
    var before: PackedByteArray = sim.frame_rgba()
    for i in range(24): sim.tick(PackedInt32Array([1]), -1, -1, 0)   # hold Down
    var after_kb: PackedByteArray = sim.frame_rgba()
    # mouse smoke: left-click a world tile (upper-left of the hub), then let click-to-move run
    sim.tick(PackedInt32Array(), 700, 350, 1)               # left button down over the world
    for i in range(30): sim.tick(PackedInt32Array(), 700, 350, 0)
    var after_mouse: PackedByteArray = sim.frame_rgba()
    Image.create_from_data(sim.fb_width(), sim.fb_height(), false, Image.FORMAT_RGBA8, after_mouse).save_png("res://play_after.png")
    var kb_moved = before != after_kb
    var mouse_moved = after_kb != after_mouse
    print("play_check: kb_moved=", kb_moved, " mouse_moved=", mouse_moved, " node=", sim.cur_node())
    quit(0 if kb_moved and mouse_moved else 1)
