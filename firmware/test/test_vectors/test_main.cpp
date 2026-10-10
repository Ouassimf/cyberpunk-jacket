// Golden vectors: protocol/vectors/v1.json is read from disk and, for every
// vector, encode(fields) must equal hex and decode(hex) must equal fields.
// A vector of a type this test does not know fails the test.
//
// These are real conformance tests: the expected bytes come from the shared
// protocol repository, not from this codec.

#include <ArduinoJson.h>

#include <map>
#include <set>
#include <stdlib.h>
#include <string>

#include "../support/test_support.h"

using namespace jacket;
using support::Bytes;

void setUp() {}
void tearDown() {}

namespace {

// ---- Finding and reading the file ---------------------------------------------

bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// The vectors live outside the firmware project. Try, in order: an explicit
// override, the path relative to this source file, and the usual working
// directories (firmware/ when run by PlatformIO, the repository root).
bool loadVectors(std::string& text, std::string& used_path) {
    std::vector<std::string> candidates;
    if (const char* env = getenv("JACKET_VECTORS")) candidates.push_back(env);
    std::string here = __FILE__;
    const size_t slash = here.find_last_of("/\\");
    if (slash != std::string::npos) candidates.push_back(here.substr(0, slash) + "/../../../protocol/vectors/v1.json");
    candidates.push_back("../protocol/vectors/v1.json");
    candidates.push_back("protocol/vectors/v1.json");
    candidates.push_back("../../protocol/vectors/v1.json");
    for (const std::string& path : candidates) {
        if (readFile(path, text)) {
            used_path = path;
            return true;
        }
    }
    return false;
}

JsonDocument g_doc;
std::string g_load_error;

// ---- JSON helpers ----------------------------------------------------------------

// Thrown-away-on-first-error conversion context: the vector fails with `error`.
struct Check {
    std::string error;
    bool ok() const { return error.empty(); }
    void fail(const std::string& what) {
        if (error.empty()) error = what;
    }
};

long integer(Check& c, JsonVariantConst v, const char* what, long min, long max) {
    if (!v.is<long>()) {
        c.fail(std::string("missing or non-integer field: ") + what);
        return 0;
    }
    const long value = v.as<long>();
    if (value < min || value > max) c.fail(std::string("field does not fit its wire type: ") + what);
    return value;
}
uint8_t u8(Check& c, JsonObjectConst o, const char* key) { return static_cast<uint8_t>(integer(c, o[key], key, 0, 255)); }
uint16_t u16(Check& c, JsonObjectConst o, const char* key) { return static_cast<uint16_t>(integer(c, o[key], key, 0, 65535)); }
int8_t i8(Check& c, JsonObjectConst o, const char* key) { return static_cast<int8_t>(integer(c, o[key], key, -128, 127)); }

Rgb rgb(Check& c, JsonVariantConst v, const char* what) {
    JsonArrayConst a = v.as<JsonArrayConst>();
    if (a.isNull() || a.size() != 3) {
        c.fail(std::string("expected [r, g, b]: ") + what);
        return Rgb();
    }
    return Rgb(static_cast<uint8_t>(integer(c, a[0], what, 0, 255)), static_cast<uint8_t>(integer(c, a[1], what, 0, 255)),
               static_cast<uint8_t>(integer(c, a[2], what, 0, 255)));
}

std::string text(Check& c, JsonObjectConst o, const char* key) {
    if (!o[key].is<const char*>()) {
        c.fail(std::string("missing or non-string field: ") + key);
        return std::string();
    }
    return o[key].as<std::string>();
}

JsonObjectConst object(Check& c, JsonVariantConst v, const char* what) {
    if (!v.is<JsonObjectConst>()) c.fail(std::string("expected an object: ") + what);
    return v.as<JsonObjectConst>();
}
JsonArrayConst array(Check& c, JsonVariantConst v, const char* what) {
    if (!v.is<JsonArrayConst>()) c.fail(std::string("expected an array: ") + what);
    return v.as<JsonArrayConst>();
}

void putRgb(JsonObject o, const char* key, Rgb color) {
    JsonArray a = o[key].to<JsonArray>();
    a.add(color.r);
    a.add(color.g);
    a.add(color.b);
}

// Deep equality that does not care about key order. Extra or missing keys on
// either side make it false.
bool jsonEqual(JsonVariantConst a, JsonVariantConst b) {
    if (a.is<JsonObjectConst>() || b.is<JsonObjectConst>()) {
        if (!a.is<JsonObjectConst>() || !b.is<JsonObjectConst>()) return false;
        JsonObjectConst oa = a.as<JsonObjectConst>();
        JsonObjectConst ob = b.as<JsonObjectConst>();
        if (oa.size() != ob.size()) return false;
        for (JsonPairConst kv : oa) {
            JsonVariantConst other = ob[kv.key()];
            if (other.isUnbound() || !jsonEqual(kv.value(), other)) return false;
        }
        return true;
    }
    if (a.is<JsonArrayConst>() || b.is<JsonArrayConst>()) {
        if (!a.is<JsonArrayConst>() || !b.is<JsonArrayConst>()) return false;
        JsonArrayConst aa = a.as<JsonArrayConst>();
        JsonArrayConst ab = b.as<JsonArrayConst>();
        if (aa.size() != ab.size()) return false;
        for (size_t i = 0; i < aa.size(); ++i) {
            if (!jsonEqual(aa[i], ab[i])) return false;
        }
        return true;
    }
    if (a.is<const char*>() || b.is<const char*>()) {
        return a.is<const char*>() && b.is<const char*>() && a.as<std::string>() == b.as<std::string>();
    }
    if (a.is<long>() && b.is<long>()) return a.as<long>() == b.as<long>();
    return false;  // floats, booleans and nulls do not occur in protocol fields
}

std::string dump(JsonVariantConst v) {
    std::string out;
    serializeJson(v, out);
    return out;
}

// ---- fields <-> structs ----------------------------------------------------------

EffectConfig configFrom(Check& c, JsonVariantConst v) {
    JsonObjectConst o = object(c, v, "config");
    EffectConfig cfg;
    cfg.effect = u8(c, o, "effect");
    cfg.palette = u8(c, o, "palette");
    cfg.speed = u8(c, o, "speed");
    cfg.intensity = u8(c, o, "intensity");
    cfg.color1 = rgb(c, o["color1"], "color1");
    cfg.color2 = rgb(c, o["color2"], "color2");
    return cfg;
}
void putConfig(JsonObject parent, const EffectConfig& cfg) {
    JsonObject o = parent["config"].to<JsonObject>();
    o["effect"] = cfg.effect;
    o["palette"] = cfg.palette;
    o["speed"] = cfg.speed;
    o["intensity"] = cfg.intensity;
    putRgb(o, "color1", cfg.color1);
    putRgb(o, "color2", cfg.color2);
}

SegmentGeometry geometryFrom(Check& c, JsonObjectConst o) {
    SegmentGeometry g;
    g.strip_mask = u8(c, o, "strip_mask");
    g.start = u8(c, o, "start");
    g.length = u8(c, o, "length");
    g.flags = u8(c, o, "flags");
    return g;
}
void putGeometry(JsonObject o, const SegmentGeometry& g) {
    o["strip_mask"] = g.strip_mask;
    o["start"] = g.start;
    o["length"] = g.length;
    o["flags"] = g.flags;
}

bool isColorParam(Param p) { return p == Param::Color1 || p == Param::Color2; }

// fields -> Command, for the opcode named by the vector.
Command commandFrom(Check& c, Opcode opcode, JsonObjectConst f) {
    Command cmd;
    cmd.opcode = opcode;
    switch (opcode) {
        case Opcode::SetPower:
        case Opcode::SetDevMode:
            cmd.on = u8(c, f, "on");
            break;
        case Opcode::SetBrightness:
            cmd.value = u8(c, f, "value");
            break;
        case Opcode::BootOtherSlot:
            cmd.value = u8(c, f, "confirm");
            break;
        case Opcode::SetEffect:
            cmd.segment = u8(c, f, "segment");
            cmd.config = configFrom(c, f["config"]);
            break;
        case Opcode::SetParam:
            cmd.segment = u8(c, f, "segment");
            cmd.param = static_cast<Param>(u8(c, f, "param"));
            if (isColorParam(cmd.param)) {
                cmd.color = rgb(c, f["value"], "value");
            } else {
                cmd.value = u8(c, f, "value");
            }
            break;
        case Opcode::SetSegments: {
            JsonArrayConst list = array(c, f["segments"], "segments");
            if (list.size() > kMaxSegments) {
                c.fail("more segments than the protocol allows");
                break;
            }
            cmd.count = static_cast<uint8_t>(list.size());
            for (uint8_t i = 0; i < cmd.count; ++i) cmd.geometry[i] = geometryFrom(c, object(c, list[i], "segment"));
            break;
        }
        case Opcode::Identify:
            cmd.strip_mask = u8(c, f, "strip_mask");
            cmd.start = u8(c, f, "start");
            cmd.length = u8(c, f, "length");
            cmd.seconds = u8(c, f, "seconds");
            break;
        case Opcode::PresetSave: {
            cmd.slot = u8(c, f, "slot");
            const std::string name = text(c, f, "name");
            if (name.size() > kMaxPresetName) c.fail("name longer than the protocol allows");
            cmd.name = makeName(name.c_str());
            break;
        }
        case Opcode::PresetLoad:
        case Opcode::PresetDelete:
            cmd.slot = u8(c, f, "slot");
            break;
        case Opcode::GetPresets:
        case Opcode::GetInfo:
            break;
        case Opcode::GetCatalog:
            cmd.kind = u8(c, f, "kind");
            cmd.start = u8(c, f, "start");
            break;
        case Opcode::SetBudget:
            cmd.milliamps = u16(c, f, "milliamps");
            break;
    }
    return cmd;
}

// Command -> fields.
void putCommand(JsonObject f, const Command& cmd) {
    switch (cmd.opcode) {
        case Opcode::SetPower:
        case Opcode::SetDevMode:
            f["on"] = cmd.on;
            break;
        case Opcode::SetBrightness:
            f["value"] = cmd.value;
            break;
        case Opcode::BootOtherSlot:
            f["confirm"] = cmd.value;
            break;
        case Opcode::SetEffect:
            f["segment"] = cmd.segment;
            putConfig(f, cmd.config);
            break;
        case Opcode::SetParam:
            f["segment"] = cmd.segment;
            f["param"] = static_cast<uint8_t>(cmd.param);
            if (isColorParam(cmd.param)) {
                putRgb(f, "value", cmd.color);
            } else {
                f["value"] = cmd.value;
            }
            break;
        case Opcode::SetSegments: {
            JsonArray list = f["segments"].to<JsonArray>();
            for (uint8_t i = 0; i < cmd.count; ++i) putGeometry(list.add<JsonObject>(), cmd.geometry[i]);
            break;
        }
        case Opcode::Identify:
            f["strip_mask"] = cmd.strip_mask;
            f["start"] = cmd.start;
            f["length"] = cmd.length;
            f["seconds"] = cmd.seconds;
            break;
        case Opcode::PresetSave:
            f["slot"] = cmd.slot;
            f["name"] = std::string(cmd.name.text, cmd.name.len);
            break;
        case Opcode::PresetLoad:
        case Opcode::PresetDelete:
            f["slot"] = cmd.slot;
            break;
        case Opcode::GetPresets:
        case Opcode::GetInfo:
            break;
        case Opcode::GetCatalog:
            f["kind"] = cmd.kind;
            f["start"] = cmd.start;
            break;
        case Opcode::SetBudget:
            f["milliamps"] = cmd.milliamps;
            break;
    }
}

const std::map<std::string, Opcode> kCommandTypes = {
    {"SET_POWER", Opcode::SetPower},       {"SET_BRIGHTNESS", Opcode::SetBrightness},
    {"SET_EFFECT", Opcode::SetEffect},     {"SET_PARAM", Opcode::SetParam},
    {"SET_SEGMENTS", Opcode::SetSegments}, {"IDENTIFY", Opcode::Identify},
    {"PRESET_SAVE", Opcode::PresetSave},   {"PRESET_LOAD", Opcode::PresetLoad},
    {"PRESET_DELETE", Opcode::PresetDelete}, {"GET_PRESETS", Opcode::GetPresets},
    {"GET_INFO", Opcode::GetInfo},         {"GET_CATALOG", Opcode::GetCatalog},
    {"SET_BUDGET", Opcode::SetBudget},     {"SET_DEV_MODE", Opcode::SetDevMode},
    {"BOOT_OTHER_SLOT", Opcode::BootOtherSlot},
};
const std::set<std::string> kStateTypes = {"STATE", "CATALOG", "PRESETS", "INFO", "ERROR"};

// ---- One vector --------------------------------------------------------------------
// Each checker encodes `fields` into `encoded` and decodes `frame` into `decoded`.
// Problems go into `c`.

void checkCommand(Check& c, Opcode opcode, JsonObjectConst fields, const Bytes& frame, Bytes& encoded, JsonObject decoded) {
    const Command cmd = commandFrom(c, opcode, fields);
    if (c.ok()) {
        uint8_t out[kMaxFrame];
        encoded.assign(out, out + encodeCommand(cmd, out, sizeof out));
    }
    Command back;
    const ErrorCode error = decodeCommand(frame.data(), frame.size(), back);
    if (error != ErrorCode::None) {
        c.fail("decodeCommand rejected the frame with code " + std::to_string(static_cast<int>(error)));
        return;
    }
    if (back.opcode != opcode) c.fail("frame decodes to a different opcode than the vector's type");
    putCommand(decoded, back);
}

void checkState(Check& c, JsonObjectConst fields, const Bytes& frame, Bytes& encoded, JsonObject decoded) {
    StateMsg msg;
    msg.proto = u8(c, fields, "proto");
    msg.rev = u16(c, fields, "rev");
    msg.power = u8(c, fields, "power");
    msg.brightness = u8(c, fields, "brightness");
    msg.budget_ma = u16(c, fields, "budget_ma");
    msg.flags = u8(c, fields, "flags");
    msg.preset = u8(c, fields, "preset");
    JsonArrayConst list = array(c, fields["segments"], "segments");
    if (list.size() > kMaxSegments) {
        c.fail("more segments than the protocol allows");
    } else {
        msg.segment_count = static_cast<uint8_t>(list.size());
        for (uint8_t i = 0; i < msg.segment_count; ++i) {
            JsonObjectConst o = object(c, list[i], "segment");
            msg.segments[i].geometry = geometryFrom(c, o);
            msg.segments[i].config = configFrom(c, o["config"]);
        }
    }
    if (c.ok()) {
        uint8_t out[kMaxFrame];
        encoded.assign(out, out + encodeState(msg, out, sizeof out));
    }

    StateMsg back;
    const ErrorCode error = decodeState(frame.data(), frame.size(), back);
    if (error != ErrorCode::None) {
        c.fail("decodeState failed with code " + std::to_string(static_cast<int>(error)));
        return;
    }
    decoded["proto"] = back.proto;
    decoded["rev"] = back.rev;
    decoded["power"] = back.power;
    decoded["brightness"] = back.brightness;
    decoded["budget_ma"] = back.budget_ma;
    decoded["flags"] = back.flags;
    decoded["preset"] = back.preset;
    JsonArray out_list = decoded["segments"].to<JsonArray>();
    for (uint8_t i = 0; i < back.segment_count; ++i) {
        JsonObject o = out_list.add<JsonObject>();
        putGeometry(o, back.segments[i].geometry);
        putConfig(o, back.segments[i].config);
    }
}

void checkCatalog(Check& c, JsonObjectConst fields, const Bytes& frame, Bytes& encoded, JsonObject decoded) {
    const uint8_t kind = u8(c, fields, "kind");
    const uint8_t start = u8(c, fields, "start");
    const uint8_t total = u8(c, fields, "total");
    JsonArrayConst list = array(c, fields["entries"], "entries");
    if (c.ok()) {
        uint8_t out[kMaxFrame];
        CatalogWriter writer(out, sizeof out, kind, start, total);
        std::vector<std::string> names(list.size());  // keep the bytes alive while the page is built
        size_t i = 0;
        for (JsonVariantConst item : list) {
            JsonObjectConst o = object(c, item, "entry");
            names[i] = text(c, o, "name");
            const uint8_t flags = kind == kCatalogEffects ? u8(c, o, "param_flags") : 0;
            StrView name;
            name.data = names[i].data();
            name.len = static_cast<uint8_t>(names[i].size());
            if (!writer.add(u8(c, o, "id"), flags, name)) c.fail("catalogue entry does not fit in a frame");
            ++i;
        }
        if (c.ok()) encoded.assign(out, out + writer.finish());
    }

    CatalogReader reader;
    ErrorCode error = reader.begin(frame.data(), frame.size());
    if (error != ErrorCode::None) {
        c.fail("CatalogReader::begin failed with code " + std::to_string(static_cast<int>(error)));
        return;
    }
    decoded["kind"] = reader.kind;
    decoded["start"] = reader.start;
    decoded["total"] = reader.total;
    JsonArray out_list = decoded["entries"].to<JsonArray>();
    for (uint8_t i = 0; i < reader.count; ++i) {
        CatalogEntry entry;
        error = reader.next(entry);
        if (error != ErrorCode::None) {
            c.fail("CatalogReader::next failed with code " + std::to_string(static_cast<int>(error)));
            return;
        }
        JsonObject o = out_list.add<JsonObject>();
        o["id"] = entry.id;
        if (reader.kind == kCatalogEffects) o["param_flags"] = entry.param_flags;
        o["name"] = std::string(entry.name.data, entry.name.len);
    }
}

void checkPresets(Check& c, JsonObjectConst fields, const Bytes& frame, Bytes& encoded, JsonObject decoded) {
    PresetList msg;
    JsonArrayConst list = array(c, fields["presets"], "presets");
    if (list.size() > kPresetSlots) {
        c.fail("more presets than there are slots");
    } else {
        for (JsonVariantConst item : list) {
            JsonObjectConst o = object(c, item, "preset");
            const std::string name = text(c, o, "name");
            if (name.size() > kMaxPresetName) c.fail("name longer than the protocol allows");
            msg.items[msg.count].slot = u8(c, o, "slot");
            msg.items[msg.count].name = makeName(name.c_str());
            ++msg.count;
        }
    }
    if (c.ok()) {
        uint8_t out[kMaxFrame];
        encoded.assign(out, out + encodePresets(msg, out, sizeof out));
    }

    PresetList back;
    const ErrorCode error = decodePresets(frame.data(), frame.size(), back);
    if (error != ErrorCode::None) {
        c.fail("decodePresets failed with code " + std::to_string(static_cast<int>(error)));
        return;
    }
    JsonArray out_list = decoded["presets"].to<JsonArray>();
    for (uint8_t i = 0; i < back.count; ++i) {
        JsonObject o = out_list.add<JsonObject>();
        o["slot"] = back.items[i].slot;
        o["name"] = std::string(back.items[i].name.text, back.items[i].name.len);
    }
}

void checkInfo(Check& c, JsonObjectConst fields, const Bytes& frame, Bytes& encoded, JsonObject decoded) {
    InfoMsg msg;
    msg.proto = u8(c, fields, "proto");
    msg.fw_major = u8(c, fields, "fw_major");
    msg.fw_minor = u8(c, fields, "fw_minor");
    msg.fw_patch = u8(c, fields, "fw_patch");
    JsonArrayConst leds = array(c, fields["leds"], "leds");
    if (leds.size() > kMaxInfoStrips) {
        c.fail("more strips than this codec supports");
    } else {
        msg.strip_count = static_cast<uint8_t>(leds.size());
        for (uint8_t i = 0; i < msg.strip_count; ++i) msg.leds[i] = static_cast<uint16_t>(integer(c, leds[i], "leds", 0, 65535));
    }
    msg.max_segments = u8(c, fields, "max_segments");
    msg.preset_slots = u8(c, fields, "preset_slots");
    msg.effect_count = u8(c, fields, "effect_count");
    msg.palette_count = u8(c, fields, "palette_count");
    const std::string build = text(c, fields, "build");
    msg.build.data = build.data();
    msg.build.len = static_cast<uint8_t>(build.size());
    if (build.size() > 255) c.fail("build string too long");
    if (c.ok()) {
        uint8_t out[kMaxFrame];
        encoded.assign(out, out + encodeInfo(msg, out, sizeof out));
    }

    InfoMsg back;
    const ErrorCode error = decodeInfo(frame.data(), frame.size(), back);
    if (error != ErrorCode::None) {
        c.fail("decodeInfo failed with code " + std::to_string(static_cast<int>(error)));
        return;
    }
    decoded["proto"] = back.proto;
    decoded["fw_major"] = back.fw_major;
    decoded["fw_minor"] = back.fw_minor;
    decoded["fw_patch"] = back.fw_patch;
    JsonArray out_leds = decoded["leds"].to<JsonArray>();
    for (uint8_t i = 0; i < back.strip_count; ++i) out_leds.add(back.leds[i]);
    decoded["max_segments"] = back.max_segments;
    decoded["preset_slots"] = back.preset_slots;
    decoded["effect_count"] = back.effect_count;
    decoded["palette_count"] = back.palette_count;
    decoded["build"] = std::string(back.build.data, back.build.len);
}

void checkError(Check& c, JsonObjectConst fields, const Bytes& frame, Bytes& encoded, JsonObject decoded) {
    const uint8_t opcode = u8(c, fields, "opcode");
    const uint8_t code = u8(c, fields, "code");
    if (c.ok()) {
        uint8_t out[kMaxFrame];
        encoded.assign(out, out + encodeError(opcode, static_cast<ErrorCode>(code), out, sizeof out));
    }
    ErrorMsg back;
    const ErrorCode error = decodeError(frame.data(), frame.size(), back);
    if (error != ErrorCode::None) {
        c.fail("decodeError failed with code " + std::to_string(static_cast<int>(error)));
        return;
    }
    decoded["opcode"] = back.opcode;
    decoded["code"] = back.code;
}

void checkLive(Check& c, JsonObjectConst fields, const Bytes& frame, Bytes& encoded, JsonObject decoded) {
    LiveInput in;
    in.flags = u8(c, fields, "flags");
    in.seq = u8(c, fields, "seq");
    in.level = u8(c, fields, "level");
    in.bass = u8(c, fields, "bass");
    in.mid = u8(c, fields, "mid");
    in.treble = u8(c, fields, "treble");
    in.bpm_x10 = u16(c, fields, "bpm_x10");
    in.beat_phase = u8(c, fields, "beat_phase");
    in.motion = u8(c, fields, "motion");
    in.tilt_x = i8(c, fields, "tilt_x");
    in.tilt_y = i8(c, fields, "tilt_y");
    in.t_ms = u16(c, fields, "t_ms");
    if (c.ok()) {
        uint8_t out[kMaxFrame];
        encoded.assign(out, out + encodeLive(in, out, sizeof out));
    }
    LiveInput back;
    if (decodeLive(frame.data(), frame.size(), back) != ErrorCode::None) {
        c.fail("decodeLive rejected the frame");
        return;
    }
    decoded["flags"] = back.flags;
    decoded["seq"] = back.seq;
    decoded["level"] = back.level;
    decoded["bass"] = back.bass;
    decoded["mid"] = back.mid;
    decoded["treble"] = back.treble;
    decoded["bpm_x10"] = back.bpm_x10;
    decoded["beat_phase"] = back.beat_phase;
    decoded["motion"] = back.motion;
    decoded["tilt_x"] = back.tilt_x;
    decoded["tilt_y"] = back.tilt_y;
    decoded["t_ms"] = back.t_ms;
}

// Returns an empty string if the vector passes, otherwise what is wrong.
std::string checkVector(JsonObjectConst v) {
    Check c;
    const std::string channel = text(c, v, "channel");
    const std::string type = text(c, v, "type");
    const std::string hex = text(c, v, "hex");
    JsonObjectConst fields = object(c, v["fields"], "fields");
    if (!c.ok()) return c.error;

    const Bytes frame = support::fromHex(hex);
    if (hex.size() % 2 != 0 || support::toHex(frame) != hex) return "hex is not lower-case hexadecimal bytes";
    if (frame.size() > kMaxFrame) return "frame is longer than the 180 bytes PROTOCOL.md allows";

    Bytes encoded;
    JsonDocument decoded_doc;
    JsonObject decoded = decoded_doc.to<JsonObject>();

    if (channel == "command") {
        auto it = kCommandTypes.find(type);
        if (it == kCommandTypes.end()) return "unknown command type '" + type + "'";
        checkCommand(c, it->second, fields, frame, encoded, decoded);
    } else if (channel == "state") {
        if (type == "STATE") {
            checkState(c, fields, frame, encoded, decoded);
        } else if (type == "CATALOG") {
            checkCatalog(c, fields, frame, encoded, decoded);
        } else if (type == "PRESETS") {
            checkPresets(c, fields, frame, encoded, decoded);
        } else if (type == "INFO") {
            checkInfo(c, fields, frame, encoded, decoded);
        } else if (type == "ERROR") {
            checkError(c, fields, frame, encoded, decoded);
        } else {
            return "unknown notification type '" + type + "'";
        }
    } else if (channel == "live") {
        if (type != "LIVE") return "unknown live type '" + type + "'";
        checkLive(c, fields, frame, encoded, decoded);
    } else {
        return "unknown channel '" + channel + "'";
    }
    if (!c.ok()) return c.error;

    if (encoded != frame) return "encode(fields) = " + support::toHex(encoded) + ", expected " + hex;
    if (!jsonEqual(decoded, fields)) return "decode(hex) = " + dump(decoded) + ", expected " + dump(fields);
    return std::string();
}

JsonArrayConst vectors() {
    TEST_ASSERT_TRUE_MESSAGE(g_load_error.empty(), g_load_error.c_str());
    JsonArrayConst list = g_doc["vectors"].as<JsonArrayConst>();
    TEST_ASSERT_FALSE_MESSAGE(list.isNull(), "v1.json has no \"vectors\" array");
    return list;
}

// ---- Tests ---------------------------------------------------------------------------

void test_file_is_protocol_1() {
    vectors();
    TEST_ASSERT_EQUAL_MESSAGE(kProtoVersion, g_doc["protocol"].as<int>(), "v1.json is for another protocol version");
}

void test_every_vector_encodes_and_decodes() {
    JsonArrayConst list = vectors();
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, list.size(), "no vectors in the file");
    int failures = 0;
    for (JsonVariantConst item : list) {
        JsonObjectConst v = item.as<JsonObjectConst>();
        const std::string name = v["name"].is<const char*>() ? v["name"].as<std::string>() : std::string("(unnamed)");
        const std::string problem = v.isNull() ? std::string("vector is not an object") : checkVector(v);
        if (!problem.empty()) {
            ++failures;
            printf("VECTOR FAILED  %s: %s\n", name.c_str(), problem.c_str());
        }
    }
    char summary[96];
    snprintf(summary, sizeof summary, "%d of %d golden vectors failed (details above)", failures, static_cast<int>(list.size()));
    TEST_ASSERT_EQUAL_MESSAGE(0, failures, summary);
}

