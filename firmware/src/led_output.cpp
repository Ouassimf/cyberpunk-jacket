#include "led_output.h"

#include <FastLED.h>
#include <string.h>

#include "config.h"

namespace {

constexpr uint16_t kLeds = jacket::kLedsPerStrip;
constexpr uint16_t kRailMv = 5000;  // FastLED's power figures are milliwatts at 5 V

CRGB g_left[kLeds];
CRGB g_right[kLeds];

// What FastLED's model adds for a strip of dark LEDs. Measured once from the
// library itself, so the light term stays right if FastLED changes its
// constants.
uint32_t g_dark_mw_per_strip = 0;

static_assert(sizeof(jacket::Rgb) == 3 && sizeof(CRGB) == 3, "pixels are copied byte for byte");

uint16_t idleMa() { return static_cast<uint16_t>((2u * kLeds * cfg::kLedIdleMaX10 + 5u) / 10u); }

// Milliwatts the lit channels of both strips would draw at full scale.
uint32_t lightMwAtFullScale() {
    const uint32_t left = calculate_unscaled_power_mW(g_left, kLeds);
    const uint32_t right = calculate_unscaled_power_mW(g_right, kLeds);
    return (left - g_dark_mw_per_strip) + (right - g_dark_mw_per_strip);
}

}  // namespace

uint16_t LedOutput::standbyMa(uint16_t system_ma) { return static_cast<uint16_t>(system_ma + idleMa()); }

void LedOutput::begin() {
    FastLED.addLeds<WS2812B, cfg::kPinLeft, GRB>(g_left, kLeds);
    FastLED.addLeds<WS2812B, cfg::kPinRight, GRB>(g_right, kLeds);
    FastLED.setDither(DISABLE_DITHER);  // frames are not sent often enough for temporal dithering
    FastLED.clear(true);
    g_dark_mw_per_strip = calculate_unscaled_power_mW(g_left, kLeds);  // all black here
    last_show_us_ = micros();
}

void LedOutput::show(const jacket::Rgb* left, const jacket::Rgb* right, uint8_t brightness, uint16_t budget_ma,
                     uint16_t system_ma) {
    if (suspended_) return;
    memcpy(static_cast<void*>(g_left), left, sizeof g_left);
    memcpy(static_cast<void*>(g_right), right, sizeof g_right);

    const jacket::PowerPlan plan = jacket::planPower(lightMwAtFullScale(), brightness, budget_ma, standbyMa(system_ma), kRailMv);
    limited_ = plan.limited;
    applied_scale_ = plan.scale;
    estimated_ma_ = plan.estimated_ma;

    FastLED.show(applied_scale_);
    last_show_us_ = micros();
}

void LedOutput::suspend() {
    if (suspended_) return;
    FastLED.clear(true);
    delay(10);  // let the last frame leave before flash writes start
    suspended_ = true;
    applied_scale_ = 0;
    limited_ = false;
}
