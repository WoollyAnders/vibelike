# DUSTFALL — Roadmap

A Dune roguelike with Diablo-era tech: software rendering, one global 256-color palette,
colormap lighting, FM-synth music. Isometric presentation, roguelike guts. C++20 + SDL2
(static), cross-compiled from WSL2 with MinGW-w64 for Windows.

Full design details live in the project plan; this file tracks build order and exit criteria.
Each milestone is done only when its **Exit** criterion is demonstrated in the running game.

Companion roadmaps: [ROADMAP_GAMEPLAY.md](ROADMAP_GAMEPLAY.md) — the animation &
game-feel overhaul (directional facing, attack/use animations, smooth movement).

---

## Engine milestones

### E0 — Toolchain ✅
Repo skeleton, CMake presets (MinGW cross + MSVC verification), SDL2 static via
FetchContent, `build.sh` (build → deploy exe+data to Windows temp → launch via interop),
size report script.
**Exit:** an SDL window with a cleared color, cross-built from WSL and launched on Windows.

### E1 — Indexed framebuffer + palette ✅
640x360 8-bit indexed buffer, palette LUT expand → 32-bit SDL streaming texture, integer
scale + letterbox, vsynced fixed 60Hz tick, borderless-fullscreen toggle (Alt+Enter),
palette fade machinery.
**Exit:** palette-cycling gradient with a smooth fade-to-black at 60fps.

### E2 — Input ✅
`InputState` with per-key down/pressed/released edges, key repeat, rebindable mapping
table, mouse position → framebuffer coordinates.
**Exit:** move a square around; input feels tick-exact.

### E3 — Assets + isometric renderer ✅
Manifest + indexed-PNG loader (RGB→palette-index mapping, off-palette pixels are errors),
2:1 diamond tilemap rendering (32x16 floors), y-sorted sprites with anchors and animation
frames, Doom-style colormap lighting bands, bitmap font, smooth-scrolling camera,
screen→tile picking.
**Exit:** scrolling iso desert with an animated sprite, night-lighting toggle, and text.

### E4 — Audio ✅
Mixer on the SDL audio callback: 2-op FM synth voices + tracked song playback, 8-bit WAV
one-shots, parameterized worm-rumble channel (filtered noise + low sine).
**Exit:** keypress plays a crunchy worm roar over an FM ambient track; no glitches while
dragging the window.

### E5 — RNG, saves, game seam ✅
pcg32 (streamed: world/combat/loot saved, fx firewalled), chunked binary save with CRC +
atomic write, finalize `game.h` seam (`game_init/update/render/serialize`), stub harness
proving the full seam.
**Exit:** stub game walks the seam end to end, saves and restores.

---

## Game milestones

### M1 — Vertical slice ✅
Sietch interior + one desert region (procgen), energy-system turns, shadowcasting FOV
(drives colormap light), Dijkstra map, bump combat vs dust hound, water drain + **heat
curve + exposure meter** (static shade), deathstill corpse harvest, cistern, HUD orbs
(blood + water) + sun-arc thermometer + message log, death/restart.
**Exit:** leave the sietch at noon and cook, or at dusk and dehydrate slower; kill a
hound, drink it, die.

### M2 — The sandworm ✅
Noise heatmap + worm brain (DORMANT→STIRRING→HUNTING→BREACHING→SURFACED), segment-chain
entity (body-follows-head, crush/swallow, armored segments + weak points,
retreat-when-hurt), ripple/mound/breach set pieces, screen shake + rumble channel,
thumper, sandwalk toggle, seismograph meter, sun-tracked moving shadows, region travel +
delta persistence.
**Exit:** sand-shortcut-vs-rock-road is a real decision; you can watch the worm surface
and eat a patrol you baited.

### M3 — First harvest op ✅
Harvester crawler (big animated multi-sprite machine) + hold timer + carryall extraction,
spotter crew, escort AI, all five sabotage methods (thumper bait / demolition / kill
spotters / contaminate / steal), local alarm + escape phase.
**Exit:** one full op completable by at least the thumper-bait and spotter-kill routes.

### M4 — Campaign + win ⬜
Season clock, overlapping op spawning, op mutators, intel sources (scouts / smuggler tips
/ vantage scouting), quota (CHOAM ledger) / Harkonnen alert / sietch standing, water
rings, Naib + Sayyadina + smuggler vendors and dialogue (seeded stock), Holtzman shield
rules + shield-troopers, side-quest templates, climax op, all endings, meta save.
**Exit:** a full season is winnable, both loss endings trigger correctly, and the slow
blade penetrates the shield.

### M5 — Worm-riding + polish ⬜ *(riding cuttable)*
Maker hooks + mount/steer, starting backgrounds (Fedaykin / Saboteur /
Sayyadina-touched), random desert events (Coriolis storms, spice blow eruptions,
hunter-seekers), remaining enemies, ornithopter sweeps, Eyes of Ibad, Litany + crysknife
etiquette, hallucination/chatter text, look-mode polish, FM music pass (~6–8 tracks),
tuning pass against `tunables.h`, seed-race determinism test.
**Exit:** same seed + same input script twice → identical final state hash; a full
season plays with all systems live.

### M6 — Art & UI overhaul (Diablo 1 vibes) ⬜
Target the look and feel of Diablo 1 at its native **640x480** (done: the
framebuffer is now 4:3, pillarboxed by integer scaling on modern displays).
**AI-generated art pipeline**: `tools/genart.py` calls the OpenAI image API
(gpt-image-1) with per-asset prompts, then post-processes — downscale to
target sprite size, quantize to the master 256-color palette, key out
backgrounds, slice into aligned sheet frames — and rewrites the manifest.
Regenerate every asset class: terrain floor sets (dune flow, rock, spice
blows), character sprites (player, Fremen, Harkonnen types), the worm as a
showpiece (layered segments, breach eruption), crawler/carryall machinery,
sietch architecture, props. UI restyle to Diablo-quality: carved-stone bottom
panel frame, glass orbs with meniscus shimmer, etched seismograph bezel,
gothic-flavored bitmap font, styled modal frames, title + death screens.
**Exit:** a side-by-side before/after where every screen reads as a finished
retro game; no placeholder sprite remains in the manifest.

### M7 — Lighting ✅
Diablo-style proper lighting on the colormap infrastructure: per-tile light
levels composed from multiple sources (player torch radius, glowglobes,
crawler floodlights, spice-blow ember glow, explosion flashes), radial
falloff with dithered band transitions, night that is actually *dark* beyond
your light, interiors lit by pools of lamplight, worm breach lightning-dust
flashes, and light-radius items (glowglobe, suit lamp). FOV and light unify:
you see what your light touches.
**Exit:** standing in the open erg at midnight with a glowglobe reads like
Tristram at night — a warm pool of light in a black desert.

---

## Standing verification

- `build.sh` runs the size report on every build (informational ceiling: 50MB total).
- Each milestone is demonstrated by launching the Windows exe from WSL via interop.
- Final distributable: exe + `data/`, 2–50MB, no DLL dependencies beyond OS libraries.
