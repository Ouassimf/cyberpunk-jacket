// Engine behaviour, driven the way device code drives it: raw command frames
// in, reply frames and STATE out, pixels rendered into two buffers.
//
// Commands are written as hex by hand from PROTOCOL.md rather than produced
// with the library's own encoder, so a codec bug cannot hide an engine bug.
// Where PROTOCOL.md is silent the test pins the behaviour documented in
// jacket/engine.h and says so with "decision:".

#include <cstdlib>
#include <string>

#include "../support/test_support.h"

using namespace jacket;
using support::Bytes;
using support::CaptureSink;
using support::Picture;
using support::fromHex;

void setUp() {}
void tearDown() {}

namespace {

constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kGreen{0, 255, 0};
constexpr Rgb kBlue{0, 0, 255};
constexpr Rgb kCyan{0, 255, 204};
constexpr Rgb kMagenta{255, 0, 255};

enum : uint8_t { kSolid = 0, kComet = 1, kRainbow = 2, kMatrix = 3, kBreathe = 4, kHeartbeat = 5, kCylon = 6 };

struct Geom {
    uint8_t mask, start, length, flags;
};

// An engine with its storage, a clock and the frames it replied with.
struct Rig {
    explicit Rig(MemoryStorage& shared) : engine(shared) { engine.begin(); }
    explicit Rig(const EngineConfig& config) : engine(own, config) { engine.begin(); }
    Rig() : engine(own) { engine.begin(); }

    MemoryStorage own;
    Engine engine;
    CaptureSink sink;
    uint32_t now = 1000;
    Picture pic;
    bool animating = false;

    ErrorCode send(const Bytes& frame) { return engine.handleCommand(frame.data(), frame.size(), now, sink); }
    ErrorCode send(const std::string& hex) { return send(fromHex(hex)); }

    ErrorCode setEffect(uint8_t segment, uint8_t effect, Rgb c1 = kCyan, Rgb c2 = kMagenta, uint8_t speed = 128,
                        uint8_t intensity = 128, uint8_t palette = 0) {
        return send(Bytes{0x03, segment, effect, palette, speed, intensity, c1.r, c1.g, c1.b, c2.r, c2.g, c2.b});
    }
    ErrorCode setSegments(std::initializer_list<Geom> list) {
        Bytes frame{0x05, static_cast<uint8_t>(list.size())};
        for (const Geom& g : list) frame.insert(frame.end(), {g.mask, g.start, g.length, g.flags});
        return send(frame);
    }

    StateMsg state() {
        uint8_t buf[kMaxFrame];
        const size_t n = engine.encodeState(buf, sizeof buf);
        StateMsg msg;
        TEST_ASSERT_EQUAL_MESSAGE(0, static_cast<int>(decodeState(buf, n, msg)), "engine produced an undecodable STATE");
        return msg;
    }
    std::string lastReply() { return sink.frames.empty() ? std::string("(none)") : support::toHex(sink.frames.back()); }

    const Picture& render(uint32_t at) {
        now = at;
        animating = engine.render(at, pic.left, pic.right);
        return pic;
    }
};

#define ASSERT_OK(call) TEST_ASSERT_EQUAL_MESSAGE(0, static_cast<int>(call), #call)
#define ASSERT_ERR(code, call) TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(code), static_cast<int>(call), #call)

// ---- State and commands -----------------------------------------------------------

void test_defaults() {
    Rig rig;
    const StateMsg s = rig.state();
    TEST_ASSERT_EQUAL(1, s.proto);
    TEST_ASSERT_EQUAL(0, s.rev);
    TEST_ASSERT_EQUAL(1, s.power);
    TEST_ASSERT_EQUAL(900, s.budget_ma);  // "Default 900"
    TEST_ASSERT_EQUAL(0, s.flags);
    TEST_ASSERT_EQUAL(0xFF, s.preset);
    TEST_ASSERT_EQUAL(1, s.segment_count);
    TEST_ASSERT_EQUAL(3, s.segments[0].geometry.strip_mask);
    TEST_ASSERT_EQUAL(0, s.segments[0].geometry.start);
    TEST_ASSERT_EQUAL(144, s.segments[0].geometry.length);
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());
    TEST_ASSERT_FALSE(rig.engine.needsSave());
}

void test_state_frame_layout() {
    // Byte offsets straight from the STATE table in PROTOCOL.md.
    Rig rig;
    ASSERT_OK(rig.send("02c8"));    // brightness 200
    ASSERT_OK(rig.send("30dc05"));  // budget 1500
    ASSERT_OK(rig.send("3101"));    // dev mode
    ASSERT_OK(rig.setSegments({{1, 0, 100, 0}, {2, 44, 100, 1}}));
    ASSERT_OK(rig.setEffect(1, kComet, kRed, kBlue, 10, 20, 3));
    uint8_t f[kMaxFrame];
    const size_t n = rig.engine.encodeState(f, sizeof f);
    TEST_ASSERT_EQUAL(11 + 2 * 14, n);
    TEST_ASSERT_EQUAL_HEX8(0x01, f[0]);
    TEST_ASSERT_EQUAL(1, f[1]);
    TEST_ASSERT_EQUAL(5, f[2] | (f[3] << 8));  // five accepted commands
    TEST_ASSERT_EQUAL(1, f[4]);
    TEST_ASSERT_EQUAL(200, f[5]);
    TEST_ASSERT_EQUAL(1500, f[6] | (f[7] << 8));
    TEST_ASSERT_EQUAL_HEX8(0x01, f[8]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, f[9]);
    TEST_ASSERT_EQUAL(2, f[10]);
    const uint8_t seg1[14] = {2, 44, 100, 1, kComet, 3, 10, 20, 255, 0, 0, 0, 0, 255};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(seg1, f + 11 + 14, 14);
}

void test_set_power() {
    Rig rig;
    ASSERT_OK(rig.send("0100"));
    TEST_ASSERT_EQUAL(0, rig.state().power);
    TEST_ASSERT_EQUAL(1, rig.state().rev);
    TEST_ASSERT_FALSE(rig.engine.power());
    ASSERT_OK(rig.send("0101"));
    TEST_ASSERT_EQUAL(1, rig.state().power);
    TEST_ASSERT_EQUAL(2, rig.state().rev);
    TEST_ASSERT_EQUAL_MESSAGE(0, rig.sink.frames.size(), "changes are announced through STATE, not a reply frame");
}

void test_set_brightness() {
    Rig rig;
    ASSERT_OK(rig.send("0240"));
    TEST_ASSERT_EQUAL(64, rig.state().brightness);
    TEST_ASSERT_EQUAL(64, rig.engine.brightness());
    TEST_ASSERT_EQUAL(1, rig.state().rev);
}

void test_set_effect() {
    Rig rig;
    ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 0}}));
    ASSERT_OK(rig.setEffect(1, kBreathe, kRed, kGreen, 77, 99, 2));
    StateMsg s = rig.state();
    TEST_ASSERT_EQUAL(2, s.rev);
    TEST_ASSERT_EQUAL(kBreathe, s.segments[1].config.effect);
    TEST_ASSERT_EQUAL(2, s.segments[1].config.palette);
    TEST_ASSERT_EQUAL(77, s.segments[1].config.speed);
    TEST_ASSERT_EQUAL(99, s.segments[1].config.intensity);
    TEST_ASSERT_RGB(kRed, s.segments[1].config.color1);
    TEST_ASSERT_RGB(kGreen, s.segments[1].config.color2);
    TEST_ASSERT_EQUAL_MESSAGE(kCylon, s.segments[0].config.effect, "the other segment must not change");

    // "segment = 0xFF means every segment"
    ASSERT_OK(rig.setEffect(0xFF, kSolid, kBlue));
    s = rig.state();
    TEST_ASSERT_EQUAL(kSolid, s.segments[0].config.effect);
    TEST_ASSERT_EQUAL(kSolid, s.segments[1].config.effect);
    TEST_ASSERT_RGB(kBlue, s.segments[0].config.color1);
    TEST_ASSERT_RGB(kBlue, s.segments[1].config.color1);
}

void test_set_effect_rejections() {
    Rig rig;
    const StateMsg before = rig.state();
    // decision: ids that do not exist are "value out of range" (3).
    ASSERT_ERR(ErrorCode::OutOfRange, rig.setEffect(0, 10));
    TEST_ASSERT_EQUAL_STRING("7f0303", rig.lastReply().c_str());
    ASSERT_ERR(ErrorCode::OutOfRange, rig.setEffect(0, kSolid, kRed, kRed, 128, 128, 6));  // palette 6 of 0..5
    ASSERT_ERR(ErrorCode::OutOfRange, rig.setEffect(1, kSolid));                           // only segment 0 exists
    ASSERT_ERR(ErrorCode::OutOfRange, rig.send("040101" "00"));                            // SET_PARAM on segment 1
    ASSERT_ERR(ErrorCode::OutOfRange, rig.send("040001" "63"));                            // effect 99
    ASSERT_ERR(ErrorCode::OutOfRange, rig.send("040002" "63"));                            // palette 99
    TEST_ASSERT_EQUAL(6, rig.sink.frames.size());

    const StateMsg after = rig.state();
    TEST_ASSERT_EQUAL_MESSAGE(before.rev, after.rev, "a rejected command must not bump rev");
    TEST_ASSERT_TRUE(after.segments[0].config == before.segments[0].config);
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());
}

void test_set_param() {
    Rig rig;
    ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 0}}));
    ASSERT_OK(rig.send("040001" "04"));      // effect = Breathe
    ASSERT_OK(rig.send("040002" "03"));      // palette = 3
    ASSERT_OK(rig.send("040003" "c8"));      // speed = 200
    ASSERT_OK(rig.send("040004" "11"));      // intensity = 17
    ASSERT_OK(rig.send("040005" "ff0080"));  // color1
    ASSERT_OK(rig.send("040006" "010203"));  // color2
    StateMsg s = rig.state();
    TEST_ASSERT_EQUAL(7, s.rev);
    const EffectConfig& c = s.segments[0].config;
    TEST_ASSERT_EQUAL(kBreathe, c.effect);
    TEST_ASSERT_EQUAL(3, c.palette);
    TEST_ASSERT_EQUAL(200, c.speed);
    TEST_ASSERT_EQUAL(17, c.intensity);
    TEST_ASSERT_RGB(Rgb(255, 0, 128), c.color1);
    TEST_ASSERT_RGB(Rgb(1, 2, 3), c.color2);
    TEST_ASSERT_EQUAL_MESSAGE(kCylon, s.segments[1].config.effect, "segment 1 was not addressed");
    TEST_ASSERT_EQUAL(128, s.segments[1].config.speed);

    ASSERT_OK(rig.send("04ff03" "20"));  // speed on every segment
    s = rig.state();
    TEST_ASSERT_EQUAL(0x20, s.segments[0].config.speed);
    TEST_ASSERT_EQUAL(0x20, s.segments[1].config.speed);
    TEST_ASSERT_EQUAL_MESSAGE(17, s.segments[0].config.intensity, "one parameter only");
}

// "SET_SEGMENTS replaces the layout. Segment i keeps the EffectConfig it had;
// new segments copy segment 0."
void test_set_segments_inheritance() {
    Rig rig;
    ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 0}}));
    ASSERT_OK(rig.setEffect(0, kComet, kRed, kGreen, 10, 20, 1));
    ASSERT_OK(rig.setEffect(1, kBreathe, kBlue, kCyan, 30, 40, 2));
    const EffectConfig zero = rig.state().segments[0].config;
    const EffectConfig one = rig.state().segments[1].config;
    TEST_ASSERT_FALSE(zero == one);

    ASSERT_OK(rig.setSegments({{3, 0, 36, 0}, {3, 36, 36, 1}, {1, 72, 72, 0}, {2, 72, 72, 1}}));
    StateMsg s = rig.state();
    TEST_ASSERT_EQUAL(4, s.segment_count);
    TEST_ASSERT_TRUE_MESSAGE(s.segments[0].config == zero, "segment 0 keeps its config");
    TEST_ASSERT_TRUE_MESSAGE(s.segments[1].config == one, "segment 1 keeps its config");
    TEST_ASSERT_TRUE_MESSAGE(s.segments[2].config == zero, "new segment 2 copies segment 0");
    TEST_ASSERT_TRUE_MESSAGE(s.segments[3].config == zero, "new segment 3 copies segment 0");
    TEST_ASSERT_EQUAL(36, s.segments[1].geometry.start);
    TEST_ASSERT_EQUAL(1, s.segments[1].geometry.flags);
    TEST_ASSERT_EQUAL(2, s.segments[3].geometry.strip_mask);

    // Shrink, then grow again: the dropped segment's config is gone.
    ASSERT_OK(rig.setSegments({{3, 0, 144, 0}}));
    TEST_ASSERT_EQUAL(1, rig.state().segment_count);
    TEST_ASSERT_TRUE(rig.state().segments[0].config == zero);
    ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 0}}));
    TEST_ASSERT_TRUE_MESSAGE(rig.state().segments[1].config == zero, "a re-created segment copies segment 0");

    // An invalid layout changes nothing.
    const uint16_t rev = rig.state().rev;
    ASSERT_ERR(ErrorCode::OutOfRange, rig.setSegments({{1, 0, 144, 0}, {2, 100, 100, 0}}));
    TEST_ASSERT_EQUAL(2, rig.state().segment_count);
    TEST_ASSERT_EQUAL(rev, rig.state().rev);
}

// "clamped to 400...2000 mA"
void test_budget_is_clamped() {
    Rig rig;
    ASSERT_OK(rig.send("306400"));  // 100
    TEST_ASSERT_EQUAL(400, rig.state().budget_ma);
    TEST_ASSERT_EQUAL(400, rig.engine.budgetMa());
    ASSERT_OK(rig.send("308813"));  // 5000
    TEST_ASSERT_EQUAL(2000, rig.state().budget_ma);
    ASSERT_OK(rig.send("309001"));  // 400
    TEST_ASSERT_EQUAL(400, rig.state().budget_ma);
    ASSERT_OK(rig.send("30d007"));  // 2000
    TEST_ASSERT_EQUAL(2000, rig.state().budget_ma);
    ASSERT_OK(rig.send("30b004"));  // 1200
    TEST_ASSERT_EQUAL(1200, rig.state().budget_ma);
    TEST_ASSERT_EQUAL(5, rig.state().rev);
    TEST_ASSERT_EQUAL_MESSAGE(0, rig.sink.frames.size(), "clamping is not an error");
}

