# DUSTFALL — Post-Playtest Overhaul Roadmap

The first playtest produced a large, coherent set of change requests plus one
critical rendering bug. This roadmap turns that feedback into an execution plan:
DUSTFALL becomes a **roguelike / extraction / ARPG blend** set inside **Paul Muad'Dib's
prescient visions**: a persistent Fremen (who never Awakens) is Paul's instrument, enacting
his *possible futures* — a *Hades*-style gauntlet climb **across Arrakis to Arrakeen to
confront the Baron**. The journey is **5 acts (biomes) × 2 Awaken levels = 10 pinnacle
bosses**; you **ride a sandworm home** to have the Fremen **forge/secure the vision's yield**
(gear + spice + water), or die and lose only that run's gathered resources (gear & progress
are safe). The campaign climbs an **"Awaken" ladder (1→10, Hades "Heat")** — the sharpening of
Paul's path — gated so only clearing the current level advances the story, culminating in
**Baron Harkonnen at Awaken 10** (a first, undefeatable confrontation at Awaken 5).

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

### R1 — RGBA truecolor pipeline ✅
Framebuffer → 32-bit RGBA; sprites converted to RGBA at load (via palette); `draw_sprite`
alpha-blends RGBA with an RGB light multiply (replaces colormap remap); `fill_rect`/
`draw_text` map palette index → RGB; present uploads RGBA directly (fade becomes an RGBA
blend). Stays 640×480 — truecolor foundation, ~same look.
**Exit:** ✅ game renders identically in truecolor; tests green (sim untouched).

### R2 — 1080p internal + coordinate rework ✅
Framebuffer → 1920×1080; iso tiles render at 2× (128×64 on-screen) for a Diablo-II zoom
via a global `set_draw_scale`; camera/glide/snap feel constants doubled to match the 2×
world-pixel space; `screen_to_tile` picking follows `iso::kHalfW/kHalfH`. UI drawn at a 2×
(16px) font via a global `set_text_scale`; taller HUD panel, bigger orbs, and every
HUD/menu/minimap coordinate + the `menu_row_under_mouse` hit-test reworked for 1080p. The
window now opens at ≤90% of the usable desktop (was `fb*2` = 3840×2160, overflowing the
screen) and `present` aspect-fits with down/upscaling + linear filtering. Art still low-res
(upscaled) here — crisp native art is R3.
**Exit:** ✅ runs at 1080p (screenshots); HUD/menus/minimap laid out correctly; `clicktest`
picking accurate + deterministic (4412b797977030e6); `dettest` bit-identical
(942bc42517b8679f); 10/10 existing tests green.

### R3 — High-res native-RGBA art ✅
Regenerate all art at high resolution as native RGBA (characters, tiles, props, UI),
dropping palette-quantize. New atlas format (RGBA pages). The actual "modern" payoff.
Landed in three groups: **A** — atlas.bin v3 packs native 32-bit RGBA pages (B,G,R,A
on disk → uint32 == 0xAARRGGBB), quantize dropped from the pipeline, loader + draw_sprite
read ARGB directly with a 1-bit alpha key (palette kept only for text/fills). **B** —
art authored at native on-screen size (make_assets SCALE 2→4 ⇒ 128×64 tiles, 64×128
chars; genart HEIGHTS + walkgen ×2), regenerated from the committed 1024px masters +
cached walk grids (zero API calls), and the world blit-scale dropped to 1 (iso
projection unchanged, so all logic hashes hold). **C** — fixed the "invisible mobs" bug
(hound/spotter/carryall were a sand subject on a sand backdrop that corner-flood keying
erased): regenerated those three on a chroma-key green screen with a two-tier green key.
A magenta placeholder now stands in for any missing key (no crash, visible marker).
**Exit:** ✅ crisp high-res truecolor characters/terrain/UI (screenshots); a detailed
desert hound now renders where mobs were invisible; no missing-asset fallbacks in normal
play. Atlas 16MB, total 27.7MB (<50MB). Presentation-only — `dettest` bit-identical
(942bc42517b8679f), `clicktest` deterministic, 10/10 tests green.

