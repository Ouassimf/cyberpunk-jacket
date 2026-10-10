#include "jacket/effect.h"

#include "jacket/effects/effects.h"

namespace jacket {

namespace {

constexpr uint8_t kSpeedAmount = kUsesSpeed | kUsesIntensity;

// The effect catalogue. Row index = id (PROTOCOL.md "Catalogue ids").
// Append only: ids are stable once published.
const EffectInfo kEffects[] = {
    {0, "Solid", kUsesColor1, effects::solid},
    {1, "Comet", kUsesColor1 | kUsesPalette | kSpeedAmount, effects::comet},
    {2, "Rainbow", kSpeedAmount, effects::rainbow},
    {3, "Matrix", kUsesPalette | kSpeedAmount, effects::matrix},
    {4, "Breathe", kUsesColor1 | kSpeedAmount, effects::breathe},
    {5, "Heartbeat", kUsesColor1 | kSpeedAmount, effects::heartbeat},
    {6, "Cylon", kUsesPalette | kSpeedAmount, effects::cylon},
    {7, "Beat Pulse", kUsesColor1 | kUsesColor2 | kSpeedAmount | kReactsSound, effects::beatPulse},
    {8, "VU Meter", kUsesPalette | kSpeedAmount | kReactsSound, effects::vuMeter},
    {9, "Sparkle", kUsesPalette | kSpeedAmount | kReactsMotion, effects::sparkle},
};

constexpr uint8_t kEffectCount = static_cast<uint8_t>(sizeof(kEffects) / sizeof(kEffects[0]));

// round(65536 * 2^(k / 32)) for k = 0..31
const uint32_t kPow2Q16[32] = {
    65536,  66971,  68438,  69936,  71468,  73032,  74632,  76266,  77936,  79642,  81386,
    83169,  84990,  86851,  88752,  90696,  92682,  94711,  96785,  98905,  101070, 103283,
    105545, 107856, 110218, 112631, 115098, 117618, 120194, 122825, 125515, 128263,
};

}  // namespace

uint8_t effectCount() { return kEffectCount; }

const EffectInfo* effectInfo(uint8_t id) { return id < kEffectCount ? &kEffects[id] : nullptr; }

uint32_t speedMultiplierQ16(uint8_t speed) {
    const uint8_t octave = speed >> 5;  // 0..7, where 4 is 1x
    const uint32_t mantissa = kPow2Q16[speed & 31];
    return octave >= 4 ? mantissa << (octave - 4) : mantissa >> (4 - octave);
}

}  // namespace jacket