void test_flags() {
    Rig rig;
    ASSERT_OK(rig.send("3101"));
    TEST_ASSERT_EQUAL_HEX8(0x01, rig.state().flags);
    TEST_ASSERT_TRUE(rig.engine.devMode());
    TEST_ASSERT_EQUAL(1, rig.state().rev);

    rig.engine.setWifiConnected(true);
    TEST_ASSERT_EQUAL_HEX8(0x03, rig.state().flags);
    TEST_ASSERT_EQUAL_MESSAGE(2, rig.state().rev, "a flag change is a change");
    TEST_ASSERT_TRUE(rig.engine.takeStateChanged());
    rig.engine.setWifiConnected(true);
    TEST_ASSERT_EQUAL_MESSAGE(2, rig.state().rev, "setting the same status again is not a change");
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());

    ASSERT_OK(rig.send("3100"));
    rig.engine.setWifiConnected(false);
    TEST_ASSERT_EQUAL_HEX8(0x00, rig.state().flags);
}

void test_state_change_is_coalesced() {
    Rig rig;
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());
    ASSERT_OK(rig.send("0240"));
    ASSERT_OK(rig.send("0241"));
    ASSERT_OK(rig.send("0242"));
    TEST_ASSERT_TRUE(rig.engine.takeStateChanged());
    TEST_ASSERT_FALSE_MESSAGE(rig.engine.takeStateChanged(), "one notification covers the burst");
    TEST_ASSERT_EQUAL(3, rig.state().rev);
    TEST_ASSERT_EQUAL(0x42, rig.state().brightness);
}

void test_rev_wraps() {
    Rig rig;
    for (int i = 0; i < 65535; ++i) rig.send(i & 1 ? "0101" : "0100");
    TEST_ASSERT_EQUAL(65535, rig.state().rev);
    ASSERT_OK(rig.send("0101"));
    TEST_ASSERT_EQUAL(0, rig.state().rev);
}

void test_error_replies() {
    Rig rig;
    ASSERT_ERR(ErrorCode::UnknownOpcode, rig.send("42"));
    TEST_ASSERT_EQUAL_STRING("7f4201", rig.lastReply().c_str());  // the golden ERROR frame
    ASSERT_ERR(ErrorCode::TooShort, rig.send("03ff0100"));
    TEST_ASSERT_EQUAL_STRING("7f0302", rig.lastReply().c_str());
    ASSERT_ERR(ErrorCode::OutOfRange, rig.send("0105"));
    TEST_ASSERT_EQUAL_STRING("7f0103", rig.lastReply().c_str());
    ASSERT_ERR(ErrorCode::NotFound, rig.send("1103"));
    TEST_ASSERT_EQUAL_STRING("7f1104", rig.lastReply().c_str());
    // decision: an empty write is reported as opcode 0, too short.
    ASSERT_ERR(ErrorCode::TooShort, rig.send(Bytes{}));
    TEST_ASSERT_EQUAL_STRING("7f0002", rig.lastReply().c_str());

    TEST_ASSERT_EQUAL(5, rig.sink.frames.size());
    TEST_ASSERT_EQUAL_MESSAGE(0, rig.state().rev, "errors change nothing");
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());
    TEST_ASSERT_FALSE(rig.engine.needsSave());
}

// ---- GET_* --------------------------------------------------------------------------

void test_get_info() {
    MemoryStorage storage;
    EngineConfig config;
    config.fw_major = 3;
    config.fw_minor = 4;
    config.fw_patch = 5;
    config.build = "a1b2c3d";
    Engine engine(storage, config);
    CaptureSink sink;
    const uint8_t get_info[] = {0x20};
    ASSERT_OK(engine.handleCommand(get_info, 1, 0, sink));
    TEST_ASSERT_EQUAL(1, sink.frames.size());
    InfoMsg info;
    ASSERT_OK(decodeInfo(sink.frames[0].data(), sink.frames[0].size(), info));
    TEST_ASSERT_EQUAL(1, info.proto);
    TEST_ASSERT_EQUAL(3, info.fw_major);
    TEST_ASSERT_EQUAL(4, info.fw_minor);
    TEST_ASSERT_EQUAL(5, info.fw_patch);
    TEST_ASSERT_EQUAL(2, info.strip_count);
    TEST_ASSERT_EQUAL(144, info.leds[0]);
    TEST_ASSERT_EQUAL(144, info.leds[1]);
    TEST_ASSERT_EQUAL(8, info.max_segments);
    TEST_ASSERT_EQUAL(8, info.preset_slots);
    TEST_ASSERT_EQUAL(10, info.effect_count);
    TEST_ASSERT_GREATER_OR_EQUAL(6, info.palette_count);
    TEST_ASSERT_TRUE(info.build == strView("a1b2c3d"));
    TEST_ASSERT_EQUAL_MESSAGE(0, engine.rev(), "reading does not change state");
}

// Pages through a catalogue exactly as PROTOCOL.md tells the app to:
// "asks again with start + count until it has total entries".
std::vector<std::pair<std::string, uint8_t>> fetchCatalog(Rig& rig, uint8_t kind, int& pages) {
    std::vector<std::pair<std::string, uint8_t>> entries;
    uint8_t start = 0;
    pages = 0;
    for (;;) {
        rig.sink.frames.clear();
        TEST_ASSERT_EQUAL(0, static_cast<int>(rig.send(Bytes{0x21, kind, start})));
        TEST_ASSERT_EQUAL_MESSAGE(1, rig.sink.frames.size(), "GET_* answers with one notification");
        const Bytes& frame = rig.sink.frames[0];
        TEST_ASSERT_LESS_OR_EQUAL(kMaxFrame, frame.size());
        CatalogReader reader;
        TEST_ASSERT_EQUAL(0, static_cast<int>(reader.begin(frame.data(), frame.size())));
        TEST_ASSERT_EQUAL(kind, reader.kind);
        TEST_ASSERT_EQUAL(start, reader.start);
        ++pages;
        for (uint8_t i = 0; i < reader.count; ++i) {
            CatalogEntry e;
            TEST_ASSERT_EQUAL(0, static_cast<int>(reader.next(e)));
            TEST_ASSERT_EQUAL_MESSAGE(entries.size(), e.id, "ids are consecutive from 0");
            entries.emplace_back(std::string(e.name.data, e.name.len), e.param_flags);
        }
        if (entries.size() >= reader.total) break;
        TEST_ASSERT_GREATER_THAN_MESSAGE(0, reader.count, "an unfinished catalogue must make progress");
        start = static_cast<uint8_t>(start + reader.count);
    }
    return entries;
}

void test_effect_catalogue() {
    Rig rig;
    int pages = 0;
    const auto effects = fetchCatalog(rig, 0, pages);
    // "Catalogue ids (v1 firmware)" in PROTOCOL.md.
    const char* const names[] = {"Solid",     "Comet", "Rainbow",    "Matrix",   "Breathe",
                                 "Heartbeat", "Cylon", "Beat Pulse", "VU Meter", "Sparkle"};
    // "bit 5 reacts to sound, bit 6 reacts to motion": ids 0..6 do neither.
    const uint8_t reacts[] = {0, 0, 0, 0, 0, 0, 0, 0x20, 0x20, 0x40};
    TEST_ASSERT_EQUAL(10, effects.size());
    TEST_ASSERT_EQUAL_MESSAGE(1, pages, "ten effects still fit one 180-byte page");
    for (size_t i = 0; i < 10; ++i) {
        TEST_ASSERT_EQUAL_STRING(names[i], effects[i].first.c_str());
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(reacts[i], effects[i].second & 0xE0, names[i]);  // and bit 7 is undefined
    }
    TEST_ASSERT_EQUAL_MESSAGE(0, rig.state().rev, "reading does not change state");
}

void test_palette_catalogue() {
    Rig rig;
    int pages = 0;
    const auto palettes = fetchCatalog(rig, 1, pages);
    TEST_ASSERT_GREATER_OR_EQUAL(6, palettes.size());
    TEST_ASSERT_EQUAL_STRING("Colours", palettes[0].first.c_str());  // "Palette 0 is always Colours"
    const char* const wanted[] = {"Rainbow", "Matrix", "Cyberpunk", "Fire", "Ocean"};
    for (const char* name : wanted) {
        bool found = false;
        for (const auto& p : palettes) found = found || p.first == name;
        TEST_ASSERT_TRUE_MESSAGE(found, name);
    }
}

void test_catalogue_start_offsets() {
    Rig rig;
    ASSERT_OK(rig.send("210005"));  // effects from 5
    CatalogReader reader;
    ASSERT_OK(reader.begin(rig.sink.frames[0].data(), rig.sink.frames[0].size()));
    TEST_ASSERT_EQUAL(5, reader.start);
    TEST_ASSERT_EQUAL(10, reader.total);
    TEST_ASSERT_EQUAL(5, reader.count);
    CatalogEntry e;
    ASSERT_OK(reader.next(e));
    TEST_ASSERT_EQUAL(5, e.id);
    TEST_ASSERT_TRUE(e.name == strView("Heartbeat"));

    // decision: a start at or past the end is an empty page, not an error.
    rig.sink.frames.clear();
    ASSERT_OK(rig.send("21000a"));
    TEST_ASSERT_EQUAL_STRING("02000a0a00", rig.lastReply().c_str());
    ASSERT_OK(rig.send("2100c8"));
    TEST_ASSERT_EQUAL_STRING("0200c80a00", rig.lastReply().c_str());
}

// ---- Rendering ------------------------------------------------------------------------

void test_power_off_renders_black_and_static() {
    Rig rig;
    ASSERT_OK(rig.setEffect(0, kComet));
    rig.render(1000);
    rig.render(1500);
    TEST_ASSERT_TRUE(rig.animating);
    TEST_ASSERT_FALSE(rig.pic.allBlack());

    ASSERT_OK(rig.send("0100"));
    TEST_ASSERT_TRUE_MESSAGE(rig.engine.needsRender(), "the strips must be blanked once");
    rig.render(1600);
    TEST_ASSERT_TRUE(rig.pic.allBlack());
    TEST_ASSERT_FALSE_MESSAGE(rig.animating, "nothing changes while off");
    TEST_ASSERT_FALSE(rig.engine.needsRender());
    rig.render(5000);
    TEST_ASSERT_TRUE(rig.pic.allBlack());

    ASSERT_OK(rig.send("0101"));
    TEST_ASSERT_TRUE(rig.engine.needsRender());
    rig.render(5100);
    TEST_ASSERT_FALSE(rig.pic.allBlack());
    TEST_ASSERT_TRUE(rig.animating);
}

void test_static_scene_stops_refreshing() {
    Rig rig;
    ASSERT_OK(rig.setEffect(0, kSolid, kRed));
    TEST_ASSERT_TRUE(rig.engine.needsRender());
    rig.render(1000);
    TEST_ASSERT_FALSE(rig.animating);
    TEST_ASSERT_FALSE(rig.engine.needsRender());
    for (uint16_t i = 0; i < 144; ++i) {
        TEST_ASSERT_RGB(kRed, rig.pic.left[i]);
        TEST_ASSERT_RGB(kRed, rig.pic.right[i]);
    }
    // Brightness is applied by the output layer: pixels stay full range, but a
    // new frame must be pushed.
    ASSERT_OK(rig.send("0210"));
    TEST_ASSERT_TRUE(rig.engine.needsRender());
    rig.render(1100);
    TEST_ASSERT_RGB(kRed, rig.pic.left[0]);
    TEST_ASSERT_EQUAL(16, rig.engine.brightness());
    // So must a budget change.
    ASSERT_OK(rig.send("302c01"));
    TEST_ASSERT_TRUE(rig.engine.needsRender());
}

void test_segment_strip_mask_and_range() {
    Rig rig;
    ASSERT_OK(rig.setSegments({{1, 10, 20, 0}, {2, 100, 44, 0}}));
    ASSERT_OK(rig.setEffect(0, kSolid, kRed));
    ASSERT_OK(rig.setEffect(1, kSolid, kGreen));
    rig.render(1000);
    for (uint16_t i = 0; i < 144; ++i) {
        TEST_ASSERT_RGB(i >= 10 && i < 30 ? kRed : kBlack, rig.pic.left[i]);
        TEST_ASSERT_RGB(i >= 100 ? kGreen : kBlack, rig.pic.right[i]);
    }
    // Both bits: "the same pixels on both strips".
    ASSERT_OK(rig.setSegments({{3, 50, 5, 0}}));
    rig.render(1100);
    for (uint16_t i = 0; i < 144; ++i) {
        TEST_ASSERT_RGB(i >= 50 && i < 55 ? kRed : kBlack, rig.pic.left[i]);
        TEST_ASSERT_RGB(rig.pic.left[i], rig.pic.right[i]);
    }
}

// "Where segments overlap, the higher index wins."
void test_overlap_higher_index_wins() {
    Rig rig;
    ASSERT_OK(rig.setSegments({{3, 0, 100, 0}, {3, 10, 20, 0}, {1, 20, 30, 0}}));
    ASSERT_OK(rig.setEffect(0, kSolid, kRed));
    ASSERT_OK(rig.setEffect(1, kSolid, kGreen));
    ASSERT_OK(rig.setEffect(2, kSolid, kBlue));
    rig.render(1000);
    for (uint16_t i = 0; i < 144; ++i) {
        const Rgb left = i < 10 ? kRed : i < 20 ? kGreen : i < 50 ? kBlue : i < 100 ? kRed : kBlack;
        const Rgb right = i < 10 ? kRed : i < 30 ? kGreen : i < 100 ? kRed : kBlack;
        TEST_ASSERT_RGB(left, rig.pic.left[i]);
        TEST_ASSERT_RGB(right, rig.pic.right[i]);
    }
    // A higher segment wins with its dark pixels too: a comet's black
    // background hides the solid segment underneath.
    ASSERT_OK(rig.setSegments({{3, 0, 144, 0}, {3, 0, 144, 0}}));
    ASSERT_OK(rig.setEffect(1, kComet, kGreen, kGreen, 128, 0));
    rig.render(2000);
    rig.render(2000 + 1600);  // head at pixel 100, one-pixel tail
    TEST_ASSERT_RGB(kGreen, rig.pic.left[100]);
    TEST_ASSERT_RGB(kBlack, rig.pic.left[50]);
}

