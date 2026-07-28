# DUSTFALL — Godot 4 Migration Roadmap

## Context

The SDL2 build shipped a complete playable core (B1–B5) but the remaining roadmap is UI/content/
tooling-heavy (paperdoll, a 50–80-node passive tree, crafting bench, boards) — exactly where a
hand-rolled software-framebuffer UI is expensive and **Godot 4** is cheap. A de-risking **spike**
(now merged in `godot/`) proved the migration is a *port, not a rewrite*: the SDL-free, deterministic
C++ **sim** (`src/game/*` + `src/engine/*`) compiles into a **GDExtension** (`DustfallSim` node),
runs from GDScript, and reproduces the exact `dettest` hash `b583a955958b6341` **through Godot 4.7.1**
headless. Target engine: **Godot 4.7** (extension `compatibility_minimum` ~4.4, built against
`godot-cpp` `master` — no per-minor branch past 4.5).

**Guiding principle — always playable, never a big-bang.** `game_render()` already draws into a
software `Framebuffer`; Godot blits it to a `TextureRect`, so the whole game runs in Godot *today*.
Each phase replaces one layer with native Godot while the rest keeps blitting — the build is
shippable at every step. **The sim stays the single source of truth** (the GDExtension = model;
Godot = view/UI); the **determinism firewall + the 13 headless tests are preserved throughout** and
rebaselined only for legit sim changes.

**Invariants (unchanged):** gameplay RNG = `combat/world/loot`, visuals = `fx`; `dettest`/`clicktest`
stay deterministic; `save.dat` no-migration OK; `meta.dat` keeps a back-compat read. The SDL build on
`main` stays as a fallback until G3 retires it.

---

## G0 — Spike (done ✅)
`godot/`: `DustfallSim` GDExtension (`init_run/tick_key/state_hash(_hex)/frame_rgba` + `cur_node/
hostiles/use_door/warp_node`), a Godot project blitting the framebuffer, a headless `determinism.gd`.
`libdustfall.{so,dll}` (Windows dll static-linked, zero MinGW deps). Verified deterministic in 4.7.1.

## G1 — Godot becomes the baseline (playable + fully tested; SDL retired from the run path)
- **Production extension**: build against `godot-cpp` `master` (pin a commit), `compatibility_minimum
  = "4.4"`; reuse the spike binding. Set up `godot/CMakeLists.txt` to emit both `.so` (headless CI in
  WSL) and the MinGW `.dll` (Windows), static runtime.
- **Full input plumbing**: expand the binding so Godot forwards the complete input the sim consumes —
  all `Key`s **and the mouse** (`mouse_x/y`, `mouse_pressed/down[]`) which the click-to-move / click
  interactions and the Revelation overlay already read. `main.gd` maps Godot input → the sim's
  `InputState` each `_process`.
- **Port the 13 headless tests** to run through the extension (`godot --headless --script tests/*.gd`,
  or one driver script): `dettest`/`clicktest` assert the SAME hashes; `seamtest/wormtest/optest/
  slicetest/rpgtest/campaigntest/ridetest/reveltest/gauntlettest/extractiontest/metatest` assert their
  outcomes via the debug hooks (expose the remaining `game_debug_*` through the binding). This carries
  the determinism discipline into Godot. A `godot/run_tests.sh` runs them in WSL headless.
- World still renders via the **framebuffer blit**; the game is fully playable in Godot 4.7.
- **Exit:** the game runs in Godot (blit) with real keyboard+mouse input; all 13 tests pass headless
  through the extension; `dettest` == baseline. SDL is no longer needed to *play* (kept only as the
  fallback build until G3).

## G2 — Native Godot UI (the payoff)
Replace the framebuffer-drawn overlays with native `Control` scenes that read sim state (queries) and
issue commands (the sim stays the model). Order by value:
- **HUD** (life/water orbs, wormsign, thermometer, log) → `Control` + `TextureProgressBar`.
- **Inventory paperdoll (E1)** — the 10-slot Fremen paperdoll + drag-drop + affix tooltips, native
  `Control` drag/drop signals (vs. the hand-rolled framebuffer menu).
- **Passive tree (C4)** — a pannable/zoomable graph of 50–80 nodes (`GraphEdit` or a custom
  `Camera2D` canvas) — *the* screen that motivated the move.
- **Crafting bench (D2)**, vendor/quest **boards (C8)**, the **Awaken selector** at the ritual.
- **Binding surface**: add a clean query/command API — `get_inventory()/get_equip()/equip(slot)/
  get_stats()/pick_revelation(i)/get_awaken()/set_awaken_choice(n)/...` — mapping to the sim's
  existing menu logic (kept in C++) or thin new commands.
