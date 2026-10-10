// Effects.
//
// Three kinds of test live here, and they prove different things:
//
//  PROPERTY tests (everything except the last test) assert facts that follow
//  from the requirements or from the documented look of an effect: time base,
//  speed law, frame-rate independence, determinism, "uses exactly the
//  parameters its param_flags claim", where the comet's head is at a given
//  time, and so on. They would catch a wrong implementation.
//
//  The effect looks themselves (62.5 px/s, 4.096 s per breath, ...) are this
//  firmware's own design, written in the header comment of each effect file;
//  PROTOCOL.md does not define them. Tests of those numbers check the code
//  against that description.
//
//  The SNAPSHOT test at the end hashes rendered pictures and compares them
//  with hashes recorded from this same code. It proves nothing about
//  correctness: it only reports that the output changed.

#include <jacket/effects/effects.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../support/live_sim.h"
#include "../support/test_support.h"

using namespace jacket;
using support::Picture;
using support::brightnessOf;

void setUp() {}
void tearDown() {}

namespace {

constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kGreen{0, 255, 0};
constexpr Rgb kBlue{0, 0, 255};
constexpr Rgb kWhite{255, 255, 255};

enum : uint8_t {
    kSolid = 0,
    kComet = 1,
    kRainbow = 2,
    kMatrix = 3,
    kBreathe = 4,
    kHeartbeat = 5,
    kCylon = 6,
    kBeatPulse = 7,
    kVuMeter = 8,
    kSparkle = 9,
};
constexpr uint8_t kEffectIds = 10;

using Span = std::vector<Rgb>;

EffectConfig config(uint8_t effect, Rgb c1 = kRed, Rgb c2 = kBlue, uint8_t intensity = 128, uint8_t palette = 0) {
    EffectConfig c;
    c.effect = effect;
    c.palette = palette;
    c.speed = 128;
    c.intensity = intensity;
    c.color1 = c1;
    c.color2 = c2;
    return c;
}

// Renders one effect directly, at effect time `time_ms`, into `count` pixels.
Span draw(const EffectConfig& cfg, uint32_t time_ms, uint16_t count = 144, uint32_t seed = 12345, bool* animating = nullptr) {
    Span pixels(count);  // black
    EffectState state;
    state.seed = seed;
    const Palette palette(cfg.palette, cfg.color1, cfg.color2);
    EffectContext ctx{pixels.data(), count, time_ms, cfg, palette, nullptr, state};
    const EffectInfo* info = effectInfo(cfg.effect);
    TEST_ASSERT_NOT_NULL(info);
    const bool result = info->render(ctx);
    if (animating != nullptr) *animating = result;
    return pixels;
}

int brightest(const Span& s) {
    int best = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        if (brightnessOf(s[i]) > brightnessOf(s[best])) best = static_cast<int>(i);
    }
    return best;
}

int litCount(const Span& s) {
    int n = 0;
    for (const Rgb& p : s) n += p != kBlack;
    return n;
}

// An engine showing `cfg` on one full-length segment (both strips).
struct Show {
    MemoryStorage storage;
    Engine engine;
    support::CaptureSink sink;
    Picture pic;

    explicit Show(const EffectConfig& cfg, uint32_t seed = 0x4A41434B) : engine(storage, withSeed(seed)) { set(0xFF, cfg); }

    static EngineConfig withSeed(uint32_t seed) {
        EngineConfig c;
        c.seed = seed;
        return c;
    }
    void set(uint8_t segment, const EffectConfig& cfg) {
        const uint8_t frame[] = {0x03,         segment,      cfg.effect,   cfg.palette,  cfg.speed,    cfg.intensity,
                                 cfg.color1.r, cfg.color1.g, cfg.color1.b, cfg.color2.r, cfg.color2.g, cfg.color2.b};
        TEST_ASSERT_EQUAL(0, static_cast<int>(engine.handleCommand(frame, sizeof frame, 0, sink)));
    }
    void segments(std::initializer_list<SegmentGeometry> list) {
        std::vector<uint8_t> frame{0x05, static_cast<uint8_t>(list.size())};
        for (const SegmentGeometry& g : list) frame.insert(frame.end(), {g.strip_mask, g.start, g.length, g.flags});
        TEST_ASSERT_EQUAL(0, static_cast<int>(engine.handleCommand(frame.data(), frame.size(), 0, sink)));
    }
    const Picture& at(uint32_t now_ms) {
        engine.render(now_ms, pic.left, pic.right);
        return pic;
    }
};

// ---- Catalogue and time base -----------------------------------------------------------

void test_registry_matches_protocol() {
    const char* const names[] = {"Solid",     "Comet", "Rainbow",    "Matrix",   "Breathe",
                                 "Heartbeat", "Cylon", "Beat Pulse", "VU Meter", "Sparkle"};
    TEST_ASSERT_EQUAL(10, effectCount());
    for (uint8_t id = 0; id < 10; ++id) {
        const EffectInfo* info = effectInfo(id);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_EQUAL(id, info->id);
        TEST_ASSERT_EQUAL_STRING(names[id], info->name);
        TEST_ASSERT_NOT_NULL(info->render);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, info->param_flags & 0x80, "bit 7 is not defined");
    }
    TEST_ASSERT_NULL(effectInfo(10));
    TEST_ASSERT_NULL(effectInfo(255));
    // The reactive effects say honestly what they react to (bit 5 sound, bit 6 motion).
    for (uint8_t id = 0; id < 7; ++id) TEST_ASSERT_EQUAL_HEX8(0, effectInfo(id)->param_flags & 0x60);
    TEST_ASSERT_EQUAL_HEX8(0x3B, effectInfo(kBeatPulse)->param_flags);
    TEST_ASSERT_EQUAL_HEX8(0x3C, effectInfo(kVuMeter)->param_flags);
    TEST_ASSERT_EQUAL_HEX8(0x5C, effectInfo(kSparkle)->param_flags);
    // The two rows that appear in the golden vectors.
    TEST_ASSERT_EQUAL_HEX8(0x01, effectInfo(kSolid)->param_flags);
    TEST_ASSERT_EQUAL_HEX8(0x1D, effectInfo(kComet)->param_flags);
}

// "time multiplier = 2^((speed - 128) / 32); 128 = 1x"
void test_speed_multiplier_follows_the_formula() {
    TEST_ASSERT_EQUAL_UINT32(65536, speedMultiplierQ16(128));
    TEST_ASSERT_EQUAL_UINT32(131072, speedMultiplierQ16(160));
    TEST_ASSERT_EQUAL_UINT32(262144, speedMultiplierQ16(192));
    TEST_ASSERT_EQUAL_UINT32(32768, speedMultiplierQ16(96));
    TEST_ASSERT_EQUAL_UINT32(4096, speedMultiplierQ16(0));
    uint32_t previous = 0;
    for (int speed = 0; speed < 256; ++speed) {
        const double exact = 65536.0 * std::pow(2.0, (speed - 128) / 32.0);
        const uint32_t got = speedMultiplierQ16(static_cast<uint8_t>(speed));
        char msg[64];
        snprintf(msg, sizeof msg, "speed %d: got %u, exact %.1f", speed, static_cast<unsigned>(got), exact);
        TEST_ASSERT_TRUE_MESSAGE(std::fabs(got - exact) <= exact * 0.0002 + 1.0, msg);
        TEST_ASSERT_TRUE_MESSAGE(got > previous, "strictly increasing");
        previous = got;
    }
}

// Requirement: "the speed multiplier doubles the rate". 32 speed steps are one
// octave, so the picture at speed 160 after d ms is the picture at speed 128
// after 2d ms, for every effect.
void test_speed_scales_time_for_every_effect() {
    for (uint8_t effect = 0; effect < kEffectIds; ++effect) {
        EffectConfig normal = config(effect);
        EffectConfig twice = normal;
        twice.speed = 160;
        EffectConfig half = normal;
        half.speed = 96;
        Show a(normal), b(twice), c(half);
        a.at(5000);
        b.at(5000);
        c.at(5000);
        TEST_ASSERT_TRUE_MESSAGE(a.pic == b.pic && a.pic == c.pic, "all start from effect time 0");
        for (uint32_t d : {40u, 250u, 1234u, 9000u}) {
            const Picture reference = a.at(5000 + 2 * d);  // 1x after 2d
            b.at(5000 + d);                                // 2x after d
            c.at(5000 + 4 * d);                            // 0.5x after 4d
            TEST_ASSERT_TRUE_MESSAGE(reference == b.pic, effectInfo(effect)->name);
            TEST_ASSERT_TRUE_MESSAGE(reference == c.pic, effectInfo(effect)->name);
        }
    }
}

