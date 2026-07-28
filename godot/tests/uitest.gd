extends SceneTree
func _init():
    var sim = DustfallSim.new()
    sim.load_data(ProjectSettings.globalize_path("res://data"))
    sim.init_run(20260721)
    sim.tick(PackedInt32Array(), -1, -1, 0)
    var s = sim.player_stats(); var eq = sim.equipment(); var inv = sim.inventory()
    var equipped = 0
    for e in eq: if e != "-": equipped += 1
    var ok = s["hp"] > 0 and s["max_hp"] > 0 and s["level"] == 1 and eq.size() == 10 and equipped >= 6 and inv.size() == 12
    print("uitest: ", "PASS" if ok else "FAIL", " hp=", s["hp"], "/", s["max_hp"], " lvl=", s["level"], " spice=", s["spice"], " equipped=", equipped, "/10")
    print("  equip=", eq)
    quit(0 if ok else 1)