- World still blits; only UI is native.
- **Exit:** the UI-heavy screens are native Godot Controls over a blitted world; drag-drop + tooltips
  + the passive tree work; tests still green.

## G3 — Native world rendering + Godot data resources (retire the software renderer + SDL)
- **Native 2D world**: the extension exposes the resident zone (tile grid) + entity list (type / x,y /
  facing / anim-frame / light). Godot draws it with `TileMapLayer` + `AnimatedSprite2D`/`Sprite2D`
  reading the atlas (or re-imported per-sprite textures), with the iso transform in Godot. Retire the
  `engine/render` software framebuffer from the shipping build (optionally keep it for headless
  golden-image tests). **Delete the SDL build** once parity is reached.
- **Data as `.tres` Resources** (supersedes the hand-rolled JSON plan): author monster statlines, loot
  tables, Awaken scaling, affixes, and skill params as typed Godot Resources in the editor; the
  extension reads them (or consumes an exported blob). Editor-authored, hot-reloadable, type-checked —
  the data-authoring win, native.
- **Exit:** world renders natively (no software framebuffer in the shipping build); data authored as
  Resources; SDL removed; a Godot **export** replaces `build.sh` for the shippable Windows/Linux/web
  build.

## G4 — Art pipeline in Godot + God Mode AI animation
Art now targets Godot resources (`SpriteFrames`/`AnimatedSprite2D`, `TileSet`). New pipeline stage:
1. **OpenAI (genart.py)** generates the *still* — character/gear/named-boss/hub-cast/biome art (as now).
2. **God Mode AI** takes that still and produces an **animated sprite** (frame sequence / sheet) — e.g.
   idle/walk/attack for characters, ambient loops for props.
3. **Import** the frames as Godot `SpriteFrames` (`AnimatedSprite2D`) or a sheet + `AnimationPlayer`.
This replaces `animsynth.py`'s procedural walk-derivation with AI-authored animation, and is *far*
easier than hand-authoring frames.
> **Integration (user-provided):** God Mode AI ships an **MCP server** + a client config the user can
> add, and an **agent skill** documenting the flow — pick a valid *action* + *camera view*, upload the
> image, run the *async* generation, poll for the result. It works standalone against the **REST API**
> or via the **MCP server**. So the DUSTFALL side is: `genart.py` OpenAI still → invoke God Mode AI
> (MCP tool, or a `tools/godmode_animate.py` REST client following the skill) → frame sequence →
> import as Godot `SpriteFrames`. When the MCP config + skill are added they surface here (MCP tools
> load on demand via ToolSearch; the skill via the Skill tool) — I'll follow the skill to self-teach
> the action/camera/upload/async flow. `animsynth.py` stays as the offline fallback. Confirm the
> licensing/terms for shipping AI-animated assets.
- **Exit:** named bosses, Paul + the growing hub cast, and per-biome tiles authored via
  OpenAI → God Mode AI → Godot animation resources; no missing-asset fallbacks.

## G5 — Resume the content roadmap, natively
With the engine in place, build the remaining `ROADMAP_OVERHAUL.md` milestones in Godot: **B6** sietch
suspicion + Guild bribe · **B7** field NPCs · **B8** patrols/events · **B9** caves/Jacurutu · **B10**
cartography endgame + Corrino super-bosses · **C1–C8** character systems (native UI for the trees/
vendors) · **D1–D3** itemization (finalize the provisional per-slot stat balance; affixes/crafting/
flasks as Resources) · **E1/E1b/E2** paperdoll/roster/art (mostly folded into G2/G4). Sim logic lands
in the GDExtension; presentation + data in Godot.

---

## Risks / watch-items
- **godot-cpp `master` churn** — pin a commit; re-verify the determinism test after any bump.
- **Binding surface growth** — keep it a disciplined query/command API; don't leak Godot types into the
  sim (the sim stays engine-agnostic C++, which is what made this cheap).
- **Export size** — a Godot export is ~40MB vs the 12MB SDL exe; acceptable for the tooling/UI gains.
- **God Mode AI unknown** — treat as pluggable; procedural `animsynth` fallback keeps the build green.
- **Determinism regressions** — the non-negotiable gate: the 13 headless tests (esp. `dettest`) must
  stay green through every phase.

## Verification (every phase)
Build the extension (`.so` + `.dll`); run `godot/run_tests.sh` headless in WSL (all 13 pass, `dettest`
== `b583a955958b6341` until a legit sim change rebaselines it); open the project in the Godot 4.7
editor for a visual smoke test; export a Windows build for the user's playtest.
