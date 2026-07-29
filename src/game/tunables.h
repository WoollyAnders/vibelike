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

}  // namespace tune
