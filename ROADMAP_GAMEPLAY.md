# DUSTFALL — Gameplay Overhaul Roadmap (Animation & Game Feel)

The sim is turn-based and stays turn-based — but the *presentation* should move
like Diablo: characters that face where they walk, swing when they strike,
kneel when they plant, and glide between tiles instead of teleporting. This
roadmap turns static bouncing PNGs into an animated world.

Same rules as the main roadmap: a milestone is done only when its **Exit**
criterion is demonstrated in the running game.

---

## G1 — Animation core ✅
The foundation everything else sits on.
- **Atlas v2**: sheets gain rows — an entry becomes `(state, direction, frame)`
  addressed as grid cells, with an `anims.txt` sidecar per sheet: state name,
  row range, frame count, ticks-per-frame, loop/one-shot, and **event frames**
  (e.g. `impact@3`) that fire gameplay callbacks.
- **Animator**: a small per-entity state machine (idle / walk / attack / use /
  hurt / death) that owns current state, frame clock, and facing; one-shot
  states fall back to idle; event frames invoke a callback (`on_impact`,
  `on_footstep`).
- **Smooth movement**: entities keep a visual pixel position that glides
  toward their logical tile (~4px/tick); turn resolution stays instant in the
  sim. Input buffering: a held direction queues the next step the moment the
  glide lands, so walking reads as continuous motion.
**Exit:** the player glides tile-to-tile with a looping walk cycle and stops
into an idle breathe loop; a debug overlay shows state/frame/facing.

## G2 — Directional character ✅
- **Facing**: 8-way, tracked from the last move/attack input; W/NW/SW render
  as mirrored E/NE/SE so only 5 generated directions are needed (S, SE, E,
  NE, N).
- **Generation strategy**: prompt gpt-image-1 for a single **rotation sheet**
  ("the same character in 5 views, turnaround sheet") per background so views
  stay consistent; a new `tools/slice_rotation.py` cuts, aligns and
  height-normalizes the views; walk frames synthesized per view (lean + bob +
  alternating stride shear) until/unless true per-frame art proves viable.
- All three backgrounds (Fedaykin / Saboteur / Sayyadina) plus trooper,
  spotter and hound get directional sheets; static props stay single-view.
**Exit:** walking a circle shows the character turning through all 8 facings,
enemies face the player as they hunt, and mirrored views are seam-free.

## G3 — Attack & action animations ⬜
- **Attack state**: windup → impact → recover for player and melee enemies;
  damage, sound and hit-flash land exactly on the `impact` event frame (the
  sim result is precomputed; the animation replays it).
- **Hit reactions**: victims flash bright + nudge 4px away from the blow;
  kills play a fall/crumple one-shot before the corpse sprite swap.
- **Use animations**: kneel-and-plant for thumpers and demolition charges
  (the 3-turn channel shows progressive placement), crouch-and-work for
  deathstill harvesting, a short underhand toss arc for glowglobes (the globe
  visually flies to its tile, then ignites its light).
- **Turn choreography**: when multiple world actors act in one player turn,
  their animations play overlapped but ordered (player first, then NPCs by
  distance), capped at ~12 ticks so turn cadence never drags.
**Exit:** a fight against two hounds reads as an exchange of visible blows —
swing, impact flash, lunge back — and planting a thumper shows the kneel and
the spike driven into sand before it starts pounding.

## G4 — World motion ⬜
- **Worm slither**: surfaced segments interpolate along their path with
  per-segment phase lag (body flows, not a chain of teleports); the head gets
  a strike lunge when it crushes; burrowed ripples ripple *along* the travel
  direction.
- **Crawler machinery**: intake churn becomes a 4-frame loop with dust
  particles synced to it; floodlight cones flicker subtly at night.
- **Carryall**: flies in from the map edge as an animated shadow + engine
  rumble crescendo, hovers over the crawler during lift, and departs — not a
  popping sprite.
- **Environmental idle motion**: heat-shimmer palette wobble in SCORCHING
  hours, wind-blown sand streak particles drifting across open erg, banner
  cloth flutter in the sietch.
**Exit:** watching a harvest op from a dune at night — churning crawler,
flickering floodlights, carryall fly-in, worm ripples flowing — looks alive
with zero player input.

## G5 — Feel pass ⬜
- Footstep dust puffs sized by terrain (big on drum sand) via animation
  events; audio footsteps alternate L/R samples.
- Impact particles: blood flecks on flesh hits, sparks on the crawler hull,
  sand spray on worm breach (already particled — synced to event frames).
- **Hit-stop**: 2-3 tick freeze on killing blows; **micro-shake** on your own
  hits landing (distinct from worm rumble shake).
- SFX retrigger discipline: animation events drive audio so sound and motion
  never desync; UI orb sloshes when values change instead of snapping.
**Exit:** a 30-second play clip feels contemporary-retro — nothing pops,
nothing teleports, every action has weight — side-by-side against the current
build the difference is unmistakable.

---

## Dependencies & risks
- G1's atlas v2 must land before any directional art is generated (G2+ art
  is authored against the grid format).
- gpt-image-1 turnaround-sheet consistency is the main art risk; the
  mitigation is the mirror trick (5 views), height normalization in the
  slicer, and synthesized walk frames. If per-view quality diverges too far,
  fall back to 4 facings (S/E/N + mirror) before hand-fixing pixels.
- Turn choreography (G3) touches the sim/render seam: the sim stays
  authoritative and instant; animation is strictly a replay layer. No
  gameplay state may ever wait on an animation.
