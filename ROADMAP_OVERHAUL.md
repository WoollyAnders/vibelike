# DUSTFALL — Post-Playtest Overhaul Roadmap

The first playtest produced a large, coherent set of change requests plus one
critical rendering bug. This roadmap turns that feedback into an execution plan:
DUSTFALL becomes a **roguelike / extraction / ARPG blend** — a *persistent
character* runs *procedural excursions* out of the sietch through a *branching
world map* shaped like *Hades* (**12 regular areas across 4 tiers, each tier
capped by a boss = 4 bosses**), extracting by **riding a sandworm home** to bank
loot or dying and losing what was carried. The campaign climbs an **"Awaken"
difficulty ladder (1→10, Hades "Heat")**: each level has a dedicated pinnacle boss,
gated so only clearing the current level advances the story, culminating in **Baron
Harkonnen at Awaken 10** (a first, undefeatable confrontation at Awaken 5).

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

> **DESIGN PIVOT (mid-project): Hades-style gauntlet climb inside Paul's prescient visions.**
>
> **Framing — the runs are Paul's possible futures.** The player is a **Fremen** (Fedaykin /
> Saboteur / Sayyadina) — **not** Paul; no Voice, no weirding way. **Paul Muad'Dib is an NPC in
> the sietch hub** (Sietch Tabr, in parallel to the novel). **A run begins by drinking the Water
> of Life** — the poison Paul transmuted after he awoke as Kwisatz Haderach, shared at the Fremen
> tau gathering — which links the Fremen to Paul's prescience. Each run = **walking one of Paul's
> *possible* futures: a directed journey across Arrakis to Arrakeen to confront the Baron.**
> Because they are *possible* futures, **book canon is never broken** — bosses "die" only within
> the vision. **"Awaken"** = awakening prescient sight; the ladder = ever-deeper, harder visions.
>
> **Within a run** — a climb of **combat rooms**, not time-scheduled harvests. Each field node is
> a ROOM: enter → fight an **elite-mob escort** → **deny/loot the harvester it guards** (the room's
> reward) → **pick 1 of a few boons** (run-scoped powerups, Hades-style) → advance through a
> previewed forward door. The graph is **4 tiers of 3 rooms (12 regular areas), each tier capped by
> a boss (4 bosses)**; difficulty escalates tier by tier. The 4th (summit) boss is the run's
> **pinnacle**. The old op-scheduling (schedule_ops, global quota, deadlines, non-current-zone
> look-ahead) is **retired** — the harvester + escort now **spawn on room entry**, which is what lets
> B1 use a single resident zone. **Extraction = hybrid:** after clearing a tier boss you may **ride
> the worm home to bank** carried spice/gear/boons-as-meta and end the run, or **push deeper** for
> more; **die anywhere = lose everything carried.**
>
> **Environments — the journey to Arrakeen (curated, NOT procedural biomes).** The campaign's
> environments are **fixed per tier**, a geography march toward the city: **Tier 1 deep desert /
> open erg** (Tanzerouft) → **Tier 2 rimwall & the Shield Wall** → **Tier 3 graben-pan &
> Harkonnen-held outskirts** → **Tier 4 Arrakeen city & the Residency** (the pinnacle waits here).
> The campaign **never rolls its biome** — the tier decides it (`node.seed` still varies room
> *layout* within a tier; that is not "biome generation"). **Procedural biome *variety* is an
> endgame-only feature**, living exclusively in the B10 cartography "maps." A distinct **Arrakeen
> city generator** (streets/buildings/the Residency) is a new environment — its gen + art is its own
> downstream task.
>
> **Across runs — the "Awaken" ladder (Hades "Heat").** 10 escalating levels, **Awaken 1 … 10**.
> You advance the story only by clearing the current level's run and defeating its **pinnacle boss**,
> unlocking Awaken N+1. Re-running a cleared level yields **no better drops and no campaign progress**
> (pure practice/farm). **Awaken 10**'s pinnacle is the defeatable **Baron Harkonnen** — beating him
> is the ending. (`awaken_level` + `awaken_cleared` live in meta.dat — B5; the graph's
> `NodeType::Pinnacle` summit slot is filled by the Awaken level's boss — B3.)
>
> **Run launch — no hub exit door (pure roguelike).** There is **no walk-out door** in the sietch.
> Runs launch from hub interactions, gated by progress: the **Water-of-Life vision ritual** (Paul)
> launches a **campaign run** — available from the start, the *only* launcher during the campaign;
> the **cartography table** launches **endgame resource raids** (procedural "maps") and **unlocks
> only after the campaign is complete** (Baron defeated at Awaken 10). Removing the physical exit
> tile + building the ritual is **B2**; the progress-gated table is **B10**.
>
> **Sietch hub cast — a progress-gated, growing roster.** The hub **starts nearly empty** — only
> **Paul** + a crowd of ambient **Fremen** — and **fills as the campaign advances** (each named NPC
> gated on an `awaken` threshold in `Meta`), the sietch swelling as the rebellion gathers.
> Shopkeepers (roles to finalize): **Stilgar** (the Naib — leadership/quests/Awaken selector, extends
> `EType::Naib`), **Lady Jessica** (Reverend Mother — abilities / passive-tree / Water-of-Life &
> consumables / poison-craft, elevates `EType::Sayyadina`), **Chani** (desert-craft — stillsuits /
> water / Sandcraft gear), **Gurney Halleck** (warmaster & smuggler contact — weapons / black-market,
> takes `EType::Smuggler`). Lore/quest residents: **Alia** (pre-born oracle), **Harah** (Jamis's
> widow / household), **Otheym** & **Korba** (Fedaykin). A **larger crowd of ambient Fremen**
> bystanders (idle-wander + barks, reusing `kFremenSprites`/`kFremenBarks`) keeps the warren alive.
> Characters dead or with the Harkonnens by this era (Duncan, Kynes, Jamis, Yueh, Hawat) are **not**
> living hub NPCs — some appear as bosses or in lore. Code home: **B2** hub pass + **C8** vendors +
> **E2** art.
>
> **Narrative spine — bosses are informants (climbing the Harkonnen chain).** A Fremen insurgency
> striking back: the **3 per-run tier bosses** are seeded Harkonnen informant-officers you break for
> intel; **each Awaken level's pinnacle is a *named* informant one rung higher**, the intel pointing
> at the next. **LOCKED Awaken→pinnacle roster:**
>
> | Awaken | Pinnacle |
> |---|---|
> | 1 | Esmar Tuek (smuggler — a turned/coerced "ally" you must break) |
> | 2 | Wellington Yueh |
> | 3 | Caid Orlan Kest |
> | 4 | Iakin Nefud (Captain) |
> | 5 | Baron Harkonnen + Thufir Hawat — you kill Hawat, the Baron escapes |
> | 6 | Bashar Vorek Harkonnen |
> | 7 | Piter de Vries |
> | 8 | Glossu Rabban |
> | 9 | Feyd-Rautha |
> | 10 | Baron Harkonnen (defeatable = ending) |
>
> *(Roster fills the `NodeType::Pinnacle` slot via a B3/B5 data table keyed by `awaken_level`.)*
>
> **Sietch replayability device (PoE "maps") + hidden Corrino super-bosses.** The **cartography
> table** (B10, post-campaign) lets the player craft/roll modified raids for bonus rewards — the
> endgame loop layered on the Awaken ladder. **Rare charts unlock hidden pinnacle bosses drawn from
> House Corrino** — Princess Irulan, Emperor Shaddam IV, Sardaukar command (and possibly the
> Truthsayer, Gaius Helen Mohiam) — optional super-bosses *beyond* the Baron arc.

### B1 — World graph + single resident zone ⬜ *(B1.1 ✅, B1.2 ✅)*
Per-run `world::Graph` (nodes = sietch hub + 4 tiers of 3 room nodes each capped by a
boss, summit `Pinnacle`) + one resident `map::Zone`; node_seed = node_hash(run_seed, i).
**B1.1 ✅** — `world.h`: deterministic graph built at run start (additive; not yet
driving zones). **B1.2 ✅** — graph reshaped to **17 nodes = sietch + 12 fields + 4
bosses** (kTiers=4, kSectionsPerTier=3); replaced `g.zones[3]` with one resident
`g.zone_`; `apply_deltas`/`spawn_zone_wildlife`/`spawn_op_in`/`place_npc_near` take a
`map::Zone&`; the op look-ahead into a non-current zone uses a temp buffer (retired in
B3); serialize/deserialize store one zone's vis; save.dat format changed (roguelike, no
migration); dettest hash unchanged (`af5340c1f7cb0ba2`), seamtest green. **B1.3 (next):**
reframe `world::Biome`→`Stage {Sietch, DeepDesert, Rimwall, Graben, Arrakeen}` with `build()`
assigning each tier its stage by the **geography march to Arrakeen** (deletes the tier-parity
biome toggle — the campaign no longer *rolls* its environment); add `cur_node` + `enter_node(idx)`
graph traversal (regenerate the resident zone from `nodes[idx].seed` via a `Stage→ZoneId` map;
Graben & Arrakeen use placeholder generators for now); a `begin_run()` launch trigger repoints the
existing sietch-exit interaction; serialize `cur_node` + node flags, bump `kSaveVersion` 4→5;
rebaseline dettest/clicktest once.
**Exit:** launch into the graph, traverse node-to-node (tier 1→4), save/load round-trips.

