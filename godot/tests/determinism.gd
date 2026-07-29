extends SceneTree
# Headless make-or-break: drive the sim (via the GDExtension) with dettest's exact input
# sequence and check the hash equals the SDL build's baseline. Run:
#   godot --headless --path <godot/> --script res://tests/determinism.gd
func _init():
    var sim = DustfallSim.new()
    if not sim.load_data(ProjectSettings.globalize_path("res://data")):
        print("determinism: FAIL (data load)"); quit(2); return
    var hashes = []
    for run in range(2):
        sim.init_run(20260721)
        sim.tick_key(1, 13)   # Down
        sim.tick_key(3, 8)    # Right
        sim.tick_key(17, 1)   # Thumper
        sim.tick_key(2, 7)    # Left
        sim.tick_key(8, 120)  # Wait
        sim.tick_key(3, 10)   # Right
        sim.tick_key(8, 60)   # Wait
        hashes.append(sim.state_hash_hex())
    var det = hashes[0] == hashes[1]
    var matches = hashes[0] == "5dc9443f6cf23331"  # rebaselined for C1/C2 (attributes + energy shield)
    print("godot determinism: hash=", hashes[0], " det=", det, " baseline_match=", matches)
    quit(0 if det and matches else 1)
