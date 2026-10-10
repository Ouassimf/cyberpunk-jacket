#include "jacket/color.h"

namespace jacket {

Rgb hsvToRgb(uint16_t hue, uint8_t saturation, uint8_t value) {
    const uint32_t h6 = static_cast<uint32_t>(hue) * 6u;       // 16.16: sector . fraction
    const uint8_t sector = static_cast<uint8_t>(h6 >> 16);     // 0..5
    const uint8_t f = static_cast<uint8_t>((h6 >> 8) & 0xFF);  // position inside the sector

    const uint8_t p = scale8(value, static_cast<uint8_t>(255 - saturation));
    const uint8_t q = scale8(value, static_cast<uint8_t>(255 - scale8(saturation, f)));
    const uint8_t t = scale8(value, static_cast<uint8_t>(255 - scale8(saturation, static_cast<uint8_t>(255 - f))));

    switch (sector) {
        case 0: return Rgb(value, t, p);
        case 1: return Rgb(q, value, p);
        case 2: return Rgb(p, value, t);
        case 3: return Rgb(p, q, value);
        case 4: return Rgb(t, p, value);
        default: return Rgb(value, p, q);
    }
}

uint8_t wave8(uint16_t phase) {
    // Triangle 0..65534, then smoothstep x^2 (3 - 2x) in 16-bit fixed point.
    const uint32_t x = (phase < 0x8000u) ? (static_cast<uint32_t>(phase) << 1)
                                         : (static_cast<uint32_t>(0xFFFFu - phase) << 1);
    const uint32_t x2 = (x * x) >> 16;
    const uint32_t eased = static_cast<uint32_t>((static_cast<uint64_t>(x2) * (3u * 65536u - 2u * x)) >> 16);
    return static_cast<uint8_t>(eased >> 8);
}

}  // namespace jacket
