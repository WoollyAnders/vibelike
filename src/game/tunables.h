#pragma once
// Every balance constant in the game lives here.

namespace tune {

// --- time ---
inline constexpr int kTurnsPerDay = 300;

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
inline constexpr int kExposureRecover = 6;        // per turn in shade/indoors/night
inline constexpr int kExposureSlow = 50;          // moves cost 1.5 turns of energy
inline constexpr int kExposureCollapse = 90;      // damage per turn
inline constexpr int kExposureDamage = 2;

// --- movement feel (presentation/cadence only; must NOT affect the sim) ---
inline constexpr int kStepBufferPx = 18;      // held walking re-steps this close to touchdown
inline constexpr int kGlideStep = 4;          // px/tick the sprite slides toward its tile
inline constexpr int kGlideStepSandwalk = 2;  // sandwalk stays deliberate + slow
inline constexpr int kHeldStepFloor = 3;      // min ticks a key is held before auto-repeat

// --- fov ---
inline constexpr int kFovDay = 9;
inline constexpr int kFovNight = 5;
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
