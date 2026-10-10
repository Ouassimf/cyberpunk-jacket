// Render functions of the built-in effects (one file each) and the helpers
// they share. Internal to the library; the public face is jacket/effect.h.
#pragma once

#include "jacket/effect.h"
#include "jacket/live_signals.h"

namespace jacket {
namespace effects {

bool solid(EffectContext& ctx);
bool comet(EffectContext& ctx);
bool rainbow(EffectContext& ctx);
bool matrix(EffectContext& ctx);
bool breathe(EffectContext& ctx);
bool heartbeat(EffectContext& ctx);
bool cylon(EffectContext& ctx);
bool beatPulse(EffectContext& ctx);
bool vuMeter(EffectContext& ctx);
bool sparkle(EffectContext& ctx);

// Timing constants. All times are effect time: milliseconds at speed 128.
constexpr uint32_t kSweepQ8PerMs = 16;        // Comet / Cylon head: 1/16 pixel per ms = 62.5 px/s
constexpr uint16_t kMaxTailPixels = 64;       // Comet / Cylon tail at intensity 255
constexpr uint32_t kHeartbeatPeriodMs = 1024;
constexpr uint32_t kHeartbeatExpandMs = 384;  // first ring: centre to full radius
constexpr uint32_t kHeartbeatSecondMs = 192;  // delay of the second ring ("dub")
constexpr uint32_t kMatrixTickMs = 32;        // sparks appear on this grid
constexpr uint32_t kMatrixLifeTicks = 16;     // a spark is black again after this
constexpr uint32_t kPulseFallbackBeatMs = 500; // Beat Pulse with no beat to follow: 120 BPM

// The smoothed live input of this frame. An effect rendered without any
// (ctx.reactive == nullptr) gets one whose weights are all 0: "play the fallback".
inline const Reactive& reactiveOf(const EffectContext& ctx) {
    static const Reactive none;
    return ctx.reactive != nullptr ? *ctx.reactive : none;
}

// Brightness of a spark (Matrix, Sparkle) `age` ticks old, `into_tick` / 256
// of the way to the next tick: 255 * 0.75^age, 0 at the end of its life.
inline uint8_t sparkFade(uint32_t age, uint32_t into_tick) {
    static const uint8_t decay[kMatrixLifeTicks + 1] = {255, 191, 143, 108, 81, 61, 45, 34, 26, 19, 14, 11, 8, 6, 5, 3, 0};
    return static_cast<uint8_t>(decay[age] - (((decay[age] - decay[age + 1]) * into_tick) >> 8));
}

// Tail length in pixels for Comet / Cylon: 1 at intensity 0, 64 at 255.
constexpr uint16_t tailPixels(uint8_t intensity) {
    return static_cast<uint16_t>(1 + (static_cast<uint16_t>(intensity) * (kMaxTailPixels - 1)) / 255);
}

// A point of light travelling round a ring, with an anti-aliased leading edge
// and a tail that fades quadratically. Positions are Q8 pixels (256 = 1 pixel).
// The tail only exists where the head has already been: at effect time 0 the
// sweep is a bare head.
class Sweep {
public:
    // The head has moved kSweepQ8PerMs per ms of `time_ms` round a ring of
    // `ring_pixels`; the tail is `tail_pixels` long (1..64).
    Sweep(uint32_t time_ms, uint32_t ring_pixels, uint32_t tail_pixels)
        : ring_(ring_pixels << 8), tail_(tail_pixels << 8), inv_tail_((1u << 24) / tail_) {
        const uint64_t travelled = static_cast<uint64_t>(time_ms) * kSweepQ8PerMs;
        head_ = static_cast<uint32_t>(travelled % ring_);
        if (travelled < tail_) tail_ = static_cast<uint32_t>(travelled) + 1;  // inv_tail_ keeps the full-length fade
    }

    // Brightness 0..255 of the ring position `at`. `along` is where in the
    // tail it lies: 0 at the head, 255 at the far end.
    uint8_t level(uint32_t at, uint8_t& along) const {
        const uint32_t behind = head_ >= at ? head_ - at : head_ + ring_ - at;  // 0 .. ring-1
        uint8_t result = 0;
        along = 0;
        if (behind < tail_) {
            along = static_cast<uint8_t>((behind * inv_tail_) >> 16);
            const uint32_t left = 255u - along;
            result = static_cast<uint8_t>((left * left + left) >> 8);
        }
        const uint32_t ahead = ring_ - behind;  // the pixel the head is entering
        if (ahead < 256u && 256u - ahead > result) {
            result = static_cast<uint8_t>(256u - ahead);
            along = 0;
        }
        return result;
    }

private:
    uint32_t ring_;
    uint32_t head_;
    uint32_t tail_;
    uint32_t inv_tail_;
};

}  // namespace effects
}  // namespace jacket
