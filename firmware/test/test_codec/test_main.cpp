// Codec robustness: malformed frames must produce the error codes of
// PROTOCOL.md and must never be read past their end.
//
// Expectations here come from the protocol document (and, where it is
// silent, from the rules written down in jacket/protocol.h). Frames are
// decoded from GuardedBytes: the byte after the frame is in an unreadable
// page, so an out-of-bounds read crashes the suite.

#include <string>

#include "../support/test_support.h"

using namespace jacket;
using support::Bytes;
using support::GuardedBytes;
using support::fromHex;

void setUp() {}
void tearDown() {}

namespace {

ErrorCode decodeGuarded(const Bytes& frame, Command& out) {
    GuardedBytes guarded(frame);
    TEST_ASSERT_TRUE_MESSAGE(guarded.ok(), "could not set up the guard page");
    return decodeCommand(guarded.data(), guarded.size(), out);
}

ErrorCode decodeGuarded(const std::string& hex) {
    Command cmd;
    return decodeGuarded(fromHex(hex), cmd);
}

#define ASSERT_CODE(expected, hex) \
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(expected), static_cast<int>(decodeGuarded(hex)), hex)

// One well-formed frame per opcode (and per SET_PARAM value width).
const char* const kValidCommands[] = {
    "0101",                          // SET_POWER
    "0280",                          // SET_BRIGHTNESS
    "03ff010080c800ffccff00ff",      // SET_EFFECT
    "040003c8",                      // SET_PARAM speed
    "040105ff0080",                  // SET_PARAM color1
    "05020100900002009001",          // SET_SEGMENTS
    "06031e0a05",                    // IDENTIFY
    "1002044e656f6e",                // PRESET_SAVE
    "1102",                          // PRESET_LOAD
    "1202",                          // PRESET_DELETE
    "13",                            // GET_PRESETS
    "20",                            // GET_INFO
    "210000",                        // GET_CATALOG
    "308403",                        // SET_BUDGET
    "3101",                          // SET_DEV_MODE
    "325a",                          // BOOT_OTHER_SLOT
};

const uint8_t kOpcodes[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x30, 0x31, 0x32};

bool isOpcode(uint8_t byte) {
    for (uint8_t op : kOpcodes) {
        if (op == byte) return true;
    }
    return false;
}

// ---- Commands -------------------------------------------------------------------

void test_valid_commands_decode() {
    for (const char* hex : kValidCommands) ASSERT_CODE(ErrorCode::None, hex);
}

void test_empty_frame_is_too_short() {
    ASSERT_CODE(ErrorCode::TooShort, "");
    Command cmd;
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::TooShort), static_cast<int>(decodeCommand(nullptr, 0, cmd)));
}

// "replies ERROR to frames that are too short": every proper prefix of a
// valid frame that still has its opcode is too short.
void test_every_truncation_is_too_short() {
    for (const char* hex : kValidCommands) {
        const Bytes full = fromHex(hex);
        for (size_t len = 1; len < full.size(); ++len) {
            const Bytes cut(full.begin(), full.begin() + len);
            Command cmd;
            const ErrorCode code = decodeGuarded(cut, cmd);
            char msg[96];
            snprintf(msg, sizeof msg, "%s cut to %d bytes", hex, static_cast<int>(len));
            TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(ErrorCode::TooShort), static_cast<int>(code), msg);
        }
    }
}

// "A receiver ignores trailing bytes it does not understand."
void test_trailing_bytes_are_ignored() {
    for (const char* hex : kValidCommands) {
        const Bytes plain = fromHex(hex);
        Bytes padded = plain;
        padded.insert(padded.end(), {0xDE, 0xAD, 0xBE, 0xEF, 0xFF});
        Command a, b;
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(ErrorCode::None), static_cast<int>(decodeGuarded(plain, a)), hex);
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(ErrorCode::None), static_cast<int>(decodeGuarded(padded, b)), hex);
        // Same meaning: both re-encode to the frame without the padding.
        uint8_t out_a[kMaxFrame], out_b[kMaxFrame];
        const size_t na = encodeCommand(a, out_a, sizeof out_a);
        const size_t nb = encodeCommand(b, out_b, sizeof out_b);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(hex, support::toHex(out_a, na).c_str(), "plain frame does not round-trip");
        TEST_ASSERT_EQUAL_STRING_MESSAGE(hex, support::toHex(out_b, nb).c_str(), "padding changed the decoded command");
    }
}

