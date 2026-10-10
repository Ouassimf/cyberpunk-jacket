// Scene model, persistence format, colour maths, palettes and the in-memory
// storage. Colour functions are checked against floating-point references
// written here, the blob formats against bytes written out by hand from the
// layout documented in jacket/scene.h.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "../support/test_support.h"

using namespace jacket;
using support::Bytes;
using support::fromHex;

void setUp() {}
void tearDown() {}

namespace {

#define ASSERT_NEAR(tolerance, expected, actual) \
    TEST_ASSERT_TRUE(std::fabs(static_cast<double>(expected) - static_cast<double>(actual)) <= (tolerance))

// ---- Colour maths -------------------------------------------------------------------

void test_scale_and_blend_endpoints() {
    for (int v = 0; v < 256; ++v) {
        TEST_ASSERT_EQUAL(v, scale8(static_cast<uint8_t>(v), 255));
        TEST_ASSERT_EQUAL(0, scale8(static_cast<uint8_t>(v), 0));
        TEST_ASSERT_EQUAL(v, lerp8(static_cast<uint8_t>(v), 77, 0));
        TEST_ASSERT_EQUAL(v, lerp8(77, static_cast<uint8_t>(v), 255));
    }
    const Rgb a(10, 200, 30), b(250, 0, 90);
    TEST_ASSERT_RGB(a, blend(a, b, 0));
    TEST_ASSERT_RGB(b, blend(a, b, 255));
    TEST_ASSERT_RGB(a, scale(a, 255));
    TEST_ASSERT_RGB(kBlack, scale(a, 0));
    TEST_ASSERT_RGB(a, fade(a, 0));
    TEST_ASSERT_RGB(kBlack, fade(a, 255));
    TEST_ASSERT_RGB(Rgb(250, 200, 90), lighter(a, b));
}

void test_scale_and_blend_against_reference() {
    for (int v = 0; v < 256; v += 3) {
        for (int s = 0; s < 256; s += 5) {
            TEST_ASSERT_INT_WITHIN(1, static_cast<int>(std::lround(v * s / 255.0)), scale8(static_cast<uint8_t>(v), static_cast<uint8_t>(s)));
            const int from = v, to = 255 - s;
            for (int amount = 0; amount < 256; amount += 17) {
                const int exact = static_cast<int>(std::lround(from + (to - from) * amount / 255.0));
                TEST_ASSERT_INT_WITHIN(1, exact, lerp8(static_cast<uint8_t>(from), static_cast<uint8_t>(to), static_cast<uint8_t>(amount)));
            }
        }
    }
    // Dimming never brightens and keeps order.
    for (int s = 1; s < 256; ++s) {
        TEST_ASSERT_GREATER_OR_EQUAL(scale8(200, static_cast<uint8_t>(s - 1)), scale8(200, static_cast<uint8_t>(s)));
        TEST_ASSERT_LESS_OR_EQUAL(200, scale8(200, static_cast<uint8_t>(s)));
    }
}

// Textbook HSV -> RGB in floating point.
void referenceHsv(double h, double s, double v, double out[3]) {
    const double h6 = h * 6.0;
    const int sector = static_cast<int>(std::floor(h6)) % 6;
    const double f = h6 - std::floor(h6);
    const double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    const double table[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
    for (int i = 0; i < 3; ++i) out[i] = table[sector][i] * 255.0;
}

void test_hsv_against_reference() {
    TEST_ASSERT_RGB(Rgb(255, 0, 0), hsvToRgb(0, 255, 255));
    TEST_ASSERT_RGB(kBlack, hsvToRgb(12345, 255, 0));
    TEST_ASSERT_RGB(Rgb(99, 99, 99), hsvToRgb(40000, 0, 99));
    for (uint32_t hue = 0; hue < 65536; hue += 97) {
        for (int s : {255, 200, 64}) {
            for (int v : {255, 128, 20}) {
                double ref[3];
                referenceHsv(hue / 65536.0, s / 255.0, v / 255.0, ref);
                const Rgb got = hsvToRgb(static_cast<uint16_t>(hue), static_cast<uint8_t>(s), static_cast<uint8_t>(v));
                ASSERT_NEAR(4.0, ref[0], got.r);
                ASSERT_NEAR(4.0, ref[1], got.g);
                ASSERT_NEAR(4.0, ref[2], got.b);
            }
        }
    }
}

void test_wave() {
    TEST_ASSERT_EQUAL(0, wave8(0));
    TEST_ASSERT_EQUAL(255, wave8(0x8000));
    int previous = 0;
    for (uint32_t phase = 0; phase <= 0x8000; phase += 16) {
        const int w = wave8(static_cast<uint16_t>(phase));
        TEST_ASSERT_GREATER_OR_EQUAL(previous, w);
        previous = w;
        // Cubic ease of a triangle wave, and symmetric about the peak.
        const double x = phase / 32768.0;
        ASSERT_NEAR(2.0, 255.0 * x * x * (3 - 2 * x), w);
        TEST_ASSERT_INT_WITHIN(1, w, wave8(static_cast<uint16_t>(0x10000 - phase)));
    }
    TEST_ASSERT_INT_WITHIN(2, 128, wave8(0x4000));
}

// ---- Palettes -------------------------------------------------------------------------

void test_palette_zero_is_the_colour_gradient() {
    const Rgb a(0, 255, 204), b(255, 0, 255);
    const Palette p(0, a, b);
    TEST_ASSERT_RGB(a, p.at(0));
    TEST_ASSERT_RGB(b, p.at(255));
    for (int pos = 0; pos < 256; ++pos) {
        const Rgb c = p.at(static_cast<uint8_t>(pos));
        TEST_ASSERT_INT_WITHIN(1, static_cast<int>(std::lround(a.r + (b.r - a.r) * pos / 255.0)), c.r);
        TEST_ASSERT_INT_WITHIN(1, static_cast<int>(std::lround(a.g + (b.g - a.g) * pos / 255.0)), c.g);
        TEST_ASSERT_INT_WITHIN(1, static_cast<int>(std::lround(a.b + (b.b - a.b) * pos / 255.0)), c.b);
    }
    // An id this firmware does not know behaves like palette 0.
    TEST_ASSERT_RGB(p.at(100), Palette(200, a, b).at(100));
    // A palette can be copied (it is bound to colours, not to an address).
    const Palette copy = p;
    TEST_ASSERT_RGB(p.at(77), copy.at(77));
}

void test_palette_catalogue() {
    TEST_ASSERT_GREATER_OR_EQUAL(6, paletteCount());
    TEST_ASSERT_EQUAL_STRING("Colours", paletteName(0));
    TEST_ASSERT_NULL(paletteName(paletteCount()));
    for (uint8_t id = 0; id < paletteCount(); ++id) {
        TEST_ASSERT_NOT_NULL(paletteName(id));
        TEST_ASSERT_GREATER_THAN(0, strlen(paletteName(id)));
        // Smooth: no sudden jump between neighbouring positions.
        const Palette p(id, Rgb(0, 255, 204), Rgb(255, 0, 255));
        for (int pos = 1; pos < 256; ++pos) {
            const Rgb x = p.at(static_cast<uint8_t>(pos - 1)), y = p.at(static_cast<uint8_t>(pos));
            TEST_ASSERT_INT_WITHIN_MESSAGE(12, x.r, y.r, paletteName(id));
            TEST_ASSERT_INT_WITHIN_MESSAGE(12, x.g, y.g, paletteName(id));
            TEST_ASSERT_INT_WITHIN_MESSAGE(12, x.b, y.b, paletteName(id));
        }
    }
}

uint8_t paletteId(const char* name) {
    for (uint8_t id = 0; id < paletteCount(); ++id) {
        if (std::string(paletteName(id)) == name) return id;
    }
    TEST_FAIL_MESSAGE(name);
    return 0;
}

// The fixed palettes must look like their names (and ignore the segment's colours).
void test_fixed_palettes_look_like_their_names() {
    const Rgb x(1, 2, 3), y(4, 5, 6);
    bool white = false, pure_green = false;
    for (int pos = 0; pos < 256; ++pos) {
        const uint8_t at = static_cast<uint8_t>(pos);
        const Rgb rainbow = Palette(paletteId("Rainbow"), x, y).at(at);
        TEST_ASSERT_EQUAL(255, std::max(rainbow.r, std::max(rainbow.g, rainbow.b)));
        TEST_ASSERT_EQUAL(0, std::min(rainbow.r, std::min(rainbow.g, rainbow.b)));

        const Rgb matrix = Palette(paletteId("Matrix"), x, y).at(at);  // green / white
        TEST_ASSERT_TRUE(matrix.g >= matrix.r && matrix.g >= matrix.b);
        white = white || (matrix.r > 240 && matrix.g > 240 && matrix.b > 240);
        pure_green = pure_green || (matrix.g == 255 && matrix.r == 0 && matrix.b < 40);

        const Rgb cyber = Palette(paletteId("Cyberpunk"), x, y).at(at);  // cyan / magenta / violet
        TEST_ASSERT_GREATER_OR_EQUAL(180, cyber.b);

        const Rgb fire = Palette(paletteId("Fire"), x, y).at(at);
        TEST_ASSERT_TRUE(fire.r >= fire.g && fire.g >= fire.b);

        const Rgb ocean = Palette(paletteId("Ocean"), x, y).at(at);
        TEST_ASSERT_TRUE(ocean.r <= ocean.g && ocean.r <= ocean.b);

        TEST_ASSERT_RGB(Palette(paletteId("Fire"), x, y).at(at), Palette(paletteId("Fire"), y, x).at(at));
    }
    TEST_ASSERT_TRUE(white && pure_green);
    const Palette rainbow(paletteId("Rainbow"), x, y);
    TEST_ASSERT_RGB(Rgb(255, 0, 0), rainbow.at(0));
    TEST_ASSERT_RGB(Rgb(0, 255, 0), rainbow.at(85));
    TEST_ASSERT_RGB(Rgb(0, 0, 255), rainbow.at(170));
    TEST_ASSERT_RGB(Rgb(255, 0, 0), rainbow.at(255));
    const Palette cyber(paletteId("Cyberpunk"), x, y);
    TEST_ASSERT_TRUE(cyber.at(0).g > 200 && cyber.at(0).r < 40);    // cyan end
    TEST_ASSERT_TRUE(cyber.at(255).r > 200 && cyber.at(255).g < 40);  // magenta end
    const Rgb violet = cyber.at(170);
    TEST_ASSERT_TRUE(violet.b > violet.r && violet.r > violet.g);
}

// ---- Geometry and scenes ----------------------------------------------------------------

SegmentGeometry geom(uint8_t mask, uint8_t start, uint8_t length, uint8_t flags) {
    SegmentGeometry g;
    g.strip_mask = mask;
    g.start = start;
    g.length = length;
    g.flags = flags;
    return g;
}

void test_constants_match_protocol() {
    TEST_ASSERT_EQUAL(1, kProtoVersion);
    TEST_ASSERT_EQUAL(2, kStripCount);
    TEST_ASSERT_EQUAL(144, kLedsPerStrip);
    TEST_ASSERT_EQUAL(8, kMaxSegments);
    TEST_ASSERT_EQUAL(8, kPresetSlots);
    TEST_ASSERT_EQUAL(16, kMaxPresetName);
    TEST_ASSERT_EQUAL(400, kBudgetMinMa);
    TEST_ASSERT_EQUAL(2000, kBudgetMaxMa);
    TEST_ASSERT_EQUAL(900, kBudgetDefaultMa);
    TEST_ASSERT_EQUAL(180, kMaxFrame);
    TEST_ASSERT_EQUAL(500, kLiveTimeoutMs);
}

void test_geometry_validation() {
    // "length 1...144, start + length <= strip length", strip_mask bits 0 and 1.
    TEST_ASSERT_TRUE(isValid(geom(1, 0, 144, 0)));
    TEST_ASSERT_TRUE(isValid(geom(2, 0, 144, 1)));
    TEST_ASSERT_TRUE(isValid(geom(3, 143, 1, 0)));
    TEST_ASSERT_TRUE(isValid(geom(3, 44, 100, 1)));
    TEST_ASSERT_FALSE(isValid(geom(0, 0, 144, 0)));
    TEST_ASSERT_FALSE(isValid(geom(4, 0, 144, 0)));
    TEST_ASSERT_FALSE(isValid(geom(7, 0, 144, 0)));
    TEST_ASSERT_FALSE(isValid(geom(3, 0, 0, 0)));
    TEST_ASSERT_FALSE(isValid(geom(3, 0, 145, 0)));
    TEST_ASSERT_FALSE(isValid(geom(3, 1, 144, 0)));
    TEST_ASSERT_FALSE(isValid(geom(3, 144, 1, 0)));
    TEST_ASSERT_FALSE(isValid(geom(3, 255, 255, 0)));
    TEST_ASSERT_FALSE(isValid(geom(3, 0, 144, 2)));
    // Exhaustive over start and length.
    for (int start = 0; start < 256; ++start) {
        for (int length = 0; length < 256; ++length) {
            const bool expected = length >= 1 && start + length <= 144;
            TEST_ASSERT_EQUAL(expected, isValid(geom(3, static_cast<uint8_t>(start), static_cast<uint8_t>(length), 0)));
        }
    }
    TEST_ASSERT_TRUE(geom(1, 0, 1, 1).reversed());
    TEST_ASSERT_FALSE(geom(1, 0, 1, 0).reversed());
}

void test_default_scene_is_valid() {
    const Scene s = defaultScene();
    TEST_ASSERT_TRUE(isValid(s));
    TEST_ASSERT_EQUAL(1, s.segment_count);
    TEST_ASSERT_EQUAL(3, s.segments[0].geometry.strip_mask);
    TEST_ASSERT_EQUAL(144, s.segments[0].geometry.length);
    Scene bad = s;
    bad.segment_count = 0;
    TEST_ASSERT_FALSE(isValid(bad));
    bad.segment_count = 9;
    TEST_ASSERT_FALSE(isValid(bad));
}

void test_budget_clamp() {
    TEST_ASSERT_EQUAL(400, clampBudget(0));
    TEST_ASSERT_EQUAL(400, clampBudget(300));
    TEST_ASSERT_EQUAL(400, clampBudget(399));
    TEST_ASSERT_EQUAL(400, clampBudget(400));
    TEST_ASSERT_EQUAL(401, clampBudget(401));
    TEST_ASSERT_EQUAL(1234, clampBudget(1234));
    TEST_ASSERT_EQUAL(2000, clampBudget(2000));
    TEST_ASSERT_EQUAL(2000, clampBudget(2001));
    TEST_ASSERT_EQUAL(2000, clampBudget(65535));
}

// ---- Power budget arithmetic (jacket/power.h) ------------------------------------------

// 288 LEDs full white by FastLED's figures: (80 + 55 + 75) mW each.
constexpr uint32_t kAllWhiteMw = 288u * 210u;

void test_power_plan_leaves_a_frame_alone_when_it_fits() {
    // 1000 mW of light at full scale is 200 mA at 5 V.
    PowerPlan plan = planPower(1000, 255, 900, 368);
    TEST_ASSERT_EQUAL(255, plan.scale);
    TEST_ASSERT_FALSE(plan.limited);
    TEST_ASSERT_EQUAL_MESSAGE(568, plan.estimated_ma, "scale 255 is exactly full: (255 + 1) / 256");

    // Half brightness sends (127 + 1) / 256 of it.
    plan = planPower(1000, 127, 900, 368);
    TEST_ASSERT_EQUAL(127, plan.scale);
    TEST_ASSERT_EQUAL(468, plan.estimated_ma);

    // A black frame, and brightness 0, draw the standby current whatever the budget.
    plan = planPower(0, 200, 400, 368);
    TEST_ASSERT_EQUAL(200, plan.scale);
    TEST_ASSERT_FALSE(plan.limited);
    TEST_ASSERT_EQUAL(368, plan.estimated_ma);
    plan = planPower(kAllWhiteMw, 0, 400, 368);
    TEST_ASSERT_EQUAL(0, plan.scale);
    TEST_ASSERT_FALSE(plan.limited);
    TEST_ASSERT_EQUAL(368, plan.estimated_ma);
}

void test_power_plan_dims_to_the_budget() {
    // All white would draw 12 A. 900 mA budget, 368 mA standby: 532 mA for light.
    const PowerPlan plan = planPower(kAllWhiteMw, 255, 900, 368);
    TEST_ASSERT_TRUE(plan.limited);
    TEST_ASSERT_EQUAL_MESSAGE(10, plan.scale, "(10 + 1) / 256 of 12096 mA is 519 mA; one step more would be 567");
    TEST_ASSERT_EQUAL(368 + 519, plan.estimated_ma);
    TEST_ASSERT_LESS_OR_EQUAL(900, plan.estimated_ma);

    // Swept: the result is never above the setting, never over budget unless
    // it is the floor of 1, and never lower than it has to be.
    uint32_t seed = 12345;
    auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (int i = 0; i < 20000; ++i) {
        const uint32_t light = next() % (kAllWhiteMw + 1);
        const uint8_t brightness = static_cast<uint8_t>(next());
        const uint16_t budget = static_cast<uint16_t>(400 + next() % 1601);
        const uint16_t standby = next() % 2 ? 368 : 468;
        const PowerPlan p = planPower(light, brightness, budget, standby);
        const uint32_t light_ma = p.estimated_ma - standby;
        TEST_ASSERT_LESS_OR_EQUAL(brightness, p.scale);
        TEST_ASSERT_EQUAL(p.scale < brightness, p.limited);
        if (brightness == 0) {
            TEST_ASSERT_EQUAL(0, p.scale);
            continue;
        }
        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(1, p.scale, "dim, never black, while the brightness setting is above 0");
        // Above the floor of 1 the light fits what the budget leaves after standby
        // (which may be nothing: then only light below 1 mW gets through).
        if (p.scale > 1) TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(budget > standby ? budget : standby, p.estimated_ma, "over budget");
        if (p.limited && p.scale + 1 <= brightness) {
            // One step brighter would not have fitted.
            const uint64_t brighter_mw = static_cast<uint64_t>(light) * (p.scale + 2u) / 256u;
            const uint64_t allowed_mw = budget > standby ? (budget - standby) * 5u : 0;
            TEST_ASSERT_TRUE_MESSAGE(brighter_mw > allowed_mw, "dimmed more than the budget asks");
        }
        TEST_ASSERT_LESS_OR_EQUAL(light / 5 + 1, light_ma);
        // A larger budget never dims more.
        TEST_ASSERT_GREATER_OR_EQUAL(p.scale, planPower(light, brightness, static_cast<uint16_t>(budget + 100), standby).scale);
    }
}

// A budget at or below the standby current strictly leaves nothing for light
// (400 mA with WiFi up: standby 468 mA). The picture is dimmed to the lowest
// step, not switched off, and the estimate owns up to the overdraw.
void test_power_plan_never_goes_black() {
    PowerPlan plan = planPower(kAllWhiteMw, 255, 400, 468);
    TEST_ASSERT_EQUAL(1, plan.scale);
    TEST_ASSERT_TRUE(plan.limited);
    TEST_ASSERT_EQUAL_MESSAGE(468 + 94, plan.estimated_ma, "all white at scale 1: 2 / 256 of 12096 mA");

    // A typical frame (a few lit pixels, 500 mW at full scale): under 1 mA of light at scale 1.
    plan = planPower(500, 64, 400, 468);
    TEST_ASSERT_EQUAL(1, plan.scale);
    TEST_ASSERT_EQUAL(468, plan.estimated_ma);

    // Just above standby: whatever fits, but at least 1.
    plan = planPower(kAllWhiteMw, 255, 400, 368);  // 32 mA for light: 160 mW of 60480
    TEST_ASSERT_EQUAL(1, plan.scale);
    plan = planPower(20000, 255, 400, 368);        // 160 mW of 20000: factor 2 / 256 fits exactly
    TEST_ASSERT_EQUAL(1, plan.scale);
    TEST_ASSERT_LESS_OR_EQUAL(400, plan.estimated_ma);
    plan = planPower(5000, 255, 400, 368);         // factor 8 / 256
    TEST_ASSERT_EQUAL(7, plan.scale);
    TEST_ASSERT_LESS_OR_EQUAL(400, plan.estimated_ma);

    // Brightness 1 was asked for: it stays 1.
    plan = planPower(kAllWhiteMw, 1, 400, 468);
    TEST_ASSERT_EQUAL(1, plan.scale);
    TEST_ASSERT_FALSE(plan.limited);
}

// "Segment i keeps the EffectConfig it had; new segments copy segment 0."
void test_apply_segments() {
    Scene s = defaultScene();
    s.segments[0].config.effect = 1;
    s.segments[0].config.color1 = Rgb(1, 2, 3);
    const SegmentGeometry three[] = {geom(1, 0, 50, 0), geom(2, 50, 50, 1), geom(3, 100, 44, 0)};
    TEST_ASSERT_TRUE(applySegments(s, three, 3));
    TEST_ASSERT_EQUAL(3, s.segment_count);
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(s.segments[i].geometry == three[i]);
        TEST_ASSERT_TRUE(s.segments[i].config == s.segments[0].config);
    }
    s.segments[1].config.effect = 4;
    s.segments[2].config.effect = 5;
    const SegmentGeometry two[] = {geom(3, 0, 72, 0), geom(3, 72, 72, 0)};
    TEST_ASSERT_TRUE(applySegments(s, two, 2));
    TEST_ASSERT_EQUAL(1, s.segments[0].config.effect);
    TEST_ASSERT_EQUAL(4, s.segments[1].config.effect);

    // Invalid layouts are refused and change nothing.
    const Scene before = s;
    const SegmentGeometry bad[] = {geom(3, 0, 72, 0), geom(3, 100, 72, 0)};
    TEST_ASSERT_FALSE(applySegments(s, bad, 2));
    TEST_ASSERT_FALSE(applySegments(s, two, 0));
    TEST_ASSERT_FALSE(applySegments(s, two, 9));
    TEST_ASSERT_FALSE(applySegments(s, nullptr, 1));
    TEST_ASSERT_TRUE(s == before);
}

// ---- Persistence format -------------------------------------------------------------------

void test_scene_blob_layout() {
    // [version 1][brightness][count] then geometry (4) + config (10) per segment.
    uint8_t out[kMaxSceneBlob];
    const size_t n = serializeScene(defaultScene(), out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("018001" "03009000" "06008080" "00ffcc" "ff00ff", support::toHex(out, n).c_str());
    TEST_ASSERT_EQUAL(3 + 8 * 14, kMaxSceneBlob);
}

Scene randomScene(Rng& rng) {
    Scene s;
    s.brightness = rng.next8();
    s.segment_count = static_cast<uint8_t>(1 + rng.below(kMaxSegments));
    for (uint8_t i = 0; i < s.segment_count; ++i) {
        const uint8_t length = static_cast<uint8_t>(1 + rng.below(144));
        s.segments[i].geometry = geom(static_cast<uint8_t>(1 + rng.below(3)), static_cast<uint8_t>(rng.below(145u - length)), length,
                                      static_cast<uint8_t>(rng.below(2)));
        EffectConfig& c = s.segments[i].config;
        c.effect = rng.next8();
        c.palette = rng.next8();
        c.speed = rng.next8();
        c.intensity = rng.next8();
        c.color1 = Rgb(rng.next8(), rng.next8(), rng.next8());
        c.color2 = Rgb(rng.next8(), rng.next8(), rng.next8());
    }
    return s;
}

void test_scene_blob_roundtrip() {
    Rng rng(20241010);
    for (int i = 0; i < 500; ++i) {
        const Scene scene = randomScene(rng);
        TEST_ASSERT_TRUE(isValid(scene));
        uint8_t out[kMaxSceneBlob + 4];
        const size_t n = serializeScene(scene, out, kMaxSceneBlob);
        TEST_ASSERT_EQUAL(3 + 14 * scene.segment_count, n);
        Scene back;
        {
            support::GuardedBytes guarded(Bytes(out, out + n));
            TEST_ASSERT_TRUE(deserializeScene(guarded.data(), guarded.size(), back));
        }
        TEST_ASSERT_TRUE(back == scene);

        // Trailing bytes (a later format revision) are ignored.
        out[n] = 0xEE;
        TEST_ASSERT_TRUE(deserializeScene(out, n + 1, back));
        TEST_ASSERT_TRUE(back == scene);

        // Every truncation is refused, without reading past the end.
        for (size_t cut = 0; cut < n; ++cut) {
            support::GuardedBytes guarded(Bytes(out, out + cut));
            Scene untouched = defaultScene();
            TEST_ASSERT_FALSE(deserializeScene(guarded.data(), guarded.size(), untouched));
            TEST_ASSERT_TRUE_MESSAGE(untouched == defaultScene(), "a failed read must leave the output alone");
        }
        // A buffer that is too small is refused.
        TEST_ASSERT_EQUAL(0, serializeScene(scene, out, n - 1));
    }
}

void test_scene_blob_rejects_bad_content() {
    Scene s;
    TEST_ASSERT_FALSE(deserializeScene(fromHex("028001" "03009000" "06008080" "00ffcc" "ff00ff").data(), 17, s));  // version 2
    TEST_ASSERT_FALSE(deserializeScene(fromHex("018000").data(), 3, s));                                          // no segments
    TEST_ASSERT_FALSE(deserializeScene(fromHex("018009").data(), 3, s));                                          // nine segments
    TEST_ASSERT_FALSE(deserializeScene(fromHex("018001" "03019000" "06008080" "00ffcc" "ff00ff").data(), 17, s));  // 1 + 144 > 144
    TEST_ASSERT_FALSE(deserializeScene(fromHex("018001" "00009000" "06008080" "00ffcc" "ff00ff").data(), 17, s));  // no strip
    TEST_ASSERT_FALSE(deserializeScene(nullptr, 0, s));
    Scene invalid = defaultScene();
    invalid.segments[0].geometry.length = 0;
    uint8_t out[kMaxSceneBlob];
    TEST_ASSERT_EQUAL_MESSAGE(0, serializeScene(invalid, out, sizeof out), "an invalid scene is never written");
}

void test_settings_blob() {
    // [version 1][budget u16 little-endian][flags: bit 0 dev mode]
    Settings settings;
    uint8_t out[kSettingsBlobSize];
    TEST_ASSERT_EQUAL(4, serializeSettings(settings, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("01840300", support::toHex(out, 4).c_str());
    settings.budget_ma = 1500;
    settings.dev_mode = true;
    TEST_ASSERT_EQUAL(4, serializeSettings(settings, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("01dc0501", support::toHex(out, 4).c_str());
    TEST_ASSERT_EQUAL(0, serializeSettings(settings, out, 3));

    Settings back;
    TEST_ASSERT_TRUE(deserializeSettings(out, 4, back));
    TEST_ASSERT_EQUAL(1500, back.budget_ma);
    TEST_ASSERT_TRUE(back.dev_mode);
    // Out-of-range budgets in storage are clamped, not trusted.
    TEST_ASSERT_TRUE(deserializeSettings(fromHex("01ffff00").data(), 4, back));
    TEST_ASSERT_EQUAL(2000, back.budget_ma);
    TEST_ASSERT_FALSE(back.dev_mode);
    TEST_ASSERT_TRUE(deserializeSettings(fromHex("0100000100ff").data(), 6, back));  // trailing bytes ignored
    TEST_ASSERT_EQUAL(400, back.budget_ma);
    TEST_ASSERT_TRUE(back.dev_mode);
    // Wrong version and truncations are refused and leave the output alone.
    back = Settings();
    TEST_ASSERT_FALSE(deserializeSettings(fromHex("02dc0501").data(), 4, back));
    for (size_t cut = 0; cut < 4; ++cut) {
        support::GuardedBytes guarded(Bytes(out, out + cut));
        TEST_ASSERT_FALSE(deserializeSettings(guarded.data(), guarded.size(), back));
    }
    TEST_ASSERT_EQUAL(900, back.budget_ma);
    TEST_ASSERT_FALSE(back.dev_mode);
}

// ---- MemoryStorage ---------------------------------------------------------------------

void test_memory_storage() {
    MemoryStorage storage;
    uint8_t buf[kMaxStorageBlob];
    TEST_ASSERT_EQUAL(0, storage.read("missing", buf, sizeof buf));

    const uint8_t a[] = {1, 2, 3};
    const uint8_t b[] = {9, 8, 7, 6, 5};
    TEST_ASSERT_TRUE(storage.write("a", a, sizeof a));
    TEST_ASSERT_TRUE(storage.write("b", b, sizeof b));
    TEST_ASSERT_EQUAL(3, storage.read("a", buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a, buf, 3);
    TEST_ASSERT_EQUAL(5, storage.read("b", buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b, buf, 5);
    TEST_ASSERT_EQUAL_MESSAGE(0, storage.read("b", buf, 4), "a blob that does not fit is not returned");

    TEST_ASSERT_TRUE(storage.write("a", b, sizeof b));  // replace
    TEST_ASSERT_EQUAL(5, storage.read("a", buf, sizeof buf));
    TEST_ASSERT_EQUAL(3, storage.write_count);

    TEST_ASSERT_TRUE(storage.erase("a"));
    TEST_ASSERT_EQUAL(0, storage.read("a", buf, sizeof buf));
    TEST_ASSERT_TRUE_MESSAGE(storage.erase("a"), "erasing a missing key is fine");
    TEST_ASSERT_EQUAL(5, storage.read("b", buf, sizeof buf));

    uint8_t big[kMaxStorageBlob + 1] = {};
    TEST_ASSERT_TRUE(storage.write("big", big, kMaxStorageBlob));
    TEST_ASSERT_FALSE(storage.write("toobig", big, sizeof big));
    TEST_ASSERT_FALSE(storage.write("a-key-longer-than-nvs-allows", a, sizeof a));

    storage.fail_writes = true;
    TEST_ASSERT_FALSE(storage.write("c", a, sizeof a));
    TEST_ASSERT_EQUAL(0, storage.read("c", buf, sizeof buf));
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_scale_and_blend_endpoints);
    RUN_TEST(test_scale_and_blend_against_reference);
    RUN_TEST(test_hsv_against_reference);
    RUN_TEST(test_wave);
    RUN_TEST(test_palette_zero_is_the_colour_gradient);
    RUN_TEST(test_palette_catalogue);
    RUN_TEST(test_fixed_palettes_look_like_their_names);
    RUN_TEST(test_constants_match_protocol);
    RUN_TEST(test_geometry_validation);
    RUN_TEST(test_default_scene_is_valid);
    RUN_TEST(test_budget_clamp);
    RUN_TEST(test_power_plan_leaves_a_frame_alone_when_it_fits);
    RUN_TEST(test_power_plan_dims_to_the_budget);
    RUN_TEST(test_power_plan_never_goes_black);
    RUN_TEST(test_apply_segments);
    RUN_TEST(test_scene_blob_layout);
    RUN_TEST(test_scene_blob_roundtrip);
    RUN_TEST(test_scene_blob_rejects_bad_content);
    RUN_TEST(test_settings_blob);
    RUN_TEST(test_memory_storage);
    return UNITY_END();
}