// Requirement: "rendering at 30 fps and 120 fps gives the same picture at the
// same timestamp". Three engines play the same three-segment scene (every
// animated effect, different speeds): one is rendered at 120 fps, one at
// 30 fps, one only when compared. They must agree exactly whenever their
// timestamps coincide.
void test_frame_rate_independence() {
    const uint8_t trios[2][3] = {{kComet, kMatrix, kCylon}, {kRainbow, kBreathe, kHeartbeat}};
    for (const auto& trio : trios) {
        Show fast(config(kSolid)), slow(config(kSolid)), lazy(config(kSolid));
        for (Show* show : {&fast, &slow, &lazy}) {
            show->segments({{1, 0, 144, 0}, {2, 0, 100, 1}, {3, 100, 44, 0}});
            for (uint8_t s = 0; s < 3; ++s) {
                EffectConfig cfg = config(trio[s], kRed, kBlue, static_cast<uint8_t>(60 + 70 * s), s);
                cfg.speed = static_cast<uint8_t>(100 + 37 * s);  // not powers of two
                show->set(s, cfg);
            }
            show->at(777);
        }
        TEST_ASSERT_TRUE(fast.pic == slow.pic && fast.pic == lazy.pic);

        int compared = 0;
        for (uint32_t frame = 1; frame <= 120 * 6; ++frame) {
            const uint32_t now = 777 + (frame * 1000 + 60) / 120;  // 120 fps, rounded to whole ms
            fast.at(now);
            if (frame % 4 != 0) continue;
            slow.at(now);                                          // 30 fps
            TEST_ASSERT_TRUE_MESSAGE(fast.pic == slow.pic, "120 fps and 30 fps disagree");
            if (frame % 120 == 0) {
                lazy.at(now);                                      // 1 fps
                TEST_ASSERT_TRUE_MESSAGE(fast.pic == lazy.pic, "120 fps and 1 fps disagree");
            }
            ++compared;
        }
        TEST_ASSERT_EQUAL(180, compared);
        TEST_ASSERT_FALSE_MESSAGE(fast.pic.allBlack(), "the scene should be showing something");
    }
}

// Rendering the same moment twice changes nothing (no hidden per-call state).
void test_rendering_is_idempotent_at_a_fixed_time() {
    for (uint8_t effect = 0; effect < kEffectIds; ++effect) {
        Show show(config(effect));
        show.at(100);
        const Picture first = show.at(2345);
        show.at(2345);
        show.at(2345);
        TEST_ASSERT_TRUE_MESSAGE(first == show.pic, effectInfo(effect)->name);
    }
}

// ---- param_flags are honest ---------------------------------------------------------

// Does changing one parameter change the picture at any of a set of times?
bool matters(const EffectConfig& base, const EffectConfig& changed) {
    for (uint32_t t : {0u, 37u, 333u, 801u, 1500u, 2048u, 3210u, 7777u}) {
        if (draw(base, t) != draw(changed, t)) return true;
    }
    return false;
}

// Requirement: effects "honour colour/palette/intensity as their param_flags
// claim". For every effect and parameter: if the flag is set the parameter
// changes the picture, if it is clear it does not. Colours are judged with a
// fixed palette selected, because palette 0 is itself made of the colours.
void test_param_flags_are_honest() {
    for (uint8_t effect = 0; effect < kEffectIds; ++effect) {
        const EffectInfo* info = effectInfo(effect);
        const uint8_t flags = info->param_flags;
        const bool uses_palette = (flags & kUsesPalette) != 0;
        const EffectConfig base = config(effect, kRed, kBlue, 128, uses_palette ? 1 : 0);

        EffectConfig other = base;
        other.color1 = kGreen;
        TEST_ASSERT_EQUAL_MESSAGE((flags & kUsesColor1) != 0, matters(base, other), info->name);

        other = base;
        other.color2 = kGreen;
        TEST_ASSERT_EQUAL_MESSAGE((flags & kUsesColor2) != 0, matters(base, other), info->name);

        other = base;
        other.palette = 4;
        TEST_ASSERT_EQUAL_MESSAGE(uses_palette, matters(base, other), info->name);

        other = base;
        other.intensity = 20;
        TEST_ASSERT_EQUAL_MESSAGE((flags & kUsesIntensity) != 0, matters(base, other), info->name);

        // Speed acts through effect time: an effect uses it exactly when its
        // picture changes with time.
        bool moves = false;
        for (uint32_t t : {37u, 333u, 801u, 1500u, 2048u, 3210u}) moves = moves || draw(base, t) != draw(base, 0);
        TEST_ASSERT_EQUAL_MESSAGE((flags & kUsesSpeed) != 0, moves, info->name);

        // Palette 0 is the gradient color1 -> color2: with it selected, both
        // colours matter to an effect that uses the palette.
        if (uses_palette) {
            const EffectConfig colours = config(effect, kRed, kBlue, 128, 0);
            other = colours;
            other.color1 = kGreen;
            TEST_ASSERT_TRUE_MESSAGE(matters(colours, other), info->name);
            other = colours;
            other.color2 = kGreen;
            TEST_ASSERT_TRUE_MESSAGE(matters(colours, other), info->name);
        }
    }
}

// Every effect must cope with any segment length and stay inside its span.
void test_any_length_and_no_overrun() {
    for (uint8_t effect = 0; effect < kEffectIds; ++effect) {
        for (uint16_t count : {1, 2, 3, 5, 17, 143, 144}) {
            for (uint8_t intensity : {0, 128, 255}) {
                for (uint32_t t : {0u, 1u, 999u, 4095u, 123456789u, 0xFFFFFFFFu}) {
                    const Rgb canary(0xAB, 0xCD, 0xEF);
                    std::vector<Rgb> buffer(count + 16, canary);
                    for (uint16_t i = 0; i < count; ++i) buffer[8 + i] = kBlack;
                    EffectState state;
                    state.seed = 99;
                    const EffectConfig cfg = config(effect, kRed, kBlue, intensity);
                    const Palette palette(cfg.palette, cfg.color1, cfg.color2);
                    EffectContext ctx{buffer.data() + 8, count, t, cfg, palette, nullptr, state};
                    effectInfo(effect)->render(ctx);
                    for (int i = 0; i < 8; ++i) {
                        TEST_ASSERT_TRUE_MESSAGE(buffer[i] == canary && buffer[8 + count + i] == canary, effectInfo(effect)->name);
                    }
                }
            }
        }
    }
}

// ---- Solid ------------------------------------------------------------------------------

void test_solid_is_color1_and_static() {
    for (uint32_t t : {0u, 500u, 100000u}) {
        bool animating = true;
        const Span s = draw(config(kSolid, Rgb(12, 200, 99), kBlue, 7, 3), t, 144, 1, &animating);
        for (const Rgb& p : s) TEST_ASSERT_RGB(Rgb(12, 200, 99), p);
        TEST_ASSERT_FALSE_MESSAGE(animating, "Solid must let the device stop refreshing");
    }
}

// ---- Comet ------------------------------------------------------------------------------

// Head velocity is 62.5 px/s at speed 128: 10 pixels every 160 ms, wrapping.
void test_comet_head_position() {
    for (uint32_t k = 0; k < 40; ++k) {
        bool animating = false;
        const Span s = draw(config(kComet), k * 160, 144, 1, &animating);
        const int head = static_cast<int>((k * 10) % 144);
        TEST_ASSERT_EQUAL(head, brightest(s));
        TEST_ASSERT_RGB(kRed, s[head]);  // the head is color1 at full level
        TEST_ASSERT_RGB(kBlack, s[(head + 1) % 144]);
        TEST_ASSERT_TRUE(animating);
    }
    // On a short segment it wraps at that segment's length.
    TEST_ASSERT_EQUAL(5, brightest(draw(config(kComet, kRed, kRed, 0), 25 * 16, 20)));
    // It starts as a bare head on pixel 0 and grows its tail as it goes:
    // nothing is lit where the comet has not been yet.
    TEST_ASSERT_EQUAL(1, litCount(draw(config(kComet, kRed, kBlue, 255), 0)));
    const Span young = draw(config(kComet, kRed, kBlue, 255), 320);  // head at 20, tail could be 64
    for (int i = 21; i < 144; ++i) TEST_ASSERT_RGB(kBlack, young[i]);
    // After one lap the tail does wrap round the end of the segment.
    TEST_ASSERT_TRUE(draw(config(kComet, kRed, kBlue, 255), (144 + 20) * 16)[140] != kBlack);
}

