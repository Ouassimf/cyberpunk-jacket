// Comet: a bright head in color1 runs along the segment and wraps round, with
// a fading tail that dissolves from color1 into the palette.
// (Replaces the prototype's "beat": a moving dot over a frame-by-frame fade.)
//   speed      head velocity, 62.5 px/s at 128
//   intensity  tail length, 1..64 pixels
#include "jacket/effects/effects.h"

namespace jacket {
namespace effects {

bool comet(EffectContext& ctx) {
    uint16_t tail_px = tailPixels(ctx.config.intensity);
    if (tail_px > ctx.count) tail_px = ctx.count;
    const Sweep sweep(ctx.time_ms, ctx.count, tail_px);
    const Rgb head_color = ctx.config.color1;

    for (uint16_t i = 0; i < ctx.count; ++i) {
        uint8_t along;
        const uint8_t level = sweep.level(static_cast<uint32_t>(i) << 8, along);
        if (level == 0) continue;
        Rgb color = head_color;
        if (along != 0) {
            // The first quarter of the tail cross-fades from color1 into the palette.
            const uint8_t mix = along >= 64 ? 255 : static_cast<uint8_t>(along * 4);
            color = blend(head_color, ctx.palette.at(along), mix);
        }
        ctx.pixels[i] = scale(color, level);
    }
    return true;
}

}  // namespace effects
}  // namespace jacket