void test_unknown_opcodes() {
    int unknown = 0;
    for (int op = 0; op < 256; ++op) {
        Bytes frame(24, 0x01);  // long enough for any command
        frame[0] = static_cast<uint8_t>(op);
        Command cmd;
        const ErrorCode code = decodeGuarded(frame, cmd);
        if (isOpcode(static_cast<uint8_t>(op))) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<int>(ErrorCode::UnknownOpcode), static_cast<int>(code), "known opcode");
        } else {
            TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(ErrorCode::UnknownOpcode), static_cast<int>(code), "unknown opcode");
            ++unknown;
        }
        // An unknown opcode is unknown whatever its length.
        if (!isOpcode(static_cast<uint8_t>(op))) {
            const Bytes one(1, static_cast<uint8_t>(op));
            TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::UnknownOpcode), static_cast<int>(decodeGuarded(one, cmd)));
        }
    }
    TEST_ASSERT_EQUAL(256 - 15, unknown);
}

void test_out_of_range_values() {
    // on: 0/1 only
    ASSERT_CODE(ErrorCode::OutOfRange, "0102");
    ASSERT_CODE(ErrorCode::OutOfRange, "31ff");
    // BOOT_OTHER_SLOT: the confirmation byte, and nothing else
    ASSERT_CODE(ErrorCode::OutOfRange, "3200");
    ASSERT_CODE(ErrorCode::OutOfRange, "3201");
    ASSERT_CODE(ErrorCode::OutOfRange, "32a5");
    ASSERT_CODE(ErrorCode::OutOfRange, "32ff");
    for (int value = 0; value < 256; ++value) {
        const Bytes frame{0x32, static_cast<uint8_t>(value)};
        Command cmd;
        TEST_ASSERT_EQUAL(static_cast<int>(value == 0x5A ? ErrorCode::None : ErrorCode::OutOfRange), static_cast<int>(decodeGuarded(frame, cmd)));
    }
    // segment selector: 0..7 or 0xFF
    ASSERT_CODE(ErrorCode::OutOfRange, "0308010080c800ffccff00ff");
    ASSERT_CODE(ErrorCode::OutOfRange, "03fe010080c800ffccff00ff");
    ASSERT_CODE(ErrorCode::OutOfRange, "040803c8");
    // SET_PARAM ids are 1..6
    ASSERT_CODE(ErrorCode::OutOfRange, "040000c8");
    ASSERT_CODE(ErrorCode::OutOfRange, "040007c8");
    // SET_SEGMENTS: 1..8 segments
    ASSERT_CODE(ErrorCode::OutOfRange, "0500");
    ASSERT_CODE(ErrorCode::OutOfRange, "0509" "01009000" "01009000" "01009000" "01009000" "01009000" "01009000" "01009000" "01009000" "01009000");
    // geometry: strip_mask 1..3
    ASSERT_CODE(ErrorCode::OutOfRange, "050100009000");
    ASSERT_CODE(ErrorCode::OutOfRange, "050104009000");
    // geometry: length 1..144
    ASSERT_CODE(ErrorCode::OutOfRange, "050101000000");
    ASSERT_CODE(ErrorCode::OutOfRange, "050101009100");
    // geometry: start + length <= 144
    ASSERT_CODE(ErrorCode::OutOfRange, "050101019000");
    ASSERT_CODE(ErrorCode::OutOfRange, "050101900100");
    // geometry: only flag bit 0 is defined
    ASSERT_CODE(ErrorCode::OutOfRange, "050101009002");
    // a bad geometry anywhere in the list
    ASSERT_CODE(ErrorCode::OutOfRange, "05020100900002909001");
    // preset slots are 0..7
    ASSERT_CODE(ErrorCode::OutOfRange, "1008044e656f6e");
    ASSERT_CODE(ErrorCode::OutOfRange, "1108");
    ASSERT_CODE(ErrorCode::OutOfRange, "12ff");
    // preset name: at most 16 bytes
    ASSERT_CODE(ErrorCode::OutOfRange, "100011" "4141414141414141414141414141414141");
    // catalogue kind 0/1
    ASSERT_CODE(ErrorCode::OutOfRange, "210200");
    // IDENTIFY range must exist and fit
    ASSERT_CODE(ErrorCode::OutOfRange, "06001e0a05");
    ASSERT_CODE(ErrorCode::OutOfRange, "06041e0a05");
    ASSERT_CODE(ErrorCode::OutOfRange, "0603900105");
    ASSERT_CODE(ErrorCode::OutOfRange, "0603009105");
}

