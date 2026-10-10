// Beat Pulse: the whole segment flashes on every beat and fades through the
// beat. Three beats flash in color1 and fade in place; every fourth flashes in
// color2 and drains from the middle of the segment outwards, the ends holding
// their light a quarter of a beat longer. The brightest moment of every flash
// is the beat itself.
//   sound      the beat comes from the phone's beat clock
//   speed      the tempo when there is no beat to follow: 120 BPM at 128
//   intensity  how long a flash lingers: a quarter of the beat at 0, all of it at 255
//
// Fallback (no beat): the same picture at the tempo set by speed. Which beat
// of four is the accent is counted on the jacket; the phone does not say
// where a bar starts.
#include "jacket/effects/effects.h"

namespace jacket {
namespace effects {

namespace {

constexpr uint32_t kAccentHoldQ16 = 16384;  // a quarter of a beat

// Brightness of a flash `phase` (Q16 beats) after it began, fading over `length`.
uint8_t flash(uint32_t phase, uint32_t length) {
    if (phase >= length) return 0;
    const uint32_t left = 255u - (phase * 255u) / length;
    return static_cast<uint8_t>((left * left + left) >> 8);
}

// The picture at one beat position (16.16 beats).
class Pulse {
public:
    Pulse(uint32_t position, const EffectConfig& config, uint16_t count)
        : phase_(position & 0xFFFFu),
          accent_(((position >> 16) & 3u) == 0),
          length_(16384u + config.intensity * 192u),
          count_(count),
          color_(accent_ ? config.color2 : config.color1),
          plain_(scale(color_, flash(phase_, length_))) {}

    Rgb at(uint16_t i) const {
        if (!accent_) return plain_;
        // Twice the distance from the middle of the segment, 0 .. count-1.
        const uint32_t reach = 2u * i + 1u > count_ ? 2u * i + 1u - count_ : count_ - 2u * i - 1u;
        const uint32_t hold = reach * kAccentHoldQ16 / count_;
        return scale(color_, flash(phase_ > hold ? phase_ - hold : 0, length_));
    }

private:
    uint32_t phase_;
    bool accent_;
    uint32_t length_;
    uint16_t count_;
    Rgb color_;
    Rgb plain_;
};

}  // namespace

bool beatPulse(EffectContext& ctx) {
    const Reactive& live = reactiveOf(ctx);
    const uint8_t mix = live.beat_mix;
    const uint32_t own_position = static_cast<uint32_t>((static_cast<uint64_t>(ctx.time_ms) << 16) / kPulseFallbackBeatMs);
    const Pulse own(own_position, ctx.config, ctx.count);
    const Pulse led(live.beat, ctx.config, ctx.count);

    for (uint16_t i = 0; i < ctx.count; ++i) {
        if (mix == 0) {
            ctx.pixels[i] = own.at(i);
        } else if (mix == 255) {
            ctx.pixels[i] = led.at(i);
        } else {
            ctx.pixels[i] = blend(own.at(i), led.at(i), mix);
        }
    }
    return true;
}

}  // namespace effects
}  // namespace jacket
