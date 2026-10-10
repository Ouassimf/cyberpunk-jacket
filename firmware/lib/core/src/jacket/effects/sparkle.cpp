// Sparkle: sparks in palette colours flash up and fade, as many and as bright
// as the wearer moves. While there is movement, leaning (tilt_y) gathers the
// sparks towards one end of the segment and tipping sideways (tilt_x) slides
// their colours along the palette.
//   motion     more movement, more and brighter sparks; tilt steers them
//   speed      how fast sparks appear and fade
//   intensity  how many sparks at full movement
//
// Fallback (no motion data, or standing still): a sparse, even shimmer, as if
// motion were kSparkleRest. Standing still with the phone tilted is still the
// fallback: tilt only counts while moving.
//
// Like Matrix, the sparks of tick k are a pure function of (seed, k); what
// motion decides is how many of them are let through and how bright, so the
// picture is a function of effect time and the smoothed live values alone.
#include "jacket/effects/effects.h"

namespace jacket {
namespace effects {

namespace {

constexpr uint8_t kSparkleRest = 48;       // energy with no movement: about a fifth of the sparks, dim
constexpr uint32_t kSparkleDensity = 54;   // candidates per tick, as Matrix's 36: half as many again at full movement
constexpr uint8_t kSparkleWindowDim = 191; // how far the end away from a full tilt is dimmed (of 255)

uint8_t saturate2x(uint32_t v) { return v >= 128u ? 255 : static_cast<uint8_t>(v * 2u); }

}  // namespace

bool sparkle(EffectContext& ctx) {
    const Reactive& live = reactiveOf(ctx);
    const uint16_t n = ctx.count;

    const uint8_t moving = live.motion > kSparkleRest ? live.motion : kSparkleRest;
    const uint8_t energy = lerp8(kSparkleRest, moving, live.motion_mix);
    // Tilt only steers while there is movement to steer.
    const uint8_t drive = scale8(saturate2x(live.motion), live.motion_mix);
    const int32_t tilt_x = live.tilt_x;
    const int32_t tilt_y = live.tilt_y;
    const uint8_t hue_shift = static_cast<uint8_t>(tilt_x * drive / 255);
    const uint8_t window = scale8(saturate2x(static_cast<uint32_t>(tilt_y < 0 ? -tilt_y : tilt_y)), drive);
    const int32_t centre_q8 = n * 128 + tilt_y * n * 128 / kTiltFull;  // where the sparks gather, in 1/256 pixels

    const uint32_t tick = ctx.time_ms / kMatrixTickMs;
    const uint32_t into_tick = ((ctx.time_ms % kMatrixTickMs) << 8) / kMatrixTickMs;  // 0..255
    const uint32_t per_tick = (n * (ctx.config.intensity + 1u) * kSparkleDensity) >> 8;  // 24.8 candidates per tick

    for (uint32_t age = kMatrixLifeTicks; age-- > 0;) {
        if (age > tick) continue;  // before the effect started
        const uint8_t fade = sparkFade(age, into_tick);
        Rng rng(hash32(ctx.state.seed + (tick - age) * 0x9E3779B9u));
        uint32_t sparks = per_tick >> 8;
        if (rng.next8() < (per_tick & 0xFFu)) ++sparks;
        for (uint32_t s = 0; s < sparks; ++s) {
            const uint32_t pixel = rng.below(n);
            const uint8_t palette_pos = rng.next8();
            const uint8_t peak = static_cast<uint8_t>(64u + ((rng.next8() * 3u) >> 2));  // 64..255
            const uint8_t threshold = rng.next8();
            if (energy <= threshold) continue;  // this spark needs more movement
            const uint8_t strength = saturate2x((energy - threshold) * 2u);
            uint8_t level = scale8(scale8(peak, fade), strength);
            if (window != 0) {
                const int32_t at_q8 = static_cast<int32_t>(pixel) * 256 + 128;
                const uint32_t distance = static_cast<uint32_t>(at_q8 > centre_q8 ? at_q8 - centre_q8 : centre_q8 - at_q8) >> 8;
                uint32_t dim = distance * 2u * kSparkleWindowDim / n;
                if (dim > kSparkleWindowDim) dim = kSparkleWindowDim;
                level = scale8(level, static_cast<uint8_t>(255u - scale8(static_cast<uint8_t>(dim), window)));
            }
            const Rgb color = scale(ctx.palette.at(static_cast<uint8_t>(palette_pos + hue_shift)), level);
            ctx.pixels[pixel] = lighter(ctx.pixels[pixel], color);
        }
    }
    return true;
}

}  // namespace effects
}  // namespace jacket
