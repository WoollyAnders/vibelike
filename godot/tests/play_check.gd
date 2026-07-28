extends SceneTree
func _init():
    var sim = DustfallSim.new()
    sim.load_data(ProjectSettings.globalize_path("res://data"))
    sim.init_run(20260721)
    sim.tick(PackedInt32Array())                 # settle one frame
    var before: PackedByteArray = sim.frame_rgba()
    for i in range(24): sim.tick(PackedInt32Array([1]))   # hold Down (Key id 1)
    var after: PackedByteArray = sim.frame_rgba()
    var w: int = sim.fb_width(); var h: int = sim.fb_height()
    Image.create_from_data(w, h, false, Image.FORMAT_RGBA8, after).save_png("res://play_after.png")
    print("play_check: frame_changed=", before != after, " node=", sim.cur_node())
    quit(0 if before != after else 1)
