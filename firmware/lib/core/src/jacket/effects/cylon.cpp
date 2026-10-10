// Cylon: an eye sweeps to the end of the segment and back, dragging a fading
// trail. Each pixel takes its colour from the palette stretched over the
// segment, so with palette 0 the eye is color1 at one end and color2 at the
// other. (The prototype's version was a single hard dot.)
//   speed      eye velocity, 62.5 px/s at 128
//   intensity  trail length, 1..64 pixels
#include "jacket/effects/effects.h"

namespace jacket {
namespace effects {

bool cylon(EffectContext& ctx) {
    const uint16_t n = ctx.count;
    if (n == 1) {
        ctx.pixels[0] = ctx.palette.at(0);
        return false;
    }
    // Unfold the bounce into one lap of a ring of 2(n-1) pixels: ring position
    // p and position 2(n-1) - p are the same pixel, out and back.
    const uint32_t lap = 2u * (n - 1u);
    uint16_t tail_px = tailPixels(ctx.config.intensity);
    if (tail_px > n - 1) tail_px = static_cast<uint16_t>(n - 1);
    const Sweep sweep(ctx.time_ms, lap, tail_px);
    const uint32_t palette_step = (255u << 16) / (n - 1u) + 1u;  // 16.16 palette position per pixel

    for (uint16_t i = 0; i < n; ++i) {
        uint8_t along;
        const uint8_t out = sweep.level(static_cast<uint32_t>(i) << 8, along);
        const uint8_t back = (i == 0) ? out : sweep.level((lap - i) << 8, along);
        const uint8_t level = out > back ? out : back;
        if (level == 0) continue;
        const uint8_t position = static_cast<uint8_t>((i * palette_step) >> 16);
        ctx.pixels[i] = scale(ctx.palette.at(position), level);
    }
    return true;
}

}  // namespace effects
}  // namespace jacket
