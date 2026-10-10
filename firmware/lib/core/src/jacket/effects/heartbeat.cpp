// Heartbeat: a double pulse in color1. On each beat a ring of light spreads
// out from a point 23 % along the segment ("lub"), and a moment later a
// second, smaller one from 84 % ("dub"); each pixel flashes as the ring passes
// and then fades. The two centres and their reach are the prototype's.
//   speed      58.6 beats per minute at 128
//   intensity  how long the glow lingers after the ring has passed
#include "jacket/effects/effects.h"

namespace jacket {
namespace effects {

namespace {

struct Ring {
    uint16_t centre;
    uint16_t radius;    // pixels reached on each side
    uint32_t start_ms;  // within the beat
};

}  // namespace

bool heartbeat(EffectContext& ctx) {
    const uint16_t n = ctx.count;
    const uint32_t beat_ms = ctx.time_ms % kHeartbeatPeriodMs;

    const uint16_t centre_a = static_cast<uint16_t>((n * 59u + 128u) >> 8);  // 23 %
    uint16_t centre_b = static_cast<uint16_t>((n * 215u + 128u) >> 8);       // 84 %
    if (centre_b > n - 1) centre_b = static_cast<uint16_t>(n - 1);
    const Ring rings[2] = {
        {centre_a, centre_a, 0},
        {centre_b, static_cast<uint16_t>(n - 1 - centre_b), kHeartbeatSecondMs},
    };
    // Both rings travel at the speed that takes the first one to its edge in
    // kHeartbeatExpandMs. Q8 milliseconds per pixel.
    const uint32_t ms_per_pixel_q8 = (kHeartbeatExpandMs << 8) / (centre_a > 0 ? centre_a : 1u);
    const uint32_t linger_ms = 48u + ctx.config.intensity * 2u;  // 48..558: over before the next beat
    const uint32_t inv_linger = (255u << 16) / linger_ms;

    for (uint16_t i = 0; i < n; ++i) {
        uint8_t level = 0;
        for (const Ring& ring : rings) {
            const uint16_t distance = static_cast<uint16_t>(i > ring.centre ? i - ring.centre : ring.centre - i);
            if (distance > ring.radius) continue;
            const uint32_t lit_at = ring.start_ms + ((distance * ms_per_pixel_q8) >> 8);
            if (beat_ms < lit_at) continue;
            const uint32_t age = beat_ms - lit_at;
            if (age >= linger_ms) continue;
            const uint32_t left = 255u - ((age * inv_linger) >> 16);
            const uint8_t glow = static_cast<uint8_t>((left * left + left) >> 8);
            if (glow > level) level = glow;
        }
        if (level != 0) ctx.pixels[i] = scale(ctx.config.color1, level);
    }
    return true;
}

}  // namespace effects
}  // namespace jacket
