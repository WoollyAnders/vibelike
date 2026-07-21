#pragma once
// M4: the harvest season. The Baron ships spice; you deny it. The CHOAM
// ledger tracks both. Denials raise the global Harkonnen alert (their
// response escalates; past the threshold they start triangulating your
// sietch) and your sietch standing. Deny enough and the Baron stakes
// everything on one climactic twin-escort harvest.
#include <cstdint>

namespace campaign {

// --- season tunables ---
inline constexpr int kQuotaLossAt = 8;      // loads shipped -> the Baron wins
inline constexpr int kDenialsForClimax = 4; // denials before the climax op
inline constexpr int kOpGapMin = 60;        // turns between ops
inline constexpr int kOpGapMax = 140;
inline constexpr int kOpDeadline = 260;     // unattended ops ship after this

// --- alert ---
inline constexpr int kAlertDemolition = 2;
inline constexpr int kAlertWormBait = 3;    // spectacle
inline constexpr int kAlertQuietWorm = 1;   // spotters knifed first
inline constexpr int kAlertEliteAt = 40;    // ops always get elite escorts
inline constexpr int kAlertDiscoveryAt = 80;
inline constexpr int kDiscoveryRaidAt = 300;  // turns above threshold -> raid
inline constexpr int kAlertDecayPerDay = 4;

// --- standing ---
inline constexpr int kStandingPerDenial = 8;
inline constexpr int kStandingCrysknifeAt = 60;  // Honored: the Naib's gift

enum Ending : uint8_t { kNone = 0, kVictory = 1, kQuotaLoss = 2, kRaidLoss = 3 };

// op mutators (rolled per op)
inline constexpr uint8_t MUT_ELITE = 1 << 0;         // bigger escort + shields
inline constexpr uint8_t MUT_SLOPPY = 1 << 1;        // rich theft, slow fill
inline constexpr uint8_t MUT_TWIN_SPOTTERS = 1 << 2; // four eyes on the sand
inline constexpr uint8_t MUT_WORMSIGN = 1 << 3;      // worm already stirring
inline constexpr uint8_t MUT_CLIMAX = 1 << 7;        // the Baron's last throw

struct State {
    int32_t shipped = 0;
    int32_t denied = 0;
    int32_t alert = 0;          // 0..100
    int32_t standing = 10;      // 0..100
    int32_t discovery = 0;      // turns spent above the discovery threshold
    int32_t next_op_turn = 40;  // when the next harvest starts
    uint8_t ending = kNone;
    uint8_t crysknife_granted = 0;
    uint8_t climax_pending = 0;
};

}  // namespace campaign
