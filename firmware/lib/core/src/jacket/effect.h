// Effect interface and catalogue.
//
// Adding an effect: write one file in effects/ with a render function, declare
// it in effects/effects.h and append one row to the table in
// effect_registry.cpp. Ids are published to the app and never change.
//
// Rules for a render function:
//  - It draws `count` virtual pixels into `pixels` (which arrive black). The
//    engine maps them onto the strips; an effect never sees strips, start
//    offsets or the reversed flag.
//  - Time comes from `time_ms` only. It already includes the segment's speed
//    multiplier and starts at 0 when the effect is (re)started, so the picture
//    must be a function of `time_ms` and `state`, never of the frame count.
//  - Randomness comes from `state.seed` (see Rng / hash32). No global RNG.
//  - Draw at full range: global brightness is applied by the output layer.
//  - Honour exactly the parameters named in the row's param_flags:
//      kUsesColor1 / kUsesColor2  the colour is used directly
//      kUsesPalette               colours are taken from `palette`; with
//                                 palette 0 that is the color1 -> color2 ramp
//  - Return true if the picture changes with time, false if it is static.
//  - A reactive effect (kReactsSound / kReactsMotion) reads `reactive`, never
//    the raw `live` frame: those values are already smoothed for 60 fps and
//    carry a 0..255 weight per group (audio, motion, beat). With weight 0 it
//    must show a fallback that is a function of `time_ms` and `state` alone,
//    and in between it must blend, so the change is never a step.
#pragma once

#include <stdint.h>

#include "jacket/color.h"
#include "jacket/palette.h"
#include "jacket/protocol.h"
#include "jacket/scene.h"

namespace jacket {

struct Reactive;  // jacket/live_signals.h

// CATALOG param_flags
constexpr uint8_t kUsesColor1 = 0x01;
constexpr uint8_t kUsesColor2 = 0x02;
constexpr uint8_t kUsesPalette = 0x04;
constexpr uint8_t kUsesSpeed = 0x08;
constexpr uint8_t kUsesIntensity = 0x10;
constexpr uint8_t kReactsSound = 0x20;
constexpr uint8_t kReactsMotion = 0x40;

// Per-segment memory of an effect. Zeroed, then `seed` set, whenever the
// segment's effect or length changes and when a preset is loaded. Effect time
// restarts at 0 at the same moment.
struct EffectState {
    uint32_t seed = 0;        // differs per segment, constant between resets
    uint32_t scratch[4] = {}; // effect-private
};

struct EffectContext {
    Rgb* pixels;               // `count` virtual pixels, black on entry
    uint16_t count;            // 1..144
    uint32_t time_ms;          // effect time: real time x speed multiplier, wraps
    const EffectConfig& config;
    const Palette& palette;    // config.palette bound to config.color1/2
    const LiveInput* live;     // the raw frame, nullptr while no live input is arriving
    EffectState& state;
    const Reactive* reactive = nullptr;  // smoothed live input; nullptr means "none" (all weights 0)
};

using EffectFn = bool (*)(EffectContext& ctx);

struct EffectInfo {
    uint8_t id;
    const char* name;
    uint8_t param_flags;
    EffectFn render;
};

uint8_t effectCount();
const EffectInfo* effectInfo(uint8_t id);  // nullptr for an unknown id

// 2^((speed - 128) / 32) as 16.16 fixed point: 128 -> 65536 (1x), 160 -> 2x, 96 -> 0.5x.
uint32_t speedMultiplierQ16(uint8_t speed);

// Integer mixer: a well-scrambled function of its input.
constexpr uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// Small deterministic PRNG (xorshift32). Seed it from EffectState::seed.
class Rng {
public:
    explicit Rng(uint32_t seed) : s_(seed != 0 ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 17;
        s_ ^= s_ << 5;
        return s_;
    }
    uint8_t next8() { return static_cast<uint8_t>(next() >> 24); }
    // Uniform in 0..n-1 (n > 0).
    uint32_t below(uint32_t n) { return static_cast<uint32_t>((static_cast<uint64_t>(next()) * n) >> 32); }

private:
    uint32_t s_;
};

}  // namespace jacket
