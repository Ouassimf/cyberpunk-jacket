// Solid: every pixel is color1.
#include "jacket/effects/effects.h"

namespace jacket {
namespace effects {

bool solid(EffectContext& ctx) {
    for (uint16_t i = 0; i < ctx.count; ++i) ctx.pixels[i] = ctx.config.color1;
    return false;
}

}  // namespace effects
}  // namespace jacket