### B2 — Gated run flow + boons (Hades rooms) ⬜
Sietch = pure hub; leaving via the door **commits a run** (enter node 1). Each room:
clear the elite escort → deny/loot the harvester → a **boon pick** (2-3 run-scoped
powerups) → a **previewed forward door** (biome/threat/reward hint) to the next node.
Tier-end funnels to the boss room; grace retreat to the sietch only before the first
room is cleared. New: a Boon system (run-scoped stat/ability modifiers) + the boon-pick
and door-preview overlays.
**Exit:** leaving commits; clear a room → choose a boon → advance; bosses cap tiers.

### B3 — Elite gauntlet rooms + tiered bosses ⬜
Room content spawns **on entry**: an escort pack of **elite mobs** (scaled to the tier)
guarding the section's harvester; killing the escort lets you deny the harvester for
spice + loot. **3 rooms per tier then a boss** (`NodeType::Boss`, tiers 1-3) via the
champion generator, scaled by tier **and by the run's Awaken level** — these are the
**seeded Harkonnen informant-officers** you break for intel (the insurgency loop). The
tier-4 summit is `NodeType::Pinnacle`: a **named** informant selected by the Awaken level
(B5) from the inner-circle roster (Piter / Rabban / Feyd / … → **Baron Harkonnen** at
Awaken 10, defeatable = ending; **withdraw-undefeated Baron confrontation at Awaken 5**).
Retire schedule_ops / quota / deadlines / climax_pending; `denied`+`alert` become per-run.
**Exit:** clear 3 elite rooms before each tier boss; bosses scale with tier + Awaken; the
summit yields the Awaken pinnacle (Baron at 10); campaigntest updated (clear rooms → tier
boss → pinnacle).

