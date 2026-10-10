// Breathe: the whole segment swells and dims in color1, starting from the dim
// end of the breath.
//   speed      one breath per 4.096 s at 128
//   intensity  depth: 255 dims all the way to black, 0 does not dim at all
#include "jacket/effects/effects.h"

namespace jacket {
namespace effects {

bool breathe(EffectContext& ctx) {
    const uint16_t phase = static_cast<uint16_t>(ctx.time_ms * 16u);  // 65536 / 4096 ms
    const uint8_t swell = wave8(phase);
    const uint8_t dip = scale8(static_cast<uint8_t>(255 - swell), ctx.config.intensity);
    const Rgb color = scale(ctx.config.color1, static_cast<uint8_t>(255 - dip));
    for (uint16_t i = 0; i < ctx.count; ++i) ctx.pixels[i] = color;
    return ctx.config.intensity != 0;
}

}  // namespace effects
}  // namespace jacket