void test_limits_are_accepted() {
    ASSERT_CODE(ErrorCode::None, "0100");
    ASSERT_CODE(ErrorCode::None, "02ff");
    ASSERT_CODE(ErrorCode::None, "0307ffffffffffffffffffff");  // ids are checked by the engine, not the codec
    // Eight segments, including the last pixel alone and a range ending exactly at 144.
    ASSERT_CODE(ErrorCode::None, "0508" "01009000" "02009001" "03009000" "018f0100" "02000101" "03484800" "01004800" "03484801");
    ASSERT_CODE(ErrorCode::None, "0501018f0100");
    // A 16-byte name, an empty name, the last slot.
    ASSERT_CODE(ErrorCode::None, "100710" "41414141414141414141414141414141");
    ASSERT_CODE(ErrorCode::None, "100000");
    ASSERT_CODE(ErrorCode::None, "1107");
    // Budget is clamped by the engine, never rejected.
    ASSERT_CODE(ErrorCode::None, "300000");
    ASSERT_CODE(ErrorCode::None, "30ffff");
    // IDENTIFY cancel: length 0 or seconds 0, the other fields do not matter.
    ASSERT_CODE(ErrorCode::None, "0600ff0005");
    ASSERT_CODE(ErrorCode::None, "06ffffff00");
}

// Where a bad value makes the needed length unknowable, the value is what
// is reported (protocol.h); otherwise "too short" wins.
void test_error_precedence() {
    ASSERT_CODE(ErrorCode::OutOfRange, "0509");            // 9 segments, none present
    ASSERT_CODE(ErrorCode::OutOfRange, "040007");          // unknown param, no value
    ASSERT_CODE(ErrorCode::OutOfRange, "100011");          // name of 17 bytes, none present
    ASSERT_CODE(ErrorCode::TooShort, "0308010080c800ffccff00");  // bad segment, but the frame is incomplete
    ASSERT_CODE(ErrorCode::TooShort, "050200009000");      // bad first geometry, second missing
    ASSERT_CODE(ErrorCode::TooShort, "100804" "4e656f");   // bad slot, name incomplete
}

void test_decoded_fields() {
    Command cmd;
    TEST_ASSERT_EQUAL(0, static_cast<int>(decodeGuarded(fromHex("040206112233"), cmd)));
    TEST_ASSERT_EQUAL(static_cast<int>(Opcode::SetParam), static_cast<int>(cmd.opcode));
    TEST_ASSERT_EQUAL(2, cmd.segment);
    TEST_ASSERT_EQUAL(static_cast<int>(Param::Color2), static_cast<int>(cmd.param));
    TEST_ASSERT_RGB(Rgb(0x11, 0x22, 0x33), cmd.color);

    TEST_ASSERT_EQUAL(0, static_cast<int>(decodeGuarded(fromHex("30d007"), cmd)));
    TEST_ASSERT_EQUAL(2000, cmd.milliamps);  // little-endian

    TEST_ASSERT_EQUAL(0, static_cast<int>(decodeGuarded(fromHex("100503c3a961"), cmd)));
    TEST_ASSERT_EQUAL(5, cmd.slot);
    TEST_ASSERT_EQUAL(3, cmd.name.len);  // bytes, not characters: "\xC3\xA9" "a"
    TEST_ASSERT_EQUAL_HEX8(0xC3, static_cast<uint8_t>(cmd.name.text[0]));
}

