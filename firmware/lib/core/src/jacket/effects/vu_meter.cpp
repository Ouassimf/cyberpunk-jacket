// VU Meter: a bar that grows from the start of the segment with the loudness.
// The bar is painted in the palette laid along the whole segment, so a louder
// sound reaches further into the palette. Within the bar, bass makes the first
// third glow, the mids the middle and treble the far end.
//   sound      level sets the length; bass, mid and treble the glow along the bar
//   speed      how fast the fallback moves
//   intensity  gain: 128 shows the level as it is, 255 about 1.7 times, 0 a third
//
// Fallback (no audio): the level swells between 3/8 and 3/4 once per 4.096 s
// and the three bands roll at their own rates, so the bar breathes.
#include "jacket/effects/effects.h"

namespace jacket {
namespace effects {

namespace {

constexpr uint8_t kBarFloor = 80;  // glow of the bar where its band is silent: the length must stay readable

// Band energy at a place along the segment (0..255): bass, mid and treble sit
// at 1/6, 1/2 and 5/6 and are interpolated between.
uint8_t bandAt(uint8_t x, uint8_t bass, uint8_t mid, uint8_t treble) {
    if (x <= 42) return bass;
    if (x <= 127) return lerp8(bass, mid, static_cast<uint8_t>((x - 42) * 3));
    if (x <= 212) return lerp8(mid, treble, static_cast<uint8_t>((x - 127) * 3));
    return treble;
}

}  // namespace

bool vuMeter(EffectContext& ctx) {
    const Reactive& live = reactiveOf(ctx);
    const uint16_t n = ctx.count;

    // The fallback's own "sound": a function of effect time alone.
    const uint16_t turn = static_cast<uint16_t>(ctx.time_ms * 16u);  // one turn per 4.096 s
    const uint8_t own_level = static_cast<uint8_t>(96 + scale8(wave8(turn), 96));
    const uint8_t own_bass = wave8(static_cast<uint16_t>(turn * 2u));
    const uint8_t own_mid = wave8(static_cast<uint16_t>(turn * 3u + 0x4000u));
    const uint8_t own_treble = wave8(static_cast<uint16_t>(turn * 5u + 0x8000u));

    const uint8_t mix = live.audio_mix;
    const uint8_t level = lerp8(own_level, live.level, mix);
    const uint8_t bass = lerp8(own_bass, live.bass, mix);
    const uint8_t mid = lerp8(own_mid, live.mid, mix);
    const uint8_t treble = lerp8(own_treble, live.treble, mix);

    uint32_t shown = level * (64u + ctx.config.intensity) / 192u;
    if (shown > 255u) shown = 255u;
    const uint32_t bar_q8 = shown * n * 256u / 255u;  // lit length in 1/256 pixels

    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t from_q8 = static_cast<uint32_t>(i) << 8;
        if (bar_q8 <= from_q8) break;
        const uint8_t cover = bar_q8 - from_q8 >= 256u ? 255 : static_cast<uint8_t>(bar_q8 - from_q8);  // the tip is partly lit
        const uint8_t x = n > 1 ? static_cast<uint8_t>(i * 255u / (n - 1u)) : 0;
        const uint8_t glow = static_cast<uint8_t>(kBarFloor + scale8(bandAt(x, bass, mid, treble), 255 - kBarFloor));
        ctx.pixels[i] = scale(ctx.palette.at(x), scale8(glow, cover));
    }
    return true;
}

}  // namespace effects
}  // namespace jacket