void test_reversed_segment_is_mirrored() {
    // The same effect at the same moment, plain on the left strip and
    // reversed on the right: the right strip must be the mirror image.
    Rig rig;
    ASSERT_OK(rig.setSegments({{1, 20, 100, 0}, {2, 20, 100, 1}}));
    ASSERT_OK(rig.setEffect(0xFF, kComet, kRed, kBlue, 128, 200));
    rig.render(1000);
    rig.render(1700);
    int lit = 0;
    for (uint16_t j = 0; j < 100; ++j) {
        TEST_ASSERT_RGB(rig.pic.left[20 + j], rig.pic.right[20 + 99 - j]);
        if (rig.pic.left[20 + j] != kBlack) ++lit;
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(10, lit, "the comet should be visible");
    // And it is not symmetric, so the mirror is a real one.
    TEST_ASSERT_TRUE(rig.pic.left[20 + 40] != rig.pic.left[20 + 59]);
    for (uint16_t i = 0; i < 20; ++i) TEST_ASSERT_RGB(kBlack, rig.pic.right[i]);
}

void test_effect_restarts_when_effect_changes() {
    Rig rig;
    ASSERT_OK(rig.setEffect(0, kComet, kRed, kRed, 128, 0));  // one-pixel comet
    rig.render(1000);
    TEST_ASSERT_RGB(kRed, rig.pic.left[0]);  // effect time starts at 0: head on pixel 0
    rig.render(1000 + 800);
    TEST_ASSERT_RGB(kRed, rig.pic.left[50]);  // 800 ms * 62.5 px/s
    TEST_ASSERT_RGB(kBlack, rig.pic.left[0]);

    // Changing a parameter does not restart it ...
    ASSERT_OK(rig.send("040005" "00ff00"));
    rig.render(1000 + 1600);
    TEST_ASSERT_RGB(kGreen, rig.pic.left[100]);

    // ... changing the effect does, even back to the same one.
    ASSERT_OK(rig.send("040001" "00"));
    rig.render(3000);
    ASSERT_OK(rig.send("040001" "01"));
    rig.render(9000);
    TEST_ASSERT_RGB(kGreen, rig.pic.left[0]);
    TEST_ASSERT_RGB(kBlack, rig.pic.left[100]);
    rig.render(9000 + 160);
    TEST_ASSERT_RGB(kGreen, rig.pic.left[10]);
}

// decision: effect time stands still while the jacket is off.
void test_effect_time_pauses_while_off() {
    Rig rig;
    ASSERT_OK(rig.setEffect(0, kComet, kRed, kRed, 128, 0));
    rig.render(1000);
    rig.render(1000 + 800);
    TEST_ASSERT_RGB(kRed, rig.pic.left[50]);
    ASSERT_OK(rig.send("0100"));
    rig.render(2000);
    ASSERT_OK(rig.send("0101"));
    rig.render(60000);
    TEST_ASSERT_RGB(kRed, rig.pic.left[50]);
    rig.render(60000 + 160);
    TEST_ASSERT_RGB(kRed, rig.pic.left[60]);
}

// ---- IDENTIFY -------------------------------------------------------------------------

int visibleLevel(Rgb pixel, uint8_t brightness) { return (pixel.r * brightness + 127) / 255; }

void test_identify_overlay_and_timeout() {
    Rig rig;
    ASSERT_OK(rig.send("0100"));  // "even when power is off"
    rig.render(900);
    TEST_ASSERT_TRUE(rig.pic.allBlack());
    const uint16_t rev = rig.state().rev;

    rig.now = 1000;
    ASSERT_OK(rig.send("06031e0a05"));  // both strips, pixels 30..39, 5 s
    TEST_ASSERT_TRUE(rig.engine.needsRender());
    rig.render(1000);
    TEST_ASSERT_TRUE_MESSAGE(rig.animating, "keep rendering so that the overlay can end");
    for (uint16_t i = 0; i < 144; ++i) {
        const Rgb p = rig.pic.left[i];
        TEST_ASSERT_RGB(p, rig.pic.right[i]);
        if (i >= 30 && i < 40) {
            TEST_ASSERT_TRUE_MESSAGE(p.r == p.g && p.g == p.b && p.r > 0, "white");
            TEST_ASSERT_INT_WITHIN_MESSAGE(1, Engine::kIdentifyLevel, visibleLevel(p, rig.engine.brightness()), "dim");
        } else {
            TEST_ASSERT_RGB(kBlack, p);
        }
    }
    TEST_ASSERT_EQUAL_MESSAGE(rev, rig.state().rev, "IDENTIFY is not part of STATE");

    rig.render(5999);
    TEST_ASSERT_FALSE(rig.pic.allBlack());
    rig.render(6000);  // 5 s after the command
    TEST_ASSERT_TRUE(rig.pic.allBlack());
    TEST_ASSERT_FALSE(rig.animating);
    TEST_ASSERT_FALSE(rig.engine.identifyActive());
}

void test_identify_over_a_playing_scene() {
    Rig plain, marked;
    for (Rig* rig : {&plain, &marked}) {
        ASSERT_OK(rig->setEffect(0, kRainbow));
        rig->render(1000);
    }
    marked.now = 1200;
    ASSERT_OK(marked.send("06010a0503"));  // left strip only, pixels 10..14, 3 s
    plain.render(1500);
    marked.render(1500);
    for (uint16_t i = 0; i < 144; ++i) {
        if (i >= 10 && i < 15) {
            TEST_ASSERT_TRUE(marked.pic.left[i].r == marked.pic.left[i].g && marked.pic.left[i].g == marked.pic.left[i].b);
            TEST_ASSERT_TRUE(marked.pic.left[i] != plain.pic.left[i]);
        } else {
            TEST_ASSERT_RGB(plain.pic.left[i], marked.pic.left[i]);
        }
        TEST_ASSERT_RGB(plain.pic.right[i], marked.pic.right[i]);
    }
    // Afterwards the scene is as if nothing had happened.
    plain.render(4200);
    marked.render(4200);
    TEST_ASSERT_TRUE(plain.pic == marked.pic);
}

void test_identify_cancel_and_tick() {
    Rig rig;
    ASSERT_OK(rig.setEffect(0, kSolid, kRed));
    ASSERT_OK(rig.send("0603000aff"));  // 255 s
    rig.render(1000);
    TEST_ASSERT_TRUE(rig.pic.left[0] != kRed);
    // "length 0 or seconds 0 cancels it"
    ASSERT_OK(rig.send("0603000000"));
    TEST_ASSERT_TRUE(rig.engine.needsRender());
    rig.render(1100);
    TEST_ASSERT_RGB(kRed, rig.pic.left[0]);
    TEST_ASSERT_FALSE(rig.animating);

    ASSERT_OK(rig.send("0603000a02"));
    rig.render(1200);
    ASSERT_OK(rig.send("0603000a00"));
    rig.render(1300);
    TEST_ASSERT_RGB(kRed, rig.pic.left[0]);

    // tick() alone notices the end and asks for a frame.
    rig.now = 2000;
    ASSERT_OK(rig.send("0603000a01"));
    rig.render(2000);
    rig.engine.tick(2999);
    TEST_ASSERT_TRUE(rig.engine.identifyActive());
    rig.engine.tick(3000);
    TEST_ASSERT_FALSE(rig.engine.identifyActive());
    TEST_ASSERT_TRUE(rig.engine.needsRender());
}

// "dim white" must look the same whatever the brightness setting, because
// the output layer scales every pixel by brightness.
void test_identify_is_equally_dim_at_any_brightness() {
    int reference = -1;
    for (int brightness : {255, 128, 64}) {
        Rig rig;
        ASSERT_OK(rig.send(Bytes{0x02, static_cast<uint8_t>(brightness)}));
        ASSERT_OK(rig.send("0603000a05"));
        rig.render(1000);
        const int visible = visibleLevel(rig.pic.left[0], static_cast<uint8_t>(brightness));
        if (reference < 0) reference = visible;
        TEST_ASSERT_INT_WITHIN(1, reference, visible);
        TEST_ASSERT_LESS_THAN_MESSAGE(64, visible, "dim");
        TEST_ASSERT_GREATER_THAN(8, visible);
    }
    // Below that level it cannot be compensated: as bright as the setting allows.
    Rig rig;
    ASSERT_OK(rig.send("0205"));
    ASSERT_OK(rig.send("0603000a05"));
    rig.render(1000);
    TEST_ASSERT_RGB(Rgb(255, 255, 255), rig.pic.left[0]);
}

// ---- Live input -----------------------------------------------------------------------

void test_live_input_and_timeout() {
    Rig rig;
    const Bytes frame = fromHex("0709c8ff641e0005400aec053412");
    TEST_ASSERT_NULL(rig.engine.liveInput());

    rig.engine.handleLive(frame.data(), frame.size(), 1000);
    TEST_ASSERT_EQUAL_HEX8(0x04, rig.state().flags);
    TEST_ASSERT_EQUAL(1, rig.state().rev);
    TEST_ASSERT_TRUE(rig.engine.takeStateChanged());
    const LiveInput* live = rig.engine.liveInput();
    TEST_ASSERT_NOT_NULL(live);
    TEST_ASSERT_EQUAL(200, live->level);
    TEST_ASSERT_EQUAL(255, live->bass);
    TEST_ASSERT_EQUAL(1280, live->bpm_x10);
    TEST_ASSERT_EQUAL(-20, live->tilt_x);

    // Frames keep it alive without touching rev.
    for (uint32_t t = 1033; t < 3000; t += 33) {
        rig.engine.handleLive(frame.data(), frame.size(), t);
        rig.engine.tick(t + 20);
    }
    TEST_ASSERT_EQUAL(1, rig.state().rev);
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());

    // "If no frame arrives for 500 ms the jacket clears the live-input flag"
    rig.engine.handleLive(frame.data(), frame.size(), 3000);
    rig.engine.tick(3499);
    TEST_ASSERT_EQUAL_HEX8(0x04, rig.state().flags);
    rig.engine.tick(3500);
    TEST_ASSERT_EQUAL_HEX8(0x00, rig.state().flags);
    TEST_ASSERT_NULL(rig.engine.liveInput());
    TEST_ASSERT_EQUAL(2, rig.state().rev);
    TEST_ASSERT_TRUE(rig.engine.takeStateChanged());

    // render() runs the same timeout.
    rig.engine.handleLive(frame.data(), frame.size(), 4000);
    TEST_ASSERT_EQUAL_HEX8(0x04, rig.state().flags);
    rig.render(4600);
    TEST_ASSERT_EQUAL_HEX8(0x00, rig.state().flags);
}

void test_short_live_frame_is_ignored() {
    Rig rig;
    const Bytes frame = fromHex("0709c8ff641e0005400aec0534");  // 13 bytes
    rig.engine.handleLive(frame.data(), frame.size(), 1000);
    TEST_ASSERT_NULL(rig.engine.liveInput());
    TEST_ASSERT_EQUAL(0, rig.state().rev);
    rig.engine.handleLive(nullptr, 0, 1000);
    TEST_ASSERT_NULL(rig.engine.liveInput());
}

// ---- BOOT_OTHER_SLOT ------------------------------------------------------------------

void test_boot_other_slot() {
    Rig rig;

    // Until device code says there is a firmware in the other slot: code 4.
    ASSERT_ERR(ErrorCode::NotFound, rig.send("325a"));
    TEST_ASSERT_EQUAL_STRING("7f3204", rig.lastReply().c_str());
    TEST_ASSERT_FALSE(rig.engine.takeBootOtherSlot());

    rig.engine.setOtherSlotUsable(true);
    // The confirmation byte must be exactly 0x5A, and must be there.
    ASSERT_ERR(ErrorCode::OutOfRange, rig.send("3200"));
    TEST_ASSERT_EQUAL_STRING("7f3203", rig.lastReply().c_str());
    ASSERT_ERR(ErrorCode::OutOfRange, rig.send("32a5"));
    ASSERT_ERR(ErrorCode::TooShort, rig.send("32"));
    TEST_ASSERT_EQUAL_STRING("7f3202", rig.lastReply().c_str());
    TEST_ASSERT_FALSE_MESSAGE(rig.engine.takeBootOtherSlot(), "a rejected command asks for nothing");

    // Accepted: no reply, nothing in STATE moves (decision: like IDENTIFY, it
    // is not part of STATE), and device code is told exactly once.
    const size_t replies = rig.sink.frames.size();
    const StateMsg state = rig.state();
    ASSERT_OK(rig.send("325a"));
    TEST_ASSERT_EQUAL(replies, rig.sink.frames.size());
    TEST_ASSERT_EQUAL(state.rev, rig.state().rev);
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());
    TEST_ASSERT_FALSE(rig.engine.needsSave());
    TEST_ASSERT_TRUE(rig.engine.takeBootOtherSlot());
    TEST_ASSERT_FALSE(rig.engine.takeBootOtherSlot());

    // Trailing bytes are ignored, as for every command.
    ASSERT_OK(rig.send("325aff"));
    TEST_ASSERT_TRUE(rig.engine.takeBootOtherSlot());

    // The other slot can stop being usable (its image failed to verify).
    rig.engine.setOtherSlotUsable(false);
    ASSERT_ERR(ErrorCode::NotFound, rig.send("325a"));
    TEST_ASSERT_FALSE(rig.engine.takeBootOtherSlot());
}

// ---- Sound heard by the jacket itself ---------------------------------------------------
//
// A board with a microphone makes live frames of its own (MicLive) and hands
// them over with handleLocalLive(). These tests play a phone and a microphone
// against each other, millisecond by millisecond, the way the loop does:
// frames in, tick, render.

// A phone frame, as bytes on the `live` characteristic.
Bytes phoneFrame(uint8_t flags, uint8_t level, uint8_t motion, uint16_t bpm_x10 = 0, uint8_t phase = 0, uint16_t t_ms = 0) {
    return Bytes{flags, 0, level, level, level, level, static_cast<uint8_t>(bpm_x10 & 0xFF), static_cast<uint8_t>(bpm_x10 >> 8),
                 phase, motion, 0, 0, static_cast<uint8_t>(t_ms & 0xFF), static_cast<uint8_t>(t_ms >> 8)};
}

// A frame from the board's microphone: audio, and a beat when a tempo is given.
LiveInput micFrame(uint8_t level, uint32_t t_ms, uint16_t bpm_x10 = 0, uint8_t phase = 0) {
    LiveInput in;
    in.flags = static_cast<uint8_t>(kLiveAudioValid | (bpm_x10 != 0 ? kLiveBeatValid : 0));
    in.level = in.bass = in.mid = in.treble = level;
    in.bpm_x10 = bpm_x10;
    in.beat_phase = phase;
    in.t_ms = static_cast<uint16_t>(t_ms);
    return in;
}

// floor(phase * 256) of a beat of `period_ms` that fell on t = 0.
uint8_t phaseByteAt(uint32_t t_ms, uint32_t period_ms) { return static_cast<uint8_t>((t_ms % period_ms) * 256u / period_ms); }

// Distance between two beat phases (Q16), the short way round.
int phaseDistance(uint16_t a, uint16_t b) {
    const int d = static_cast<int>(static_cast<uint16_t>(a - b));
    return d > 32768 ? 65536 - d : d;
}

// Who sends what, when; run() plays it and keeps the extremes.
struct Scenario {
    uint32_t mic_from = 0, mic_to = 0;      // microphone frames every 30 ms in [from, to)
    uint8_t mic_level = 0;
    uint16_t mic_bpm_x10 = 0;
    uint32_t phone_from = 0, phone_to = 0;  // phone frames every 33 ms in [from, to)
    uint32_t phone_audio_to = 0;            // ... with the audio (and beat) bit until here, motion only afterwards
    bool phone_motion = true;
    uint8_t phone_level = 0;
    uint8_t phone_motion_value = 0;
    uint16_t phone_bpm_x10 = 0;
    int32_t phone_clock_offset = 0;         // phone clock minus the jacket's

