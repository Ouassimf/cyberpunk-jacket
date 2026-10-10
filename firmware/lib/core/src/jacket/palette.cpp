#include "jacket/palette.h"

namespace jacket {

namespace {

// Each palette is a list of evenly spaced colour stops.
const Rgb kRainbow[] = {
    {255, 0, 0}, {255, 255, 0}, {0, 255, 0}, {0, 255, 255}, {0, 0, 255}, {255, 0, 255}, {255, 0, 0},
};
// The greens and whites of the original prototype's "random sparks" effect.
const Rgb kMatrix[] = {
    {0, 255, 0}, {0, 255, 38}, {131, 247, 156}, {0, 82, 29}, {6, 147, 123}, {255, 255, 255}, {255, 255, 255}, {0, 255, 0},
};
const Rgb kCyberpunk[] = {
    {0, 255, 230}, {0, 120, 255}, {130, 40, 255}, {255, 0, 200},
};
const Rgb kFire[] = {
    {120, 0, 0}, {255, 0, 0}, {255, 90, 0}, {255, 180, 0}, {255, 240, 130},
};
const Rgb kOcean[] = {
    {0, 10, 90}, {0, 60, 200}, {0, 160, 220}, {0, 220, 180}, {180, 255, 240},
};

struct PaletteDef {
    const char* name;
    const Rgb* stops;  // nullptr: built from the segment's two colours
    uint8_t count;
};

#define JACKET_STOPS(array) array, static_cast<uint8_t>(sizeof(array) / sizeof(array[0]))

// Index = palette id. Append only: ids are part of the protocol.
const PaletteDef kPalettes[] = {
    {"Colours", nullptr, 2},
    {"Rainbow", JACKET_STOPS(kRainbow)},
    {"Matrix", JACKET_STOPS(kMatrix)},
    {"Cyberpunk", JACKET_STOPS(kCyberpunk)},
    {"Fire", JACKET_STOPS(kFire)},
    {"Ocean", JACKET_STOPS(kOcean)},
};

#undef JACKET_STOPS

constexpr uint8_t kPaletteCount = static_cast<uint8_t>(sizeof(kPalettes) / sizeof(kPalettes[0]));

}  // namespace

uint8_t paletteCount() { return kPaletteCount; }

const char* paletteName(uint8_t id) { return id < kPaletteCount ? kPalettes[id].name : nullptr; }

Palette::Palette(uint8_t id, Rgb color1, Rgb color2) : stops_(nullptr), count_(2), own_{color1, color2} {
    if (id > 0 && id < kPaletteCount) {
        stops_ = kPalettes[id].stops;
        count_ = kPalettes[id].count;
    }
}

Rgb Palette::at(uint8_t position) const {
    const Rgb* stops = stops_ != nullptr ? stops_ : own_;
    const uint16_t scaled = static_cast<uint16_t>(position) * static_cast<uint16_t>(count_ - 1);
    const uint8_t index = static_cast<uint8_t>(scaled / 255u);
    if (index >= count_ - 1) return stops[count_ - 1];
    const uint8_t amount = static_cast<uint8_t>(scaled - index * 255u);  // 0..254, in 255ths of a step
    return blend(stops[index], stops[index + 1], amount);
}

}  // namespace jacket
