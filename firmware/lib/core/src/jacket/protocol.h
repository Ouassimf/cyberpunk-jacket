// Codec for BLE protocol v1 (protocol/PROTOCOL.md): every frame in both
// directions. The firmware needs decodeCommand / decodeLive and the
// notification encoders; the other half exists so the golden vectors can be
// checked both ways on the host.
//
// No heap, no exceptions. Decoders never read past `len`; encoders never write
// past `cap` nor produce more than kMaxFrame bytes, and return 0 on failure.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "jacket/config.h"
#include "jacket/scene.h"

namespace jacket {

enum class Opcode : uint8_t {
    SetPower = 0x01,
    SetBrightness = 0x02,
    SetEffect = 0x03,
    SetParam = 0x04,
    SetSegments = 0x05,
    Identify = 0x06,
    PresetSave = 0x10,
    PresetLoad = 0x11,
    PresetDelete = 0x12,
    GetPresets = 0x13,
    GetInfo = 0x20,
    GetCatalog = 0x21,
    SetBudget = 0x30,
    SetDevMode = 0x31,
    BootOtherSlot = 0x32,
};

// BOOT_OTHER_SLOT carries this byte, so that a stray write cannot send the
// jacket back to its previous firmware.
constexpr uint8_t kBootOtherSlotConfirm = 0x5A;

enum class Notify : uint8_t {
    State = 0x01,
    Catalog = 0x02,
    Presets = 0x04,
    Info = 0x05,
    Error = 0x7F,
};

enum class ErrorCode : uint8_t {
    None = 0,           // success; never sent
    UnknownOpcode = 1,
    TooShort = 2,
    OutOfRange = 3,
    NotFound = 4,        // PRESET_LOAD / PRESET_DELETE on an empty slot; BOOT_OTHER_SLOT with no firmware to go to
    StorageFailure = 5,  // the jacket could not write to its storage
};

enum class Param : uint8_t { Effect = 1, Palette = 2, Speed = 3, Intensity = 4, Color1 = 5, Color2 = 6 };

constexpr uint8_t kCatalogEffects = 0;
constexpr uint8_t kCatalogPalettes = 1;

// STATE.flags
constexpr uint8_t kStateDevMode = 0x01;
constexpr uint8_t kStateWifi = 0x02;
constexpr uint8_t kStateLive = 0x04;

// LiveInput.flags
constexpr uint8_t kLiveAudioValid = 0x01;
constexpr uint8_t kLiveMotionValid = 0x02;
constexpr uint8_t kLiveBeatValid = 0x04;

constexpr size_t kLiveFrameSize = 14;
constexpr uint8_t kMaxInfoStrips = 4;

// A string that points into someone else's bytes (a frame or a literal).
struct StrView {
    const char* data = nullptr;
    uint8_t len = 0;
};
StrView strView(const char* zero_terminated);  // truncated at 255 bytes
bool operator==(const StrView& a, const StrView& b);

// A preset name, stored inline.
struct Name {
    uint8_t len = 0;
    char text[kMaxPresetName] = {};
};
Name makeName(const char* zero_terminated);  // truncated at kMaxPresetName bytes
bool operator==(const Name& a, const Name& b);

// One decoded command. Only the fields listed for `opcode` are meaningful.
struct Command {
    Opcode opcode = Opcode::GetInfo;
    uint8_t on = 0;                          // SET_POWER, SET_DEV_MODE
    uint8_t value = 0;                       // SET_BRIGHTNESS; SET_PARAM with a u8 parameter; BOOT_OTHER_SLOT (confirm)
    uint8_t segment = 0;                     // SET_EFFECT, SET_PARAM (kAllSegments = every segment)
    EffectConfig config;                     // SET_EFFECT
    Param param = Param::Effect;             // SET_PARAM
    Rgb color;                               // SET_PARAM with Color1 / Color2
    uint8_t count = 0;                       // SET_SEGMENTS
    SegmentGeometry geometry[kMaxSegments];  // SET_SEGMENTS
    uint8_t strip_mask = 0;                  // IDENTIFY
    uint8_t start = 0;                       // IDENTIFY; GET_CATALOG
    uint8_t length = 0;                      // IDENTIFY
    uint8_t seconds = 0;                     // IDENTIFY
    uint8_t slot = 0;                        // PRESET_*
    Name name;                               // PRESET_SAVE
    uint8_t kind = 0;                        // GET_CATALOG
    uint16_t milliamps = 0;                  // SET_BUDGET
};

struct StateMsg {
    uint8_t proto = kProtoVersion;
    uint16_t rev = 0;
    uint8_t power = 0;
    uint8_t brightness = 0;
    uint16_t budget_ma = 0;
    uint8_t flags = 0;
    uint8_t preset = kNoPreset;
    uint8_t segment_count = 0;
    Segment segments[kMaxSegments];
};

struct PresetList {
    uint8_t count = 0;
    struct Item {
        uint8_t slot = 0;
        Name name;
    } items[kPresetSlots];
};

struct InfoMsg {
    uint8_t proto = kProtoVersion;
    uint8_t fw_major = 0;
    uint8_t fw_minor = 0;
    uint8_t fw_patch = 0;
    uint8_t strip_count = 0;
    uint16_t leds[kMaxInfoStrips] = {};
    uint8_t max_segments = 0;
    uint8_t preset_slots = 0;
    uint8_t effect_count = 0;
    uint8_t palette_count = 0;
    StrView build;  // decoded: points into the frame
};

struct ErrorMsg {
    uint8_t opcode = 0;
    uint8_t code = 0;
};

struct CatalogEntry {
    uint8_t id = 0;
    uint8_t param_flags = 0;  // effects only
    StrView name;             // decoded: points into the frame
};

// Live input frame (`live` characteristic).
struct LiveInput {
    uint8_t flags = 0;
    uint8_t seq = 0;
    uint8_t level = 0;
    uint8_t bass = 0;
    uint8_t mid = 0;
    uint8_t treble = 0;
    uint16_t bpm_x10 = 0;
    uint8_t beat_phase = 0;
    uint8_t motion = 0;
    int8_t tilt_x = 0;
    int8_t tilt_y = 0;
    uint16_t t_ms = 0;
};

// ---- Commands (app -> jacket) ----------------------------------------------
// Checks everything that does not depend on the jacket's current state:
//   UnknownOpcode  first byte is not a v1 opcode
//   TooShort       empty frame, or fewer bytes than the payload needs
//   OutOfRange     on not 0/1, segment not 0..7 or 0xFF, unknown param id,
//                  a BOOT_OTHER_SLOT confirmation other than 0x5A,
//                  segment count not 1..8, invalid geometry, slot > 7,
//                  name longer than 16 bytes, catalogue kind > 1, an IDENTIFY
//                  range that does not fit (unless it is a cancel)
// TooShort is reported before OutOfRange, except where a bad value (param id,
// segment count, name length) makes the needed length unknowable.
// Trailing bytes are ignored. On error `out` is unspecified.
ErrorCode decodeCommand(const uint8_t* data, size_t len, Command& out);
size_t encodeCommand(const Command& cmd, uint8_t* out, size_t cap);

// ---- Notifications (jacket -> app) -----------------------------------------
// Decoders return UnknownOpcode for a wrong type byte, TooShort, or
// OutOfRange for counts that exceed what v1 allows.
size_t encodeState(const StateMsg& msg, uint8_t* out, size_t cap);
ErrorCode decodeState(const uint8_t* data, size_t len, StateMsg& out);

size_t encodePresets(const PresetList& msg, uint8_t* out, size_t cap);
ErrorCode decodePresets(const uint8_t* data, size_t len, PresetList& out);

size_t encodeInfo(const InfoMsg& msg, uint8_t* out, size_t cap);
ErrorCode decodeInfo(const uint8_t* data, size_t len, InfoMsg& out);

size_t encodeError(uint8_t opcode, ErrorCode code, uint8_t* out, size_t cap);
ErrorCode decodeError(const uint8_t* data, size_t len, ErrorMsg& out);

// Builds one CATALOG page entry by entry, so a page is simply "as many
// entries as fit in a frame".
class CatalogWriter {
public:
    CatalogWriter(uint8_t* out, size_t cap, uint8_t kind, uint8_t start, uint8_t total);
    // False, with nothing written, when the entry does not fit.
    // `param_flags` is not sent for palettes.
    bool add(uint8_t id, uint8_t param_flags, StrView name);
    uint8_t count() const { return count_; }
    size_t finish();  // frame length, 0 if even the header did not fit

private:
    uint8_t* out_;
    size_t cap_;
    size_t pos_ = 0;
    uint8_t kind_;
    uint8_t count_ = 0;
    bool ok_ = true;
};

// Reads a CATALOG page: begin(), then next() `count` times.
class CatalogReader {
public:
    ErrorCode begin(const uint8_t* data, size_t len);
    ErrorCode next(CatalogEntry& out);

    uint8_t kind = 0;
    uint8_t start = 0;
    uint8_t total = 0;
    uint8_t count = 0;

private:
    const uint8_t* data_ = nullptr;
    size_t len_ = 0;
    size_t pos_ = 0;
    uint8_t read_ = 0;
};

// ---- Live input -------------------------------------------------------------
ErrorCode decodeLive(const uint8_t* data, size_t len, LiveInput& out);  // None or TooShort
size_t encodeLive(const LiveInput& in, uint8_t* out, size_t cap);

}  // namespace jacket