    int min_audio_mix = 255;                // seen by run() in [watch_from, now]
    uint32_t watch_from = 0;

    void run(Rig& rig, uint32_t from, uint32_t to) {
        for (uint32_t t = from; t <= to; ++t) {
            if (t >= phone_from && t < phone_to && (t - phone_from) % 33 == 0) {
                const bool audio = t < phone_audio_to;
                const uint8_t flags = static_cast<uint8_t>((audio ? kLiveAudioValid : 0) | (phone_motion ? kLiveMotionValid : 0) |
                                                           (audio && phone_bpm_x10 != 0 ? kLiveBeatValid : 0));
                const uint32_t period = phone_bpm_x10 != 0 ? 600000u / phone_bpm_x10 : 1;
                const Bytes frame = phoneFrame(flags, audio ? phone_level : 0, phone_motion ? phone_motion_value : 0,
                                               audio ? phone_bpm_x10 : 0, audio && phone_bpm_x10 != 0 ? phaseByteAt(t, period) : 0,
                                               static_cast<uint16_t>(t + static_cast<uint32_t>(phone_clock_offset)));
                rig.engine.handleLive(frame.data(), frame.size(), t);
            }
            if (t >= mic_from && t < mic_to && (t - mic_from) % 30 == 0) {
                const uint32_t period = mic_bpm_x10 != 0 ? 600000u / mic_bpm_x10 : 1;
                rig.engine.handleLocalLive(micFrame(mic_level, t, mic_bpm_x10, mic_bpm_x10 != 0 ? phaseByteAt(t, period) : 0), t);
            }
            rig.engine.tick(t);
            if (t % 10 == 0) {
                rig.render(t);
                if (t >= watch_from && rig.engine.reactive().audio_mix < min_audio_mix) min_audio_mix = rig.engine.reactive().audio_mix;
            }
        }
        rig.now = to;
    }
};

using Source = Engine::SoundSource;

void test_local_sound_alone() {
    Rig rig;
    TEST_ASSERT_EQUAL(static_cast<int>(Source::None), static_cast<int>(rig.engine.soundSource()));
    Scenario s;
    s.mic_from = 1000;
    s.mic_to = 4000;
    s.mic_level = 180;
    s.mic_bpm_x10 = 1200;
    s.run(rig, 1000, 3000);

    const Reactive& r = rig.engine.reactive();
    TEST_ASSERT_EQUAL(255, r.audio_mix);
    TEST_ASSERT_EQUAL(180, r.level);
    TEST_ASSERT_EQUAL(180, r.treble);
    TEST_ASSERT_EQUAL(255, r.beat_mix);
    TEST_ASSERT_EQUAL_MESSAGE(0, r.motion_mix, "a microphone knows nothing of motion");
    TEST_ASSERT_EQUAL(1200, rig.engine.beatClock().bpmX10());
    TEST_ASSERT_TRUE(rig.engine.beatClock().locked());
    TEST_ASSERT_LESS_THAN_MESSAGE(65536 / 24, phaseDistance(rig.engine.beatClock().phase(), static_cast<uint16_t>((3000 % 500) * 65536 / 500)),
                                  "the beat is where the microphone heard it");
    TEST_ASSERT_EQUAL_MESSAGE(0, rig.engine.beatClock().lastExcessDelayMs(), "frames stamped with their own time have no delay");
    TEST_ASSERT_EQUAL(static_cast<int>(Source::Local), static_cast<int>(rig.engine.soundSource()));
    TEST_ASSERT_TRUE(rig.engine.localLiveActive());

    // decision: this is not live input in the protocol's sense. The flag in
    // STATE says "a phone is sending live frames", and stays clear.
    TEST_ASSERT_NULL(rig.engine.liveInput());
    TEST_ASSERT_EQUAL(0, rig.state().flags & kStateLive);
    TEST_ASSERT_EQUAL_MESSAGE(0, rig.state().rev, "and it does not count as a change of STATE");
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());

    // The microphone stops (no effect needs it any more): after the same
    // 500 ms a phone gets, the audio fades out over 300 ms; the beat coasts.
    s.run(rig, 3001, 4400);
    TEST_ASSERT_TRUE_MESSAGE(rig.engine.localLiveActive(), "400 ms after the last frame");
    TEST_ASSERT_EQUAL(255, rig.engine.reactive().audio_mix);
    s.run(rig, 4401, 4900);
    TEST_ASSERT_FALSE(rig.engine.localLiveActive());
    TEST_ASSERT_EQUAL(static_cast<int>(Source::None), static_cast<int>(rig.engine.soundSource()));
    TEST_ASSERT_EQUAL(0, rig.engine.reactive().audio_mix);
    TEST_ASSERT_EQUAL_MESSAGE(180, rig.engine.reactive().level, "the last values are held while the weight fades");
    TEST_ASSERT_TRUE_MESSAGE(rig.engine.beatClock().active(), "the beat clock coasts for 3 s");
    TEST_ASSERT_EQUAL(0, rig.state().rev);
}

void test_phone_sound_wins_over_the_microphone() {
    Rig rig;
    Scenario s;
    s.mic_from = 1000;
    s.mic_to = 9000;
    s.mic_level = 60;
    s.phone_from = 2000;
    s.phone_to = 3500;
    s.phone_audio_to = 3500;
    s.phone_level = 220;
    s.phone_motion_value = 90;

    s.run(rig, 1000, 1990);
    TEST_ASSERT_EQUAL(60, rig.engine.reactive().level);
    TEST_ASSERT_EQUAL(static_cast<int>(Source::Local), static_cast<int>(rig.engine.soundSource()));

    // The phone joins with sound of its own: its values take over, gliding.
    s.watch_from = 1990;
    s.min_audio_mix = 255;
    s.run(rig, 1991, 3400);
    TEST_ASSERT_EQUAL(static_cast<int>(Source::Phone), static_cast<int>(rig.engine.soundSource()));
    TEST_ASSERT_EQUAL_MESSAGE(220, rig.engine.reactive().level, "the microphone's frames are not applied meanwhile");
    TEST_ASSERT_EQUAL(255, rig.engine.reactive().motion_mix);
    TEST_ASSERT_NOT_NULL(rig.engine.liveInput());
    TEST_ASSERT_EQUAL(1, rig.state().rev);

    // The phone goes away. 500 ms later the microphone supplies the sound
    // again, and the audio never faded to the fallback in between.
    s.run(rig, 3401, 3950);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(Source::Phone), static_cast<int>(rig.engine.soundSource()),
                              "still the phone's for 500 ms after its last frame");
    TEST_ASSERT_EQUAL(220, rig.engine.reactive().level);
    s.run(rig, 3951, 5000);
    TEST_ASSERT_EQUAL(static_cast<int>(Source::Local), static_cast<int>(rig.engine.soundSource()));
    TEST_ASSERT_NULL(rig.engine.liveInput());
    TEST_ASSERT_INT_WITHIN_MESSAGE(2, 60, rig.engine.reactive().level, "back at what the microphone hears");
    TEST_ASSERT_EQUAL(255, rig.engine.reactive().audio_mix);
    TEST_ASSERT_EQUAL(0, rig.engine.reactive().motion_mix);
    TEST_ASSERT_GREATER_THAN_MESSAGE(200, s.min_audio_mix, "the hand-over is a glide, not a fade to the fallback and back");
    TEST_ASSERT_EQUAL_MESSAGE(2, rig.state().rev, "rev moved for the phone's live flag only: set and clear");
}

void test_phone_motion_with_the_jackets_own_hearing() {
    // "The phone sends motion only, the jacket hears the music itself."
    Rig rig;
    Scenario s;
    s.mic_from = 1000;
    s.mic_to = 9000;
    s.mic_level = 150;
    s.mic_bpm_x10 = 1280;
    s.phone_from = 1000;
    s.phone_to = 3000;
    s.phone_audio_to = 0;  // never any audio from the phone
    s.phone_motion_value = 200;
    s.watch_from = 1500;
    s.run(rig, 1000, 2990);

    const Reactive& r = rig.engine.reactive();
    TEST_ASSERT_EQUAL(255, r.audio_mix);
    TEST_ASSERT_EQUAL(150, r.level);
    TEST_ASSERT_EQUAL(255, r.beat_mix);
    TEST_ASSERT_EQUAL(1280, rig.engine.beatClock().bpmX10());
    TEST_ASSERT_EQUAL(255, r.motion_mix);
    TEST_ASSERT_EQUAL(200, r.motion);
    TEST_ASSERT_EQUAL(static_cast<int>(Source::Local), static_cast<int>(rig.engine.soundSource()));
    TEST_ASSERT_EQUAL_MESSAGE(kStateLive, rig.state().flags & kStateLive, "the phone's frames are live input as ever");
    TEST_ASSERT_EQUAL_MESSAGE(0, rig.engine.beatClock().lastExcessDelayMs(), "the phone's clock never reached the beat clock");

    // The phone leaves: motion fades, the sound is untouched.
    s.run(rig, 2991, 4500);
    TEST_ASSERT_EQUAL(0, rig.engine.reactive().motion_mix);
    TEST_ASSERT_EQUAL(255, rig.engine.reactive().audio_mix);
    TEST_ASSERT_EQUAL(150, rig.engine.reactive().level);
    TEST_ASSERT_EQUAL_MESSAGE(255, s.min_audio_mix, "the audio weight never moved");
    TEST_ASSERT_TRUE(rig.engine.beatClock().locked());
}

void test_phone_that_stops_sending_audio_hands_over_to_the_microphone() {
    // The phone keeps sending motion but its audio bit goes (the user
    // switched the phone's microphone off). For 500 ms the sound is still the
    // phone's to end; then the jacket's own hearing takes over.
    Rig rig;
    Scenario s;
    s.mic_from = 1000;
    s.mic_to = 9000;
    s.mic_level = 70;
    s.phone_from = 1000;
    s.phone_to = 9000;
    s.phone_audio_to = 2500;
    s.phone_level = 210;
    s.phone_motion_value = 40;
    s.run(rig, 1000, 2490);
    TEST_ASSERT_EQUAL(210, rig.engine.reactive().level);
    TEST_ASSERT_EQUAL(static_cast<int>(Source::Phone), static_cast<int>(rig.engine.soundSource()));

    s.run(rig, 2491, 4000);
    TEST_ASSERT_EQUAL(static_cast<int>(Source::Local), static_cast<int>(rig.engine.soundSource()));
    TEST_ASSERT_EQUAL(255, rig.engine.reactive().audio_mix);
    TEST_ASSERT_EQUAL(70, rig.engine.reactive().level);
    TEST_ASSERT_EQUAL(255, rig.engine.reactive().motion_mix);
    TEST_ASSERT_EQUAL(40, rig.engine.reactive().motion);
}

void test_without_a_microphone_the_phone_speaks_for_everything() {
    // No local frames ever: a phone frame without the audio bit means "no
    // sound", exactly as before there was such a thing as a microphone.
    Rig rig;
    Scenario s;
    s.phone_from = 1000;
    s.phone_to = 4000;
    s.phone_audio_to = 2000;
    s.phone_level = 200;
    s.phone_motion_value = 10;
    s.run(rig, 1000, 1990);
    TEST_ASSERT_EQUAL(255, rig.engine.reactive().audio_mix);
    s.run(rig, 1991, 2600);
    TEST_ASSERT_EQUAL_MESSAGE(0, rig.engine.reactive().audio_mix, "faded out 300 ms after the bit went, not 500 ms later");
    TEST_ASSERT_EQUAL(255, rig.engine.reactive().motion_mix);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(Source::None), static_cast<int>(rig.engine.soundSource()), "2000 + 500 has passed");

    // And a microphone that stopped long ago does not hold the sound hostage.
    Rig later;
    Scenario t;
    t.mic_from = 1000;
    t.mic_to = 1500;
    t.mic_level = 99;
    t.phone_from = 3000;
    t.phone_to = 5000;
    t.phone_audio_to = 0;
    t.phone_motion_value = 10;
    t.run(later, 1000, 4000);
    TEST_ASSERT_EQUAL(0, later.engine.reactive().audio_mix);
    TEST_ASSERT_FALSE(later.engine.localLiveActive());
}

void test_beat_clock_follows_one_sender_at_a_time() {
    // The microphone hears 120 BPM and stamps its frames with the jacket's own
    // clock. The phone tracks 90 BPM and its clock is 300 ms behind. Fed to
    // one beat clock without telling it, the phone's frames would look 300 ms
    // late and the beat would be placed almost half a beat off.
    Rig rig;
    Scenario s;
    s.mic_from = 1000;
    s.mic_to = 20000;
    s.mic_level = 100;
    s.mic_bpm_x10 = 1200;
    s.phone_from = 4000;
    s.phone_to = 8000;
    s.phone_audio_to = 8000;
    s.phone_level = 100;
    s.phone_bpm_x10 = 900;
    s.phone_clock_offset = -300;

    s.run(rig, 1000, 3990);
    TEST_ASSERT_EQUAL(1200, rig.engine.beatClock().bpmX10());

    s.run(rig, 3991, 7000);
    const BeatClock& beat = rig.engine.beatClock();
    TEST_ASSERT_EQUAL(900, beat.bpmX10());
    TEST_ASSERT_TRUE(beat.locked());
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(1, beat.lastExcessDelayMs(), "the phone's clock was measured afresh");
    const uint32_t phone_period = 600000u / 900u;
    TEST_ASSERT_LESS_THAN_MESSAGE(65536 / 24, phaseDistance(beat.phase(), static_cast<uint16_t>((7000 % phone_period) * 65536 / phone_period)),
                                  "the beat is the phone's");

    // Back to the microphone when the phone stops: its tempo, its phase, its clock.
    s.run(rig, 7001, 11000);
    TEST_ASSERT_EQUAL(static_cast<int>(Source::Local), static_cast<int>(rig.engine.soundSource()));
    TEST_ASSERT_EQUAL(1200, beat.bpmX10());
    TEST_ASSERT_TRUE(beat.locked());
    TEST_ASSERT_EQUAL(0, beat.lastExcessDelayMs());
    TEST_ASSERT_LESS_THAN_MESSAGE(65536 / 24, phaseDistance(beat.phase(), static_cast<uint16_t>((11000 % 500) * 65536 / 500)),
                                  "the beat is the microphone's again");
}

