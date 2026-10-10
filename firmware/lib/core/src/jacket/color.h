// Colour type and the small amount of integer colour maths the effects need.
#pragma once

#include <stdint.h>

namespace jacket {

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    constexpr Rgb() = default;
    constexpr Rgb(uint8_t red, uint8_t green, uint8_t blue) : r(red), g(green), b(blue) {}
};

constexpr bool operator==(const Rgb& a, const Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
constexpr bool operator!=(const Rgb& a, const Rgb& b) { return !(a == b); }

constexpr Rgb kBlack{0, 0, 0};

// value * scale / 255, exact at both ends: scale 255 returns value, scale 0 returns 0.
constexpr uint8_t scale8(uint8_t value, uint8_t scale) {
    return static_cast<uint8_t>((static_cast<uint16_t>(value) * (static_cast<uint16_t>(scale) + 1)) >> 8);
}

// a + (b - a) * amount / 255, rounded; amount 0 returns a, 255 returns b.
constexpr uint8_t lerp8(uint8_t a, uint8_t b, uint8_t amount) {
    return static_cast<uint8_t>((static_cast<uint32_t>(a) * (255u - amount) + static_cast<uint32_t>(b) * amount + 127u) / 255u);
}

// Dim a colour: 255 leaves it unchanged, 0 gives black.
constexpr Rgb scale(Rgb c, uint8_t amount) {
    return Rgb(scale8(c.r, amount), scale8(c.g, amount), scale8(c.b, amount));
}

// Fade towards black: 0 leaves the colour unchanged, 255 gives black.
constexpr Rgb fade(Rgb c, uint8_t amount) { return scale(c, static_cast<uint8_t>(255 - amount)); }

// Mix two colours: amount 0 returns a, 255 returns b.
constexpr Rgb blend(Rgb a, Rgb b, uint8_t amount) {
    return Rgb(lerp8(a.r, b.r, amount), lerp8(a.g, b.g, amount), lerp8(a.b, b.b, amount));
}

// Per-channel maximum, for layering light on light.
constexpr Rgb lighter(Rgb a, Rgb b) {
    return Rgb(a.r > b.r ? a.r : b.r, a.g > b.g ? a.g : b.g, a.b > b.b ? a.b : b.b);
}

// Plain HSV. `hue` is the full 16-bit circle: 0 red, 0x5555 green, 0xAAAA blue.
Rgb hsvToRgb(uint16_t hue, uint8_t saturation, uint8_t value);

// Smooth 0 -> 255 -> 0 wave over one 16-bit phase turn (cubic ease, 0 at phase 0, 255 at 0x8000).
uint8_t wave8(uint16_t phase);

}  // namespace jacket
