// The two LED strips. The only file that touches FastLED.
//
// Brightness and the current budget
// ---------------------------------
// The engine renders full-range pixels; this layer scales them by the
// brightness setting and then lowers that scale, if needed, so the whole
// jacket stays inside the budget. PROTOCOL.md defines the budget as "the total
// current allowed at the battery", so the model adds up everything on the 5 V
// rail:
//
//     total mA = system + idle + light * (scale + 1) / 256      (nothing at scale 0)
//
//     system   what the ESP32 board draws (cfg::kSystemMaBle, or
//              cfg::kSystemMaWifi while WiFi is up, per board); the caller passes it in
//     idle     every WS2812B draws about 1 mA with all channels off
//              (cfg::kLedIdleMaX10): 288 mA for the 288 LEDs, at any brightness
//     light    FastLED's measured per-channel figures at full scale: 16 mA
//              red, 11 mA green, 15 mA blue per LED, summed over the frame by
//              FastLED's calculate_unscaled_power_mW()
//
// FastLED's own limiter (setMaxPowerInVoltsAndMilliamps) is not used: it
// multiplies the idle current and a fixed 25 mA "MCU" by the brightness, so at
// brightness 64 it accounts for 72 mA of the 288 mA the dark LEDs really draw
// and the jacket would exceed its budget by a few hundred mA. Only FastLED's
// per-pixel measurement is used; the fixed terms are handled here.
//
// (scale + 1) / 256 is how FastLED applies a brightness scale to a pixel, so
// 255 is exactly full. The arithmetic is jacket::planPower (lib/core,
// jacket/power.h), tested on the host.
//
// A budget at or below system + idle (about 370 mA, or 470 mA with WiFi up)
// leaves nothing for light. The picture is then dimmed to the lowest step
// rather than switched off, which overdraws the budget slightly: 2/256 of the
// frame's full-scale light current, under 1 mA for a few lit pixels and 95 mA
// for 288 LEDs full white. The strips draw their idle current even when dark:
// no software can switch that off.
#pragma once

#include <jacket.h>

class LedOutput {
public:
    void begin();

    // Sends one frame. `left` and `right` are jacket::kLedsPerStrip pixels
    // each. Does nothing while suspended.
    void show(const jacket::Rgb* left, const jacket::Rgb* right, uint8_t brightness, uint16_t budget_ma,
              uint16_t system_ma);

    // Blanks the strips and ignores show() until resume(): no LED data is
    // sent while a firmware upload is writing to flash.
    void suspend();
    void resume() { suspended_ = false; }
    bool suspended() const { return suspended_; }

    // About the last frame sent.
    uint8_t appliedScale() const { return applied_scale_; }  // brightness after the budget
    uint16_t estimatedMa() const { return estimated_ma_; }   // modelled total draw
    bool limited() const { return limited_; }                // the budget lowered the brightness
    uint32_t lastShowUs() const { return last_show_us_; }    // micros() when it was sent

    // The model's floor: what the jacket draws with every LED dark.
    static uint16_t standbyMa(uint16_t system_ma);

private:
    bool suspended_ = false;
    bool limited_ = false;
    uint8_t applied_scale_ = 0;
    uint16_t estimated_ma_ = 0;
    uint32_t last_show_us_ = 0;
};
