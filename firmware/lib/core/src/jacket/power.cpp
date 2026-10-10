#include "jacket/power.h"

namespace jacket {

namespace {

// 256ths of full scale that reach the LEDs at a brightness scale.
uint32_t factor256(uint32_t scale) { return scale == 0 ? 0 : scale + 1; }

}  // namespace

PowerPlan planPower(uint32_t light_mw_full, uint8_t brightness, uint16_t budget_ma, uint16_t standby_ma, uint16_t rail_mv) {
    PowerPlan plan;
    const uint64_t allowed_mw = budget_ma > standby_ma ? static_cast<uint64_t>(budget_ma - standby_ma) * rail_mv / 1000u : 0;

    uint32_t scale = brightness;
    if (scale != 0 && static_cast<uint64_t>(light_mw_full) * factor256(scale) / 256u > allowed_mw) {
        // The largest factor that fits (by the same whole-milliwatt rounding as
        // the test above), then the scale that gives it; at least 1.
        const uint64_t fits256 = ((allowed_mw + 1u) * 256u - 1u) / light_mw_full;  // light_mw_full > 0 here
        scale = fits256 >= 2 ? static_cast<uint32_t>(fits256 - 1) : 1;
        if (scale > brightness) scale = brightness;
    }

    plan.scale = static_cast<uint8_t>(scale);
    plan.limited = scale < brightness;
    const uint64_t light_ma = static_cast<uint64_t>(light_mw_full) * factor256(scale) / 256u * 1000u / rail_mv;
    const uint64_t total_ma = standby_ma + light_ma;
    plan.estimated_ma = static_cast<uint16_t>(total_ma > 65535u ? 65535u : total_ma);
    return plan;
}

}  // namespace jacket