// Guards against the file quietly losing coverage: every frame type the codec
// implements must appear in at least one vector.
void test_every_frame_type_has_a_vector() {
    std::set<std::string> seen;
    for (JsonVariantConst item : vectors()) seen.insert(item["type"].as<std::string>());
    std::string missing;
    for (const auto& entry : kCommandTypes) {
        if (!seen.count(entry.first)) missing += entry.first + " ";
    }
    for (const std::string& type : kStateTypes) {
        if (!seen.count(type)) missing += type + " ";
    }
    if (!seen.count("LIVE")) missing += "LIVE ";
    TEST_ASSERT_TRUE_MESSAGE(missing.empty(), ("no vector for: " + missing).c_str());
}

// The firmware itself, not just the codec, must agree with the examples the
// app developer sees: its INFO frame and the catalogue entries that appear in
// the vectors.
void test_engine_matches_info_and_catalog_vectors() {
    MemoryStorage storage;
    Engine engine(storage);
    engine.begin();
    int info_vectors = 0;
    int effect_entries = 0;
    for (JsonVariantConst item : vectors()) {
        const std::string type = item["type"].as<std::string>();
        JsonObjectConst fields = item["fields"].as<JsonObjectConst>();
        const std::string name = item["name"].as<std::string>();
        if (type == "INFO") {
            ++info_vectors;
            support::CaptureSink sink;
            const uint8_t get_info[] = {0x20};
            engine.handleCommand(get_info, sizeof get_info, 0, sink);
            TEST_ASSERT_EQUAL_MESSAGE(1, sink.frames.size(), name.c_str());
            TEST_ASSERT_EQUAL_STRING_MESSAGE(item["hex"].as<const char*>(), support::toHex(sink.frames[0]).c_str(),
                                             "INFO from a default Engine differs from the golden frame");
        } else if (type == "CATALOG") {
            const uint8_t kind = fields["kind"].as<uint8_t>();
            if (kind == kCatalogEffects) {
                TEST_ASSERT_EQUAL_MESSAGE(effectCount(), fields["total"].as<int>(), "effect catalogue total");
            } else {
                TEST_ASSERT_EQUAL_MESSAGE(paletteCount(), fields["total"].as<int>(), "palette catalogue total");
            }
            for (JsonVariantConst e : fields["entries"].as<JsonArrayConst>()) {
                const uint8_t id = e["id"].as<uint8_t>();
                if (kind == kCatalogEffects) {
                    const EffectInfo* info = effectInfo(id);
                    TEST_ASSERT_NOT_NULL_MESSAGE(info, "vector names an effect id the firmware lacks");
                    TEST_ASSERT_EQUAL_STRING(e["name"].as<const char*>(), info->name);
                    TEST_ASSERT_EQUAL_HEX8_MESSAGE(e["param_flags"].as<uint8_t>(), info->param_flags, info->name);
                    ++effect_entries;
                } else {
                    TEST_ASSERT_NOT_NULL_MESSAGE(paletteName(id), "vector names a palette id the firmware lacks");
                    TEST_ASSERT_EQUAL_STRING(e["name"].as<const char*>(), paletteName(id));
                }
            }
        }
    }
    TEST_ASSERT_GREATER_THAN(0, info_vectors);
    TEST_ASSERT_GREATER_THAN(0, effect_entries);
}

}  // namespace

int main(int, char**) {
    std::string json, path;
    if (!loadVectors(json, path)) {
        g_load_error = "cannot find protocol/vectors/v1.json (set JACKET_VECTORS to its path)";
    } else {
        const DeserializationError error = deserializeJson(g_doc, json);
        if (error) g_load_error = "cannot parse " + path + ": " + error.c_str();
    }

    UNITY_BEGIN();
    RUN_TEST(test_file_is_protocol_1);
    RUN_TEST(test_every_vector_encodes_and_decodes);
    RUN_TEST(test_every_frame_type_has_a_vector);
    RUN_TEST(test_engine_matches_info_and_catalog_vectors);
    return UNITY_END();
}