void test_sound_wanted() {
    // The microphone runs only while this is true.
    Rig rig;
    ASSERT_OK(rig.setEffect(0xFF, kSolid));
    TEST_ASSERT_FALSE(rig.engine.soundWanted());

    enum : uint8_t { kBeatPulse = 7, kVuMeter = 8, kSparkle = 9 };
    ASSERT_OK(rig.setEffect(0xFF, kSparkle));
    TEST_ASSERT_FALSE_MESSAGE(rig.engine.soundWanted(), "Sparkle reacts to motion, not to sound");
    ASSERT_OK(rig.setEffect(0xFF, kVuMeter));
    TEST_ASSERT_TRUE(rig.engine.soundWanted());

    // One segment out of several is enough.
    ASSERT_OK(rig.setEffect(0xFF, kSolid));
    ASSERT_OK(rig.setSegments({{kStripMaskAll, 0, 50, 0}, {kStripMaskAll, 50, 50, 0}, {kStripMaskAll, 100, 44, 0}}));
    TEST_ASSERT_FALSE(rig.engine.soundWanted());
    ASSERT_OK(rig.setEffect(2, kBeatPulse));
    TEST_ASSERT_TRUE(rig.engine.soundWanted());

    // Off: nothing to react for. On again: wanted again.
    ASSERT_OK(rig.send("0100"));
    TEST_ASSERT_FALSE(rig.engine.soundWanted());
    ASSERT_OK(rig.send("0101"));
    TEST_ASSERT_TRUE(rig.engine.soundWanted());

    // The catalogue's flags decide, for every effect.
    for (uint8_t id = 0; id < effectCount(); ++id) {
        Rig one;
        ASSERT_OK(one.setEffect(0xFF, id));
        TEST_ASSERT_EQUAL_MESSAGE((effectInfo(id)->param_flags & kReactsSound) != 0, one.engine.soundWanted(), effectInfo(id)->name);
    }
}

void test_live_timeout_survives_clock_wrap() {
    Rig rig;
    const Bytes frame = fromHex("0709c8ff641e0005400aec053412");
    rig.engine.handleLive(frame.data(), frame.size(), 0xFFFFFF00u);
    rig.engine.tick(0x00000050u);  // 336 ms later, across the 32-bit wrap
    TEST_ASSERT_NOT_NULL(rig.engine.liveInput());
    rig.engine.tick(0x00000100u);  // 512 ms later
    TEST_ASSERT_NULL(rig.engine.liveInput());
}

// ---- Presets --------------------------------------------------------------------------

void test_preset_save_load_delete_list() {
    Rig rig;
    ASSERT_OK(rig.send("13"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("0400", rig.lastReply().c_str(), "only used slots are listed");

    // Build a scene and save it in slot 2 as "Neon".
    ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 1}}));
    ASSERT_OK(rig.setEffect(0, kComet, kRed, kBlue, 150, 60, 3));
    ASSERT_OK(rig.setEffect(1, kBreathe, kGreen, kGreen, 90, 255, 0));
    ASSERT_OK(rig.send("02c8"));
    ASSERT_OK(rig.send("1002044e656f6e"));
    const StateMsg saved = rig.state();
    TEST_ASSERT_EQUAL_MESSAGE(2, saved.preset, "slot last saved");
    ASSERT_OK(rig.send("13"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("040102044e656f6e", rig.lastReply().c_str(), "the golden PRESETS frame");

    // Any change to the scene clears the marker ...
    ASSERT_OK(rig.send("0220"));
    TEST_ASSERT_EQUAL(0xFF, rig.state().preset);
    ASSERT_OK(rig.setSegments({{3, 0, 144, 0}}));
    ASSERT_OK(rig.setEffect(0, kSolid, kCyan));
    ASSERT_OK(rig.send("0100"));

    // ... and loading brings the whole scene back, but not power.
    ASSERT_OK(rig.send("1102"));
    const StateMsg loaded = rig.state();
    TEST_ASSERT_EQUAL(2, loaded.preset);
    TEST_ASSERT_EQUAL(200, loaded.brightness);
    TEST_ASSERT_EQUAL(2, loaded.segment_count);
    for (uint8_t i = 0; i < 2; ++i) TEST_ASSERT_TRUE(loaded.segments[i] == saved.segments[i]);
    TEST_ASSERT_EQUAL_MESSAGE(0, loaded.power, "power is not stored in a preset");
    TEST_ASSERT_EQUAL(saved.rev + 5, loaded.rev);

    // Slots are independent and may be overwritten.
    ASSERT_OK(rig.send("02ff"));
    ASSERT_OK(rig.send("100003" "4f6e65"));        // slot 0 "One"
    ASSERT_OK(rig.send("100205" "4e656f6e32"));    // slot 2 renamed "Neon2", new content
    ASSERT_OK(rig.send("13"));
    TEST_ASSERT_EQUAL_STRING("040200034f6e6502054e656f6e32", rig.lastReply().c_str());
    ASSERT_OK(rig.send("0201"));
    ASSERT_OK(rig.send("1102"));
    TEST_ASSERT_EQUAL(255, rig.state().brightness);

    // Empty slot: "4 not found (e.g. empty preset slot)".
    ASSERT_ERR(ErrorCode::NotFound, rig.send("1105"));
    TEST_ASSERT_EQUAL_STRING("7f1104", rig.lastReply().c_str());

    // Delete.
    ASSERT_OK(rig.send("1202"));
    TEST_ASSERT_EQUAL_MESSAGE(0xFF, rig.state().preset, "the loaded slot no longer exists");
    ASSERT_OK(rig.send("13"));
    TEST_ASSERT_EQUAL_STRING("040100034f6e65", rig.lastReply().c_str());
    ASSERT_ERR(ErrorCode::NotFound, rig.send("1102"));
    // decision: deleting an empty slot is "not found" too.
    ASSERT_ERR(ErrorCode::NotFound, rig.send("1202"));
    TEST_ASSERT_EQUAL_STRING("7f1204", rig.lastReply().c_str());
}

// decision: the marker follows the scene, not the commands: sending a value
// the scene already has does not clear it (rev still counts the command).
void test_preset_marker_follows_scene_content() {
    Rig rig;
    ASSERT_OK(rig.send("0280"));
    ASSERT_OK(rig.send("100100"));  // slot 1, empty name
    TEST_ASSERT_EQUAL(1, rig.state().preset);
    const uint16_t rev = rig.state().rev;
    ASSERT_OK(rig.send("0280"));  // same brightness again
    TEST_ASSERT_EQUAL(1, rig.state().preset);
    TEST_ASSERT_EQUAL(rev + 1, rig.state().rev);
    ASSERT_OK(rig.send("0100"));  // power is not part of the scene
    TEST_ASSERT_EQUAL(1, rig.state().preset);
    ASSERT_OK(rig.send("302c01"));  // nor is the budget
    TEST_ASSERT_EQUAL(1, rig.state().preset);
    ASSERT_OK(rig.send("040003" "81"));
    TEST_ASSERT_EQUAL(0xFF, rig.state().preset);
}

void test_presets_survive_reboot() {
    MemoryStorage storage;
    StateMsg saved;
    {
        Rig rig(storage);
        ASSERT_OK(rig.setSegments({{1, 0, 72, 0}, {2, 72, 72, 1}, {3, 0, 10, 0}}));
        ASSERT_OK(rig.setEffect(2, kMatrix, kRed, kBlue, 11, 22, 2));
        ASSERT_OK(rig.send("0263"));
        ASSERT_OK(rig.send("1007" "10" "5369787465656e206279746573212121"));  // slot 7, a 16-byte name
        ASSERT_OK(rig.send("100003" "4f6e65"));
        saved = rig.state();
        ASSERT_OK(rig.send("0201"));
    }
    // A new engine on the same storage: nothing was save()d except the presets.
    Rig rig(storage);
    ASSERT_OK(rig.send("13"));
    TEST_ASSERT_EQUAL_STRING("040200034f6e65" "0710" "5369787465656e206279746573212121", rig.lastReply().c_str());
    ASSERT_OK(rig.send("1107"));
    const StateMsg s = rig.state();
    TEST_ASSERT_EQUAL(7, s.preset);
    TEST_ASSERT_EQUAL(0x63, s.brightness);
    TEST_ASSERT_EQUAL(3, s.segment_count);
    for (uint8_t i = 0; i < 3; ++i) TEST_ASSERT_TRUE(s.segments[i] == saved.segments[i]);

    // Deleting survives too.
    ASSERT_OK(rig.send("1207"));
    Rig again(storage);
    ASSERT_OK(again.send("13"));
    TEST_ASSERT_EQUAL_STRING("040100034f6e65", again.lastReply().c_str());
}

// ---- Persistence of the last scene and settings ---------------------------------------

void test_last_scene_and_settings_survive_reboot() {
    MemoryStorage storage;
    StateMsg before;
    {
        Rig rig(storage);
        TEST_ASSERT_FALSE(rig.engine.needsSave());
        rig.now = 5000;
        ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 1}}));
        ASSERT_OK(rig.setEffect(1, kHeartbeat, kRed, kBlue, 100, 50, 4));
        ASSERT_OK(rig.send("0221"));
        ASSERT_OK(rig.send("30dc05"));  // 1500 mA
        ASSERT_OK(rig.send("3101"));
        rig.now = 7000;
        ASSERT_OK(rig.send("100400"));  // also remember "this is preset 4"
        TEST_ASSERT_TRUE(rig.engine.needsSave());
        TEST_ASSERT_EQUAL_MESSAGE(7000, rig.engine.lastChangeMs(), "device code debounces on this");
        ASSERT_OK(rig.send("0100"));
        before = rig.state();
        TEST_ASSERT_TRUE(rig.engine.save());
        TEST_ASSERT_FALSE(rig.engine.needsSave());
        const uint32_t writes = storage.write_count;
        TEST_ASSERT_TRUE(rig.engine.save());
        TEST_ASSERT_EQUAL_MESSAGE(writes, storage.write_count, "nothing to write when nothing changed");
    }
    Rig rig(storage);
    const StateMsg after = rig.state();
    TEST_ASSERT_EQUAL(0x21, after.brightness);
    TEST_ASSERT_EQUAL(1500, after.budget_ma);
    TEST_ASSERT_EQUAL_HEX8(0x01, after.flags);
    TEST_ASSERT_EQUAL(4, after.preset);
    TEST_ASSERT_EQUAL(2, after.segment_count);
    for (uint8_t i = 0; i < 2; ++i) TEST_ASSERT_TRUE(after.segments[i] == before.segments[i]);
    TEST_ASSERT_EQUAL_MESSAGE(0, after.rev, "rev restarts at boot");
    // decision: power is not persisted; a power cycle always lights the jacket.
    TEST_ASSERT_EQUAL(1, after.power);
    TEST_ASSERT_FALSE(rig.engine.needsSave());
}

void test_unsaved_changes_are_lost_on_reboot() {
    MemoryStorage storage;
    {
        Rig rig(storage);
        ASSERT_OK(rig.send("0221"));
        ASSERT_OK(rig.send("30dc05"));
        TEST_ASSERT_TRUE(rig.engine.needsSave());
        // no save()
    }
    Rig rig(storage);
    TEST_ASSERT_EQUAL(128, rig.state().brightness);
    TEST_ASSERT_EQUAL(900, rig.state().budget_ma);
}

void test_what_needs_saving() {
    Rig rig;
    ASSERT_OK(rig.send("0100"));  // power: not persisted
    TEST_ASSERT_FALSE(rig.engine.needsSave());
    ASSERT_OK(rig.send("13"));
    ASSERT_OK(rig.send("06031e0a05"));
    rig.engine.setWifiConnected(true);
    TEST_ASSERT_FALSE(rig.engine.needsSave());
    ASSERT_OK(rig.send("308403"));  // budget 900: already the value
    TEST_ASSERT_FALSE(rig.engine.needsSave());
    ASSERT_OK(rig.send("308503"));
    TEST_ASSERT_TRUE(rig.engine.needsSave());
    TEST_ASSERT_TRUE(rig.engine.save());
    ASSERT_OK(rig.send("040004" "05"));
    TEST_ASSERT_TRUE(rig.engine.needsSave());
}

void test_storage_failures() {
    MemoryStorage storage;
    Rig rig(storage);
    ASSERT_OK(rig.send("0221"));
    storage.fail_writes = true;
    TEST_ASSERT_FALSE(rig.engine.save());
    TEST_ASSERT_TRUE_MESSAGE(rig.engine.needsSave(), "still pending after a failed write");
    // "Code 5: the jacket could not write to its storage."
    const uint16_t rev = rig.state().rev;
    ASSERT_ERR(ErrorCode::StorageFailure, rig.send("1002044e656f6e"));
    TEST_ASSERT_EQUAL_STRING("7f1005", rig.lastReply().c_str());
    TEST_ASSERT_EQUAL(rev, rig.state().rev);
    TEST_ASSERT_EQUAL(0xFF, rig.state().preset);
    ASSERT_OK(rig.send("13"));
    TEST_ASSERT_EQUAL_STRING("0400", rig.lastReply().c_str());
    storage.fail_writes = false;
    TEST_ASSERT_TRUE(rig.engine.save());
    TEST_ASSERT_FALSE(rig.engine.needsSave());
}

void test_corrupt_storage_falls_back_to_defaults() {
    MemoryStorage storage;
    const uint8_t junk[] = {0x01, 0xFF, 0x01, 0x80, 0x09, 0x03, 0x00, 0x90, 0x00};  // nine segments
    const uint8_t future[] = {0x63, 0x00, 0x01, 0x80, 0x01};                        // unknown record version
    const uint8_t cut[] = {0x01, 0x04, 0x4e, 0x65};                                 // preset cut off in its name
    storage.write("last", junk, sizeof junk);
    storage.write("set", future, sizeof future);
    storage.write("p3", cut, sizeof cut);
    Rig rig(storage);
    const StateMsg s = rig.state();
    TEST_ASSERT_EQUAL(1, s.segment_count);
    TEST_ASSERT_EQUAL(128, s.brightness);
    TEST_ASSERT_EQUAL(900, s.budget_ma);
    ASSERT_OK(rig.send("13"));
    TEST_ASSERT_EQUAL_STRING("0400", rig.lastReply().c_str());
    ASSERT_ERR(ErrorCode::NotFound, rig.send("1103"));
}

// A scene stored by a firmware with more effects must not crash this one.
void test_stored_unknown_effect_is_replaced() {
    MemoryStorage storage;
    // last record: version 1, no preset; scene blob: v1, brightness 99, 1 segment,
    // geometry (3, 0, 144, 0), effect 200, palette 200, speed, intensity, colours.
    const Bytes last = fromHex("01ff" "016301" "03009000" "c8c88080" "ff0000" "0000ff");
    storage.write("last", last.data(), last.size());
    Rig rig(storage);
    const StateMsg s = rig.state();
    TEST_ASSERT_EQUAL(99, s.brightness);
    TEST_ASSERT_EQUAL(0, s.segments[0].config.effect);
    TEST_ASSERT_EQUAL(0, s.segments[0].config.palette);
    rig.render(1000);
    TEST_ASSERT_RGB(kRed, rig.pic.left[0]);
}

// ---- PROTOCOL.md "Commands": PRESETS after PRESET_SAVE / PRESET_DELETE ----------------