void test_comet_tail() {
    // Head at pixel 100. Brightness falls monotonically behind it and the
    // comet is never longer than the tail length for its intensity.
    for (uint8_t intensity : {0, 60, 128, 255}) {
        const Span s = draw(config(kComet, kWhite, kWhite, intensity), 1600);
        const int tail = effects::tailPixels(intensity);
        for (int d = 1; d < 100; ++d) {
            TEST_ASSERT_LESS_OR_EQUAL(brightnessOf(s[100 - d + 1]), brightnessOf(s[100 - d]));
            if (d >= tail) TEST_ASSERT_RGB(kBlack, s[100 - d]);
        }
        for (int i = 101; i < 144; ++i) TEST_ASSERT_RGB(kBlack, s[i]);
    }
    TEST_ASSERT_EQUAL(1, effects::tailPixels(0));
    TEST_ASSERT_EQUAL(64, effects::tailPixels(255));
    TEST_ASSERT_EQUAL(1, litCount(draw(config(kComet, kRed, kBlue, 0), 1600)));
    // More intensity, longer visible tail.
    int previous = 0;
    for (uint8_t intensity : {0, 50, 100, 150, 200, 255}) {
        const int lit = litCount(draw(config(kComet, kRed, kBlue, intensity), 1600));
        TEST_ASSERT_GREATER_THAN(previous, lit);
        previous = lit;
    }
}

void test_comet_moves_smoothly() {
    // Half-way between two pixels the next pixel is fading in.
    const Span s = draw(config(kComet, kWhite, kWhite, 128), 1608);  // 100.5 px
    TEST_ASSERT_INT_WITHIN(8, 128, s[101].r);
    TEST_ASSERT_GREATER_THAN(200, s[100].r);
    TEST_ASSERT_RGB(kBlack, s[102]);
    // Total light never jumps from one millisecond to the next.
    int previous = -1;
    for (uint32_t t = 3000; t < 3400; ++t) {
        int total = 0;
        for (const Rgb& p : draw(config(kComet, kWhite, kWhite, 128), t)) total += p.r;
        if (previous >= 0) TEST_ASSERT_INT_WITHIN(40, previous, total);
        previous = total;
    }
}

void test_comet_colours() {
    // Palette 0, red to blue: the head is color1, the far tail has turned to color2.
    Span s = draw(config(kComet, kRed, kBlue, 255), 1600);
    TEST_ASSERT_RGB(kRed, s[100]);
    TEST_ASSERT_GREATER_THAN(s[98].b, s[98].r);          // just behind the head: still red
    TEST_ASSERT_GREATER_THAN(s[100 - 40].r, s[100 - 40].b);  // deep in the tail: blue
    // One colour for both: every lit pixel is a dimmed color1.
    s = draw(config(kComet, Rgb(200, 100, 0), Rgb(200, 100, 0), 255), 1600);
    for (const Rgb& p : s) {
        TEST_ASSERT_EQUAL(0, p.b);
        if (p.g > 8) TEST_ASSERT_INT_WITHIN(3, p.r / 2, p.g);
    }
    // A fixed palette colours the tail; the head stays color1.
    s = draw(config(kComet, kRed, kRed, 255, 5 /* Ocean */), 1600);
    TEST_ASSERT_RGB(kRed, s[100]);
    TEST_ASSERT_GREATER_THAN(s[100 - 30].r, s[100 - 30].b);
}

// ---- Cylon ------------------------------------------------------------------------------

void test_cylon_bounces() {
    // 62.5 px/s out to the last pixel and back: 143 pixels each way.
    for (uint32_t k = 0; k <= 57; ++k) {
        const Span s = draw(config(kCylon, kRed, kRed), k * 160);
        const int travelled = static_cast<int>(k * 10) % 286;
        const int eye = travelled <= 143 ? travelled : 286 - travelled;
        TEST_ASSERT_EQUAL(eye, brightest(s));
        TEST_ASSERT_RGB(kRed, s[eye]);
    }
    // Exactly at the far end, then exactly back at the start.
    TEST_ASSERT_EQUAL(143, brightest(draw(config(kCylon, kRed, kRed), 143 * 16)));
    TEST_ASSERT_EQUAL(0, brightest(draw(config(kCylon, kRed, kRed), 286 * 16)));
    // It starts as a bare eye: no trail from a sweep that never happened.
    TEST_ASSERT_EQUAL(1, litCount(draw(config(kCylon, kRed, kRed, 255), 0)));
}

void test_cylon_trail_is_behind_the_eye() {
    Span s = draw(config(kCylon, kRed, kRed), 800);  // outbound, eye at 50
    TEST_ASSERT_TRUE(s[49] != kBlack);
    TEST_ASSERT_RGB(kBlack, s[51]);
    TEST_ASSERT_GREATER_THAN(brightnessOf(s[45]), brightnessOf(s[49]));
    s = draw(config(kCylon, kRed, kRed), (143 + 50) * 16);  // inbound, eye at 93
    TEST_ASSERT_EQUAL(93, brightest(s));
    TEST_ASSERT_TRUE(s[94] != kBlack);
    TEST_ASSERT_RGB(kBlack, s[92]);
    // Just after turning round, the trail of the outbound run is still fading.
    s = draw(config(kCylon, kRed, kRed, 255), (143 + 5) * 16);  // eye at 138, heading back
    TEST_ASSERT_EQUAL(138, brightest(s));
    TEST_ASSERT_TRUE(s[143] != kBlack);
    TEST_ASSERT_TRUE(s[120] != kBlack);
}

void test_cylon_colour_follows_position() {
    // The palette is stretched over the segment: color1 at pixel 0, color2 at the far end.
    TEST_ASSERT_RGB(kRed, draw(config(kCylon, kRed, kBlue), 0)[0]);
    TEST_ASSERT_RGB(kBlue, draw(config(kCylon, kRed, kBlue), 143 * 16)[143]);
    const Span mid = draw(config(kCylon, kRed, kBlue), 72 * 16);
    TEST_ASSERT_INT_WITHIN(6, 127, mid[72].r);
    TEST_ASSERT_INT_WITHIN(6, 127, mid[72].b);
    // A single pixel cannot sweep: it is lit and static.
    bool animating = true;
    const Span one = draw(config(kCylon, kRed, kBlue), 1234, 1, 1, &animating);
    TEST_ASSERT_RGB(kRed, one[0]);
    TEST_ASSERT_FALSE(animating);
}

// ---- Rainbow ----------------------------------------------------------------------------

void test_rainbow() {
    const EffectConfig cfg = config(kRainbow);
    const Span start = draw(cfg, 0);
    TEST_ASSERT_RGB(kRed, start[0]);
    for (uint32_t t : {0u, 700u, 3000u}) {
        for (const Rgb& p : draw(cfg, t)) {
            // Fully saturated, full value: one channel at 255, one at 0.
            const int hi = std::max(p.r, std::max(p.g, p.b));
            const int lo = std::min(p.r, std::min(p.g, p.b));
            TEST_ASSERT_EQUAL(255, hi);
            TEST_ASSERT_EQUAL(0, lo);
        }
    }
    // One turn of the wheel every 4.096 s: red, green a third in, blue two thirds in.
    TEST_ASSERT_TRUE(draw(cfg, 4096) == start);
    const Rgb third = draw(cfg, 1365)[0];
    const Rgb two_thirds = draw(cfg, 2731)[0];
    TEST_ASSERT_TRUE(third.g == 255 && third.r < 8 && third.b < 8);
    TEST_ASSERT_TRUE(two_thirds.b == 255 && two_thirds.r < 8 && two_thirds.g < 8);
    // At intensity 128 there is one rainbow per 128 pixels, flowing towards
    // higher indices at one pixel per 32 ms.
    TEST_ASSERT_RGB(start[0], start[128]);
    const Span later = draw(cfg, 1000 + 32);
    const Span earlier = draw(cfg, 1000);
    for (int i = 0; i + 1 < 144; ++i) TEST_ASSERT_RGB(earlier[i], later[i + 1]);
    // Intensity 0: the whole segment is one colour, still cycling.
    const Span flat = draw(config(kRainbow, kRed, kBlue, 0), 900);
    for (const Rgb& p : flat) TEST_ASSERT_RGB(flat[0], p);
    TEST_ASSERT_TRUE(flat[0] != draw(config(kRainbow, kRed, kBlue, 0), 1800)[0]);
}

