extends SceneTree
# Godot-side boundary smoke: prove the sim's content (escorts + named pinnacle bosses) spawns
# through the GDExtension. Full per-subsystem coverage stays in the SDL harness until G3.
func _init():
    var sim = DustfallSim.new()
    if not sim.load_data(ProjectSettings.globalize_path("res://data")):
        print("smoke: FAIL (data)"); quit(2); return
    sim.init_run(20260721)
    sim.warp_node(1)                       # Awaken-1 field node -> elite escort
    sim.tick(PackedInt32Array(), -1, -1, 0)
    var field_hostiles: int = sim.hostiles()
    sim.warp_node(40)                      # Awaken-10 Pinnacle -> the Baron
    sim.tick(PackedInt32Array(), -1, -1, 0)
    var pin_node: int = sim.cur_node()
    var pin_hostiles: int = sim.hostiles()
    var ok = field_hostiles >= 1 and pin_node == 40 and pin_hostiles >= 1
    print("smoke: ", "PASS" if ok else "FAIL",
          " (field_hostiles=", field_hostiles, " pin_node=", pin_node, " pin_hostiles=", pin_hostiles, ")")
    quit(0 if ok else 1)