### B4 — Extraction via worm-ride (hybrid bank-out) ⬜
After clearing a **tier boss**, a worm-ride-home verb banks carried inv + spice +
materials + this run's boons-as-meta and ends the run — or push deeper. Bait/mount with
a thumper + maker hooks (as before). No mid-tier extraction; **die anywhere = lose the
carry**.
**Exit (reworked extractiontest):** clear a tier boss → ride home → land in the sietch
with loot banked; dying instead loses everything carried.

### B5 — Persistence & meta (+ Awaken ladder) ⬜
Persistent character + banked wallets in meta.dat ('DME2'→'DME3', back-compat);
death keeps the character, clears carried gear/spice/materials. **Awaken ladder** lives
here: `awaken_level` (currently unlocked, 1..10) + `awaken_cleared` (highest defeated)
in meta; **a run is launched at a chosen Awaken level ≤ unlocked**; clearing the pinnacle
at your current top level unlocks the next; re-running a cleared level gives **no drop
bonus / no story progress** (scaling reads `awaken_level`). Defeating the **Awaken-10
Baron** sets the ending flag. Sietch UI: an Awaken selector at the run-start door.
**Exit:** level up + extract; next run die; character/level/vault/banked spice + Awaken
progress survive; carried gear + spice gone; clearing a level's pinnacle unlocks the next
and re-running a cleared level grants no bonus.

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

### B10 — Sietch cartography (PoE-"maps" replayability) ⬜
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