// "PRESET_SAVE and PRESET_DELETE also answer with a PRESETS notification
// carrying the updated list."
void test_preset_save_and_delete_answer_with_presets() {
    Rig rig;
    ASSERT_OK(rig.send("1002044e656f6e"));  // slot 2 "Neon"
    TEST_ASSERT_EQUAL_MESSAGE(1, rig.sink.frames.size(), "exactly one reply frame");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("040102044e656f6e", rig.lastReply().c_str(), "the golden PRESETS frame");
    TEST_ASSERT_TRUE_MESSAGE(rig.engine.takeStateChanged(), "and a STATE notification is due as well");

    ASSERT_OK(rig.send("100003" "4f6e65"));  // slot 0 "One"
    TEST_ASSERT_EQUAL(2, rig.sink.frames.size());
    TEST_ASSERT_EQUAL_STRING("040200034f6e6502044e656f6e", rig.lastReply().c_str());

    // Loading changes the scene, not the list: STATE only.
    ASSERT_OK(rig.send("1102"));
    TEST_ASSERT_EQUAL(2, rig.sink.frames.size());

    ASSERT_OK(rig.send("1202"));
    TEST_ASSERT_EQUAL(3, rig.sink.frames.size());
    TEST_ASSERT_EQUAL_STRING("040100034f6e65", rig.lastReply().c_str());
    ASSERT_OK(rig.send("1200"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("0400", rig.lastReply().c_str(), "an empty list is still sent");

    // A rejected save or delete answers with ERROR only.
    ASSERT_ERR(ErrorCode::NotFound, rig.send("1200"));
    ASSERT_ERR(ErrorCode::OutOfRange, rig.send("1008044e656f6e"));
    TEST_ASSERT_EQUAL(6, rig.sink.frames.size());
    TEST_ASSERT_EQUAL_STRING("7f1003", rig.lastReply().c_str());
}

// ---- PROTOCOL.md "Validation" ---------------------------------------------------------

// "Code 5: the jacket could not write to its storage." A rejected command
// changes nothing: the slot, the list, the marker and rev stay as they were.
void test_storage_failure_is_code_5() {
    MemoryStorage storage;
    Rig rig(storage);
    ASSERT_OK(rig.send("1002044e656f6e"));  // slot 2 exists and is the marked slot
    rig.sink.frames.clear();
    rig.engine.takeStateChanged();
    TEST_ASSERT_TRUE(rig.engine.save());
    const uint16_t rev = rig.state().rev;

    storage.fail_writes = true;
    ASSERT_ERR(ErrorCode::StorageFailure, rig.send("100503" "4f6e65"));  // save into slot 5
    TEST_ASSERT_EQUAL_STRING("7f1005", rig.lastReply().c_str());
    ASSERT_ERR(ErrorCode::StorageFailure, rig.send("1202"));  // delete slot 2
    TEST_ASSERT_EQUAL_STRING("7f1205", rig.lastReply().c_str());
    TEST_ASSERT_EQUAL_MESSAGE(2, rig.sink.frames.size(), "ERROR only, no PRESETS frame");

    TEST_ASSERT_EQUAL(rev, rig.state().rev);
    TEST_ASSERT_EQUAL_MESSAGE(2, rig.state().preset, "the marked slot was not deleted");
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());
    TEST_ASSERT_FALSE(rig.engine.needsSave());
    ASSERT_OK(rig.send("13"));
    TEST_ASSERT_EQUAL_STRING("040102044e656f6e", rig.lastReply().c_str());
    ASSERT_OK(rig.send("1102"));  // still loadable

    // Code 4 still wins for an empty slot: there is nothing to write.
    ASSERT_ERR(ErrorCode::NotFound, rig.send("1206"));

    // Once the storage works again, so do both commands, and a reboot agrees.
    storage.fail_writes = false;
    ASSERT_OK(rig.send("1202"));
    TEST_ASSERT_EQUAL_STRING("0400", rig.lastReply().c_str());
    Rig again(storage);
    ASSERT_OK(again.send("13"));
    TEST_ASSERT_EQUAL_STRING("0400", again.lastReply().c_str());
}

// Every case the "Validation" section lists, checked on the reply frame the
// app would see, and "a rejected command changes nothing".
void test_validation_section() {
    Rig rig;
    ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 1}}));  // two segments: indexes 0 and 1 exist
    ASSERT_OK(rig.send("100300"));                                 // slot 3 is used, slot 4 is not
    rig.sink.frames.clear();
    rig.engine.takeStateChanged();
    TEST_ASSERT_TRUE(rig.engine.save());
    uint8_t before[kMaxFrame];
    const size_t before_len = rig.engine.encodeState(before, sizeof before);

    struct Case {
        const char* hex;
        const char* reply;
        const char* why;
    };
    const Case cases[] = {
        // "Code 2 (too short) takes precedence ..."
        {"01", "7f0102", "SET_POWER without a value"},
        {"02", "7f0202", "SET_BRIGHTNESS without a value"},
        {"0309630080c800ffccff00", "7f0302", "SET_EFFECT: bad segment and effect, but one byte short"},
        {"0409", "7f0402", "SET_PARAM without a parameter id"},
        {"040003", "7f0402", "SET_PARAM speed without its value"},
        {"040905ff00", "7f0402", "SET_PARAM colour cut short, bad segment"},
        {"05", "7f0502", "SET_SEGMENTS without a count"},
        {"05020100900002", "7f0502", "SET_SEGMENTS: two announced, one and a bit present"},
        {"060300", "7f0602", "IDENTIFY cut short"},
        {"1009", "7f1002", "PRESET_SAVE: bad slot, no name length"},
        {"1009044e65", "7f1002", "PRESET_SAVE: bad slot, name cut short"},
        {"11", "7f1102", "PRESET_LOAD without a slot"},
        {"12", "7f1202", "PRESET_DELETE without a slot"},
        {"2102", "7f2102", "GET_CATALOG: bad kind, no start"},
        {"3084", "7f3002", "SET_BUDGET with half a u16"},
        {"31", "7f3102", "SET_DEV_MODE without a value"},
        // "... unless a bad value makes the required length unknowable (SET_PARAM
        // id, segment count, name length); that is code 3."
        {"040007", "7f0403", "unknown SET_PARAM id, nothing after it"},
        {"0500", "7f0503", "segment count 0"},
        {"0509", "7f0503", "segment count 9, nothing after it"},
        {"100011", "7f1003", "name length 17, nothing after it"},
        // "Code 3: on other than 0/1"
        {"0102", "7f0103", "SET_POWER 2"},
        {"31ff", "7f3103", "SET_DEV_MODE 255"},
        // "unknown effect, palette or SET_PARAM id"
        {"03000a0080800000000000ff", "7f0303", "SET_EFFECT effect 10"},
        {"0300000680800000000000ff", "7f0303", "SET_EFFECT palette 6"},
        {"0400010a", "7f0403", "SET_PARAM effect 10"},
        {"04000206", "7f0403", "SET_PARAM palette 6"},
        {"04000000", "7f0403", "SET_PARAM id 0"},
        {"04000700", "7f0403", "SET_PARAM id 7"},
        // "segment index >= the current count (other than 0xFF)"
        {"0302000080800000000000ff", "7f0303", "SET_EFFECT segment 2 of 2"},
        {"0308000080800000000000ff", "7f0303", "SET_EFFECT segment 8"},
        {"03fe000080800000000000ff", "7f0303", "SET_EFFECT segment 0xFE"},
        {"04020380", "7f0403", "SET_PARAM segment 2 of 2"},
        // "slot >= 8"
        {"100800", "7f1003", "PRESET_SAVE slot 8"},
        {"1108", "7f1103", "PRESET_LOAD slot 8"},
        {"12ff", "7f1203", "PRESET_DELETE slot 255"},
        // "name longer than 16 bytes"
        {"100011" "4141414141414141414141414141414141", "7f1003", "17-byte name"},
        // "strip_mask 0 or with unknown bits"
        {"050100009000", "7f0503", "SET_SEGMENTS strip_mask 0"},
        {"050104009000", "7f0503", "SET_SEGMENTS strip_mask 4"},
        {"06000a0a05", "7f0603", "IDENTIFY strip_mask 0"},
        {"06070a0a05", "7f0603", "IDENTIFY strip_mask 7"},
        // "unknown geometry flag bits"
        {"050103009002", "7f0503", "SET_SEGMENTS flags 2"},
        // The geometry table: length 1...144, start + length <= strip length.
        {"050103000000", "7f0503", "SET_SEGMENTS length 0"},
        {"050103009100", "7f0503", "SET_SEGMENTS length 145"},
        {"050103019000", "7f0503", "SET_SEGMENTS start 1 + length 144"},
        // "GET_CATALOG kind above 1"
        {"210200", "7f2103", "GET_CATALOG kind 2"},
        // "Code 4: PRESET_LOAD or PRESET_DELETE on an empty slot."
        {"1104", "7f1104", "PRESET_LOAD empty slot"},
        {"1204", "7f1204", "PRESET_DELETE empty slot"},
        // Code 1, and "an empty write is answered ERROR(opcode 0, code 2)".
        {"00", "7f0001", "opcode 0"},
        {"07", "7f0701", "opcode 7"},
        {"7f0101", "7f7f01", "a notification type is not an opcode"},
        {"", "7f0002", "empty write"},
    };
    size_t sent = 0;
    for (const Case& c : cases) {
        const ErrorCode code = rig.send(std::string(c.hex));
        ++sent;
        TEST_ASSERT_EQUAL_MESSAGE(sent, rig.sink.frames.size(), c.why);  // exactly one reply each
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reply, rig.lastReply().c_str(), c.why);
        TEST_ASSERT_EQUAL_MESSAGE(fromHex(c.reply)[2], static_cast<int>(code), c.why);
    }

    // "A rejected command changes nothing"
    uint8_t after[kMaxFrame];
    const size_t after_len = rig.engine.encodeState(after, sizeof after);
    TEST_ASSERT_EQUAL(before_len, after_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before, after, before_len);
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());
    TEST_ASSERT_FALSE(rig.engine.needsSave());
    TEST_ASSERT_FALSE(rig.engine.identifyActive());
    ASSERT_OK(rig.send("13"));
    TEST_ASSERT_EQUAL_STRING("04010300", rig.lastReply().c_str());

    // 0xFF and the last existing index are fine; trailing bytes are ignored.
    ASSERT_OK(rig.send("04ff0380"));
    ASSERT_OK(rig.send("04010380"));
    ASSERT_OK(rig.send("0101ffff"));
}

// ---- PROTOCOL.md "State rules" --------------------------------------------------------

// "rev starts at 0 at boot and increments on every accepted state-changing
// command, even when the value is unchanged, and when the WiFi or live-input
// flag changes. IDENTIFY and GET_* do not change it."
void test_rev_rules() {
    Rig rig;
    TEST_ASSERT_EQUAL(0, rig.state().rev);
    const StateMsg start = rig.state();
    uint16_t rev = 0;

    // Every state-changing command, each sent with the value the jacket already has.
    const EffectConfig c = start.segments[0].config;
    const Bytes same_effect{0x03,       0x00,       c.effect,   c.palette,  c.speed,    c.intensity,
                            c.color1.r, c.color1.g, c.color1.b, c.color2.r, c.color2.g, c.color2.b};
    const Bytes same[] = {
        Bytes{0x01, 0x01},                          // power is already on
        Bytes{0x02, start.brightness},
        same_effect,
        Bytes{0x04, 0x00, 0x03, c.speed},
        Bytes{0x05, 0x01, 0x03, 0x00, 0x90, 0x00},  // the default layout
        Bytes{0x30, 0x84, 0x03},                    // 900 mA
        Bytes{0x31, 0x00},                          // dev mode is already off
    };
    for (const Bytes& frame : same) {
        ASSERT_OK(rig.send(frame));
        TEST_ASSERT_EQUAL_MESSAGE(++rev, rig.state().rev, support::toHex(frame).c_str());
        TEST_ASSERT_TRUE_MESSAGE(rig.engine.takeStateChanged(), "a STATE notification follows");
    }
    ASSERT_OK(rig.send("100100"));  // PRESET_SAVE
    TEST_ASSERT_EQUAL(++rev, rig.state().rev);
    ASSERT_OK(rig.send("1101"));  // PRESET_LOAD
    TEST_ASSERT_EQUAL(++rev, rig.state().rev);
    ASSERT_OK(rig.send("1201"));  // PRESET_DELETE
    TEST_ASSERT_EQUAL(++rev, rig.state().rev);
    rig.engine.takeStateChanged();

    // IDENTIFY (start and cancel) and GET_* do not.
    ASSERT_OK(rig.send("06031e0a05"));
    ASSERT_OK(rig.send("0603000000"));
    ASSERT_OK(rig.send("13"));
    ASSERT_OK(rig.send("20"));
    ASSERT_OK(rig.send("210000"));
    ASSERT_OK(rig.send("210100"));
    TEST_ASSERT_EQUAL(rev, rig.state().rev);
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());

    // The WiFi and live-input flags do, once per change.
    rig.engine.setWifiConnected(true);
    TEST_ASSERT_EQUAL(++rev, rig.state().rev);
    rig.engine.setWifiConnected(false);
    TEST_ASSERT_EQUAL(++rev, rig.state().rev);
    const Bytes live = fromHex("0709c8ff641e0005400aec053412");
    rig.engine.handleLive(live.data(), live.size(), 5000);
    TEST_ASSERT_EQUAL(++rev, rig.state().rev);
    rig.engine.handleLive(live.data(), live.size(), 5030);
    TEST_ASSERT_EQUAL(rev, rig.state().rev);
    rig.engine.tick(5530);
    TEST_ASSERT_EQUAL(++rev, rig.state().rev);
}

// "preset is cleared to 0xFF when brightness, segments or any EffectConfig
// change, and when the marked slot is deleted. Power, budget and dev mode do
// not clear it."
void test_preset_marker_rules() {
    Rig rig;
    ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 0}}));
    ASSERT_OK(rig.send("100000"));  // slot 0
    ASSERT_OK(rig.send("100100"));  // slot 1: the marked slot from here on

    const char* const keeps[] = {"0100", "0101", "30dc05", "3101", "3100", "06031e0a05", "13", "1200"};
    for (const char* hex : keeps) {
        ASSERT_OK(rig.send(std::string(hex)));
        TEST_ASSERT_EQUAL_MESSAGE(1, rig.state().preset, hex);
    }

    const char* const clears[] = {
        "0210",                      // brightness
        "05020100900002009001",      // segments: one flag differs
        "0301010080800000000000ff",  // SET_EFFECT on one segment
        "040102" "02",               // SET_PARAM palette
        "040003" "10",               // speed
        "040004" "10",               // intensity
        "040105" "112233",           // color1
        "04ff06" "445566",           // color2 on every segment
    };
    for (const char* hex : clears) {
        ASSERT_OK(rig.send("1101"));
        TEST_ASSERT_EQUAL(1, rig.state().preset);
        ASSERT_OK(rig.send(std::string(hex)));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, rig.state().preset, hex);
    }

    // Deleting the marked slot clears it too.
    ASSERT_OK(rig.send("1101"));
    ASSERT_OK(rig.send("1201"));
    TEST_ASSERT_EQUAL_HEX8(0xFF, rig.state().preset);
}

