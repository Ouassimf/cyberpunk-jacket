#include "jacket/protocol.h"

#include <string.h>

#include "jacket/wire.h"

namespace jacket {

namespace {

constexpr size_t frameCap(size_t cap) { return cap < kMaxFrame ? cap : kMaxFrame; }

bool segmentSelectorValid(uint8_t segment) { return segment < kMaxSegments || segment == kAllSegments; }

// Bytes of value that follow `param` in SET_PARAM; 0 for an unknown id.
size_t paramValueSize(uint8_t param) {
    switch (static_cast<Param>(param)) {
        case Param::Effect:
        case Param::Palette:
        case Param::Speed:
        case Param::Intensity:
            return 1;
        case Param::Color1:
        case Param::Color2:
            return 3;
    }
    return 0;
}

void writeStr(wire::Writer& w, const char* data, uint8_t len) {
    w.u8(len);
    w.bytes(data, len);
}

}  // namespace

StrView strView(const char* zero_terminated) {
    StrView v;
    if (zero_terminated == nullptr) return v;
    const size_t n = strlen(zero_terminated);
    v.data = zero_terminated;
    v.len = static_cast<uint8_t>(n > 255 ? 255 : n);
    return v;
}

bool operator==(const StrView& a, const StrView& b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
}

Name makeName(const char* zero_terminated) {
    Name name;
    if (zero_terminated == nullptr) return name;
    const size_t n = strlen(zero_terminated);
    name.len = static_cast<uint8_t>(n > kMaxPresetName ? kMaxPresetName : n);
    memcpy(name.text, zero_terminated, name.len);
    return name;
}

bool operator==(const Name& a, const Name& b) { return a.len == b.len && memcmp(a.text, b.text, a.len) == 0; }

// ---- Commands ---------------------------------------------------------------

ErrorCode decodeCommand(const uint8_t* data, size_t len, Command& out) {
    if (data == nullptr || len == 0) return ErrorCode::TooShort;
    const size_t payload = len - 1;
    wire::Reader r(data + 1, payload);
    out = Command();
    out.opcode = static_cast<Opcode>(data[0]);

    switch (out.opcode) {
        case Opcode::SetPower:
        case Opcode::SetDevMode:
            if (payload < 1) return ErrorCode::TooShort;
            out.on = r.u8();
            return out.on <= 1 ? ErrorCode::None : ErrorCode::OutOfRange;

        case Opcode::SetBrightness:
            if (payload < 1) return ErrorCode::TooShort;
            out.value = r.u8();
            return ErrorCode::None;

        case Opcode::SetEffect:
            if (payload < 11) return ErrorCode::TooShort;
            out.segment = r.u8();
            out.config = r.config();
            return segmentSelectorValid(out.segment) ? ErrorCode::None : ErrorCode::OutOfRange;

        case Opcode::SetParam: {
            if (payload < 2) return ErrorCode::TooShort;
            out.segment = r.u8();
            const uint8_t param = r.u8();
            const size_t value_size = paramValueSize(param);
            if (value_size == 0) return ErrorCode::OutOfRange;
            if (payload < 2 + value_size) return ErrorCode::TooShort;
            out.param = static_cast<Param>(param);
            if (value_size == 1) {
                out.value = r.u8();
            } else {
                out.color = r.rgb();
            }
            return segmentSelectorValid(out.segment) ? ErrorCode::None : ErrorCode::OutOfRange;
        }

        case Opcode::SetSegments:
            if (payload < 1) return ErrorCode::TooShort;
            out.count = r.u8();
            if (out.count == 0 || out.count > kMaxSegments) return ErrorCode::OutOfRange;
            if (payload < 1 + static_cast<size_t>(out.count) * 4) return ErrorCode::TooShort;
            for (uint8_t i = 0; i < out.count; ++i) out.geometry[i] = r.geometry();
            for (uint8_t i = 0; i < out.count; ++i) {
                if (!isValid(out.geometry[i])) return ErrorCode::OutOfRange;
            }
            return ErrorCode::None;

        case Opcode::Identify:
            if (payload < 4) return ErrorCode::TooShort;
            out.strip_mask = r.u8();
            out.start = r.u8();
            out.length = r.u8();
            out.seconds = r.u8();
            if (out.length != 0 && out.seconds != 0) {  // otherwise it is a cancel: nothing to check
                SegmentGeometry range;
                range.strip_mask = out.strip_mask;
                range.start = out.start;
                range.length = out.length;
                range.flags = 0;
                if (!isValid(range)) return ErrorCode::OutOfRange;
            }
            return ErrorCode::None;

        case Opcode::PresetSave: {
            if (payload < 2) return ErrorCode::TooShort;
            out.slot = r.u8();
            const uint8_t name_len = r.u8();
            if (name_len > kMaxPresetName) return ErrorCode::OutOfRange;
            const uint8_t* text = r.bytes(name_len);
            if (text == nullptr) return ErrorCode::TooShort;
            out.name.len = name_len;
            if (name_len) memcpy(out.name.text, text, name_len);
            return out.slot < kPresetSlots ? ErrorCode::None : ErrorCode::OutOfRange;
        }

        case Opcode::PresetLoad:
        case Opcode::PresetDelete:
            if (payload < 1) return ErrorCode::TooShort;
            out.slot = r.u8();
            return out.slot < kPresetSlots ? ErrorCode::None : ErrorCode::OutOfRange;

        case Opcode::GetPresets:
        case Opcode::GetInfo:
            return ErrorCode::None;

        case Opcode::GetCatalog:
            if (payload < 2) return ErrorCode::TooShort;
            out.kind = r.u8();
            out.start = r.u8();
            return out.kind <= kCatalogPalettes ? ErrorCode::None : ErrorCode::OutOfRange;

        case Opcode::SetBudget:
            if (payload < 2) return ErrorCode::TooShort;
            out.milliamps = r.u16();
            return ErrorCode::None;

        case Opcode::BootOtherSlot:
            if (payload < 1) return ErrorCode::TooShort;
            out.value = r.u8();
            return out.value == kBootOtherSlotConfirm ? ErrorCode::None : ErrorCode::OutOfRange;
    }
    return ErrorCode::UnknownOpcode;
}

size_t encodeCommand(const Command& cmd, uint8_t* out, size_t cap) {
    if (out == nullptr) return 0;
    wire::Writer w(out, frameCap(cap));
    w.u8(static_cast<uint8_t>(cmd.opcode));

    switch (cmd.opcode) {
        case Opcode::SetPower:
        case Opcode::SetDevMode:
            w.u8(cmd.on);
            return w.size();
        case Opcode::SetBrightness:
        case Opcode::BootOtherSlot:
            w.u8(cmd.value);
            return w.size();
        case Opcode::SetEffect:
            w.u8(cmd.segment);
            w.config(cmd.config);
            return w.size();
        case Opcode::SetParam: {
            const size_t value_size = paramValueSize(static_cast<uint8_t>(cmd.param));
            if (value_size == 0) return 0;
            w.u8(cmd.segment);
            w.u8(static_cast<uint8_t>(cmd.param));
            if (value_size == 1) {
                w.u8(cmd.value);
            } else {
                w.rgb(cmd.color);
            }
            return w.size();
        }
        case Opcode::SetSegments:
            if (cmd.count > kMaxSegments) return 0;
            w.u8(cmd.count);
            for (uint8_t i = 0; i < cmd.count; ++i) w.geometry(cmd.geometry[i]);
            return w.size();
        case Opcode::Identify:
            w.u8(cmd.strip_mask);
            w.u8(cmd.start);
            w.u8(cmd.length);
            w.u8(cmd.seconds);
            return w.size();
        case Opcode::PresetSave:
            if (cmd.name.len > kMaxPresetName) return 0;
            w.u8(cmd.slot);
            writeStr(w, cmd.name.text, cmd.name.len);
            return w.size();
        case Opcode::PresetLoad:
        case Opcode::PresetDelete:
            w.u8(cmd.slot);
            return w.size();
        case Opcode::GetPresets:
        case Opcode::GetInfo:
            return w.size();
        case Opcode::GetCatalog:
            w.u8(cmd.kind);
            w.u8(cmd.start);
            return w.size();
        case Opcode::SetBudget:
            w.u16(cmd.milliamps);
            return w.size();
    }
    return 0;
}

// ---- STATE ------------------------------------------------------------------

size_t encodeState(const StateMsg& msg, uint8_t* out, size_t cap) {
    if (out == nullptr || msg.segment_count > kMaxSegments) return 0;
    wire::Writer w(out, frameCap(cap));
    w.u8(static_cast<uint8_t>(Notify::State));
    w.u8(msg.proto);
    w.u16(msg.rev);
    w.u8(msg.power);
    w.u8(msg.brightness);
    w.u16(msg.budget_ma);
    w.u8(msg.flags);
    w.u8(msg.preset);
    w.u8(msg.segment_count);
    for (uint8_t i = 0; i < msg.segment_count; ++i) {
        w.geometry(msg.segments[i].geometry);
        w.config(msg.segments[i].config);
    }
    return w.size();
}

ErrorCode decodeState(const uint8_t* data, size_t len, StateMsg& out) {
    if (data == nullptr || len == 0) return ErrorCode::TooShort;
    if (data[0] != static_cast<uint8_t>(Notify::State)) return ErrorCode::UnknownOpcode;
    wire::Reader r(data + 1, len - 1);
    out = StateMsg();
    out.proto = r.u8();
    out.rev = r.u16();
    out.power = r.u8();
    out.brightness = r.u8();
    out.budget_ma = r.u16();
    out.flags = r.u8();
    out.preset = r.u8();
    out.segment_count = r.u8();
    if (!r.ok()) return ErrorCode::TooShort;
    if (out.segment_count == 0 || out.segment_count > kMaxSegments) return ErrorCode::OutOfRange;
    for (uint8_t i = 0; i < out.segment_count; ++i) {
        out.segments[i].geometry = r.geometry();
        out.segments[i].config = r.config();
    }
    return r.ok() ? ErrorCode::None : ErrorCode::TooShort;
}

// ---- PRESETS ----------------------------------------------------------------

size_t encodePresets(const PresetList& msg, uint8_t* out, size_t cap) {
    if (out == nullptr || msg.count > kPresetSlots) return 0;
    wire::Writer w(out, frameCap(cap));
    w.u8(static_cast<uint8_t>(Notify::Presets));
    w.u8(msg.count);
    for (uint8_t i = 0; i < msg.count; ++i) {
        if (msg.items[i].name.len > kMaxPresetName) return 0;
        w.u8(msg.items[i].slot);
        writeStr(w, msg.items[i].name.text, msg.items[i].name.len);
    }
    return w.size();
}

ErrorCode decodePresets(const uint8_t* data, size_t len, PresetList& out) {
    if (data == nullptr || len == 0) return ErrorCode::TooShort;
    if (data[0] != static_cast<uint8_t>(Notify::Presets)) return ErrorCode::UnknownOpcode;
    wire::Reader r(data + 1, len - 1);
    out = PresetList();
    out.count = r.u8();
    if (!r.ok()) return ErrorCode::TooShort;
    if (out.count > kPresetSlots) return ErrorCode::OutOfRange;
    for (uint8_t i = 0; i < out.count; ++i) {
        PresetList::Item& item = out.items[i];
        item.slot = r.u8();
        const uint8_t name_len = r.u8();
        if (!r.ok()) return ErrorCode::TooShort;
        if (item.slot >= kPresetSlots || name_len > kMaxPresetName) return ErrorCode::OutOfRange;
        const uint8_t* text = r.bytes(name_len);
        if (text == nullptr) return ErrorCode::TooShort;
        item.name.len = name_len;
        if (name_len) memcpy(item.name.text, text, name_len);
    }
    return ErrorCode::None;
}

// ---- INFO -------------------------------------------------------------------

size_t encodeInfo(const InfoMsg& msg, uint8_t* out, size_t cap) {
    if (out == nullptr || msg.strip_count > kMaxInfoStrips) return 0;
    wire::Writer w(out, frameCap(cap));
    w.u8(static_cast<uint8_t>(Notify::Info));
    w.u8(msg.proto);
    w.u8(msg.fw_major);
    w.u8(msg.fw_minor);
    w.u8(msg.fw_patch);
    w.u8(msg.strip_count);
    for (uint8_t i = 0; i < msg.strip_count; ++i) w.u16(msg.leds[i]);
    w.u8(msg.max_segments);
    w.u8(msg.preset_slots);
    w.u8(msg.effect_count);
    w.u8(msg.palette_count);
    writeStr(w, msg.build.data, msg.build.len);
    return w.size();
}

ErrorCode decodeInfo(const uint8_t* data, size_t len, InfoMsg& out) {
    if (data == nullptr || len == 0) return ErrorCode::TooShort;
    if (data[0] != static_cast<uint8_t>(Notify::Info)) return ErrorCode::UnknownOpcode;
    wire::Reader r(data + 1, len - 1);
    out = InfoMsg();
    out.proto = r.u8();
    out.fw_major = r.u8();
    out.fw_minor = r.u8();
    out.fw_patch = r.u8();
    out.strip_count = r.u8();
    if (!r.ok()) return ErrorCode::TooShort;
    if (out.strip_count > kMaxInfoStrips) return ErrorCode::OutOfRange;
    for (uint8_t i = 0; i < out.strip_count; ++i) out.leds[i] = r.u16();
    out.max_segments = r.u8();
    out.preset_slots = r.u8();
    out.effect_count = r.u8();
    out.palette_count = r.u8();
    const uint8_t build_len = r.u8();
    const uint8_t* build = r.bytes(build_len);
    if (!r.ok()) return ErrorCode::TooShort;
    out.build.data = reinterpret_cast<const char*>(build);
    out.build.len = build_len;
    return ErrorCode::None;
}

// ---- ERROR ------------------------------------------------------------------

size_t encodeError(uint8_t opcode, ErrorCode code, uint8_t* out, size_t cap) {
    if (out == nullptr) return 0;
    wire::Writer w(out, frameCap(cap));
    w.u8(static_cast<uint8_t>(Notify::Error));
    w.u8(opcode);
    w.u8(static_cast<uint8_t>(code));
    return w.size();
}

ErrorCode decodeError(const uint8_t* data, size_t len, ErrorMsg& out) {
    if (data == nullptr || len == 0) return ErrorCode::TooShort;
    if (data[0] != static_cast<uint8_t>(Notify::Error)) return ErrorCode::UnknownOpcode;
    if (len < 3) return ErrorCode::TooShort;
    out.opcode = data[1];
    out.code = data[2];
    return ErrorCode::None;
}

// ---- CATALOG ----------------------------------------------------------------

CatalogWriter::CatalogWriter(uint8_t* out, size_t cap, uint8_t kind, uint8_t start, uint8_t total)
    : out_(out), cap_(frameCap(cap)), kind_(kind) {
    if (out_ == nullptr || cap_ < 5 || kind > kCatalogPalettes) {
        ok_ = false;
        return;
    }
    out_[0] = static_cast<uint8_t>(Notify::Catalog);
    out_[1] = kind;
    out_[2] = start;
    out_[3] = total;
    out_[4] = 0;  // count, patched by finish()
    pos_ = 5;
}

bool CatalogWriter::add(uint8_t id, uint8_t param_flags, StrView name) {
    if (!ok_ || count_ == 255) return false;
    const bool effects = kind_ == kCatalogEffects;
    const size_t need = (effects ? 3u : 2u) + name.len;
    if (cap_ - pos_ < need) return false;
    out_[pos_++] = id;
    if (effects) out_[pos_++] = param_flags;
    out_[pos_++] = name.len;
    if (name.len) memcpy(out_ + pos_, name.data, name.len);
    pos_ += name.len;
    ++count_;
    return true;
}

size_t CatalogWriter::finish() {
    if (!ok_) return 0;
    out_[4] = count_;
    return pos_;
}

ErrorCode CatalogReader::begin(const uint8_t* data, size_t len) {
    data_ = nullptr;
    read_ = 0;
    count = 0;
    if (data == nullptr || len == 0) return ErrorCode::TooShort;
    if (data[0] != static_cast<uint8_t>(Notify::Catalog)) return ErrorCode::UnknownOpcode;
    if (len < 5) return ErrorCode::TooShort;
    if (data[1] > kCatalogPalettes) return ErrorCode::OutOfRange;
    kind = data[1];
    start = data[2];
    total = data[3];
    count = data[4];
    data_ = data;
    len_ = len;
    pos_ = 5;
    return ErrorCode::None;
}

ErrorCode CatalogReader::next(CatalogEntry& out) {
    if (data_ == nullptr || read_ >= count) return ErrorCode::NotFound;
    wire::Reader r(data_ + pos_, len_ - pos_);
    out = CatalogEntry();
    out.id = r.u8();
    if (kind == kCatalogEffects) out.param_flags = r.u8();
    const uint8_t name_len = r.u8();
    const uint8_t* text = r.bytes(name_len);
    if (!r.ok()) return ErrorCode::TooShort;
    out.name.data = reinterpret_cast<const char*>(text);
    out.name.len = name_len;
    pos_ = len_ - r.remaining();
    ++read_;
    return ErrorCode::None;
}

// ---- Live input -------------------------------------------------------------

ErrorCode decodeLive(const uint8_t* data, size_t len, LiveInput& out) {
    if (data == nullptr || len < kLiveFrameSize) return ErrorCode::TooShort;
    wire::Reader r(data, len);
    out.flags = r.u8();
    out.seq = r.u8();
    out.level = r.u8();
    out.bass = r.u8();
    out.mid = r.u8();
    out.treble = r.u8();
    out.bpm_x10 = r.u16();
    out.beat_phase = r.u8();
    out.motion = r.u8();
    out.tilt_x = static_cast<int8_t>(r.u8());
    out.tilt_y = static_cast<int8_t>(r.u8());
    out.t_ms = r.u16();
    return ErrorCode::None;
}

size_t encodeLive(const LiveInput& in, uint8_t* out, size_t cap) {
    if (out == nullptr) return 0;
    wire::Writer w(out, frameCap(cap));
    w.u8(in.flags);
    w.u8(in.seq);
    w.u8(in.level);
    w.u8(in.bass);
    w.u8(in.mid);
    w.u8(in.treble);
    w.u16(in.bpm_x10);
    w.u8(in.beat_phase);
    w.u8(in.motion);
    w.u8(static_cast<uint8_t>(in.tilt_x));
    w.u8(static_cast<uint8_t>(in.tilt_y));
    w.u16(in.t_ms);
    return w.size();
}

}  // namespace jacket