// ---- Breathe ----------------------------------------------------------------------------

void test_breathe() {
    const Rgb colour(240, 120, 60);
    const EffectConfig deep = config(kBreathe, colour, kBlue, 255);
    // Starts dark, peaks at color1 half-way through the 4.096 s breath.
    for (const Rgb& p : draw(deep, 0)) TEST_ASSERT_RGB(kBlack, p);
    for (const Rgb& p : draw(deep, 2048)) TEST_ASSERT_RGB(colour, p);
    TEST_ASSERT_TRUE(draw(deep, 4096 + 700) == draw(deep, 700));
    // Uniform, rising for the first half and falling for the second.
    int previous = -1;
    for (uint32_t t = 0; t <= 2048; t += 16) {
        const Span s = draw(deep, t);
        for (const Rgb& p : s) TEST_ASSERT_RGB(s[0], p);
        TEST_ASSERT_GREATER_OR_EQUAL(previous, brightnessOf(s[0]));
        previous = brightnessOf(s[0]);
    }
    for (uint32_t t = 2048; t <= 4096; t += 16) {
        const int now = brightnessOf(draw(deep, t)[0]);
        TEST_ASSERT_LESS_OR_EQUAL(previous, now);
        previous = now;
    }
    // The inhale mirrors the exhale.
    TEST_ASSERT_INT_WITHIN(3, brightnessOf(draw(deep, 600)[0]), brightnessOf(draw(deep, 4096 - 600)[0]));
    // Intensity is depth: 128 dims to about half, 0 does not dim and is static.
    TEST_ASSERT_INT_WITHIN(3, 120, draw(config(kBreathe, colour, kBlue, 128), 0)[0].r);
    bool animating = true;
    for (uint32_t t : {0u, 1000u, 2048u}) {
        TEST_ASSERT_RGB(colour, draw(config(kBreathe, colour, kBlue, 0), t, 144, 1, &animating)[0]);
        TEST_ASSERT_FALSE(animating);
    }
}

// ---- Heartbeat --------------------------------------------------------------------------

void test_heartbeat() {
    const EffectConfig cfg = config(kHeartbeat, kRed, kBlue, 128);
    // The beat starts with a single point of color1, 23 % along (pixel 33 of 144).
    Span s = draw(cfg, 0);
    TEST_ASSERT_EQUAL(1, litCount(s));
    TEST_ASSERT_RGB(kRed, s[33]);
    // The first ring spreads symmetrically; nothing is lit beyond its front,
    // and the second ring (centre 121) has not started before 192 ms.
    int previous_reach = 0;
    for (uint32_t t = 0; t < 192; t += 8) {
        s = draw(cfg, t);
        int reach = 0;
        for (int d = 0; d <= 33; ++d) {
            TEST_ASSERT_RGB(s[33 - d], s[33 + d]);
            if (s[33 + d] != kBlack) reach = d;
        }
        for (int i = 67; i < 144; ++i) TEST_ASSERT_RGB(kBlack, s[i]);
        TEST_ASSERT_GREATER_OR_EQUAL(previous_reach, reach);
        previous_reach = reach;
    }
    TEST_ASSERT_GREATER_THAN(10, previous_reach);
    // "dub": the second point appears at 192 ms and spreads to the end of the segment.
    s = draw(cfg, 192);
    TEST_ASSERT_RGB(kRed, s[121]);
    TEST_ASSERT_RGB(kBlack, s[120]);
    s = draw(cfg, 192 + 300);
    for (int d = 0; d <= 22; ++d) TEST_ASSERT_RGB(s[121 - d], s[121 + d]);
    TEST_ASSERT_TRUE(s[143] != kBlack);
    for (int i = 67; i < 99; ++i) TEST_ASSERT_RGB(kBlack, s[i]);  // between the two rings' reach
    // Only color1, dimmed.
    for (uint32_t t = 0; t < 1024; t += 20) {
        for (const Rgb& p : draw(cfg, t)) TEST_ASSERT_TRUE(p.g == 0 && p.b == 0);
    }
    // It repeats every 1.024 s and is dark before the next beat, even at the longest glow.
    TEST_ASSERT_TRUE(draw(cfg, 1024 + 250) == draw(cfg, 250));
    TEST_ASSERT_EQUAL(0, litCount(draw(config(kHeartbeat, kRed, kBlue, 255), 1023)));
    // Intensity: the glow lingers longer.
    TEST_ASSERT_GREATER_THAN(litCount(draw(config(kHeartbeat, kRed, kBlue, 0), 380)),
                             litCount(draw(config(kHeartbeat, kRed, kBlue, 255), 380)));
}

// ---- Matrix -----------------------------------------------------------------------------

// Requirement: "deterministic (seeded PRNG kept in segment state, no global RNG)".
void test_matrix_is_deterministic() {
    const EffectConfig cfg = config(kMatrix, kRed, kBlue, 128, 2);
    Show a(cfg), b(cfg), other_seed(cfg, 0xDEADBEEF);
    bool differs = false;
    for (uint32_t t = 1000; t < 6000; t += 97) {
        a.at(t);
        b.at(t);
        other_seed.at(t);
        TEST_ASSERT_TRUE_MESSAGE(a.pic == b.pic, "same seed, same time, same picture");
        differs = differs || a.pic != other_seed.pic;
    }
    TEST_ASSERT_TRUE_MESSAGE(differs, "a different seed gives a different sequence");

    // Directly: the picture is a function of (seed, time) and nothing else.
    TEST_ASSERT_TRUE(draw(cfg, 4321, 144, 7) == draw(cfg, 4321, 144, 7));
    TEST_ASSERT_TRUE(draw(cfg, 4321, 144, 7) != draw(cfg, 4321, 144, 8));
    TEST_ASSERT_TRUE(draw(cfg, 4321, 144, 7) != draw(cfg, 4353, 144, 7));
}

void test_matrix_segments_have_their_own_sequence() {
    Show show(config(kMatrix, kRed, kBlue, 128, 2));
    show.segments({{1, 0, 144, 0}, {2, 0, 144, 0}});  // same effect on each strip separately
    show.at(1000);
    show.at(3000);
    bool differs = false;
    for (uint16_t i = 0; i < 144; ++i) differs = differs || show.pic.left[i] != show.pic.right[i];
    TEST_ASSERT_TRUE(differs);
    // One segment on both strips is, by definition, the same pixels on both.
    Show both(config(kMatrix, kRed, kBlue, 128, 2));
    both.at(1000);
    both.at(3000);
    for (uint16_t i = 0; i < 144; ++i) TEST_ASSERT_RGB(both.pic.left[i], both.pic.right[i]);
}

