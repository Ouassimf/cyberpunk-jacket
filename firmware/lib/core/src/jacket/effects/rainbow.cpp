// Rainbow: the colour wheel laid along the segment, flowing towards higher
// pixel indices.
//   speed      one full turn of the wheel per 4.096 s at 128
//   intensity  how tightly the colours are packed: 0 = the whole segment one
//              colour, 128 = one rainbow per 128 pixels, 255 = one per 64
#include "jacket/effects/effects.h"

namespace jacket {
namespace effects {

bool rainbow(EffectContext& ctx) {
    // Hue is a 16-bit circle; 65536 / 4096 ms = 16 hue units per ms.
    const uint16_t base = static_cast<uint16_t>(ctx.time_ms * 16u);
    const uint16_t per_pixel = static_cast<uint16_t>(ctx.config.intensity * 4u);
    uint16_t hue = base;
    for (uint16_t i = 0; i < ctx.count; ++i) {
        ctx.pixels[i] = hsvToRgb(hue, 255, 255);
        hue = static_cast<uint16_t>(hue - per_pixel);
    }
    return true;
}

}  // namespace effects
}  // namespace jacket