// Random frames: never a crash or an over-read, always a documented code,
// and whatever decodes cleanly re-encodes to the bytes it was decoded from.
void test_random_frames() {
    Rng rng(0xC0FFEE);
    int accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        Bytes frame(rng.below(40));
        for (uint8_t& b : frame) b = rng.next8();
        // Bias towards real opcodes and small values so that decoding gets deep.
        if (!frame.empty() && rng.below(4) != 0) frame[0] = kOpcodes[rng.below(sizeof kOpcodes)];
        for (size_t k = 1; k < frame.size(); ++k) {
            if (rng.below(2) == 0) frame[k] = static_cast<uint8_t>(rng.below(20));
        }

        Command cmd;
        const ErrorCode code = decodeGuarded(frame, cmd);
        TEST_ASSERT_TRUE(code == ErrorCode::None || code == ErrorCode::UnknownOpcode || code == ErrorCode::TooShort ||
                         code == ErrorCode::OutOfRange);
        if (code != ErrorCode::None) continue;
        ++accepted;
        uint8_t out[kMaxFrame];
        const size_t n = encodeCommand(cmd, out, sizeof out);
        TEST_ASSERT_TRUE_MESSAGE(n > 0 && n <= frame.size(), support::toHex(frame).c_str());
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(frame.data(), out, n, support::toHex(frame).c_str());
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(1000, accepted, "the generator should produce plenty of valid frames");
}

// ---- Notifications ----------------------------------------------------------------

Bytes fullState() {
    StateMsg msg;
    msg.rev = 0xBEEF;
    msg.power = 1;
    msg.brightness = 200;
    msg.budget_ma = 2000;
    msg.flags = 7;
    msg.preset = 3;
    msg.segment_count = kMaxSegments;
    for (uint8_t i = 0; i < kMaxSegments; ++i) {
        msg.segments[i].geometry.start = static_cast<uint8_t>(i * 18);
        msg.segments[i].geometry.length = 18;
        msg.segments[i].config.effect = i % 7;
        msg.segments[i].config.color1 = Rgb(i, static_cast<uint8_t>(i * 2), static_cast<uint8_t>(i * 3));
    }
    uint8_t out[kMaxFrame];
    const size_t n = encodeState(msg, out, sizeof out);
    return Bytes(out, out + n);
}

Bytes fullPresets() {
    PresetList list;
    list.count = kPresetSlots;
    for (uint8_t i = 0; i < kPresetSlots; ++i) {
        list.items[i].slot = i;
        list.items[i].name = makeName("Sixteen bytes!!!!");  // truncated to 16
    }
    uint8_t out[kMaxFrame];
    const size_t n = encodePresets(list, out, sizeof out);
    return Bytes(out, out + n);
}

void test_largest_frames_fit() {
    // PROTOCOL.md: "No frame is longer than 180 bytes."
    TEST_ASSERT_EQUAL(11 + 8 * 14, fullState().size());
    TEST_ASSERT_EQUAL(2 + 8 * 18, fullPresets().size());
    TEST_ASSERT_LESS_OR_EQUAL(kMaxFrame, fullState().size());
    TEST_ASSERT_LESS_OR_EQUAL(kMaxFrame, fullPresets().size());
}

void test_state_roundtrip_and_truncation() {
    const Bytes full = fullState();
    StateMsg back;
    {
        GuardedBytes g(full);
        TEST_ASSERT_EQUAL(0, static_cast<int>(decodeState(g.data(), g.size(), back)));
    }
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, back.rev);
    TEST_ASSERT_EQUAL(2000, back.budget_ma);
    TEST_ASSERT_EQUAL(kMaxSegments, back.segment_count);
    TEST_ASSERT_EQUAL(7 * 18, back.segments[7].geometry.start);
    TEST_ASSERT_RGB(Rgb(7, 14, 21), back.segments[7].config.color1);

    for (size_t len = 1; len < full.size(); ++len) {
        GuardedBytes g(Bytes(full.begin(), full.begin() + len));
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(ErrorCode::TooShort), static_cast<int>(decodeState(g.data(), g.size(), back)),
                                  "truncated STATE");
    }

    Bytes bad = full;
    bad[10] = 0;  // segment_count
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::OutOfRange), static_cast<int>(decodeState(bad.data(), bad.size(), back)));
    bad[10] = 9;
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::OutOfRange), static_cast<int>(decodeState(bad.data(), bad.size(), back)));
    bad = full;
    bad[0] = 0x02;
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::UnknownOpcode), static_cast<int>(decodeState(bad.data(), bad.size(), back)));
}

