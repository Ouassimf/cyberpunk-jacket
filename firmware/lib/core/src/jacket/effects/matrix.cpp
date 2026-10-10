// Matrix: sparks in palette colours flash up at random pixels and fade away.
// With the "Matrix" palette this is the prototype's green-and-white rain.
//   speed      how fast sparks appear and fade
//   intensity  how many sparks: about 10 per tick on 144 pixels at 128
//
// Sparks are born on a fixed grid of ticks in effect time, and the sparks of
// tick k are a pure function of (seed, k). The picture is therefore a function
// of time alone: nothing depends on how often render is called.
#include "jacket/effects/effects.h"

namespace jacket {
namespace effects {

bool matrix(EffectContext& ctx) {
    const uint32_t tick = ctx.time_ms / kMatrixTickMs;
    const uint32_t into_tick = ((ctx.time_ms % kMatrixTickMs) << 8) / kMatrixTickMs;  // 0..255
    // Sparks per tick as 24.8 fixed point; the fraction is the chance of one more.
    const uint32_t per_tick = (ctx.count * (ctx.config.intensity + 1u) * 36u) >> 8;

    // Oldest tick first, so that a newer spark overwrites an older one.
    for (uint32_t age = kMatrixLifeTicks; age-- > 0;) {
        if (age > tick) continue;  // before the effect started
        const uint32_t fade = sparkFade(age, into_tick);
        Rng rng(hash32(ctx.state.seed + (tick - age) * 0x9E3779B9u));
        uint32_t sparks = per_tick >> 8;
        if (rng.next8() < (per_tick & 0xFFu)) ++sparks;
        for (uint32_t s = 0; s < sparks; ++s) {
            const uint32_t pixel = rng.below(ctx.count);
            const uint8_t palette_pos = rng.next8();
            const uint8_t peak = static_cast<uint8_t>(64u + ((rng.next8() * 3u) >> 2));  // 64..255
            ctx.pixels[pixel] = scale(ctx.palette.at(palette_pos), scale8(peak, static_cast<uint8_t>(fade)));
        }
    }
    return true;
}

}  // namespace effects
}  // namespace jacket