// "rev ... increments on every accepted command other than IDENTIFY and
// GET_*, even when nothing visible changes ... Each increment is followed by a
// STATE notification, so STATE doubles as the acknowledgement."
void test_rev_counts_commands_with_no_visible_effect() {
    Rig rig;
    ASSERT_OK(rig.send("100000"));  // slot 0
    ASSERT_OK(rig.send("100100"));  // slot 1: the marked slot
    rig.engine.takeStateChanged();
    uint16_t rev = rig.state().rev;
    uint8_t before[kMaxFrame];
    const size_t before_len = rig.engine.encodeState(before, sizeof before);

    // Deleting a slot that is not the marked one leaves STATE as it was, rev aside.
    ASSERT_OK(rig.send("1200"));
    TEST_ASSERT_EQUAL(++rev, rig.state().rev);
    TEST_ASSERT_TRUE_MESSAGE(rig.engine.takeStateChanged(), "STATE is the acknowledgement");
    uint8_t after[kMaxFrame];
    TEST_ASSERT_EQUAL(before_len, rig.engine.encodeState(after, sizeof after));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before + 4, after + 4, before_len - 4);
    TEST_ASSERT_EQUAL_MESSAGE(1, rig.state().preset, "the marked slot is another one");

    // Saving over the marked slot again, and loading what is already playing.
    ASSERT_OK(rig.send("100100"));
    TEST_ASSERT_EQUAL(++rev, rig.state().rev);
    TEST_ASSERT_TRUE(rig.engine.takeStateChanged());
    ASSERT_OK(rig.send("1101"));
    TEST_ASSERT_EQUAL(++rev, rig.state().rev);
    TEST_ASSERT_TRUE(rig.engine.takeStateChanged());

    // A rejected command is not acknowledged by STATE.
    ASSERT_ERR(ErrorCode::NotFound, rig.send("1200"));
    TEST_ASSERT_EQUAL(rev, rig.state().rev);
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());
}

// "preset is cleared to 0xFF when brightness, segments or any EffectConfig
// actually change value": sending what is already there keeps the marker.
void test_preset_marker_survives_same_values() {
    Rig rig;
    ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 1}}));
    ASSERT_OK(rig.setEffect(1, kComet, kRed, kBlue, 150, 60, 3));
    ASSERT_OK(rig.send("0240"));
    ASSERT_OK(rig.send("100500"));  // slot 5
    uint16_t rev = rig.state().rev;

    ASSERT_OK(rig.send("0240"));                                        // the same brightness
    ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 1}}));       // the same layout
    ASSERT_OK(rig.setEffect(1, kComet, kRed, kBlue, 150, 60, 3));       // the same EffectConfig
    ASSERT_OK(rig.send("040101" "01"));                                 // the same effect
    ASSERT_OK(rig.send("040102" "03"));                                 // the same palette
    ASSERT_OK(rig.send("040103" "96"));                                 // the same speed
    ASSERT_OK(rig.send("040104" "3c"));                                 // the same intensity
    ASSERT_OK(rig.send("040105" "ff0000"));                             // the same color1
    ASSERT_OK(rig.send("040106" "0000ff"));                             // the same color2
    rev = static_cast<uint16_t>(rev + 9);
    TEST_ASSERT_EQUAL_MESSAGE(5, rig.state().preset, "nothing changed value");
    TEST_ASSERT_EQUAL_MESSAGE(rev, rig.state().rev, "each command still counts");

    // 0xFF with a value only some segments already have is a change.
    ASSERT_OK(rig.send("04ff03" "96"));
    TEST_ASSERT_EQUAL_HEX8(0xFF, rig.state().preset);
}

// "geometry length 0 or start + length past the end of the strip (also for
// IDENTIFY, unless it is a cancel)" is code 3; "length 0 or seconds 0 cancels
// it" and a cancel is never rejected. "A new IDENTIFY replaces a running one."
void test_identify_range_validation() {
    Rig rig;
    ASSERT_OK(rig.setEffect(0, kSolid, kRed));
    const uint16_t rev = rig.state().rev;
    rig.engine.takeStateChanged();
    rig.sink.frames.clear();

    const char* const rejected[] = {
        "06030a8705",  // start 10 + length 135 = 145
        "0601900105",  // start 144
        "0602009105",  // length 145
        "0603ff0105",  // start 255
        "06000a0a05",  // strip_mask 0
        "06040a0a05",  // unknown strip bit
    };
    for (const char* hex : rejected) {
        ASSERT_ERR(ErrorCode::OutOfRange, rig.send(std::string(hex)));
        TEST_ASSERT_EQUAL_STRING_MESSAGE("7f0603", rig.lastReply().c_str(), hex);
        TEST_ASSERT_FALSE_MESSAGE(rig.engine.identifyActive(), hex);
    }

    // The limits themselves are fine.
    ASSERT_OK(rig.send("0603009001"));  // the whole strip
    ASSERT_OK(rig.send("06018f0101"));  // the last pixel
    TEST_ASSERT_TRUE(rig.engine.identifyActive());

    // A running overlay is replaced, not added to.
    ASSERT_OK(rig.send("0601000a05"));  // left, pixels 0..9
    ASSERT_OK(rig.send("0602140a05"));  // right, pixels 20..29
    rig.render(1000);
    TEST_ASSERT_RGB(kRed, rig.pic.left[0]);
    TEST_ASSERT_TRUE(rig.pic.right[20] != kRed);
    TEST_ASSERT_RGB(kRed, rig.pic.right[19]);

    // A cancel is accepted whatever its other fields say.
    const char* const cancels[] = {"0600000000", "0600ff0005", "06ffffff00", "0603900500", "0607000000"};
    for (const char* hex : cancels) {
        ASSERT_OK(rig.send("0603000a05"));
        ASSERT_OK(rig.send(std::string(hex)));
        TEST_ASSERT_FALSE_MESSAGE(rig.engine.identifyActive(), hex);
    }
    TEST_ASSERT_EQUAL_MESSAGE(6, rig.sink.frames.size(), "only the six rejections were answered");
    TEST_ASSERT_EQUAL_MESSAGE(rev, rig.state().rev, "IDENTIFY never changes rev");
    TEST_ASSERT_FALSE(rig.engine.takeStateChanged());
}

// "The effect state of a segment restarts when its effect or length changes,
// or when a preset is loaded."
void test_effect_restarts_on_length_change_and_preset_load() {
    Rig rig;
    ASSERT_OK(rig.setEffect(0, kComet, kRed, kRed, 128, 0));  // one-pixel comet, 62.5 px/s
    rig.render(1000);
    rig.render(1800);
    TEST_ASSERT_RGB(kRed, rig.pic.left[50]);

    // Moving the segment without changing its length keeps the effect running ...
    ASSERT_OK(rig.setSegments({{1, 0, 144, 0}}));
    rig.render(1960);
    TEST_ASSERT_RGB(kRed, rig.pic.left[60]);
    TEST_ASSERT_RGB(kBlack, rig.pic.right[60]);

    // ... a new length restarts it ...
    ASSERT_OK(rig.setSegments({{1, 0, 100, 0}}));
    rig.render(2000);
    TEST_ASSERT_RGB(kRed, rig.pic.left[0]);
    TEST_ASSERT_RGB(kBlack, rig.pic.left[60]);
    rig.render(2800);
    TEST_ASSERT_RGB(kRed, rig.pic.left[50]);

    // ... and so does loading a preset, even the one that is already playing.
    ASSERT_OK(rig.send("100100"));
    ASSERT_OK(rig.send("1101"));
    rig.render(3000);
    TEST_ASSERT_RGB(kRed, rig.pic.left[0]);
    TEST_ASSERT_RGB(kBlack, rig.pic.left[50]);
}

// ---- Boot options and configuration ---------------------------------------------------

void test_default_brightness_is_configurable() {
    MemoryStorage storage;
    EngineConfig config;
    config.default_brightness = 40;
    Engine engine(storage, config);
    engine.begin();
    TEST_ASSERT_EQUAL(40, engine.brightness());
    TEST_ASSERT_EQUAL(900, engine.budgetMa());
    TEST_ASSERT_TRUE(engine.power());
    TEST_ASSERT_FALSE(engine.needsSave());

    // A stored scene wins over the default.
    CaptureSink sink;
    const uint8_t brightness_200[] = {0x02, 0xC8};
    engine.handleCommand(brightness_200, sizeof brightness_200, 0, sink);
    TEST_ASSERT_TRUE(engine.save());
    Engine again(storage, config);
    again.begin();
    TEST_ASSERT_EQUAL(200, again.brightness());
}

// Safe mode: the stored scene is not restored, but settings and presets are,
// and the stored scene is still there for the next normal boot.
void test_boot_without_restoring_the_scene() {
    MemoryStorage storage;
    {
        Rig rig(storage);
        ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 1}}));
        ASSERT_OK(rig.setEffect(0xFF, kMatrix, kGreen, kGreen));
        ASSERT_OK(rig.send("0221"));
        ASSERT_OK(rig.send("30dc05"));  // 1500 mA
        ASSERT_OK(rig.send("100300"));  // preset 3
        TEST_ASSERT_TRUE(rig.engine.save());
    }
    Engine engine(storage);
    BootOptions safe;
    safe.restore_scene = false;
    engine.begin(safe);
    TEST_ASSERT_EQUAL(1, engine.scene().segment_count);
    TEST_ASSERT_EQUAL(kCylon, engine.scene().segments[0].config.effect);
    TEST_ASSERT_EQUAL(128, engine.brightness());
    TEST_ASSERT_EQUAL_HEX8(0xFF, engine.preset());
    TEST_ASSERT_EQUAL_MESSAGE(1500, engine.budgetMa(), "the budget is a safety limit: keep it");
    TEST_ASSERT_TRUE(engine.power());
    TEST_ASSERT_EQUAL(0, engine.rev());
    TEST_ASSERT_FALSE_MESSAGE(engine.needsSave(), "safe mode must not overwrite the stored scene by itself");
    CaptureSink sink;
    const uint8_t get_presets[] = {0x13};
    engine.handleCommand(get_presets, 1, 0, sink);
    TEST_ASSERT_EQUAL_STRING("04010300", support::toHex(sink.frames.back()).c_str());

    Rig normal(storage);
    TEST_ASSERT_EQUAL(2, normal.state().segment_count);
    TEST_ASSERT_EQUAL(0x21, normal.state().brightness);
    TEST_ASSERT_EQUAL(3, normal.state().preset);
}

// Development builds and safe mode: dev mode on at boot without touching the
// stored setting or rev.
void test_forced_dev_mode() {
    MemoryStorage storage;
    BootOptions forced;
    forced.force_dev_mode = true;
    {
        Engine engine(storage);
        engine.begin(forced);
        TEST_ASSERT_TRUE(engine.devMode());
        TEST_ASSERT_EQUAL_MESSAGE(0, engine.rev(), "rev starts at 0 at boot");
        TEST_ASSERT_FALSE(engine.takeStateChanged());
        TEST_ASSERT_FALSE(engine.needsSave());
        uint8_t f[kMaxFrame];
        engine.encodeState(f, sizeof f);
        TEST_ASSERT_EQUAL_HEX8(0x01, f[8]);

        // Saving something else must not make the forced setting permanent.
        CaptureSink sink;
        const uint8_t budget_1500[] = {0x30, 0xDC, 0x05};
        engine.handleCommand(budget_1500, sizeof budget_1500, 0, sink);
        TEST_ASSERT_TRUE(engine.save());
    }
    {
        Rig rig(storage);
        TEST_ASSERT_EQUAL(1500, rig.state().budget_ma);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, rig.state().flags, "the stored setting was never on");
    }
    // SET_DEV_MODE still decides for the rest of the session ...
    Engine engine(storage);
    engine.begin(forced);
    CaptureSink sink;
    const uint8_t dev_off[] = {0x31, 0x00};
    const uint8_t dev_on[] = {0x31, 0x01};
    engine.handleCommand(dev_off, sizeof dev_off, 0, sink);
    TEST_ASSERT_FALSE(engine.devMode());
    TEST_ASSERT_EQUAL(1, engine.rev());
    // ... and turning it on by command is stored like on any build.
    engine.handleCommand(dev_on, sizeof dev_on, 0, sink);
    TEST_ASSERT_TRUE(engine.devMode());
    TEST_ASSERT_TRUE(engine.needsSave());
    TEST_ASSERT_TRUE(engine.save());
    Rig rig(storage);
    TEST_ASSERT_EQUAL_HEX8(0x01, rig.state().flags);
}

// ---- Crossfade ------------------------------------------------------------------------
//
// EngineConfig::crossfade_ms: "when the picture would cut (power on or off, a
// preset loaded, a segment given another effect, the layout changed) the last
// frame is blended into the new picture". Most tests here run a twin engine
// without the fade next to the one under test: after the fade the two must
// show exactly the same picture, and during it every pixel must lie between
// the frame that was on the strips and what the twin shows.

constexpr uint16_t kFadeMs = 400;

EngineConfig withFade() {
    EngineConfig config;
    config.crossfade_ms = kFadeMs;
    return config;
}

struct FadePair {
    Rig fading{withFade()};
    Rig cutting;  // the twin: no crossfade

    void send(const std::string& hex) {
        ASSERT_OK(fading.send(hex));
        ASSERT_OK(cutting.send(hex));
    }
    void render(uint32_t at) {
        fading.render(at);
        cutting.render(at);
    }
    // Lets a fade that may be running finish.
    void settle(uint32_t at) {
        render(at);
        render(at + kFadeMs + 1);
        TEST_ASSERT_FALSE(fading.engine.fading());
        TEST_ASSERT_TRUE_MESSAGE(fading.pic == cutting.pic, "settled: with or without the crossfade, the same picture");
    }
};

bool between(uint8_t v, uint8_t a, uint8_t b) { return v >= (a < b ? a : b) && v <= (a < b ? b : a); }

void assertBetween(const Picture& mixed, const Picture& from, const Picture& to, const char* what) {
    for (uint16_t i = 0; i < kLedsPerStrip; ++i) {
        for (int strip = 0; strip < 2; ++strip) {
            const Rgb m = strip ? mixed.right[i] : mixed.left[i];
            const Rgb a = strip ? from.right[i] : from.left[i];
            const Rgb b = strip ? to.right[i] : to.left[i];
            TEST_ASSERT_TRUE_MESSAGE(between(m.r, a.r, b.r) && between(m.g, a.g, b.g) && between(m.b, a.b, b.b), what);
        }
    }
}