void test_presets_info_error_truncation() {
    const Bytes presets = fullPresets();
    PresetList list;
    for (size_t len = 1; len < presets.size(); ++len) {
        GuardedBytes g(Bytes(presets.begin(), presets.begin() + len));
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(ErrorCode::TooShort), static_cast<int>(decodePresets(g.data(), g.size(), list)),
                                  "truncated PRESETS");
    }
    {
        GuardedBytes g(presets);
        TEST_ASSERT_EQUAL(0, static_cast<int>(decodePresets(g.data(), g.size(), list)));
        TEST_ASSERT_EQUAL(8, list.count);
        TEST_ASSERT_EQUAL(16, list.items[7].name.len);
    }
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::OutOfRange), static_cast<int>(decodePresets(fromHex("0409").data(), 2, list)));
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::OutOfRange), static_cast<int>(decodePresets(fromHex("04010800").data(), 4, list)));
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::OutOfRange), static_cast<int>(decodePresets(fromHex("04010011").data(), 4, list)));

    const Bytes info = fromHex("050102000002900090000808070603646576");
    InfoMsg msg;
    for (size_t len = 1; len < info.size(); ++len) {
        GuardedBytes g(Bytes(info.begin(), info.begin() + len));
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(ErrorCode::TooShort), static_cast<int>(decodeInfo(g.data(), g.size(), msg)),
                                  "truncated INFO");
    }
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::OutOfRange), static_cast<int>(decodeInfo(fromHex("050102000005").data(), 6, msg)));

    ErrorMsg err;
    const Bytes error = fromHex("7f4201");
    for (size_t len = 1; len < error.size(); ++len) {
        GuardedBytes g(Bytes(error.begin(), error.begin() + len));
        TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::TooShort), static_cast<int>(decodeError(g.data(), g.size(), err)));
    }
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::UnknownOpcode), static_cast<int>(decodeError(fromHex("014201").data(), 3, err)));
}

void test_catalog_pages() {
    // Fill a page until the writer refuses: it must stop at 180 bytes and the
    // reader must get back exactly what went in.
    uint8_t out[kMaxFrame + 8];
    for (size_t i = 0; i < sizeof out; ++i) out[i] = 0xA5;
    CatalogWriter writer(out, sizeof out, kCatalogEffects, 3, 200);
    int added = 0;
    while (writer.add(static_cast<uint8_t>(added), static_cast<uint8_t>(added * 3), strView("EffectName"))) ++added;
    const size_t len = writer.finish();
    TEST_ASSERT_EQUAL(13, added);  // 5 + 13 * (3 + 10) = 174; one more would be 187
    TEST_ASSERT_EQUAL(174, len);
    TEST_ASSERT_LESS_OR_EQUAL(kMaxFrame, len);
    for (size_t i = len; i < sizeof out; ++i) TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xA5, out[i], "writer wrote past the frame");

    GuardedBytes g(Bytes(out, out + len));
    CatalogReader reader;
    TEST_ASSERT_EQUAL(0, static_cast<int>(reader.begin(g.data(), g.size())));
    TEST_ASSERT_EQUAL(kCatalogEffects, reader.kind);
    TEST_ASSERT_EQUAL(3, reader.start);
    TEST_ASSERT_EQUAL(200, reader.total);
    TEST_ASSERT_EQUAL(13, reader.count);
    for (int i = 0; i < 13; ++i) {
        CatalogEntry e;
        TEST_ASSERT_EQUAL(0, static_cast<int>(reader.next(e)));
        TEST_ASSERT_EQUAL(i, e.id);
        TEST_ASSERT_EQUAL(i * 3, e.param_flags);
        TEST_ASSERT_TRUE(e.name == strView("EffectName"));
    }
    CatalogEntry extra;
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::NotFound), static_cast<int>(reader.next(extra)));

    // Truncations: the header is too short, or an entry is.
    for (size_t cut = 1; cut < len; ++cut) {
        GuardedBytes h(Bytes(out, out + cut));
        CatalogReader r;
        ErrorCode code = r.begin(h.data(), h.size());
        CatalogEntry e;
        for (int i = 0; code == ErrorCode::None && i < r.count; ++i) code = r.next(e);
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(ErrorCode::TooShort), static_cast<int>(code), "truncated CATALOG");
    }

    CatalogReader r;
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::OutOfRange), static_cast<int>(r.begin(fromHex("0202000000").data(), 5)));
    TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::UnknownOpcode), static_cast<int>(r.begin(fromHex("0100000000").data(), 5)));
}

