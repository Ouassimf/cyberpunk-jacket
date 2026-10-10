// The arithmetic of the current budget: how far the brightness must come down
// so that the whole jacket stays inside SET_BUDGET. Pure numbers, so it can be
// tested on the host; the LED driver supplies the measurements.
//
//     total mA = standby + light * factor(scale)
//
//     standby   everything that flows with all LEDs dark (the ESP32 board and
//               the LEDs' idle current)
//     light     what the lit channels of this frame would draw at full scale
//     factor    how the LED driver applies a brightness `scale` of 0..255 to a
//               pixel: 0 at scale 0, otherwise (scale + 1) / 256 (FastLED's
//               scale8 with FASTLED_SCALE8_FIXED), so 255 is exactly full
//
// The limiter never turns a picture black: while the brightness setting is
// above 0 the scale does not go below 1, even when the budget is at or below
// standby and strictly leaves nothing for light. At scale 1 the frame draws
// 2/256 of its full-scale light current: under 1 mA for a few lit pixels, and
// 95 mA in the worst case of all 288 LEDs full white. That overdraw is
// accepted, and shows in `estimated_ma`.
#pragma once

#include <stdint.h>

namespace jacket {

struct PowerPlan {
    uint8_t scale = 0;          // brightness to send the frame with
    bool limited = false;       // the budget lowered it
    uint16_t estimated_ma = 0;  // modelled total draw with that scale
};

// `light_mw_full`: milliwatts of the frame's lit channels at full scale.
// `rail_mv`: the LED supply, to turn milliwatts into milliamps.
PowerPlan planPower(uint32_t light_mw_full, uint8_t brightness, uint16_t budget_ma, uint16_t standby_ma,
                    uint16_t rail_mv = 5000);

}  // namespace jacket
