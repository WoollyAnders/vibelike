# DUSTFALL — Post-Playtest Overhaul Roadmap

The first playtest produced a large, coherent set of change requests plus one
critical rendering bug. This roadmap turns that feedback into an execution plan:
DUSTFALL becomes a **roguelike / extraction / ARPG blend** — a *persistent
character* runs *procedural excursions* out of the sietch through a *branching
world map* shaped like *Hades* (~4 field sections per tier, then a boss,
escalating to Baron Harkonnen), extracting by **riding a sandworm home** to bank
loot or dying and losing what was carried.

House rules unchanged: **done** means the **Exit** criterion runs in the game and
the sim stays deterministic (`dettest` holds, re-baselined once per legit sim
change). Each milestone lands on its own branch and merges to `main` with a check.

Full design rationale + locked decisions live in the plan file
(`.claude/plans/this-project-is-to-nested-planet.md`).

---

## PHASE A — Playable Now (feel + the bug)

### A1 — Fix the sietch (critical bug) ✅
Indoor FOV branch (`kFovIndoor`) + a bounded **room flood-reveal** (`map::reveal_room`)
so the whole enclosed sietch is revealed regardless of shape; raised `kAmbientIndoor`
(7→24) so the lamp-lit home reads (decoupled the outdoor shade threshold so the desert
is unchanged); call `spawn_zone_wildlife(kZoneSietch)` in `game_init` so the Naib/
Sayyadina actually spawn; cheap deterministic sietch floor variety (no art).
**Exit:** ✅ a new run shows a lit, populated sietch — hall + cistern alcove + corridor
all drawn (verified by a vis dump), cistern reachable, both NPCs present; 9/9 headless
tests green, dettest bit-identical.

### A2 — HUD relayout (fix overlaps) ✅
Thermometer → top-center strip (frees the panel's right for the seismograph, now
unobstructed); log truncated to clear the seismograph; hover description lifted to
y=412 off the level/task row. Every widget survives.
**Exit:** ✅ verified by screenshots — thermometer (top-center), CHOAM/alert/standing
(top-right), op-status (top-left), level line (bottom-left), seismograph + log
(panel) no longer share pixels; 9/9 tests green.