// Sends `hex` (a change that must not cut) and follows the fade frame by frame.
void expectFade(FadePair& pair, uint32_t at, const std::string& hex, const char* what) {
    pair.settle(at);
    at += kFadeMs + 100;
    pair.render(at);
    const Picture before = pair.fading.pic;  // what is on the strips

    pair.send(hex);
    TEST_ASSERT_TRUE_MESSAGE(pair.fading.engine.fading(), what);
    TEST_ASSERT_TRUE_MESSAGE(pair.fading.engine.needsRender(), what);

    bool differed_from_both = false;
    for (uint32_t elapsed = 0; elapsed < kFadeMs; elapsed += 16) {
        pair.render(at + 20 + elapsed);
        TEST_ASSERT_TRUE_MESSAGE(pair.fading.animating, "render() reports movement for the whole fade");
        TEST_ASSERT_TRUE_MESSAGE(pair.fading.engine.needsRender(), "needsRender() stays true for the whole fade");
        if (elapsed == 0) TEST_ASSERT_TRUE_MESSAGE(pair.fading.pic == before, "the fade starts from the frame that was showing");
        assertBetween(pair.fading.pic, before, pair.cutting.pic, what);
        differed_from_both = differed_from_both || (pair.fading.pic != before && pair.fading.pic != pair.cutting.pic);
    }
    TEST_ASSERT_TRUE_MESSAGE(differed_from_both, "there should have been frames in between");

    pair.render(at + 20 + kFadeMs);
    TEST_ASSERT_FALSE_MESSAGE(pair.fading.engine.fading(), what);
    TEST_ASSERT_TRUE_MESSAGE(pair.fading.pic == pair.cutting.pic, "after the fade: exactly the new picture");
    TEST_ASSERT_EQUAL_MESSAGE(pair.cutting.animating, pair.fading.animating, "and the idle path is back if the picture is static");
    TEST_ASSERT_EQUAL(pair.cutting.engine.needsRender(), pair.fading.engine.needsRender());
}

// Sends `hex` (a slider-type change) and expects the very next frame to show it.
void expectCut(FadePair& pair, uint32_t at, const std::string& hex, const char* what) {
    pair.settle(at);
    pair.send(hex);
    TEST_ASSERT_FALSE_MESSAGE(pair.fading.engine.fading(), what);
    pair.render(at + kFadeMs + 20);
    TEST_ASSERT_TRUE_MESSAGE(pair.fading.pic == pair.cutting.pic, what);
}

void test_power_fades_out_and_in() {
    Rig rig{withFade()};
    ASSERT_OK(rig.setEffect(0, kSolid, kRed));
    rig.render(1000);
    rig.render(1000 + kFadeMs);
    TEST_ASSERT_RGB(kRed, rig.pic.left[7]);
    TEST_ASSERT_FALSE(rig.animating);

    // Off: down to black over kFadeMs, then nothing more to render.
    ASSERT_OK(rig.send("0100"));
    TEST_ASSERT_TRUE(rig.engine.needsRender());
    int last = 256;
    for (uint32_t elapsed = 0; elapsed < kFadeMs; elapsed += 20) {
        rig.render(2000 + elapsed);
        TEST_ASSERT_TRUE(rig.animating);
        const int red = rig.pic.left[7].r;
        TEST_ASSERT_INT_WITHIN_MESSAGE(2, 255 - static_cast<int>(elapsed * 255 / kFadeMs), red, "a straight line down");
        TEST_ASSERT_LESS_OR_EQUAL(last, red);
        TEST_ASSERT_EQUAL(0, rig.pic.left[7].g);
        TEST_ASSERT_RGB(rig.pic.left[7], rig.pic.right[100]);
        last = red;
    }
    rig.render(2000 + kFadeMs);
    TEST_ASSERT_TRUE(rig.pic.allBlack());
    TEST_ASSERT_FALSE(rig.animating);
    TEST_ASSERT_FALSE_MESSAGE(rig.engine.needsRender(), "off and faded: the loop can idle");
    TEST_ASSERT_FALSE(rig.engine.fading());
    TEST_ASSERT_EQUAL(0, rig.state().power);

    // On: up from black.
    ASSERT_OK(rig.send("0101"));
    last = -1;
    for (uint32_t elapsed = 0; elapsed < kFadeMs; elapsed += 20) {
        rig.render(5000 + elapsed);
        TEST_ASSERT_TRUE(rig.animating);
        const int red = rig.pic.left[7].r;
        TEST_ASSERT_INT_WITHIN(2, static_cast<int>(elapsed * 255 / kFadeMs), red);
        TEST_ASSERT_GREATER_THAN(last, red);
        last = red;
    }
    rig.render(5000 + kFadeMs);
    TEST_ASSERT_RGB(kRed, rig.pic.left[7]);
    TEST_ASSERT_FALSE(rig.animating);
    TEST_ASSERT_FALSE(rig.engine.needsRender());

    // Power commands that change nothing do not fade.
    ASSERT_OK(rig.send("0101"));
    TEST_ASSERT_FALSE(rig.engine.fading());
}

void test_abrupt_changes_fade() {
    FadePair pair;
    pair.send("0502" "01009000" "02009001");         // left strip, right strip reversed
    pair.send("0300" "00008080" "ff0000" "0000ff");  // segment 0: Solid red
    pair.send("0301" "01008080" "00ff00" "0000ff");  // segment 1: Comet green
    pair.send("0240");
    pair.send("100000");                             // preset 0: this scene
    uint32_t at = 1000;

    expectFade(pair, at, "0100", "power off");
    expectFade(pair, at += 2000, "0101", "power on");
    expectFade(pair, at += 2000, "0300" "04008080" "ff0000" "0000ff", "SET_EFFECT with another effect (Breathe)");
    expectFade(pair, at += 2000, "040101" "02", "SET_PARAM effect (Rainbow)");
    expectFade(pair, at += 2000, "0501030090" "00", "SET_SEGMENTS with another layout");
    expectFade(pair, at += 2000, "1100", "PRESET_LOAD");
    expectFade(pair, at += 2000, "1100", "PRESET_LOAD of the scene already playing: the effects restart");
    expectFade(pair, at += 2000, "03ff" "06008080" "00ffcc" "ff00ff", "SET_EFFECT on every segment");
}

void test_slider_changes_stay_immediate() {
    FadePair pair;
    pair.send("0502" "01009000" "02009001");
    pair.send("0300" "00008080" "ff0000" "0000ff");  // Solid
    pair.send("0301" "01008080" "00ff00" "0000ff");  // Comet
    uint32_t at = 1000;
    expectCut(pair, at, "0210", "brightness");
    expectCut(pair, at += 1000, "040103" "a0", "speed");
    expectCut(pair, at += 1000, "040104" "20", "intensity");
    expectCut(pair, at += 1000, "040005" "00ffff", "color1");
    expectCut(pair, at += 1000, "040106" "ff00ff", "color2");
    expectCut(pair, at += 1000, "040102" "03", "palette");
    expectCut(pair, at += 1000, "0300" "00008080" "123456" "0000ff", "SET_EFFECT keeping the effect");
    expectCut(pair, at += 1000, "040001" "00", "SET_PARAM effect with the effect it already has");
    expectCut(pair, at += 1000, "0502" "01009000" "02009001", "SET_SEGMENTS with the layout it already has");
    expectCut(pair, at += 1000, "30dc05", "budget");
    expectCut(pair, at += 1000, "3101", "dev mode");
    expectCut(pair, at += 1000, "100300", "PRESET_SAVE");
    expectCut(pair, at += 1000, "1203", "PRESET_DELETE");
    expectCut(pair, at += 1000, "0601000a05", "IDENTIFY");
    expectCut(pair, at += 1000, "0601000000", "IDENTIFY cancel");
}

// A second change during a fade carries on from the frame the fade had
// reached: nothing jumps.
void test_fade_restarts_from_where_it_was() {
    Rig rig{withFade()};
    ASSERT_OK(rig.setEffect(0, kSolid, kRed));
    rig.render(1000);
    rig.render(1000 + kFadeMs);

    ASSERT_OK(rig.send("0100"));
    int last = 255;
    int biggest = 0;
    uint32_t t = 2000;
    auto frame = [&] {
        rig.render(t);
        const int red = rig.pic.left[0].r;
        if (std::abs(red - last) > biggest) biggest = std::abs(red - last);
        last = red;
        t += 16;
    };
    for (int i = 0; i < 12; ++i) frame();  // about half-way down
    TEST_ASSERT_INT_WITHIN(15, 143, last);
    ASSERT_OK(rig.send("0101"));           // changed my mind
    for (int i = 0; i < 12; ++i) frame();  // on the way up again, from where it was
    TEST_ASSERT_GREATER_THAN(170, last);
    ASSERT_OK(rig.send("0100"));
    for (int i = 0; i < 40; ++i) frame();
    TEST_ASSERT_EQUAL(0, last);
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(12, biggest, "16 ms of a 400 ms fade is 10 or 11 steps of 255");
    TEST_ASSERT_FALSE(rig.engine.needsRender());
}

// While the jacket is off nothing visible changes, so nothing fades; and the
// IDENTIFY overlay is neither faded nor remembered as part of the picture.
void test_fade_ignores_what_cannot_be_seen() {
    Rig rig{withFade()};
    ASSERT_OK(rig.setEffect(0, kSolid, kRed));
    rig.render(1000);
    rig.render(1000 + kFadeMs);
    ASSERT_OK(rig.send("100000"));
    ASSERT_OK(rig.send("0100"));
    rig.render(2000);
    rig.render(2000 + kFadeMs);
    TEST_ASSERT_TRUE(rig.pic.allBlack());

    // Off: a new effect, a new layout, a preset. No fade, one black frame, idle.
    ASSERT_OK(rig.setEffect(0, kComet, kGreen, kBlue));
    TEST_ASSERT_FALSE(rig.engine.fading());
    ASSERT_OK(rig.setSegments({{1, 0, 144, 0}, {2, 0, 144, 1}}));
    TEST_ASSERT_FALSE(rig.engine.fading());
    ASSERT_OK(rig.send("1100"));
    TEST_ASSERT_FALSE(rig.engine.fading());
    rig.render(3000);
    TEST_ASSERT_TRUE(rig.pic.allBlack());
    TEST_ASSERT_FALSE(rig.animating);
    TEST_ASSERT_FALSE(rig.engine.needsRender());

    // IDENTIFY while off: dim white at once, gone at once.
    ASSERT_OK(rig.send("0601000a05"));
    rig.render(3100);
    TEST_ASSERT_TRUE(rig.pic.left[0] != kBlack);
    TEST_ASSERT_RGB(kBlack, rig.pic.left[10]);
    // Power on underneath it: the picture fades in from black, not from the overlay.
    ASSERT_OK(rig.send("0101"));
    rig.render(3200);
    const Rgb overlay = rig.pic.left[0];
    TEST_ASSERT_EQUAL(overlay.r, overlay.g);  // still the white overlay, not red
    TEST_ASSERT_RGB(kBlack, rig.pic.left[10]);
    ASSERT_OK(rig.send("0601000000"));
    rig.render(3216);
    TEST_ASSERT_LESS_THAN_MESSAGE(20, rig.pic.left[0].r, "under the overlay the fade had only just begun");
    TEST_ASSERT_EQUAL(0, rig.pic.left[0].g);
    rig.render(3200 + kFadeMs);
    TEST_ASSERT_RGB(kRed, rig.pic.left[0]);
    TEST_ASSERT_FALSE(rig.engine.needsRender());
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_state_frame_layout);
    RUN_TEST(test_set_power);
    RUN_TEST(test_set_brightness);
    RUN_TEST(test_set_effect);
    RUN_TEST(test_set_effect_rejections);
    RUN_TEST(test_set_param);
    RUN_TEST(test_set_segments_inheritance);
    RUN_TEST(test_budget_is_clamped);
    RUN_TEST(test_flags);
    RUN_TEST(test_state_change_is_coalesced);
    RUN_TEST(test_rev_wraps);
    RUN_TEST(test_error_replies);
    RUN_TEST(test_get_info);
    RUN_TEST(test_effect_catalogue);
    RUN_TEST(test_palette_catalogue);
    RUN_TEST(test_catalogue_start_offsets);
    RUN_TEST(test_power_off_renders_black_and_static);
    RUN_TEST(test_static_scene_stops_refreshing);
    RUN_TEST(test_segment_strip_mask_and_range);
    RUN_TEST(test_overlap_higher_index_wins);
    RUN_TEST(test_reversed_segment_is_mirrored);
    RUN_TEST(test_effect_restarts_when_effect_changes);
    RUN_TEST(test_effect_time_pauses_while_off);
    RUN_TEST(test_identify_overlay_and_timeout);
    RUN_TEST(test_identify_over_a_playing_scene);
    RUN_TEST(test_identify_cancel_and_tick);
    RUN_TEST(test_identify_is_equally_dim_at_any_brightness);
    RUN_TEST(test_live_input_and_timeout);
    RUN_TEST(test_short_live_frame_is_ignored);
    RUN_TEST(test_live_timeout_survives_clock_wrap);
    RUN_TEST(test_boot_other_slot);
    RUN_TEST(test_local_sound_alone);
    RUN_TEST(test_phone_sound_wins_over_the_microphone);
    RUN_TEST(test_phone_motion_with_the_jackets_own_hearing);
    RUN_TEST(test_phone_that_stops_sending_audio_hands_over_to_the_microphone);
    RUN_TEST(test_without_a_microphone_the_phone_speaks_for_everything);
    RUN_TEST(test_beat_clock_follows_one_sender_at_a_time);
    RUN_TEST(test_sound_wanted);
    RUN_TEST(test_preset_save_load_delete_list);
    RUN_TEST(test_preset_marker_follows_scene_content);
    RUN_TEST(test_presets_survive_reboot);
    RUN_TEST(test_last_scene_and_settings_survive_reboot);
    RUN_TEST(test_unsaved_changes_are_lost_on_reboot);
    RUN_TEST(test_what_needs_saving);
    RUN_TEST(test_storage_failures);
    RUN_TEST(test_corrupt_storage_falls_back_to_defaults);
    RUN_TEST(test_stored_unknown_effect_is_replaced);
    RUN_TEST(test_preset_save_and_delete_answer_with_presets);
    RUN_TEST(test_storage_failure_is_code_5);
    RUN_TEST(test_validation_section);
    RUN_TEST(test_rev_rules);
    RUN_TEST(test_preset_marker_rules);
    RUN_TEST(test_rev_counts_commands_with_no_visible_effect);
    RUN_TEST(test_preset_marker_survives_same_values);
    RUN_TEST(test_identify_range_validation);
    RUN_TEST(test_effect_restarts_on_length_change_and_preset_load);
    RUN_TEST(test_default_brightness_is_configurable);
    RUN_TEST(test_boot_without_restoring_the_scene);
    RUN_TEST(test_forced_dev_mode);
    RUN_TEST(test_power_fades_out_and_in);
    RUN_TEST(test_abrupt_changes_fade);
    RUN_TEST(test_slider_changes_stay_immediate);
    RUN_TEST(test_fade_restarts_from_where_it_was);
    RUN_TEST(test_fade_ignores_what_cannot_be_seen);
    return UNITY_END();
}
