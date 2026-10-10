// Palettes: colour ramps the effects sample with a position 0..255.
// Ids are published in the catalogue and are stable. Id 0 "Colours" is the
// gradient from a segment's color1 to its color2.
#pragma once

#include <stdint.h>

#include "jacket/color.h"

namespace jacket {

uint8_t paletteCount();
const char* paletteName(uint8_t id);  // nullptr for an unknown id

// A palette bound to one segment's colours. Cheap to build once per frame.
class Palette {
public:
    // An unknown id behaves like palette 0.
    Palette(uint8_t id, Rgb color1, Rgb color2);

    // Position 0 is the first colour, 255 exactly the last; linear in between.
    Rgb at(uint8_t position) const;

private:
    const Rgb* stops_;  // nullptr: own_ (palette 0)
    uint8_t count_;
    Rgb own_[2];
};

}  // namespace jacket