void test_encoders_respect_capacity() {
    const Bytes state = fullState();
    StateMsg msg;
    TEST_ASSERT_EQUAL(0, static_cast<int>(decodeState(state.data(), state.size(), msg)));
    for (size_t cap = 0; cap < state.size(); ++cap) {
        uint8_t out[kMaxFrame];
        for (uint8_t& b : out) b = 0x5A;
        TEST_ASSERT_EQUAL_MESSAGE(0, encodeState(msg, out, cap), "encodeState must fail when the frame does not fit");
        for (size_t i = cap; i < sizeof out; ++i) TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, out[i], "wrote past cap");
    }
    uint8_t exact[kMaxFrame];
    TEST_ASSERT_EQUAL(state.size(), encodeState(msg, exact, state.size()));

    Command cmd;
    cmd.opcode = Opcode::SetEffect;
    uint8_t small[11];
    TEST_ASSERT_EQUAL(0, encodeCommand(cmd, small, sizeof small));  // needs 12
    uint8_t enough[12];
    TEST_ASSERT_EQUAL(12, encodeCommand(cmd, enough, sizeof enough));

    // An INFO whose build string would push the frame past 180 bytes is refused.
    InfoMsg info;
    info.strip_count = 2;
    const std::string long_build(200, 'x');
    info.build.data = long_build.data();
    info.build.len = 200;
    uint8_t big[256];
    TEST_ASSERT_EQUAL(0, encodeInfo(info, big, sizeof big));
}

// ---- Live -------------------------------------------------------------------------

void test_live_frames() {
    const Bytes frame = fromHex("0709c8ff641e0005400aec053412");
    LiveInput in;
    for (size_t len = 0; len < frame.size(); ++len) {
        GuardedBytes g(Bytes(frame.begin(), frame.begin() + len));
        TEST_ASSERT_EQUAL(static_cast<int>(ErrorCode::TooShort), static_cast<int>(decodeLive(g.data(), g.size(), in)));
    }
    Bytes padded = frame;
    padded.push_back(0x99);
    GuardedBytes g(padded);
    TEST_ASSERT_EQUAL(0, static_cast<int>(decodeLive(g.data(), g.size(), in)));
    TEST_ASSERT_EQUAL(-20, in.tilt_x);
    TEST_ASSERT_EQUAL(1280, in.bpm_x10);
    TEST_ASSERT_EQUAL_HEX16(0x1234, in.t_ms);
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_commands_decode);
    RUN_TEST(test_empty_frame_is_too_short);
    RUN_TEST(test_every_truncation_is_too_short);
    RUN_TEST(test_trailing_bytes_are_ignored);
    RUN_TEST(test_unknown_opcodes);
    RUN_TEST(test_out_of_range_values);
    RUN_TEST(test_limits_are_accepted);
    RUN_TEST(test_error_precedence);
    RUN_TEST(test_decoded_fields);
    RUN_TEST(test_random_frames);
    RUN_TEST(test_largest_frames_fit);
    RUN_TEST(test_state_roundtrip_and_truncation);
    RUN_TEST(test_presets_info_error_truncation);
    RUN_TEST(test_catalog_pages);
    RUN_TEST(test_encoders_respect_capacity);
    RUN_TEST(test_live_frames);
    return UNITY_END();
}