void test_matrix_colours_come_from_the_palette() {
    // Palette 0 with both colours red: only red, at varying levels.
    int levels = 0;
    bool seen[256] = {};
    for (uint32_t t = 600; t < 3000; t += 50) {
        for (const Rgb& p : draw(config(kMatrix, kRed, kRed), t)) {
            TEST_ASSERT_TRUE(p.g == 0 && p.b == 0);
            if (!seen[p.r]) {
                seen[p.r] = true;
                ++levels;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(20, levels, "sparks have many brightness levels");
    // The "Matrix" palette is greens and white: green is never the weakest channel.
    bool white_seen = false;
    for (uint32_t t = 600; t < 6000; t += 50) {
        for (const Rgb& p : draw(config(kMatrix, kRed, kRed, 128, 2), t)) {
            TEST_ASSERT_TRUE(p.g >= p.r && p.g >= p.b);
            white_seen = white_seen || (p.r > 150 && p.b > 150);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(white_seen, "white sparks appear too");
}

void test_matrix_density_follows_intensity() {
    auto average_lit = [](uint8_t intensity) {
        long total = 0;
        int frames = 0;
        for (uint32_t t = 1000; t < 9000; t += 40, ++frames) total += litCount(draw(config(kMatrix, kRed, kBlue, intensity), t));
        return static_cast<double>(total) / frames;
    };
    const double sparse = average_lit(10), medium = average_lit(128), dense = average_lit(255);
    TEST_ASSERT_TRUE(sparse < medium && medium < dense);
    TEST_ASSERT_TRUE_MESSAGE(sparse > 0.5, "even low intensity sparkles");
    TEST_ASSERT_TRUE_MESSAGE(dense < 144, "never a fully lit strip");
}

void test_matrix_sparks_fade_out() {
    // At the lowest intensity sparks are rare enough to follow one at a time:
    // once lit, a pixel only gets dimmer and is black within the spark
    // lifetime (16 ticks of 32 ms), unless a new spark lands on it.
    const EffectConfig cfg = config(kMatrix, kWhite, kWhite, 0);
    int followed = 0;
    for (uint32_t t0 = 0; t0 < 60000 && followed < 25; t0 += 32) {
        const Span now = draw(cfg, t0);
        const Span before = draw(cfg, t0 >= 32 ? t0 - 32 : 0);
        for (int i = 0; i < 144; ++i) {
            if (t0 == 0 || now[i] == kBlack || before[i] != kBlack) continue;  // want a spark born at t0
            int level = now[i].r;
            bool rehit = false;
            for (uint32_t dt = 8; dt <= 16 * 32 && !rehit; dt += 8) {
                const int next = draw(cfg, t0 + dt)[i].r;
                if (next > level) {
                    rehit = true;  // another spark landed here: not the one we follow
                } else {
                    level = next;
                }
            }
            if (rehit) continue;
            TEST_ASSERT_EQUAL_MESSAGE(0, level, "a spark must be gone after its lifetime");
            TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(48, now[i].r, "a spark starts bright");
            ++followed;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(25, followed, "not enough isolated sparks found to judge");
}

// ---- Reactive effects: Beat Pulse, VU Meter, Sparkle ----------------------------------
//
// At the level of the render function the live input is a `Reactive` value,
// so most properties can be tested by handing one in. The tests that involve
// timing (the flash on the beat, frame-rate independence, the hand-over
// between reactive and fallback) drive a whole engine with a simulated phone.

Span drawLive(const EffectConfig& cfg, uint32_t time_ms, const Reactive& live, uint16_t count = 144, uint32_t seed = 12345) {
    Span pixels(count);
    EffectState state;
    state.seed = seed;
    const Palette palette(cfg.palette, cfg.color1, cfg.color2);
    EffectContext ctx{pixels.data(), count, time_ms, cfg, palette, nullptr, state, &live};
    effectInfo(cfg.effect)->render(ctx);
    return pixels;
}

int totalLight(const Span& s) {
    int sum = 0;
    for (const Rgb& p : s) sum += brightnessOf(p);
    return sum;
}

Reactive onBeat(uint32_t beat, double phase) {
    Reactive r;
    r.beat_mix = 255;
    r.beat = (beat << 16) | static_cast<uint32_t>(phase * 65536.0);
    return r;
}

Reactive sound(uint8_t level, uint8_t bass = 128, uint8_t mid = 128, uint8_t treble = 128) {
    Reactive r;
    r.audio_mix = 255;
    r.level = level;
    r.bass = bass;
    r.mid = mid;
    r.treble = treble;
    return r;
}

Reactive moving(uint8_t motion, int8_t tilt_x = 0, int8_t tilt_y = 0) {
    Reactive r;
    r.motion_mix = 255;
    r.motion = motion;
    r.tilt_x = tilt_x;
    r.tilt_y = tilt_y;
    return r;
}

void deliver(Show& show, const sim::Frame& frame) {
    const std::vector<uint8_t> bytes = frame.bytes();
    show.engine.handleLive(bytes.data(), bytes.size(), frame.arrival_ms);
}

// "reactive effects fall back to their non-reactive behaviour": with no live
// input, or with every group's weight at 0, the picture is a function of
// effect time alone, whatever the (ignored) live values say.
void test_reactive_effects_fall_back_without_live_input() {
    Reactive ignored;
    ignored.level = 200;
    ignored.bass = 10;
    ignored.mid = 250;
    ignored.treble = 99;
    ignored.motion = 255;
    ignored.tilt_x = 100;
    ignored.tilt_y = -100;
    ignored.beat = 0x00071234;
    for (uint8_t effect : {kBeatPulse, kVuMeter, kSparkle}) {
        const EffectConfig cfg = config(effect, kRed, kBlue, 128, 1);
        for (uint32_t t : {0u, 37u, 333u, 801u, 1500u, 2048u, 3210u, 7777u}) {
            const Span alone = draw(cfg, t);
            TEST_ASSERT_TRUE_MESSAGE(alone == drawLive(cfg, t, Reactive()), effectInfo(effect)->name);
            TEST_ASSERT_TRUE_MESSAGE(alone == drawLive(cfg, t, ignored), effectInfo(effect)->name);
        }
        TEST_ASSERT_GREATER_THAN_MESSAGE(0, totalLight(draw(cfg, 333)) + totalLight(draw(cfg, 2048)), "the fallback shows something");
        bool animating = false;
        draw(cfg, 100, 144, 1, &animating);
        TEST_ASSERT_TRUE_MESSAGE(animating, "a reactive effect keeps the 60 fps path busy, live input or not");
    }
}

// Beat Pulse: "the brightest moment of every flash is the beat itself".
void test_beat_pulse_flashes_on_the_beat() {
    const EffectConfig cfg = config(kBeatPulse, kRed, kBlue, 128);
    for (uint32_t beat = 0; beat < 9; ++beat) {
        const bool accent = beat % 4 == 0;
        Span s = drawLive(cfg, 9999, onBeat(beat, 0.0));
        for (const Rgb& p : s) TEST_ASSERT_RGB(accent ? kBlue : kRed, p);  // full colour on the beat: color2 every fourth
        int last = totalLight(s);
        for (int step = 1; step < 100; ++step) {
            s = drawLive(cfg, 9999, onBeat(beat, step / 100.0));
            const int now = totalLight(s);
            TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(last, now, "the light only fades between beats");
            last = now;
            if (!accent) {
                for (const Rgb& p : s) TEST_ASSERT_RGB(s[0], p);  // an ordinary beat fades in place
            }
        }
    }
    // The accent drains from the middle outwards: the ends hold a quarter beat longer.
    const Span accent = drawLive(cfg, 0, onBeat(4, 0.2));
    TEST_ASSERT_RGB(kBlue, accent[0]);
    TEST_ASSERT_RGB(kBlue, accent[143]);
    TEST_ASSERT_LESS_THAN(brightnessOf(accent[20]), brightnessOf(accent[72]));
    for (int i = 0; i < 72; ++i) {
        TEST_ASSERT_RGB(accent[i], accent[143 - i]);
        TEST_ASSERT_GREATER_OR_EQUAL(brightnessOf(accent[i + 1]), brightnessOf(accent[i]));  // darker towards the middle
    }

    // intensity: a quarter of the beat at 0, about 5/8 at 128, nearly all of it at 255.
    TEST_ASSERT_EQUAL(0, totalLight(drawLive(config(kBeatPulse, kRed, kBlue, 0), 0, onBeat(1, 0.26))));
    TEST_ASSERT_GREATER_THAN(0, totalLight(drawLive(config(kBeatPulse, kRed, kBlue, 0), 0, onBeat(1, 0.20))));
    TEST_ASSERT_EQUAL(0, totalLight(drawLive(cfg, 0, onBeat(1, 0.64))));
    TEST_ASSERT_GREATER_THAN(0, totalLight(drawLive(cfg, 0, onBeat(1, 0.55))));
    TEST_ASSERT_GREATER_THAN(0, totalLight(drawLive(config(kBeatPulse, kRed, kBlue, 255), 0, onBeat(1, 0.93))));

    // Fully on the phone's beat, effect time (and so speed) plays no part.
    TEST_ASSERT_TRUE(drawLive(cfg, 0, onBeat(2, 0.3)) == drawLive(cfg, 777777, onBeat(2, 0.3)));

    // Fallback: 120 BPM at speed 128, the accent first.
    for (const Rgb& p : draw(cfg, 0)) TEST_ASSERT_RGB(kBlue, p);
    for (const Rgb& p : draw(cfg, 500)) TEST_ASSERT_RGB(kRed, p);
    for (const Rgb& p : draw(cfg, 1500)) TEST_ASSERT_RGB(kRed, p);
    for (const Rgb& p : draw(cfg, 2000)) TEST_ASSERT_RGB(kBlue, p);
    TEST_ASSERT_LESS_THAN(totalLight(draw(cfg, 500)), totalLight(draw(cfg, 600)));
    TEST_ASSERT_TRUE(draw(cfg, 700) == drawLive(cfg, 123, onBeat(1, 0.4)));  // the same picture, either way
}

// The whole chain: a phone at 120 BPM over a jittery, lossy link, the engine
// rendered at 60 fps. Every flash appears in the first frame after the beat:
//  - on the jacket's own beat clock, less than one frame (16.7 ms = 1/30 beat) after phase 0;
//  - against the phone's true beat (as delayed by the link's minimum latency),
//    between 15 ms early and 32 ms late: the beat clock's bound plus one frame.
void test_beat_pulse_peak_coincides_with_the_beat() {
    sim::Phone phone;
    const sim::Link link;
    const std::vector<sim::Frame> frames = sim::stream(phone, link, 0, 30000, 2027);
    Show show(config(kBeatPulse, kRed, kBlue, 128));

    size_t next = 0;
    int last_light = 0;
    int flashes = 0, accents = 0;
    uint32_t last_flash_ms = 0;
    std::string colours;
    for (int k = 0;; ++k) {
        const uint32_t t = static_cast<uint32_t>(k * 1000.0 / 60.0);
        if (t >= 30000) break;
        while (next < frames.size() && frames[next].arrival_ms <= t) deliver(show, frames[next++]);
        const Rgb middle = show.at(t).left[72];
        const int light = brightnessOf(middle);
        if (t > 4000 && light > last_light + 100) {  // a flash begins in this frame
            ++flashes;
            const bool accent = middle.b > middle.r;
            accents += accent;
            colours.push_back(accent ? 'B' : 'r');
            TEST_ASSERT_EQUAL_MESSAGE(255, show.engine.reactive().beat_mix, "fully on the phone's beat by now");
            // On the jacket's own clock: within one frame after the beat.
            TEST_ASSERT_LESS_THAN_MESSAGE(65536 / 30 + 100, show.engine.beatClock().phase(), "a flash more than a frame after the beat");
            // Against the phone.
            const double beats = phone.beatsAt(t - link.min_delay_ms);
            double since_ms = (beats - std::round(beats)) * 500.0;  // ms after (+) or before (-) the nearest true beat
            TEST_ASSERT_TRUE_MESSAGE(since_ms > -15.0 && since_ms < 32.0, "a flash off the phone's beat");
            if (last_flash_ms != 0) TEST_ASSERT_UINT32_WITHIN_MESSAGE(40, 500, t - last_flash_ms, "one flash per beat");
            last_flash_ms = t;
        }
        last_light = light;
    }
    TEST_ASSERT_INT_WITHIN(1, 52, flashes);  // 26 s at 2 beats per second
    TEST_ASSERT_INT_WITHIN(1, 13, accents);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, colours.find("rrrBrrrBrrrB"), "every fourth beat is the accent");
}

// VU Meter: "bar length monotonic in level".
void test_vu_meter_bar_follows_level() {
    const EffectConfig cfg = config(kVuMeter, kRed, kBlue, 128, 1);  // the rainbow palette: no black in it
    int last_lit = 0, last_light = 0;
    for (int level = 0; level <= 255; ++level) {
        const Span s = drawLive(cfg, 4242, sound(static_cast<uint8_t>(level)));
        const int lit = litCount(s);
        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(last_lit, lit, "a louder sound never shortens the bar");
        TEST_ASSERT_GREATER_OR_EQUAL(last_light, totalLight(s));
        for (int i = 0; i < 144; ++i) TEST_ASSERT_EQUAL_MESSAGE(i < lit, s[i] != kBlack, "the bar is one piece from pixel 0");
        TEST_ASSERT_INT_WITHIN_MESSAGE(1, (level * 144 + 254) / 255, lit, "length is proportional to level");
        last_lit = lit;
        last_light = totalLight(s);
    }
    TEST_ASSERT_EQUAL(0, litCount(drawLive(cfg, 0, sound(0))));
    TEST_ASSERT_EQUAL(144, last_lit);
    // Fully live, effect time plays no part.
    TEST_ASSERT_TRUE(drawLive(cfg, 1, sound(100, 10, 20, 30)) == drawLive(cfg, 99999, sound(100, 10, 20, 30)));
    // Any segment length.
    for (uint16_t count : {1, 2, 7, 60}) {
        TEST_ASSERT_EQUAL(count, litCount(drawLive(cfg, 0, sound(255), count)));
        TEST_ASSERT_INT_WITHIN(1, count / 2, litCount(drawLive(cfg, 0, sound(128), count)));
    }

    // intensity is the gain: 128 as it is, 255 about 1.7 times, 0 a third.
    TEST_ASSERT_INT_WITHIN(1, 72, litCount(drawLive(cfg, 0, sound(128))));
    TEST_ASSERT_INT_WITHIN(2, 120, litCount(drawLive(config(kVuMeter, kRed, kBlue, 255, 1), 0, sound(128))));
    TEST_ASSERT_INT_WITHIN(2, 24, litCount(drawLive(config(kVuMeter, kRed, kBlue, 0, 1), 0, sound(128))));

    // The palette lies along the whole segment; with every band at full the bar shows it pure.
    const Span full = drawLive(cfg, 0, sound(255, 255, 255, 255));
    const Palette rainbow(1, kRed, kBlue);
    for (int i = 0; i < 144; ++i) TEST_ASSERT_RGB(rainbow.at(static_cast<uint8_t>(i * 255 / 143)), full[i]);

    // Bass glows in the first third, treble at the far end, the mids between.
    const Span flat = drawLive(cfg, 0, sound(255, 0, 0, 0));
    const Span bassy = drawLive(cfg, 0, sound(255, 255, 0, 0));
    const Span middy = drawLive(cfg, 0, sound(255, 0, 255, 0));
    const Span trebly = drawLive(cfg, 0, sound(255, 0, 0, 255));
    for (int i = 0; i < 144; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(flat[i] != kBlack, "a silent band still shows the bar");
        const int base = brightnessOf(flat[i]);
        if (i <= 23) TEST_ASSERT_GREATER_THAN(base, brightnessOf(bassy[i]));
        if (i >= 72) TEST_ASSERT_EQUAL(base, brightnessOf(bassy[i]));
        if (i >= 121) TEST_ASSERT_GREATER_THAN(base, brightnessOf(trebly[i]));
        if (i <= 71) TEST_ASSERT_EQUAL(base, brightnessOf(trebly[i]));
        if (i >= 60 && i <= 84) TEST_ASSERT_GREATER_THAN(base, brightnessOf(middy[i]));
        if (i <= 23 || i >= 121) TEST_ASSERT_EQUAL(base, brightnessOf(middy[i]));
    }

    // Half-way between fallback and live, the bar is between the two.
    Reactive half = sound(255);
    half.audio_mix = 128;
    const int alone = litCount(draw(cfg, 4242));
    const int blended = litCount(drawLive(cfg, 4242, half));
    TEST_ASSERT_TRUE(blended > alone && blended < 144);
}

// Sparkle: "motion 0 -> fallback look", and energy that rises with motion.
void test_sparkle_follows_motion() {
    const EffectConfig cfg = config(kSparkle, kRed, kBlue, 128, 1);
    double low = 0, level_centre = 0, high = 0;
    for (uint32_t t : {100u, 1234u, 5000u, 9999u, 20011u, 31337u}) {
        const Span still = draw(cfg, t);
        TEST_ASSERT_GREATER_THAN_MESSAGE(0, totalLight(still), "the fallback is a shimmer, not darkness");
        TEST_ASSERT_TRUE_MESSAGE(still == drawLive(cfg, t, moving(0)), "standing still is the fallback look");
        TEST_ASSERT_TRUE_MESSAGE(still == drawLive(cfg, t, moving(0, 100, -100)), "even with the phone tilted");

        int last = totalLight(still);
        for (int motion = 0; motion <= 255; motion += 3) {
            const int now = totalLight(drawLive(cfg, t, moving(static_cast<uint8_t>(motion))));
            TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(last, now, "more movement never means less light");
            last = now;
        }
        const Span wild = drawLive(cfg, t, moving(255));
        TEST_ASSERT_GREATER_THAN(3 * totalLight(still), totalLight(wild));
        TEST_ASSERT_GREATER_THAN(2 * litCount(still), litCount(wild));

        // Leaning gathers the sparks towards one end.
        auto centroid = [](const Span& s) {
            double weighted = 0, sum = 0;
            for (size_t i = 0; i < s.size(); ++i) {
                weighted += i * static_cast<double>(brightnessOf(s[i]));
                sum += brightnessOf(s[i]);
            }
            return weighted / sum;
        };
        low += centroid(drawLive(cfg, t, moving(255, 0, -127))) / 6;
        level_centre += centroid(wild) / 6;
        high += centroid(drawLive(cfg, t, moving(255, 0, 127))) / 6;

        // Tipping sideways slides the colours along the palette: the same sparks, other colours.
        const Span tipped = drawLive(cfg, t, moving(255, 90, 0));
        TEST_ASSERT_TRUE(tipped != wild);
        for (int i = 0; i < 144; ++i) TEST_ASSERT_EQUAL((wild[i] != kBlack), (tipped[i] != kBlack));
    }
    TEST_ASSERT_TRUE_MESSAGE(low < level_centre - 10 && high > level_centre + 10, "tilt_y moves the centre of the sparkle");
    TEST_ASSERT_INT_WITHIN(12, 72, static_cast<int>(level_centre));

    // Seeded like Matrix: another segment, another sequence; the same seed, the same picture.
    TEST_ASSERT_TRUE(drawLive(cfg, 4321, moving(200), 144, 7) == drawLive(cfg, 4321, moving(200), 144, 7));
    TEST_ASSERT_TRUE(drawLive(cfg, 4321, moving(200), 144, 7) != drawLive(cfg, 4321, moving(200), 144, 8));
}

// Any live values, any length: deterministic, and never a write outside the span.
void test_reactive_effects_stay_inside_their_span() {
    sim::Lcg rng(4711);
    for (uint8_t effect : {kBeatPulse, kVuMeter, kSparkle}) {
        for (uint16_t count : {1, 2, 3, 5, 17, 143, 144}) {
            for (int round = 0; round < 40; ++round) {
                Reactive live;
                live.audio_mix = static_cast<uint8_t>(rng.next());
                live.motion_mix = static_cast<uint8_t>(rng.next());
                live.beat_mix = static_cast<uint8_t>(rng.next());
                live.level = static_cast<uint8_t>(rng.next());
                live.bass = static_cast<uint8_t>(rng.next());
                live.mid = static_cast<uint8_t>(rng.next());
                live.treble = static_cast<uint8_t>(rng.next());
                live.motion = static_cast<uint8_t>(rng.next());
                live.tilt_x = static_cast<int8_t>(rng.next() % 255 - 127);
                live.tilt_y = static_cast<int8_t>(rng.next() % 255 - 127);
                live.beat = rng.next() * 977u;
                const uint32_t t = rng.next() * 251u;
                const EffectConfig cfg = config(effect, kRed, kBlue, static_cast<uint8_t>(rng.next()), static_cast<uint8_t>(rng.next() % 6));

                const Rgb canary(0xAB, 0xCD, 0xEF);
                std::vector<Rgb> buffer(count + 16, canary);
                for (uint16_t i = 0; i < count; ++i) buffer[8 + i] = kBlack;
                EffectState state;
                state.seed = 99;
                const Palette palette(cfg.palette, cfg.color1, cfg.color2);
                EffectContext ctx{buffer.data() + 8, count, t, cfg, palette, nullptr, state, &live};
                effectInfo(effect)->render(ctx);
                for (int i = 0; i < 8; ++i) {
                    TEST_ASSERT_TRUE_MESSAGE(buffer[i] == canary && buffer[8 + count + i] == canary, effectInfo(effect)->name);
                }
                const Span again = drawLive(cfg, t, live, count, 99);
                TEST_ASSERT_TRUE_MESSAGE(std::equal(again.begin(), again.end(), buffer.begin() + 8), "same inputs, same picture");
            }
        }
    }
}

// "rendering at 30 fps and 120 fps gives the same picture at the same
// timestamp", with a live stream driving all three reactive effects: before
// it starts, while it runs, when it stops, through the 500 ms timeout and the
// end of the beat clock's coast.
void test_frame_rate_independence_with_live_input() {
    sim::Phone phone;
    phone.bpm = 133;
    phone.level = 190;
    phone.bass = 220;
    phone.mid = 90;
    phone.treble = 40;
    phone.motion = 170;
    phone.tilt_x = 60;
    phone.tilt_y = -80;
    const sim::Link link;
    std::vector<sim::Frame> frames = sim::stream(phone, link, 2000, 6000, 606);
    phone.level = 40;  // the music and the dancer calm down
    phone.motion = 20;
    phone.tilt_y = 90;
    const std::vector<sim::Frame> second = sim::stream(phone, link, 6000, 9000, 607);
    for (const sim::Frame& f : second) {
        if (f.arrival_ms >= frames.back().arrival_ms) frames.push_back(f);
    }

    Show fast(config(kSolid)), slow(config(kSolid)), lazy(config(kSolid));
    for (Show* show : {&fast, &slow, &lazy}) {
        show->segments({{1, 0, 144, 0}, {2, 0, 100, 1}, {3, 100, 44, 0}});
        const uint8_t trio[3] = {kBeatPulse, kVuMeter, kSparkle};
        for (uint8_t s = 0; s < 3; ++s) {
            EffectConfig cfg = config(trio[s], kRed, kBlue, static_cast<uint8_t>(60 + 70 * s), static_cast<uint8_t>(s + 1));
            cfg.speed = static_cast<uint8_t>(100 + 37 * s);
            show->set(s, cfg);
        }
        show->at(777);
    }
    TEST_ASSERT_TRUE(fast.pic == slow.pic && fast.pic == lazy.pic);

    size_t next = 0;
    int compared = 0;
    bool saw_live = false, saw_fallback_again = false;
    for (uint32_t frame = 1; frame <= 120 * 14; ++frame) {
        const uint32_t now = 777 + (frame * 1000 + 60) / 120;  // 120 fps, rounded to whole ms
        for (; next < frames.size() && frames[next].arrival_ms <= now; ++next) {
            for (Show* show : {&fast, &slow, &lazy}) deliver(*show, frames[next]);  // frames arrive when they arrive
        }
        fast.at(now);
        const Reactive& live = fast.engine.reactive();
        saw_live = saw_live || (live.audio_mix == 255 && live.motion_mix == 255 && live.beat_mix == 255);
        saw_fallback_again = saw_fallback_again || (saw_live && live.audio_mix == 0 && live.motion_mix == 0 && live.beat_mix == 0);
        if (frame % 4 != 0) continue;
        slow.at(now);  // 30 fps
        TEST_ASSERT_TRUE_MESSAGE(fast.pic == slow.pic, "120 fps and 30 fps disagree");
        if (frame % 120 == 0) {
            lazy.at(now);  // 1 fps
            TEST_ASSERT_TRUE_MESSAGE(fast.pic == lazy.pic, "120 fps and 1 fps disagree");
        }
        ++compared;
    }
    TEST_ASSERT_EQUAL(420, compared);
    TEST_ASSERT_TRUE_MESSAGE(saw_live, "the stream should have taken over");
    TEST_ASSERT_TRUE_MESSAGE(saw_fallback_again, "and let go again");
}

// "must move between reactive and fallback without a visible jump".
// VU Meter: a loud stream starts and later stops; the bar never moves by more
// than 10 pixels (7 % of the strip) from one 60 fps frame to the next, and it
// ends up exactly where an untouched jacket is.
void test_vu_meter_hands_over_smoothly() {
    sim::Phone phone;
    phone.level = 255;
    phone.bass = phone.mid = phone.treble = 255;
    phone.flags = kLiveAudioValid;
    const std::vector<sim::Frame> frames = sim::stream(phone, sim::Link(), 1000, 4000, 99);
    const EffectConfig cfg = config(kVuMeter, kRed, kBlue, 128, 1);
    Show show(cfg), untouched(cfg);

    size_t next = 0;
    int last_lit = -1, biggest_step = 0, longest = 0;
    for (int k = 0;; ++k) {
        const uint32_t t = static_cast<uint32_t>(k * 1000.0 / 60.0);
        if (t >= 6000) break;
        while (next < frames.size() && frames[next].arrival_ms <= t) deliver(show, frames[next++]);
        int lit = 0;
        const Picture& pic = show.at(t);
        for (int i = 0; i < 144; ++i) lit += pic.left[i] != kBlack;
        if (last_lit >= 0 && std::abs(lit - last_lit) > biggest_step) biggest_step = std::abs(lit - last_lit);
        if (lit > longest) longest = lit;
        last_lit = lit;
        untouched.at(t);
        if (t < 1000 || t > 5200) TEST_ASSERT_TRUE_MESSAGE(show.pic == untouched.pic, "outside the stream it is the fallback, exactly");
    }
    TEST_ASSERT_EQUAL_MESSAGE(144, longest, "the bar reached the top while the stream ran");
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(10, biggest_step, "the bar jumped");
}

// Beat Pulse: while the beat clock takes over (and when it lets go after its
// coast), every frame lies between what the fallback alone and the phone's
// beat alone would show, and the weight moves by one small step per frame.
void test_beat_pulse_hands_over_smoothly() {
    sim::Phone phone;
    phone.bpm = 97;
    phone.flags = kLiveBeatValid;
    const std::vector<sim::Frame> frames = sim::stream(phone, sim::Link(), 1500, 5000, 31);
    const EffectConfig cfg = config(kBeatPulse, kRed, kBlue, 200);
    Show show(cfg);

    auto within = [](uint8_t v, uint8_t a, uint8_t b) { return v >= std::min(a, b) && v <= std::max(a, b); };
    size_t next = 0;
    int last_mix = 0, biggest_step = 0;
    bool rose = false, fell = false;
    for (int k = 0;; ++k) {
        const uint32_t t = static_cast<uint32_t>(k * 1000.0 / 60.0);
        if (t >= 10000) break;
        while (next < frames.size() && frames[next].arrival_ms <= t) deliver(show, frames[next++]);
        const Picture& pic = show.at(t);
        Reactive live = show.engine.reactive();
        const int mix = live.beat_mix;
        if (std::abs(mix - last_mix) > biggest_step) biggest_step = std::abs(mix - last_mix);
        rose = rose || mix == 255;
        fell = fell || (rose && mix == 0);
        last_mix = mix;

        // Effect time is real time here: speed 128, first rendered at t = 0.
        const Span own = drawLive(cfg, t, Reactive());
        live.beat_mix = 255;
        const Span led = drawLive(cfg, t, live);
        for (int i = 0; i < 144; ++i) {
            const Rgb p = pic.left[i];
            if (mix == 0) TEST_ASSERT_RGB(own[i], p);
            if (mix == 255) TEST_ASSERT_RGB(led[i], p);
            TEST_ASSERT_TRUE_MESSAGE(within(p.r, own[i].r, led[i].r) && within(p.g, own[i].g, led[i].g) && within(p.b, own[i].b, led[i].b),
                                     "a frame outside the blend of fallback and beat");
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(rose && fell, "the beat should have taken over and let go");
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(15, biggest_step, "the weight moves 255 / 300 per millisecond: 14 or 15 per frame");
}

// ---- Snapshot (regression guard only) ---------------------------------------------------

uint32_t pictureHash(uint8_t effect) {
    // FNV-1a over a few frames of a fixed scene.
    uint32_t h = 2166136261u;
    for (uint32_t t : {0u, 333u, 1000u, 2500u, 7000u}) {
        for (uint16_t count : {144, 37}) {
            for (const Rgb& p : draw(config(effect, Rgb(0, 255, 204), Rgb(255, 0, 255), 128, effect % 3), t, count, 2024)) {
                for (uint8_t byte : {p.r, p.g, p.b}) {
                    h ^= byte;
                    h *= 16777619u;
                }
            }
        }
    }
    return h;
}

// NOT a correctness test: the expected values were recorded from this
// implementation. A failure means "the look of this effect changed"; if that
// was intended, replace the value with the one printed.
void test_snapshot_regression_guard() {
    const uint32_t recorded[kEffectIds] = {
        0x472CD41Cu,  // Solid
        0xEC485B2Fu,  // Comet
        0x7BF9DA9Cu,  // Rainbow
        0x0D1B28BDu,  // Matrix
        0x0559AE3Bu,  // Breathe
        0x93264877u,  // Heartbeat
        0x0CE8E94Du,  // Cylon
        0x9E0FC462u,  // Beat Pulse (its fallback: no live input here)
        0x803E0489u,  // VU Meter (fallback)
        0xFC74D509u,  // Sparkle (fallback)
    };
    int changed = 0;
    for (uint8_t effect = 0; effect < kEffectIds; ++effect) {
        const uint32_t now = pictureHash(effect);
        if (now != recorded[effect]) {
            ++changed;
            printf("SNAPSHOT CHANGED  %s: recorded 0x%08Xu, now 0x%08Xu\n", effectInfo(effect)->name,
                   static_cast<unsigned>(recorded[effect]), static_cast<unsigned>(now));
        }
    }
    TEST_ASSERT_EQUAL_MESSAGE(0, changed, "effect output differs from the recorded snapshots (see above)");
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_registry_matches_protocol);
    RUN_TEST(test_speed_multiplier_follows_the_formula);
    RUN_TEST(test_speed_scales_time_for_every_effect);
    RUN_TEST(test_frame_rate_independence);
    RUN_TEST(test_rendering_is_idempotent_at_a_fixed_time);
    RUN_TEST(test_param_flags_are_honest);
    RUN_TEST(test_any_length_and_no_overrun);
    RUN_TEST(test_solid_is_color1_and_static);
    RUN_TEST(test_comet_head_position);
    RUN_TEST(test_comet_tail);
    RUN_TEST(test_comet_moves_smoothly);
    RUN_TEST(test_comet_colours);
    RUN_TEST(test_cylon_bounces);
    RUN_TEST(test_cylon_trail_is_behind_the_eye);
    RUN_TEST(test_cylon_colour_follows_position);
    RUN_TEST(test_rainbow);
    RUN_TEST(test_breathe);
    RUN_TEST(test_heartbeat);
    RUN_TEST(test_matrix_is_deterministic);
    RUN_TEST(test_matrix_segments_have_their_own_sequence);
    RUN_TEST(test_matrix_colours_come_from_the_palette);
    RUN_TEST(test_matrix_density_follows_intensity);
    RUN_TEST(test_matrix_sparks_fade_out);
    RUN_TEST(test_reactive_effects_fall_back_without_live_input);
    RUN_TEST(test_beat_pulse_flashes_on_the_beat);
    RUN_TEST(test_beat_pulse_peak_coincides_with_the_beat);
    RUN_TEST(test_vu_meter_bar_follows_level);
    RUN_TEST(test_sparkle_follows_motion);
    RUN_TEST(test_reactive_effects_stay_inside_their_span);
    RUN_TEST(test_frame_rate_independence_with_live_input);
    RUN_TEST(test_vu_meter_hands_over_smoothly);
    RUN_TEST(test_beat_pulse_hands_over_smoothly);
    RUN_TEST(test_snapshot_regression_guard);
    return UNITY_END();
}