### R4 — RGB lighting + effects + upscaling polish ⬜
RGB per-light shading + fog-of-war dimming (replacing colormap levels), particle/effect
recolor, and a good window-scaling filter.
**Exit:** lighting reads well; effects intact; scales cleanly to any window.

---

## PHASE B — Run / World / Extraction Architecture

> **DESIGN PIVOT (mid-project): a Hades-style gauntlet climb inside Paul's prescient visions.**
>
> **Framing — Paul's visions, steered by the player.** *Setting:* the **~2–3 year time-jump after
> Paul takes the Water of Life at Sietch Tabr** — he consolidates Fremen power and plans to **overthrow
> the Emperor and take revenge on the Harkonnens**; the campaign plays out inside the visions he weaves.
> The **visions are Paul's** — the prescience is his alone; **Paul Muad'Dib is an NPC in the hub**. The
> player is a **Fremen** (Fedaykin / Saboteur / Sayyadina) — **not** Paul, no Voice / no weirding way,
> and **never Awakens** (no prescience of their own); the player is Paul's **instrument inside the
> vision**, enlisted to *act within* his foresight and **steer it onto the chosen path** to Arrakeen.
> A run begins at the **Water of Life** (shared at the tau gathering), which draws the Fremen **into
> Paul's vision as a participant, not a seer**. Each run = **enacting one of Paul's *possible* futures:
> a directed journey across Arrakis to Arrakeen to confront the Baron** — so **book canon is never
> broken** (bosses "die" only within the vision). **"Awaken"** = the sharpening of *Paul's* prescient
> path (not the player's): each level pushes his foresight deeper toward the decisive confrontation.
>
> **Structure — 5 acts (biomes) × 2 Awaken levels.** The campaign is **5 ACTS, one per biome** (the
> geography march), each holding **2 Awaken levels = 2 pinnacle bosses** → **10 Awaken levels / 10
> pinnacles** (Act 1 = Awaken 1–2 … Act 5 = Awaken 9–10). **A run/vision plays within its biome** (set
> by your Awaken level): a gauntlet of **elite-escort rooms** (clear escort → complete the biome
> objective → **boon pick** → forward door) capped by the level's **pinnacle boss**. Beat the pinnacle →
> advance one Awaken level; re-running a cleared level gives **no better drops and no story progress**.
> You *can* **chain** act-to-act in one continuous vision, but it's brutal because **points are only
> allotted when you END the vision** (extract) — you carry back what you learned. The old op-scheduling
> (schedule_ops, quota, deadlines, look-ahead) is **retired**; content **spawns on room entry**.
>
> **Environments — the journey to Arrakeen (curated, NOT procedural biomes).** Five **fixed biomes,
> one per act**: **Act 1 deep desert / open erg** (Tanzerouft) → **Act 2 rimwall / broken rock & sinks**
> → **Act 3 the Shield Wall** → **Act 4 graben-pan & Harkonnen-held outskirts** → **Act 5 Arrakeen city
> & the Residency**. The campaign **never rolls its biome** — the act decides it (`node.seed` still
> varies room *layout*; that is not "biome generation"). **Procedural biome *variety* is endgame-only**,
> living exclusively in the B10 cartography "maps." A distinct **Arrakeen city generator** is a new
> environment — its gen + art is its own downstream task.
>
> **Room objective shifts with the biome** (the `Stage` drives both environment *and* objective; B3):
> Act 1 **spice-harvester sabotage** → Act 2 **caravan raids** → Act 3 **breach the Shield Wall**
> (garrisons / emplacements) → Act 4 **liberate the settled land** (raid garrisons, free Fremen, seize
> water) → Act 5 **urban insurgency** / storm the Residency. Sabotaging harvesters makes no sense in
> Arrakeen — the objective fits the place.
>
> **Vision economy — lootable gear, made real by the sietch (canon-safe).** Nothing in a vision
> "really" happens (a Fremen never truly kills the Baron), yet loot is **fully lootable ARPG gear** from
> caches, enemies, **and bosses**. The vision is Paul's **recon** and the sietch acts on it: **gear** you
> "loot" is Paul's prescient **spec**, which the **Fremen smiths forge for real** on extraction (grounds
> the crafting bench D2 + Gurney/armory — a "Baron drop" is gear the vision marked necessary, not looted
> from a kill that never happened); **spice / water / materials** are real Harkonnen caches / spice blows
> / water stores the vision reveals and the **tribe then secures**. **Loss rules (crafting is expensive,
> so the kit is protected):** the **character + gear + full inventory are NEVER lost** (real kit in the
> sietch; a trance can't destroy them); **only this run's gathered spice / water / materials** are
> **extract-or-lose** (die → the thread collapses, that haul is never secured/forged). Death costs the
> run's *resource* haul, not gear or progress. (Extraction/forge-on-return = B4/B5.)
>
> **Run launch — no hub exit door (pure roguelike).** There is **no walk-out door**. Two launchers:
> the **Water-of-Life vision ritual** (Paul) launches a **campaign run** — from the start, the *only*
> launcher during the campaign; the **cartography table** launches **endgame resource raids**
> (procedural maps) and **unlocks only after the campaign is complete** (Baron defeated at Awaken 10).
> Removing the exit tile + building the ritual = **B2**; the progress-gated table = **B10**.
>
> **Sietch hub cast — a progress-gated, growing roster.** The hub **starts nearly empty** — only **Paul**
> + a crowd of ambient **Fremen** — and **fills as the campaign advances** (each named NPC gated on an
> `awaken` threshold in `Meta`), swelling as the rebellion gathers. Shopkeepers: **Stilgar** (the Naib —
> leadership / quests / Awaken selector, extends `EType::Naib`), **Lady Jessica** (Reverend Mother —
> abilities / passive-tree / Water-of-Life & consumables / poison-craft, elevates `EType::Sayyadina`),
> **Chani** (desert-craft — stillsuits / water / Sandcraft gear), **Gurney Halleck** (warmaster &
> smuggler contact — weapons / black-market, takes `EType::Smuggler`). Lore/quest residents: **Alia**
> (pre-born oracle), **Harah** (Jamis's widow / household), **Otheym** & **Korba** (Fedaykin). A **larger
> ambient Fremen crowd** (idle-wander + barks, reusing `kFremenSprites`/`kFremenBarks`) keeps the warren
> alive. Characters dead or with the Harkonnens by this era (Duncan, Kynes, Jamis, Yueh, Hawat) are
> **not** living hub NPCs. Code home: **B2** hub pass + **C8** vendors + **E2** art.
>
> **Narrative spine — bosses are informants (climbing the Harkonnen chain).** The **room/gauntlet
> bosses** are seeded Harkonnen informant-officers you break for intel (faceless → may drop materiel);
> **each Awaken level's pinnacle is a *named* informant one rung higher**, the intel pointing at the
> next. Pinnacles drop lootable gear like any ARPG (the sietch forges it real). **LOCKED roster, 2 per
> act / by biome:**
>
> | Act (biome) | Awaken | Pinnacle |
> |---|---|---|
> | 1 — Deep desert | 1 | Esmar Tuek (smuggler — a turned/coerced "ally" you must break) |
> | 1 — Deep desert | 2 | Wellington Yueh — flavor: Piter de Vries's stab wasn't fatal; Yueh lived |
> | 2 — Rimwall | 3 | Caid Orlan Kest |
> | 2 — Rimwall | 4 | Iakin Nefud (Captain) |
> | 3 — Shield Wall | 5 | Baron Harkonnen + Thufir Hawat (the *poisoned* Hawat, coerced into Baron's service) — kill Hawat, the Baron escapes |
> | 3 — Shield Wall | 6 | Bashar Vorek Harkonnen |
> | 4 — Outskirts | 7 | Piter de Vries — a replacement Tleilaxu clone en route to the Baron; you intercept & kill it (the original died to Duke Leto's poison gas) |
> | 4 — Outskirts | 8 | Glossu Rabban |
> | 5 — Arrakeen | 9 | Feyd-Rautha |
> | 5 — Arrakeen | 10 | Baron Harkonnen (defeated = ending) |
>
> *(Roster fills the `NodeType::Pinnacle` slot via a B3/B5 data table keyed by `awaken_level`.)*
>
> **Sietch replayability device (PoE "maps") + hidden Corrino super-bosses.** The **cartography table**
> (B10, post-campaign) lets the player craft/roll modified raids for bonus rewards — the endgame loop
> layered on the Awaken ladder. **Rare charts unlock hidden pinnacle bosses drawn from House Corrino** —
> Princess Irulan, Emperor Shaddam IV, Sardaukar command (and possibly the Truthsayer, Gaius Helen
> Mohiam) — optional super-bosses *beyond* the Baron arc.

### B1 — World graph + single resident zone ✅ *(B1.1 ✅, B1.2 ✅, B1.3 ✅)*
Per-run `world::Graph` (sietch hub + the campaign spine) + one resident `map::Zone`;
node_seed = node_hash(run_seed, i). **B1.1 ✅** — `world.h`: deterministic graph built at run
start (additive). **B1.2 ✅** — replaced `g.zones[3]` with one resident `g.zone_`;
`apply_deltas`/`spawn_zone_wildlife`/`spawn_op_in`/`place_npc_near` take a `map::Zone&`; op
look-ahead uses a temp buffer (retired in B3); serialize/deserialize store one zone's vis.
**B1.3 ✅** — reframed `world::Biome`→`Stage {Sietch, DeepDesert, Rimwall, ShieldWall, Outskirts,
Arrakeen}`; `build()` now lays the **5-act / 10-Awaken spine** (5 acts × 2 Awaken levels, each = 3
rooms + a pinnacle = 41 nodes; stage fixed by act — the **geography march to Arrakeen**, no
tier-parity roll). Added `cur_node` + `enter_node(idx)` (regenerates the resident zone from
`nodes[idx].seed` via `Stage→ZoneId`, clears per-node entities/op/deltas; ShieldWall/Outskirts/
Arrakeen use placeholder generators for now), `begin_run()`, graph-driven `use_door()`, retired edge
travel; serialize `cur_node` + node flags, `kSaveVersion` 4→5; dettest `46ae0056dfad38ea`, clicktest
`dcc065906642ebff` (rebaselined), seamtest round-trips a field node; full suite (9) green.
**Exit ✅:** launch into the graph, traverse node-to-node along the campaign spine, save/load round-trips.

### B2 — Gated run flow + Revelation gear picks ✅
Sietch = pure hub with **no walk-out door**; **Paul** is a hub NPC and clicking him runs the
**Water-of-Life ritual → `begin_run()`** (the physical exit tile is gone). Field nodes have an east
**forward door** that advances node-to-node. **Reward = a "Revelation"** (replaces the stat-boon idea):
at a forward door the vision reveals gear — a two-step **prescient gear suggestion from Paul** (pick 1
of 3 rolled *slots*, then 1 of 3 rolled *items* for it → into the pack), with a preview of where the
road leads. **B2a foundation:** the gear model became a **granular Fremen stillsuit paperdoll (10
slots)** — Weapon, Hood, Mask, NosePlugs, WaterTube, Body, Catchpockets, Pumps, Cloak, **Token** (the
ARPG amulet-of-identity); every piece contributes an intrinsic affix summed via `affix_total`
(+`AK_RECLAIM`/`AK_HEAT`); `kSaveVersion` 5→6. *(Per-slot stat balance is provisional until D1 affixes.
Carried Revelation gear becomes extract-or-lose / forged-on-return in B4/B5. Grace retreat deferred —
the spine terminates at the sietch, no softlock.)*
**Exit ✅:** ritual commits a run; advancing a field node opens the Revelation → pick slot → pick item
→ gear enters the pack + node advances; `reveltest` + full suite green.

### B3 — Elite gauntlet escorts + named pinnacle bosses ✅ *(core)*
`enter_node` spawns content **on entry**: Field nodes field an **elite Harkonnen escort** (count
scales with `node.awaken`); Pinnacle nodes field the **LOCKED named boss** for that Awaken level
(`kPinnacles[10]`: Tuek/Yueh/Kest/Nefud/Hawat/Vorek/Piter/Rabban/Feyd/Baron), scaled by awaken
(`data2 = kPinnacleTag - idx`). Killing a pinnacle marks the node `NF_CLEARED`; killing the **Awaken-10
Baron** ends the vision (victory), via melee + worm-swallow. Boss label/sprite handle named pinnacles.
`gauntlettest` (escort spawns; Awaken-10 pinnacle is the Baron; slaying him → victory) + full suite green.
**Deferred:** per-biome objective *variety* (currently all escorts), and fully retiring the legacy
op-scheduler/quota (kept — campaigntest still exercises it; harmless alongside nodes). Bosses use
`node.awaken` directly, so no B5 dependency.
**Exit ✅:** field escort spawns; pinnacle = the level's named boss; the Awaken-10 Baron kill = victory.

### B4 — Extraction via worm-ride (forge-on-return) ✅
Worm-riding is now the **one-way extraction verb**: mount a surfaced Maker in a field node, ride a
few strides, and `extract_run()` ends the vision → wake in the sietch, banking the run's spice and
persisting the character ("the Fremen forge what Muad'Dib showed you"). Maker hooks granted at run
start; free-roam dismount removed; no riding in the hub. `extractiontest` (field → surface → mount →
ride home → wake in the sietch) + `ridetest` green.
**Exit ✅:** ride home → wake in the sietch with the haul made real; the full death loss model is B5.

### B5 — Persistence & meta (+ Awaken ladder) ✅
meta.dat DME2→**DME3** (back-compat DME1/DME2): the **persistent character** (`CharStats` +
`equip[10]` + `inv[12]`), a **banked_spice** wallet, and the **Awaken ladder** (`awaken_unlocked`
1..10 + `awaken_cleared`). `persist_character()` snapshots on extraction + death. **Loss model:**
`extract_run` banks the run's spice + persists; `die` persists the character (level/gear kept) but
does **not** bank spice (the run's haul is lost). `game_init` restores the character + wallet for
interactive play. `begin_run()` launches at the chosen Awaken level's first node; clearing a pinnacle
(`award_pinnacle_clear`) unlocks the next level; Awaken-10 Baron = ending. `metatest` (kill Awaken-1
pinnacle → Awaken 2 unlocks) + full suite (13) green.
**Deferred:** water/materials wallets (materials are D2); the interactive Awaken *selector* UI at the
ritual (headless uses `awaken_choice`); moving `standing`/`crysknife` fully to meta.
**Exit ✅:** extract banks + persists; death keeps character/gear but loses the run's spice; clearing
a pinnacle unlocks the next Awaken level.

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

### B10 — The Jihad Table (endgame maps + pinnacle bosses) ⬜
**Reframed (2026-07-28):** after the campaign (Baron at Awaken 10) the player selects the
**Chosen Path** — enacting **Paul's Jihad** across the Imperium — which turns the cartography
table into the **Jihad Table**, the endgame map device. Its **pinnacle bosses** are the powers
behind the throne, each on a **special map of their homeworld** (PoE-pinnacle-arena style):
**Bene Gesserit** (Wallach IX — a Reverend Mother) · **Tleilaxu** (a Face-Dancer/Master) ·
**Ixians** (Ix) · **House Corrino** (Kaitain/Salusa — Shaddam, Irulan, a Sardaukar Bashar) ·
**Spacing Guild** (Junction — a Navigator). They drop the endgame D2 reagents — the **Ixian
Mechanism** (Fracturing) and the global-rare **Maker Hide** (Divine) — the true endgame chase,
distinct from the farmable A1–A10 act bosses. Supersedes the old cartography/Corrino framing
below (which stays as the mechanical basis for the map/chart machinery).

An in-hub station (a **cartography table** / Fremen route-cache) that gives the endgame
its own loop on top of the Awaken ladder. The player holds **route-charts** — a
consumable, craftable item that seeds and modifies a bonus excursion: rolled **modifiers**
(more/tougher elites, extra harvesters, hazards, worm density) in exchange for scaled
**reward mods** (spice/loot/material quantity + rarity), plus a **chart tier** that gates
depth. Slotting a chart into the table launches a run built from `hash_combine(chart_seed,
…)` (deterministic, no new resident-zone machinery — reuses the B1 graph + biome params);
completing it can drop **higher-tier charts** (the self-sustaining map economy) and the
rarest charts point at **hidden House Corrino super-bosses** — Princess Irulan, Emperor
Shaddam IV, Sardaukar command (optional pinnacles *beyond* the Baron arc) — and at
**Jacurutu (B9)**. Charts are crafted/rerolled at the **D2 bench** with materials, tying
itemization into replayability. Runs are extract-or-lose like any other. Files: new chart
item kind (items.h), `world::Graph` build-from-chart path, a cartography-table hub menu,
the Corrino boss table, chart drops in the loot roll.
**Exit:** find/craft a route-chart → slot it at the table → run a modified excursion whose
danger + rewards match the chart's mods → extract with the loot → it drops higher-tier
charts; deterministic across save/load.

---

## PHASE C — Character Systems

### C1 — Three-attribute foundation ✅
Conditioning / Versatility / Awareness → Life+Lethality / Precision+Flow /
Focus+Will-max; crit → gear only.
**Exit:** allocate the three attributes; each moves its derived pair; crit from
gear only. **Done:** `CharStats{str,dex,vit}` renamed to `{cond,vers,awar}`;
`player_max_hp`(Life)/`player_lethality`/`player_dodge`(Flow)/Precision-tohit all
re-sourced; new `player_max_shield`(Focus)/`player_max_will`; crit now `AK_CRIT`
(gear) + talent, never an attribute (added a Kindjal Charm token). Char sheet +
level-up allocator relabelled Conditioning/Versatility/Awareness.

### C2 — Energy Shield (Focus) + retire Holtzman ✅
PoE-style shield buffer over Life (absorbs all, recharges); ShieldTrooper →
heavy-armor bulwark; slow-blade rule + shieldtest retired.
**Exit:** an Awareness build soaks hits on the shield; it recharges; no Holtzman
anywhere. **Done:** `pl.shield`/`shield_delay`, `damage_player` soaks then Life,
recharge in `upkeep` (base Focus 0 so unspecced play is unchanged); slow-blade
branch + `BT_SHIELDED` + shimmer FX + "shielded" labels all removed; ShieldTrooper
is now a bulwark (higher armor/hp). `shieldtest` retired, **`focustest`** added
(soak → recharge → spill). Full SDL suite green; dettest rebaselined
`d9a06b6d27ca7e33` (confirmed via the SDL-free spike too).

### C3 — Base classes + branching ascendancies (PoE-style) ⬜
**Reworked (2026-07-29) — approved design:** three *basic* Fremen **base classes** (one per
attribute) that **ascend** into one of **three ascendancies** each (PoE base→ascendancy, branching).
Shared Will fuel. Full design record: the approved plan
`/root/.claude/plans/this-project-is-to-nested-planet.md`.

| Attribute | Base | Ascendancies (element → ailment) |
|---|---|---|
| Conditioning | **Knifehand** | **Fedaykin** (tank: Block/Counter/durability) · **Desert Ghost** (Storm Lethality → **Blind** + Evasion/Stealth) · **Water Reaver** (**Bleed** Lethality, bypasses armor + Siphon) |
| Versatility | **Wanderer** | **Saboteur** (traps/demolition) · **Wayfinder** (long-range weapons) · **Sunwalker** (**Scorch** — Heat DoT) |
| Awareness | **Oracle** | **Sayyadina** (**Poison** DoT + Focus) · **Reverend Mother** (control/Focus — Cold → **Slow/Freeze**) · **Spiceweaver** (Will nuker — more Will spent = more Lethality) |

**Ascension trigger (canon):** NOT the player drinking the Water of Life — that is **Paul's** alone
(how *he* gets the visions the player enacts). Ascension is a **sietch rite of passage** (riding the
maker / earning a crysknife / the naming), gated by the Awaken ladder; Paul's prescience marks the
path. Pick an ascendancy for your base at the rite.

**Ailment system (two-way, PoE-style) — the spine of the ascendancy design:** each element has a
status effect. Ascendancies grant **offensive** mastery (more elemental damage + inflict the
ailment), NOT resistance; **gear resistances (D1) are the defense**; and **enemies randomly roll
these elements** and inflict the ailments on the player, so every build needs resistance.
- **Scorch** (Heat, `DT_HEAT`) burning DoT — Sunwalker ↔ Heat Resist (`AK_HEAT`)
- **Slow/Freeze** (Cold, `DT_COLD`) control — Reverend Mother ↔ Cold Resist (`AK_RES_COLD`)
- **Blind** (Storm, `DT_STORM`) enemy accuracy down — Desert Ghost ↔ Storm Resist (`AK_RES_STORM`)
- **Poison** (Toxin, `AK_POISON`) DoT — Sayyadina ↔ Toxin Resist (`AK_RES_TOX`)
- **Bleed** (physical) DoT that **bypasses armor/Toughness** (only Fortitude/life mitigates) — Water Reaver + Siphon
Reuses D1's `DmgType` plumbing + the inert `AK_POISON`; the **combat-ailment pass** adds ailment
status timers on `Entity` + the enemy-rolls-an-element hook, and wires the ascendancy nodes.

**Proficiency comes from the ascendancy tree, not flat class bonuses** — a PoE-ascendancy-style tree
(new system, distinct from the C4 passive tree; talent nodes TBD "as we go"). Poison-mastery lives on
**Sayyadina**; other classes only have generic gear access. Today's `poisons` consumable is separate
(Saboteur *sabotage* — spoils a spice hold).

**Bene Gesserit vocabulary:** *Reverend Mother* is fair game for Fremen (canon Fremen RMs exist). A
future **Bene Gesserit class** stays distinct via ascendancies named for the box-tests — **Agony ·
Euphoria · Time**; keep *Acolyte / Truthsayer / Kwisatz Haderach* reserved for BG. `Sandrider` is
freed (worm-riding is universal via `maker_hooks`, not a class).

**Implementation scope (when C3 is built, per D1 patterns):** split `enum class Background` →
`enum class BaseClass { Knifehand, Wanderer, Oracle }` + `enum class Ascendancy { None, Fedaykin,
DesertGhost, WaterReaver, Saboteur, Wayfinder, Sunwalker, Sayyadina, ReverendMother, Spiceweaver }`;
rename `apply_background`→`apply_base_class`; update the char-create menu (`Background::Count` sites)
+ `meta.dat`/`save.dat` persistence (new field, magic/version bump with back-compat). Ascendancy
tree + sietch-rite ascension UI are their own systems. Verify with a `classtest` (create each base,
ascend each, `meta.dat` round-trip) + the full suite/determinism baseline.
**Exit:** each base starts distinct; ascension at the sietch rite grants an ascendancy that persists
across a death.

### C4 — Passive skill tree (PoE-style) ⬜
One shared node graph, class start per corner, adjacency-gated allocation, passive
points per level.
**Exit:** allocate an adjacent node → the derived stat moves; hybrids by pathing;
persists across a death.

### C5 — Sietch talent tree + split currencies ⬜
Meta talent tree bought with banked spice; Sandcraft + tribe's-blessing tracks;
currencies stay distinct.
**Exit:** spend banked spice on a Sietch node → persists across a death.
**Economy (locked 2026-07-28):** two currencies — **Spice = "gold"** (general trade + crafting
base cost + undo-Consecration) and **Litrejons = tribal water**, the **sietch-tree currency**
(spending them = contributing water to the tribe). Litrejons are banked via a new **Deathstill**
mechanic: slain enemies yield reclaimed water (scaled by the *of the Deathstill* affix
`AK_WATER_FIND` + enemy size), rendered into litrejons on **extraction** (lost on death — the water
portion of the run stake). Closes the loop combat → Deathstill → litrejons → sietch upgrades.

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

### D1 — PoE-style affixes ✅
Widen `Inst` for item-level + multiple tiered prefixes/suffixes + rarities.
**Exit:** a rare shows item-level + tiered affixes; deeper runs roll higher tiers;
the vault round-trips. **Done:** `Inst` widened (ilvl + rarity + ≤3 prefix/≤3
suffix `RolledAffix` + reserved `quality`/`enchant`); a **~40-family spec-divergent
pool** (`kFamilies` in items.h) with **5 Awaken-gated tiers** + PoE **spawn weights**
+ slot-gating; ~28 new `AffixKind`s wired into the derived-stat math (eff-attributes,
flat+% increases, crit-mult, Siphon/on-hit/on-kill, Life/Focus regen, a live Will
pool, Fortitude/Block/Deflect, a **4-type resistance system** (Heat/Cold/Storm/Toxin,
canon non-magical hazards, no casters), Spice/Rarity/XP/Water find, Thrift). On-map
loot moved to a serialized `g.loot_inst[]` pool (retired the 32-bit entity packing).
Save `v6→v7`; vault **DME3→DME4** with a legacy converter (banked items survive).
Generated Fremen rare titles + an in-menu affix detail readout. New tests
`statmodtest`/`affixtest`/`metamigratetest`; full suite green; dettest rebaselined
`0bd2007def2ca2bb` (SDL-free spike confirms). *(One stat, AK_POISON outgoing-poison
potency, is defined but inert — no clean player-poisons-enemy mechanic yet; wire in a
combat pass.)*

### D2 — Sietch Crafting Table (D4×PoE blend, canon Fremen materials) ⬜
A single sietch bench (Diablo-4 place-item UX) that replays **PoE's base crafting
currencies** — each mapped to a **stillsuit fabrication material** dropped **randomly,
weighted by rarity**: Osmotic Weave=**Quality** · Fremseal=**Transmute** ·
Thermofilament=**Augment** · Microsieve Mesh=**Alteration** · Absorbent Matrix=**Alchemy**
· Duraluminum Fitting=**Regal** · Shigawire Coil=**Chaos** · Precipitator Cell=**Exalt**
· **Maker Hide**=**Divine** (rarest, Jihad-Table-only global drop) · **Ixian Mechanism**=
**Fracturing** (lock 1 random affix; from Jihad-Table pinnacle bosses). **Consecration**
(a Sayyadina rite → the reserved `Inst.enchant`) costs **Spice Essence** to apply / plain
spice to undo. Crafting is **mostly random + lossy**; the two endgame reagents don't hand
you BiS; every op is **ilvl-capped** + reforges carry a per-item attempt budget. Reuses
D1's `RolledAffix`/tier model + `roll_affixes`.
**Exit:** craft an item's affixes for material cost; a bricked item re-rolls;
banked materials survive a death.

### D3 — Flasks (Life / Will / Utility) ⬜
A flask belt; charges refill on kills/time; used via number keys.
**Exit:** equip Life/Will/Utility flasks; number keys consume a charge; charges
refill on kills.

---

## PHASE E — UI Polish + Art

### E0 — Front-end: title menu + field manual ✅ *(added mid-project)*
A "login"/title screen shown at launch (interactive only; headless tests jump
straight to Play): **Enter the Sietch / Field Manual / Options / Leave the
Desert**, keyboard + mouse. **Options**: sound on/off, fullscreen toggle, wipe
character+vault (platform reads game_muted()/game_consume_fullscreen_request()).
**Field Manual**: an 8-page paged primer (premise, controls, water/heat, combat &
classes, harvesters & the worm, extraction, the sietch, gear & crafting) so new
players learn the game. New game state `GameState::Screen` (Menu/Options/Manual/
Play); the world is paused on the front-end.
**Exit:** ✅ launch opens on the menu; Manual pages turn; Options toggle; Enter
starts play; 10/10 tests green (dettest unchanged — headless skips the menu).

### E1 — Diablo character/inventory + drag-and-drop ⬜
Paperdoll + equip slots + flask belt + grid; mouse drag-and-drop; affix tooltips.
**Exit:** hovering shows affixes; drag equips/swaps/reorders; keyboard still works.

### E1b — Character roster + creation (10 slots) ⬜ *(added mid-project)*
Replace the single-class chooser with a **character-select screen** off the title
menu: up to **10 saved character slots** (name, class, level, look), pick one to
continue or **New Character** → **type a name** + **choose a class**. Needs SDL
text-input plumbed into InputState and a multi-character roster in meta.dat
(pairs with **B5** persistence — each slot stores its persistent character +
banked vault). Delete-slot with confirm.
**Exit:** launch → pick from up to 10 named characters or create one (name +
class); the roster persists across launches; deleting a slot frees it.

### E2 — Art regen + terrain/animation refinement ⬜
Portraits, item/flask/material icons, sietch/cave tiles, footprint decals, walk
re-timing. **Plus (added mid-project):** generated **main-menu art + an animated
background** for the title screen, and **regenerated character-select art**
(class portraits / full-body renders for the roster + creation screen), and the
hand-authored isometric prop/decor art the billboard hub is a placeholder for.
**Exit:** atlas rebuilds; portraits + icons + menu art render; animated title
background; terrain visibly varied; no missing-asset fallbacks.