### A3 — Smoother movement (kill the hopping) ✅
Glide pre-buffer 8→18px (`kStepBufferPx`), glide step 3→4px (`kGlideStep`;
sandwalk stays 2), held-repeat floor 6→3 (`kHeldStepFloor`), camera lerp /6→/8.
The raised buffer + gate is the queued-next-step (a held key re-steps the instant
the glide crosses the buffer; reused by A4's click-path). Cadence/visual only.
**Exit:** ✅ held walking re-steps continuously; `dettest` bit-identical
(942bc42517b8679f), 9/9 tests green — the scripted walk hashes exactly as before.

### A4 — PoE2 controls: WASD movement + cursor aim ✅
Screen-relative WASD (new `Key::Walk*`; W walks *up the screen* via the inverse iso
transform `dx=sign(Sx+Sy), dy=sign(Sy-Sx)`); arrows/numpad keep tile directions
(so tests are unchanged). Mouse cursor sets a click-to-move goal (`click_step_toward`,
goal-rooted Dijkstra); the final bump onto a hostile/NPC/cistern attacks or interacts;
right-click/Cancel/keyboard override. A pulsing diamond reticle marks the hovered tile.
**Exit:** ✅ new `clicktest` pathfinds to the goal and is deterministic (4412b797977030e6);
reticle verified by screenshot; `dettest` bit-identical (942bc42517b8679f); 10/10 tests green.

### A5 — Minimap ✅
`M` toggles a gold-framed top-right panel: a 2px/tile render of remembered tiles
coloured by feature (wall / rock / spice / cistern / door) with a blinking player
dot; hidden under menus. `map_scope` reserved for the Phase-B world overview.
**Exit:** ✅ M shows a legible map of everywhere seen this run (verified by
screenshot — the sietch cross-shape renders); unexplored absent; 10/10 tests green.

### A6 — Playtest polish ✅ *(second-playtest feedback)*
Five engineering fixes from the second playtest:
- **Cursor aim true** — tiles are centre-anchored, so the reticle was drawn half a
  tile down-right ("aim with the bottom-right of the cursor"); now offset by
  `-half` so it sits on the picked tile.
- **Sietch brighter** — `kAmbientIndoor` 24→28.
- **Mobs visible** — night FOV 5→8 and an entity min-light (`kEntityMinLight=16`)
  so actors in view are never near-black.
- **Heatstroke survivable** — an equipped stillsuit blunts sun exposure (Mk I
  halves, Mk III negates); recovery 6→8.
- **Clickable menus** — hovering a row selects it, left-click confirms, right/middle
  click closes (all menus: vendor lists, character, pack, vault).
**Exit:** ✅ reticle centred on the cursor tile (screenshot); sietch reads lit; mobs
render in the dark; heat is survivable with a suit; menus work by mouse; `dettest`
still `942bc42517b8679f`, 10/10 tests green.
*(Deferred to the art pass: OpenAI-generated walk animations and detailed/varied
terrain — the remaining "hopping animation" and "flat world" feedback.)*

### A7 — Real walk animation (OpenAI) + crisp base art ✅ *(second-playtest feedback)*
- **Real walk cycles, not a nudged pose.** New `tools/walkgen.py` generates one
  side-profile walk-cycle sheet per class via gpt-image-1 (the model stays
  consistent within a single image), slices frames by transparent gaps, and — to
  kill the model's per-frame size drift — normalizes every frame to constant
  opaque **pixel-mass**, centres by centroid, and bottom-anchors the feet. Wired
  as a `dirs=1` Walk state (mirrored L/R at draw time). Fedaykin 5 / Saboteur 4 /
  Sayyadina 3 frames; legs genuinely stride, character stays one size.
- **Crisper characters.** Regenerated all three class turnarounds (idle) via
  gpt-image-1 with a sharpened prompt (bold silhouette, rim light, clean edges).
- **Grounded gait** for the remaining synth-walk mobs (trooper/hound/hermit) in
  `animsynth.py`: feet planted, lean + settle, no airborne bob.
- **Terrain** folded into B1 per decision.
**Exit:** ✅ in-game filmstrip shows real striding legs at a constant size; new
class art renders; 10/10 tests green. (AI frames aren't perfectly frame-coherent —
honest ceiling of image-gen animation at this sprite scale.)

---

## PHASE R — High-resolution RGBA render rework *(post-playtest pivot)*
The 640×480 8-bit indexed engine read as "budget 80s". Decision: rebuild the render
layer for a modern **1080p, RGBA truecolor, GPU** look. Rendering is presentation-only,
so the sim + all 10 headless tests stay green throughout — Phase R is verified visually.
This supersedes the original "640×480 indexed" guardrail. Runs before the art-dependent
game work; gameplay logic (B/C) is resolution-independent and follows.

### R1 — RGBA truecolor pipeline ⬜
Framebuffer → 32-bit RGBA; sprites converted to RGBA at load (via palette); `draw_sprite`
alpha-blends RGBA with an RGB light multiply (replaces colormap remap); `fill_rect`/
`draw_text` map palette index → RGB; present uploads RGBA directly (fade becomes an RGBA
blend). Stays 640×480 — truecolor foundation, ~same look.
**Exit:** game renders identically in truecolor; 10/10 tests green (sim untouched).

### R2 — 1080p internal + coordinate rework ⬜
Framebuffer → 1920×1080; iso tile scale, camera, world-pixel + `screen_to_tile` picking
rescaled; every HUD/UI coordinate reworked for 1080p. Art still low-res (upscaled) here.
**Exit:** the game runs at 1080p; HUD/menus/minimap laid out correctly; picking accurate.

### R3 — High-res native-RGBA art ⬜
Regenerate all art at high resolution as native RGBA (characters, tiles, props, UI),
dropping palette-quantize. New atlas format (RGBA pages). The actual "modern" payoff.
**Exit:** crisp high-res truecolor characters/terrain/UI; no missing-asset fallbacks.

### R4 — RGB lighting + effects + upscaling polish ⬜
RGB per-light shading + fog-of-war dimming (replacing colormap levels), particle/effect
recolor, and a good window-scaling filter.
**Exit:** lighting reads well; effects intact; scales cleanly to any window.

---

## PHASE B — Run / World / Extraction Architecture

### B1 — World graph + single resident zone ⬜
Per-run `world::Graph` (≤~20 nodes, tiers of ~4 field sections capped by a boss,
summit Baron) + one resident `map::Zone`; node_seed = hash_combine(run_seed, i).
**Exit:** leave the sietch, traverse the chain, save/load round-trips; seamtest/
dettest hold (rebaselined once).

### B2 — Gated, branching run flow ⬜
Sietch = pure hub; leaving commits a run; section exits offer 2-3 previewed doors;
tier-end funnels to the boss; grace retreat only pre-denial.
**Exit:** leaving commits; you pick doors each section; no free return after a
denial.

### B3 — Harvesters per section + tiered bosses (Hades cadence) ⬜
One harvester per field section; ~4 sections per tier then a scaled boss; bosses
escalate to Baron Harkonnen.
**Exit:** clear ~4 sections before the first boss; each tier boss tougher; summit
yields the Baron; campaigntest updated.

### B4 — Extraction via worm-ride ⬜
Repurpose worm-riding into a one-way extraction verb; sweep carried inv + spice +
materials into the banked wallets.
**Exit (reworked extractiontest):** deny → thumper → surface → mount → land home
with loot banked; dying loses everything carried.

### B5 — Persistence & meta ⬜
Persistent character + banked wallets in meta.dat ('DME2'→'DME3', back-compat);
death keeps the character, clears carried gear/spice/materials.
**Exit:** level up + extract; next run die; character/level/vault/banked spice
survive; carried gear + spice gone.

### B6 — Sietch suspicion (meta pressure) + Guild bribe ⬜
Persistent suspicion meter; Guild envoy bribe cools it; maxing = a raid that costs
part of the vault but not the character.
**Exit:** run loud → suspicion climbs → a raid fires; paying the envoy cools it.

### B7 — Field NPCs, wandering vendors & footprints ⬜
Occasional field NPCs (water-seller / Guild rep / smuggler) on the side-objective
clock; footprint trails lead to them.
**Exit:** follow a footprint trail to a wandering vendor, trade; it expires if
ignored; identical across save/load.

### B8 — Desert patrols & world events ⬜
`Ai::Patrol` Harkonnen roam the desert; expanded enter_zone event roll.
**Exit:** a patrol marches a route and gives chase on sight; new events fire;
deterministic.

### B9 — Rare side-areas: caves & Jacurutu ⬜
Rare cave sub-maps + a super-rare legendary Jacurutu hoard as transient sub-zones.
**Exit:** a cave entrance rarely appears → clear it for a dense reward; Jacurutu
appears once across many runs; deterministic.

---

## PHASE C — Character Systems

### C1 — Three-attribute foundation ⬜
Conditioning / Versatility / Awareness → Life+Lethality / Precision+Flow /
Focus+Will-max; crit → gear only.
**Exit:** allocate the three attributes; each moves its derived pair; crit from
gear only.

### C2 — Energy Shield (Focus) + retire Holtzman ⬜
PoE-style shield buffer over Life (absorbs all, recharges); ShieldTrooper →
heavy-armor bulwark; slow-blade rule + shieldtest retired.
**Exit:** an Awareness build soaks hits on the shield; it recharges; no Holtzman
anywhere.

### C3 — Class identities (three corners) ⬜
Fedaykin=Conditioning, Saboteur=Versatility, Sayyadina=Awareness; shared Will fuel.
**Exit:** each corner starts distinct (bruiser / precise-skirmisher / shielded
spice-caster).

### C4 — Passive skill tree (PoE-style) ⬜
One shared node graph, class start per corner, adjacency-gated allocation, passive
points per level.
**Exit:** allocate an adjacent node → the derived stat moves; hybrids by pathing;
persists across a death.

### C5 — Sietch talent tree + split currencies ⬜
Meta talent tree bought with banked spice; Sandcraft + tribe's-blessing tracks;
currencies stay distinct.
**Exit:** spend banked spice on a Sietch node → persists across a death.

### C6 — Ranged combat (Saboteur maula) ⬜
Cursor-aimed maula; Bresenham line-of-fire; Lethality-scaled damage; loud.
**Exit:** aim + fire down a corridor; a wall blocks the shot; drop a trooper at
range.

### C7 — Ability + Will (universal fuel) + six skill slots ⬜
Shared Will resource; cooldowns; `Entity.stun`; six skill slots on LMB/RMB/MMB/
Q/E/R aimed at the cursor; skill-loadout screen.
**Exit:** each class fires slotted skills; Will drains/regens/flask-refills;
cooldowns tick.

### C8 — Vendors refresh + sietch quest board ⬜
Per-run seeded vendor stock; a sietch quest board reset per run.
**Exit:** vendor wares differ between runs; accept/complete/turn-in a board quest;
the board resets.

---

## PHASE D — Itemization, Crafting & Flasks

### D1 — PoE-style affixes ⬜
Widen `Inst` for item-level + multiple tiered prefixes/suffixes + rarities.
**Exit:** a rare shows item-level + tiered affixes; deeper runs roll higher tiers;
the vault round-trips.

### D2 — Crafting materials + sietch bench ⬜
Distinct run-found materials + a bench with reroll / add / remove / upgrade-tier /
upgrade-rarity.
**Exit:** craft an item's affixes for material cost; a bricked item re-rolls;
banked materials survive a death.

### D3 — Flasks (Life / Will / Utility) ⬜
A flask belt; charges refill on kills/time; used via number keys.
**Exit:** equip Life/Will/Utility flasks; number keys consume a charge; charges
refill on kills.

---

## PHASE E — UI Polish + Art

### E1 — Diablo character/inventory + drag-and-drop ⬜
Paperdoll + equip slots + flask belt + grid; mouse drag-and-drop; affix tooltips.
**Exit:** hovering shows affixes; drag equips/swaps/reorders; keyboard still works.

### E2 — Art regen + terrain/animation refinement ⬜
Portraits, item/flask/material icons, sietch/cave tiles, footprint decals, walk
re-timing.
**Exit:** atlas rebuilds; portraits + icons render; terrain visibly varied; no
missing-asset fallbacks.
