#pragma once
// Every balance constant in the game lives here.

namespace tune {

// --- time ---
inline constexpr int kTurnsPerDay = 300;
// RT1 (real-time conversion): the sim runs at kTickHz (60) fixed ticks/sec. A "world tick"
// (the old per-turn upkeep+AI step) fires every kWorldStepTicks sim-ticks (game_update timer,
// RT1.2); the player may take a grid action at most once every kPlayerActionTicks (action cooldown).
inline constexpr int kTickHz = 60;
inline constexpr int kWorldStepTicks = 9;    // ~0.15 s per world tick
inline constexpr int kPlayerActionTicks = 7; // ~0.12 s player action cooldown

// RT2: ailment durations are expressed in SECONDS but stored as integer world-tick counts
// (1 world tick = kWorldStepTicks / kTickHz s). secs_to_ticks converts at compile time; the sim
// uses integers only (float determinism is not safe across the firewall's two builds).
inline constexpr int secs_to_ticks(double s) { return int(s * kTickHz / kWorldStepTicks + 0.5); }

// Freeze (Cold): a cold hit freezes for a duration scaled by cold_dmg : target eff-max-Life,
// clamped to [0.3, 3.0] s (ratio 1.0 == full). Below the 0.3 s floor (ratio < ~0.1) -> no freeze.
inline constexpr int kFreezeMinTicks = secs_to_ticks(0.3);  // 2 ticks
inline constexpr int kFreezeMaxTicks = secs_to_ticks(3.0);  // 20 ticks

// Stun (PoE-style threshold + chance): a hit whose damage is >= kStunThresholdPct of the target's
// eff-max-Life can stun. Chance ramps with the excess ratio; duration scales with the ratio,
// clamped [0.2, 1.5] s. Stun buildup (offense) inflates the ratio; stun reduction (defense) lowers
// both chance and duration. Brief action-lockout, both ways.
inline constexpr int kStunThresholdPct = 15;   // hit must be >= 15% of max Life to threaten a stun
inline constexpr int kStunRatioCapPct  = 100;  // ratio saturates here (a hit for full Life)
inline constexpr int kStunChanceNum    = 2;    // chance% = (ratioPct - threshold) * this, capped 100
inline constexpr int kStunMinTicks = secs_to_ticks(0.2);  // 1 tick
inline constexpr int kStunMaxTicks = secs_to_ticks(1.5);  // 10 ticks

// C3 (defensive half): deep runs make enemy hits ELEMENTAL — a chance (scaling with Awaken depth)
// to inflict a random element's ailment on the player, each mitigated by the matching gear
// resistance. Gated so shallow runs (depth < kElemEnemyMinAwaken) never roll — early/headless play
// stays plain physical and deterministic. So every deep build must cap its resistances.
inline constexpr int kElemEnemyMinAwaken = 3;      // elemental enemy hits begin at Awaken 3
inline constexpr int kElemEnemyChancePerDepth = 6; // +6% chance per Awaken level above the threshold

// --- player ---
inline constexpr int kPlayerHpMax = 20;
inline constexpr int kPlayerAtk = 3;
inline constexpr int kWaterMax = 1000;
inline constexpr int kStartWater = 700;

// --- thirst (per turn, before multipliers) ---
inline constexpr int kThirstSand = 2;
inline constexpr int kThirstRock = 1;
inline constexpr int kThirstIndoor = 0;
// ambient heat 1..9 -> thirst multiplier x4 (fixed point /4)
inline constexpr int kThirstHeatMul4[10] = {2, 2, 2, 3, 4, 4, 5, 6, 8, 8};
inline constexpr int kSuitTier1Mul4 = 3;  // stillsuit I: x0.75
inline constexpr int kDehydrateDamage = 2;  // hp per turn at 0 water

// --- heat / exposure ---
// ambient heat by hour 0..23 (1 = frigid, 9 = scorching)
inline constexpr int kAmbientByHour[24] = {2, 2, 1, 1, 1, 2, 3, 4, 5, 6, 7, 8,
                                           9, 9, 9, 8, 7, 6, 5, 4, 3, 3, 2, 2};
inline constexpr int kExposureHotThreshold = 8;   // ambient >= this builds exposure
inline constexpr int kExposureGain = 4;           // per turn in direct sun
inline constexpr int kExposureRecover = 8;        // per turn in shade/indoors/night
inline constexpr int kExposureSlow = 50;          // moves cost 1.5 turns of energy
inline constexpr int kExposureCollapse = 90;      // damage per turn
inline constexpr int kExposureDamage = 2;

// C2: Energy Shield (Focus, from Awareness) — a buffer that soaks damage before
// Life, then recharges after a quiet spell. Base (uninvested) Focus is 0, so an
// unspecced character behaves exactly as before; an Awareness build gets a real buffer.
inline constexpr int kShieldRechargeDelay = 6;    // quiet turns before the shield refills
inline constexpr int kShieldRechargePerTurn = 4;  // Focus regained per turn once refilling
inline constexpr int kWillRegenPerTurn = 2;       // D1: Will regained per turn (abilities spend it in C7)
inline constexpr int kLifePerLevel = 3;           // C3: flat max-Life gained per level (same for all classes)
inline constexpr int kWillPerLevel = 2;           // C3: flat max-Will gained per level (same for all classes)
inline constexpr int kBlindAcc = 40;              // C3: accuracy lost while Blinded (Storm ailment)

// --- movement feel (presentation/cadence only; must NOT affect the sim) ---
// R2: world-pixel space is 2x (iso::kView), so these are doubled to keep the feel.
inline constexpr int kStepBufferPx = 36;      // held walking re-steps this close to touchdown
inline constexpr int kGlideStep = 8;          // px/tick the sprite slides toward its tile
inline constexpr int kGlideStepSandwalk = 4;  // sandwalk stays deliberate + slow
inline constexpr int kHeldStepFloor = 3;      // min ticks a key is held before auto-repeat

// --- fov ---
inline constexpr int kFovDay = 9;
inline constexpr int kFovNight = 8;  // was 5 — see mobs/terrain approaching at night
inline constexpr int kFovDehydrated = 3;
inline constexpr int kFovIndoor = 10;  // sietch/caves: lit interior, day or night

// --- dust hound ---
inline constexpr int kHoundHp = 6;
inline constexpr int kHoundAtkMin = 1;
inline constexpr int kHoundAtkMax = 2;
inline constexpr int kHoundSense = 7;    // hunt when player within this and visible
inline constexpr int kHoundWaterYield = 60;
inline constexpr int kHoundCount = 7;

// --- corpses ---
inline constexpr int kCorpseWaterHound = 60;

// --- B6: the Guild tithe (an OPTIONAL, player-controlled difficulty/reward dial) ---
// Withholding the tithe (guild_debt) makes the world harder AND richer; paying the Guild grinds it
// down. Withholding is the aspirational path (harder + better loot; canonically "starving the
// Guild"). The campaign caps the debt low; killing the Guild Navigator (endgame) raises the cap.
// Debt defaults 0, so a character who never engages plays at the baseline — determinism unaffected.
inline constexpr int kGuildDebtPerRun = 1;        // debt gained per extracted run (the Guild's cut)
inline constexpr int kGuildBribeReduce = 2;       // debt removed per tithe payment
inline constexpr int kGuildDebtCampaignCap = 5;   // campaign difficulty tops out here
inline constexpr int kGuildDebtEndgameCap = 15;   // raised once the Guild Navigator falls (guild_broken)
inline constexpr int kGuildHpPerDebt = 12;        // +12% mob HP per debt level
inline constexpr int kGuildMobDiv = 2;            // +1 escort mob per this many debt levels

// --- B8: deep-desert patrols + expanded entry events ---
// Harkonnen search parties (Ai::Patrol) pace a fixed beat and hunt on sight. They (and the
// wormsign event) only appear at depth >= kPatrolMinDepth, matching the D1 deep-desert hazard
// gate, so shallow/headless runs never see them and the determinism baseline is untouched.
inline constexpr int kPatrolMinDepth = 3;   // Awaken depth at which patrols/wormsign begin
inline constexpr int kPatrolLeash = 6;      // a patroller paces within this Chebyshev radius of its anchor

// --- D3c: PoE-style crafting-currency drops (area-level-gated, whole-game) ---
inline constexpr int kCurrencyDropDenom = 12;  // ~1/N kills at area level >= kDropMinArea drop currency
inline constexpr int kDropMinArea = 2;         // no currency/unID drops in the very shallowest areas
inline constexpr int kUnidMinArea = 4;         // gear can drop unidentified only at this area level up
inline constexpr int kUnidChanceDenom = 3;     // ~1/N eligible gear drops are unidentified

// --- D3e: Spice Essence, the Awakening resource (a rare deep-desert find, NOT a bench currency) ---
inline constexpr int kSpiceEssenceMinArea = 7;   // Spice Essence drops only this deep and beyond
inline constexpr int kSpiceEssenceDenom  = 40;   // ~1/N kills at that depth yield a Spice Essence

// --- G: PoE/Grim Dawn grid inventory (variable-size items) ---
inline constexpr int kPackW   = 8;               // the pack grid: 8 wide x 12 tall = 96 cells
inline constexpr int kPackH   = 12;
inline constexpr int kInvCap  = kPackW * kPackH;  // pool size (worst case: an all-1x1 grid)

// --- C4: the passive skill tree ---
inline constexpr int kPassivePerLevel = 1;    // passive points earned per character level

// --- C7c: ability Teachings drop rarely in deep content (reuse kSpiceEssenceMinArea as the gate) ---
inline constexpr int kTeachingDropDenom = 60; // ~1/N deep kills teach a new (un-owned) ability

// --- C6: the maula pistol (cursor-aimed ranged attack) ---
inline constexpr int kMaulaNoise = 22;        // the maula CRACKS — much louder than a blade (12)
inline constexpr int kMaulaRange = 12;        // max tiles the dart travels

// --- C5: the Deathstill economy — reclaimed water banks to Litrejons (the sietch-tree currency) ---
inline constexpr int kLitrejonPerWater = 2;   // reclaimed water per 1 Litrejon banked on extraction

// --- G3: the Vault — PoE-style stash tabs (each a grid bigger than the pack), bought with spice ---
inline constexpr int kStashW       = 12;         // a stash tab: 12 wide x 12 tall = 144 cells
inline constexpr int kStashH       = 12;
inline constexpr int kStashCap     = kStashW * kStashH;  // per-tab pool size
inline constexpr int kMaxStashTabs = 8;          // hard cap on owned tabs
inline constexpr int kStashTabBase = 20;         // spice cost of the 2nd tab; each next tab doubles

// --- Endgame: the Jihad (spice-commissioned strikes -> directed faction pressure -> pinnacles) ---
// Each cleared strike presses its target Great Power; at kPinnaclePressure that power's homeworld
// pinnacle opens. Higher-tier strikes press harder (and cost more spice / hit harder).
inline constexpr int kPinnaclePressure = 100;   // pressure at which a faction's pinnacle opens
inline constexpr int kStrikePressPerTier = 12;  // pressure a cleared strike adds, x its tier (1..)
inline constexpr int kStrikeTiers = 5;          // commission tiers (difficulty/reward ladder)
inline constexpr int kStrikeSpiceBase = 20;     // spice to commission a tier-1 strike (x tier)

// --- Endgame: The Long Ride (Arrakis deep-desert greed gauntlet — sibling to the Jihad Table) ---
// Alternating leg loop: ride the Maker to a site, bank the haul or push on. Restlessness climbs each
// leg; past a threshold the Maker may BUCK & dive, ending the ride and costing the un-banked haul.
// Difficulty is intrinsic (leg-scaled), NEVER the Guild tithe. Jacurutu (the outcast hoard) is the
// SOLE source of the Maker Hide (a Divine reagent), its odds rising with depth.
inline constexpr int kRideBuckThreshold = 3;    // legs ridden before the Maker may buck
inline constexpr int kRideBuckPerLeg = 8;       // +8% buck chance per leg past the threshold
inline constexpr int kRideBuckMax = 70;         // buck-chance cap
inline constexpr int kRideHaulPerLeg = 6;       // base spice per site, x leg (deeper legs = richer)
inline constexpr int kJacurutuBaseChance = 3;   // % Jacurutu (Maker Hide) at leg 1
inline constexpr int kJacurutuPerLeg = 2;       // +2% per leg
inline constexpr int kJacurutuMax = 40;         // Jacurutu-chance cap

// --- Endgame: the Landsraad / CHOAM politics layer (the economic face of the conquest) ---
// CHOAM control is derived: each toppled homeworld is a seized directorship; each courted House adds
// its votes. Allies pay dividends; a Great Power you are actively warring (high pressure, un-toppled)
// opposes you in the Landsraad and reinforces your strikes with hired forces.
inline constexpr int kChoamPerPinnacle = 16;    // CHOAM control per toppled homeworld (conquest)
inline constexpr int kChoamPerAlly = 6;         // CHOAM control per courted House (diplomacy)
inline constexpr int kCourtCost = 30;           // spice to court a House into a CHOAM alliance
inline constexpr int kHouseOpposePressure = 50; // Jihad-pressure at which an un-toppled House opposes
inline constexpr int kDividendPerControl = 1;   // spice dividend per point of CHOAM control

// --- C7: Will-powered prescience abilities (universal active skills, one shared Will pool) ---
// Cast at the cursor; each spends Will and shares one cooldown. Effects reuse existing systems
// (stun, teleport, Focus shield). Input binding + targeting reticle + FX are the UI phase.
inline constexpr int kAbilityCdTurns = 4;   // shared cooldown after any cast (world turns)
inline constexpr int kWillVoice = 6;        // the Voice — stun the nearest seen foe
inline constexpr int kWillBlink = 5;        // Prescient Step — blink toward the cursor
inline constexpr int kWillPrescience = 8;   // Prescient Calm — a Focus-shield burst
inline constexpr int kVoiceRange = 6;       // the Voice's reach (tiles)
inline constexpr int kVoiceStunTurns = 8;   // stun world-turns the Voice inflicts
inline constexpr int kBlinkRange = 5;       // Prescient Step distance (tiles)
inline constexpr int kPrescienceShield = 30;// Focus restored by Prescient Calm

// --- Flasks: a three-slot belt (Life / Will / Utility) that recharges from kills + time ---
// Used on the number keys (input binding is the UI phase); the charge economy + effects are here.
inline constexpr int kFlaskSlots = 3;
inline constexpr int kFlaskMaxCharges = 6;      // full belt per flask
inline constexpr int kFlaskUseCost = 3;         // charges per sip (two sips per full flask)
inline constexpr int kFlaskChargePerKill = 1;   // charges each flask gains per kill
inline constexpr int kFlaskRechargeTurns = 8;   // + a slow time trickle: 1 charge every N turns
inline constexpr int kFlaskLifeHeal = 40;       // Life flask: HP restored
inline constexpr int kFlaskWillRestore = 12;    // Will flask: Will restored
inline constexpr int kFlaskUtilShield = 20;     // Utility flask: Focus ward (also cleanses ailments)

}  // namespace tune
