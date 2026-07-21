# DUSTFALL — Extraction & RPG Roadmap

The pivot: DUSTFALL becomes an **extraction roguelike**. Runs are still
sabotage seasons on the sand — but now the dead drop **gear**, gear has
**stats and affixes**, killing feeds a **level**, and anything you haul back
to the sietch **vault survives your death**. Risk the deep desert wearing
your best kit and you can lose it; bank it and it outlives you.

Combat moves to Diablo-style resolution: stats, to-hit rolls, damage ranges,
armor, crits — visible numbers, meaningful gear.

Milestones follow the house rules: done means the **Exit** criterion runs in
the game, and the sim stays deterministic (dettest must hold).

---

## X0 — Canonical sprite scale ✅
Every generated model was fit-to-frame, so relative sizes are wrong (a hound
fills its box as boldly as a man). Add a **figure-height table** to the
pipeline: each asset declares its canonical on-screen height (humans 52px,
hound 22px, wormhead 104px, thumper 26px, ...) and `fit_sprite`/
`slice_turnaround` scale to that height instead of filling the frame, all
bottom-anchored to a shared baseline. Regenerate every sheet from gen_cache
(no new API spend).
**Exit:** a lineup screenshot — player, trooper, hound, naib, thumper,
cistern side by side — reads with believable relative scale.

## X1 — Items & loot ✅
- `game/items.h`: constexpr **ItemDef** table (~20 defs) — weapons (dust
  knife → crysknife tiers), armor (rag wrap → carapace plate), stillsuits
  I–III (absorbing the suit multiplier), trinkets — each with level, damage
  range / armor / suit rating, and spice value.
- **Affixes**: prefix + suffix pools (Keen +dmg, Watered +max water, of the
  Maker −noise, of Ibad +tox resist, Guarded +armor, of the Hawk +to-hit...).
  An item instance is 4 bytes: def, prefix, suffix, rarity (common / fine =
  one affix / rare = both).
- **Inventory**: 12 slots + 4 equipment slots (weapon / armor / suit /
  trinket). `I` opens the inventory screen: equip, inspect, drop.
- **Drops**: human enemies (trooper, shield-trooper, spotter, hermit) roll
  loot on death — ground satchel entity, `G` to pick up, rarity echoed in
  the pickup line.
**Exit:** kill a trooper, pick up a "Keen Dust Knife of the Hawk", equip it,
see the stats change.

## X2 — Diablo-style combat ✅
- **Character stats**: Strength (damage), Dexterity (to-hit, crit, dodge),
  Vitality (HP). Enemies get level / to-hit / damage range / AC / XP tables.
- **Resolution**: to-hit roll vs dodge/AC (clamped 15–95%), weapon damage
  range + Str bonus, armor mitigation, Dex-driven crits (x2, distinct
  message + heavier hit feel). Misses whiff visibly. All rolls on the
  combat RNG stream — determinism preserved.
- Holtzman rules survive intact on top (the slow blade bypasses the shield
  check, not the roll).
**Exit:** combat log shows rolls ("You hit the trooper for 7." / "The
trooper's blade misses you." / "CRIT — 14!") and gear/stats visibly move
the numbers.

## X3 — Leveling ✅ *(+ talent tree)*
XP per kill by enemy level (scaled down when out-leveled), quadratic-ish XP
curve, level-up grants +2 stat points and a full heal; character screen
(`P`) shows stats/level/XP and spends points. HUD gains level + XP bar.
**Exit:** grind two levels in the Rimwall, allocate points, watch to-hit and
HP move.

## X4 — The extraction loop ✅
- **The vault**: a new menu at the Naib — deposit carried gear into the
  tribe vault, withdraw banked gear while in the sietch. The vault (24
  slots) persists in the meta file **across runs and deaths**.
- **The stakes**: death loses everything carried and equipped; only the
  vault endures. Ending screens report gear lost / gear banked.
- New runs start at the chooser as ever; your banked arsenal waits at the
  Naib.
**Exit:** kill, loot, bank an item, die on purpose, start a new season,
withdraw the same item — it survived; a twin item left un-banked did not.

## X5 — Integration & proof ✅
Enemy stat/drop tiers scale with alert and region; death/ending screens
enumerate lost gear; `--rpgtest` proves the loop headlessly (kill → XP →
level → loot → equip → deposit → fresh init → withdraw); the full
regression suite (now 9 tests) passes and dettest stays bit-identical
through pure-presentation changes (combat changes legitimately change sim
hashes — dettest re-baselines once, then holds).
**Exit:** all tests green; a played season features loot decisions,
level-ups, and at least one agonizing "extract now or push deeper?" moment.

---

# Part II — Run Structure (the roguelike / extraction / ARPG blend)

DUSTFALL's genre, stated: **roguelike** (procedural, per-run progression),
**extraction** (bank it or lose it), **ARPG** (gear, levels, bosses).

## Y1 — Dune-flavored sheet & honest death ✅/⬜
Stats renamed: Strength → **Ferocity**, Dexterity → **Weirding** (the
weirding way — speed, precision, evasion), Vitality → **Endurance**.
Death and raid screens enumerate every piece of gear that just returned to
the sand — the extraction sting made visible.

## Y2 — Difficulty & the extraction gate ✅/⬜
A run **difficulty** scalar (harvesters denied + alert tier) scales drop
tables: higher item-level defs and better rarity odds the deeper the run.
The vault refuses deposits until you have denied at least one harvest —
extraction must be earned ("The Naib banks nothing for the unblooded").

## Y3 — Champions & the Baron ✅/⬜
Every denied harvester summons a **randomly generated champion** — named
from a seed ("Warmaster Dreth the Unbowed"), 2 random traits (Shielded /
Swift / Brute / Stalwart / Toxic), stats scaled by how many harvesters have
fallen — Hades-style escalation. Champions drop guaranteed rares. Denying
the climax harvest summons **BARON HARKONNEN** himself (shielded, brutal,
unique): victory now means killing him, not just his machine.

## Y4 — Side objectives ✅/⬜
Timed opportunities bloom in the desert: a **smuggler caravan** (reach it
before it moves on: contraband gear), a **Spacing Guild envoy** (pay the
spice bribe to reset the discovery countdown and cool the alert — the Guild
keeps the Baron's surveys blind). HUD tracks the active objective and its
clock; more objective kinds slot into the same frame later.

## Y5 — Proof ✅/⬜
campaigntest updated: victory leg now kills the Baron blade-to-shield;
boss/objective debug hooks; the full suite passes.
