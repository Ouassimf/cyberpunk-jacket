#include "jacket/engine.h"

#include <string.h>

#include "jacket/palette.h"
#include "jacket/wire.h"

namespace jacket {

namespace {

constexpr const char* kKeySettings = "set";
constexpr const char* kKeyLast = "last";
constexpr const char* kPresetKeys[kPresetSlots] = {"p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7"};

constexpr uint8_t kLastRecordVersion = 1;    // [1][preset marker][scene blob]
constexpr uint8_t kPresetRecordVersion = 1;  // [1][name len][name][scene blob]
constexpr uint8_t kMaxBuildLen = 32;

// Reads a preset record. Either output may be nullptr.
bool parsePreset(const uint8_t* data, size_t len, Name* name, Scene* scene) {
    wire::Reader r(data, len);
    if (r.u8() != kPresetRecordVersion) return false;
    const uint8_t name_len = r.u8();
    if (!r.ok() || name_len > kMaxPresetName) return false;
    const uint8_t* text = r.bytes(name_len);
    if (text == nullptr) return false;
    Scene loaded;
    if (!deserializeScene(data + (len - r.remaining()), r.remaining(), loaded)) return false;
    if (name != nullptr) {
        *name = Name();
        name->len = name_len;
        if (name_len) memcpy(name->text, text, name_len);
    }
    if (scene != nullptr) *scene = loaded;
    return true;
}

}  // namespace

Engine::Engine(Storage& storage, const EngineConfig& config) : storage_(storage), config_(config) {
    scene_ = defaultScene();
    scene_.brightness = config_.default_brightness;
    for (uint8_t i = 0; i < kMaxSegments; ++i) resetSegment(i);
}

void Engine::begin(const BootOptions& options) {
    uint8_t buf[kMaxStorageBlob];

    settings_ = Settings();
    size_t n = storage_.read(kKeySettings, buf, sizeof buf);
    if (n != 0) deserializeSettings(buf, n, settings_);
    dev_forced_ = options.force_dev_mode;

    scene_ = defaultScene();
    scene_.brightness = config_.default_brightness;
    preset_ = kNoPreset;
    n = options.restore_scene ? storage_.read(kKeyLast, buf, sizeof buf) : 0;
    if (n >= 2 && buf[0] == kLastRecordVersion) {
        Scene loaded;
        if (deserializeScene(buf + 2, n - 2, loaded)) {
            sanitize(loaded);
            scene_ = loaded;
            preset_ = buf[1];
        }
    }

    for (uint8_t slot = 0; slot < kPresetSlots; ++slot) {
        n = storage_.read(kPresetKeys[slot], buf, sizeof buf);
        preset_used_[slot] = n != 0 && parsePreset(buf, n, &preset_names_[slot], nullptr);
        if (!preset_used_[slot]) preset_names_[slot] = Name();
    }
    if (preset_ >= kPresetSlots || !preset_used_[preset_]) preset_ = kNoPreset;

    power_ = true;
    rev_ = 0;
    state_changed_ = false;
    pending_preset_ = kNoPreset;
    signals_.reset();
    live_active_ = false;
    phone_sound_ = false;
    local_active_ = false;
    fed_by_ = SoundSource::None;
    fading_ = false;
    for (auto& strip : shown_) {
        for (Rgb& pixel : strip) pixel = kBlack;
    }
    for (uint8_t i = 0; i < kMaxSegments; ++i) resetSegment(i);
    last_dirty_ = false;
    settings_dirty_ = false;
    clock_running_ = false;
    render_dirty_ = true;
}

// ---- Commands -----------------------------------------------------------------

ErrorCode Engine::handleCommand(const uint8_t* frame, size_t len, uint32_t now_ms, FrameSink& reply) {
    Command cmd;
    ErrorCode error = decodeCommand(frame, len, cmd);
    if (error == ErrorCode::None) error = apply(cmd, now_ms, reply);
    if (error != ErrorCode::None) {
        uint8_t out[3];
        const uint8_t opcode = (frame != nullptr && len > 0) ? frame[0] : 0;
        const size_t n = encodeError(opcode, error, out, sizeof out);
        reply.send(out, n);
    }
    return error;
}

ErrorCode Engine::apply(const Command& cmd, uint32_t now_ms, FrameSink& reply) {
    const Scene before = scene_;
    ErrorCode error = ErrorCode::None;

    switch (cmd.opcode) {
        case Opcode::SetPower:
            if ((cmd.on != 0) != power_) {
                power_ = cmd.on != 0;
                startFade();  // down to black, or up from it
            }
            break;
        case Opcode::SetBrightness:
            scene_.brightness = cmd.value;
            break;
        case Opcode::SetEffect:
            error = setEffect(cmd.segment, cmd.config);
            break;
        case Opcode::SetParam:
            error = setParam(cmd);
            break;
        case Opcode::SetSegments:
            setSegments(cmd);
            break;
        case Opcode::Identify:
            // Not part of STATE: no rev bump, the overlay just starts or stops.
            identify_.active = cmd.length != 0 && cmd.seconds != 0;
            identify_.strip_mask = cmd.strip_mask;
            identify_.start = cmd.start;
            identify_.length = cmd.length;
            identify_.start_ms = now_ms;
            identify_.duration_ms = static_cast<uint32_t>(cmd.seconds) * 1000u;
            render_dirty_ = true;
            return ErrorCode::None;
        case Opcode::PresetSave:
            error = presetSave(cmd.slot, cmd.name);
            break;
        case Opcode::PresetLoad:
            error = presetLoad(cmd.slot);
            break;
        case Opcode::PresetDelete:
            error = presetDelete(cmd.slot);
            break;
        case Opcode::GetPresets:
            sendPresets(reply);
            return ErrorCode::None;
        case Opcode::GetInfo:
            sendInfo(reply);
            return ErrorCode::None;
        case Opcode::GetCatalog:
            sendCatalog(cmd.kind, cmd.start, reply);
            return ErrorCode::None;
        case Opcode::SetBudget: {
            const uint16_t budget = clampBudget(cmd.milliamps);
            if (budget != settings_.budget_ma) {
                settings_.budget_ma = budget;
                settings_dirty_ = true;
                last_change_ms_ = now_ms;
            }
            break;
        }
        case Opcode::SetDevMode:
            dev_forced_ = false;  // from here on the command decides
            if ((cmd.on != 0) != settings_.dev_mode) {
                settings_.dev_mode = cmd.on != 0;
                settings_dirty_ = true;
                last_change_ms_ = now_ms;
            }
            break;
        case Opcode::BootOtherSlot:
            // Not part of STATE: no rev bump. The jacket restarts instead.
            if (!other_slot_usable_) return ErrorCode::NotFound;
            boot_other_slot_ = true;
            return ErrorCode::None;
    }

    if (error != ErrorCode::None) return error;
    committed(before, now_ms);
    // "PRESET_SAVE and PRESET_DELETE also answer with a PRESETS notification
    // carrying the updated list."
    if (cmd.opcode == Opcode::PresetSave || cmd.opcode == Opcode::PresetDelete) sendPresets(reply);
    return ErrorCode::None;
}

// Every accepted command that can change STATE ends here.
void Engine::committed(const Scene& before, uint32_t now_ms) {
    if (scene_ != before) {
        preset_ = kNoPreset;  // the scene no longer matches the slot it came from
        last_dirty_ = true;
    }
    if (pending_preset_ != kNoPreset) {  // ... unless this command was the save or load itself
        preset_ = pending_preset_;
        pending_preset_ = kNoPreset;
        last_dirty_ = true;
    }
    if (last_dirty_) last_change_ms_ = now_ms;
    render_dirty_ = true;
    stateChanged();
}

void Engine::stateChanged() {
    ++rev_;
    state_changed_ = true;
}

// The picture is about to cut: blend from what is on the strips instead.
// A fade that is already running restarts from the frame it had reached.
void Engine::startFade() {
    if (config_.crossfade_ms == 0) return;
    memcpy(static_cast<void*>(fade_from_), shown_, sizeof fade_from_);
    fading_ = true;
    fade_started_ = false;
}

ErrorCode Engine::setEffect(uint8_t segment, const EffectConfig& config) {
    if (segment != kAllSegments && segment >= scene_.segment_count) return ErrorCode::OutOfRange;
    if (effectInfo(config.effect) == nullptr || config.palette >= paletteCount()) return ErrorCode::OutOfRange;
    for (uint8_t i = 0; i < scene_.segment_count; ++i) {
        if (segment != kAllSegments && segment != i) continue;
        if (scene_.segments[i].config.effect != config.effect) {
            resetSegment(i);
            if (power_) startFade();
        }
        scene_.segments[i].config = config;
    }
    return ErrorCode::None;
}

ErrorCode Engine::setParam(const Command& cmd) {
    if (cmd.segment != kAllSegments && cmd.segment >= scene_.segment_count) return ErrorCode::OutOfRange;
    if (cmd.param == Param::Effect && effectInfo(cmd.value) == nullptr) return ErrorCode::OutOfRange;
    if (cmd.param == Param::Palette && cmd.value >= paletteCount()) return ErrorCode::OutOfRange;
    for (uint8_t i = 0; i < scene_.segment_count; ++i) {
        if (cmd.segment != kAllSegments && cmd.segment != i) continue;
        EffectConfig& config = scene_.segments[i].config;
        switch (cmd.param) {
            case Param::Effect:
                if (config.effect != cmd.value) {
                    resetSegment(i);
                    if (power_) startFade();
                }
                config.effect = cmd.value;
                break;
            case Param::Palette:
                config.palette = cmd.value;
                break;
            case Param::Speed:
                config.speed = cmd.value;
                break;
            case Param::Intensity:
                config.intensity = cmd.value;
                break;
            case Param::Color1:
                config.color1 = cmd.color;
                break;
            case Param::Color2:
                config.color2 = cmd.color;
                break;
        }
    }
    return ErrorCode::None;
}

void Engine::setSegments(const Command& cmd) {
    const Scene old = scene_;
    applySegments(scene_, cmd.geometry, cmd.count);  // cannot fail: the codec validated the layout
    for (uint8_t i = 0; i < scene_.segment_count; ++i) {
        const bool is_new = i >= old.segment_count;
        if (is_new || old.segments[i].geometry.length != scene_.segments[i].geometry.length) resetSegment(i);
    }
    if (power_ && scene_ != old) startFade();
}

// ---- Presets ------------------------------------------------------------------

ErrorCode Engine::presetSave(uint8_t slot, const Name& name) {
    uint8_t buf[kMaxStorageBlob];
    wire::Writer w(buf, sizeof buf);
    w.u8(kPresetRecordVersion);
    w.u8(name.len);
    w.bytes(name.text, name.len);
    const size_t head = w.size();
    const size_t body = serializeScene(scene_, buf + head, sizeof buf - head);
    if (head == 0 || body == 0 || !storage_.write(kPresetKeys[slot], buf, head + body)) {
        return ErrorCode::StorageFailure;
    }
    preset_used_[slot] = true;
    preset_names_[slot] = name;
    pending_preset_ = slot;
    return ErrorCode::None;
}

ErrorCode Engine::presetLoad(uint8_t slot) {
    if (!preset_used_[slot]) return ErrorCode::NotFound;
    uint8_t buf[kMaxStorageBlob];
    const size_t n = storage_.read(kPresetKeys[slot], buf, sizeof buf);
    Scene loaded;
    if (n == 0 || !parsePreset(buf, n, nullptr, &loaded)) return ErrorCode::NotFound;
    sanitize(loaded);
    scene_ = loaded;
    for (uint8_t i = 0; i < kMaxSegments; ++i) resetSegment(i);
    if (power_) startFade();
    pending_preset_ = slot;
    return ErrorCode::None;
}

ErrorCode Engine::presetDelete(uint8_t slot) {
    if (!preset_used_[slot]) return ErrorCode::NotFound;
    // A slot that could not be erased is still there after a reboot: refuse,
    // so the list the app holds stays true.
    if (!storage_.erase(kPresetKeys[slot])) return ErrorCode::StorageFailure;
    preset_used_[slot] = false;
    preset_names_[slot] = Name();
    if (preset_ == slot) {
        preset_ = kNoPreset;
        last_dirty_ = true;
    }
    return ErrorCode::None;
}

// ---- Replies ------------------------------------------------------------------

void Engine::sendPresets(FrameSink& reply) const {
    PresetList list;
    for (uint8_t slot = 0; slot < kPresetSlots; ++slot) {
        if (!preset_used_[slot]) continue;
        list.items[list.count].slot = slot;
        list.items[list.count].name = preset_names_[slot];
        ++list.count;
    }
    uint8_t out[kMaxFrame];
    reply.send(out, encodePresets(list, out, sizeof out));
}

void Engine::sendInfo(FrameSink& reply) const {
    InfoMsg info;
    info.proto = kProtoVersion;
    info.fw_major = config_.fw_major;
    info.fw_minor = config_.fw_minor;
    info.fw_patch = config_.fw_patch;
    info.strip_count = kStripCount;
    for (uint8_t i = 0; i < kStripCount; ++i) info.leds[i] = kLedsPerStrip;
    info.max_segments = kMaxSegments;
    info.preset_slots = kPresetSlots;
    info.effect_count = effectCount();
    info.palette_count = paletteCount();
    info.build = strView(config_.build);
    if (info.build.len > kMaxBuildLen) info.build.len = kMaxBuildLen;
    uint8_t out[kMaxFrame];
    reply.send(out, encodeInfo(info, out, sizeof out));
}

void Engine::sendCatalog(uint8_t kind, uint8_t start, FrameSink& reply) const {
    const uint8_t total = kind == kCatalogEffects ? effectCount() : paletteCount();
    uint8_t out[kMaxFrame];
    CatalogWriter page(out, sizeof out, kind, start, total);
    // As many entries as fit. `start` at or past the end gives an empty page.
    for (uint16_t id = start; id < total; ++id) {
        bool added;
        if (kind == kCatalogEffects) {
            const EffectInfo* info = effectInfo(static_cast<uint8_t>(id));
            added = page.add(info->id, info->param_flags, strView(info->name));
        } else {
            added = page.add(static_cast<uint8_t>(id), 0, strView(paletteName(static_cast<uint8_t>(id))));
        }
        if (!added) break;
    }
    reply.send(out, page.finish());
}

size_t Engine::encodeState(uint8_t* out, size_t cap) const {
    StateMsg msg;
    msg.proto = kProtoVersion;
    msg.rev = rev_;
    msg.power = power_ ? 1 : 0;
    msg.brightness = scene_.brightness;
    msg.budget_ma = settings_.budget_ma;
    msg.flags = static_cast<uint8_t>((devMode() ? kStateDevMode : 0) | (wifi_connected_ ? kStateWifi : 0) |
                                     (live_active_ ? kStateLive : 0));
    msg.preset = preset_;
    msg.segment_count = scene_.segment_count;
    for (uint8_t i = 0; i < scene_.segment_count; ++i) msg.segments[i] = scene_.segments[i];
    return jacket::encodeState(msg, out, cap);
}

bool Engine::takeBootOtherSlot() {
    const bool asked = boot_other_slot_;
    boot_other_slot_ = false;
    return asked;
}

bool Engine::takeStateChanged() {
    const bool changed = state_changed_;
    state_changed_ = false;
    return changed;
}

// ---- Status, live input, timeouts ------------------------------------------------

void Engine::setWifiConnected(bool connected) {
    if (connected == wifi_connected_) return;
    wifi_connected_ = connected;
    stateChanged();
}

// ---- Who supplies the sound ---------------------------------------------------------

namespace {

// Signed: a stamp a moment after `now_ms` was sampled is not 49 days old.
bool within(uint32_t now_ms, uint32_t since_ms) { return static_cast<int32_t>(now_ms - since_ms) < static_cast<int32_t>(kLiveTimeoutMs); }

}  // namespace

bool Engine::phoneSoundAt(uint32_t now_ms) const { return phone_sound_ && within(now_ms, phone_sound_ms_); }

void Engine::soundFrom(SoundSource source) {
    if (fed_by_ == source) return;
    if (fed_by_ != SoundSource::None) signals_.soundSourceChanged();  // another clock behind t_ms
    fed_by_ = source;
}

Engine::SoundSource Engine::soundSource() const {
    if (phone_sound_) return SoundSource::Phone;
    if (local_active_) return SoundSource::Local;
    return SoundSource::None;
}

bool Engine::soundWanted() const {
    if (!power_) return false;
    for (uint8_t i = 0; i < scene_.segment_count; ++i) {
        const EffectInfo* effect = effectInfo(scene_.segments[i].config.effect);
        if (effect != nullptr && (effect->param_flags & kReactsSound) != 0) return true;
    }
    return false;
}

void Engine::handleLive(const uint8_t* frame, size_t len, uint32_t now_ms) {
    LiveInput in;
    if (decodeLive(frame, len, in) != ErrorCode::None) return;
    live_ = in;
    live_rx_ms_ = now_ms;

    if ((in.flags & kLiveAudioValid) != 0) {
        phone_sound_ = true;
        phone_sound_ms_ = now_ms;
    }
    // The phone always speaks for motion. It speaks for sound while it sends
    // sound, and when nobody else does: then a frame without the audio bit
    // says "no sound", as it always has.
    uint8_t groups = kLiveMotionValid;
    const bool local = local_active_ && within(now_ms, local_rx_ms_);
    if (phoneSoundAt(now_ms) || !local) {
        groups |= kLiveSoundGroups;
        soundFrom(SoundSource::Phone);
    }
    signals_.onFrame(in, now_ms, groups);
    if (!live_active_) {
        live_active_ = true;
        stateChanged();
    }
}

void Engine::handleLocalLive(const LiveInput& in, uint32_t now_ms) {
    local_active_ = true;
    local_rx_ms_ = now_ms;
    if (phoneSoundAt(now_ms)) return;  // the phone's sound wins
    soundFrom(SoundSource::Local);
    signals_.onFrame(in, now_ms, kLiveSoundGroups);
}

void Engine::tick(uint32_t now_ms) {
    if (phone_sound_ && !within(now_ms, phone_sound_ms_)) phone_sound_ = false;
    if (live_active_ && !within(now_ms, live_rx_ms_)) {
        live_active_ = false;
        // What the phone was supplying fades out, at the moment it ran out,
        // whenever we noticed. Sound the board supplies itself carries on.
        const uint8_t groups = fed_by_ == SoundSource::Local ? kLiveMotionValid : kLiveAllGroups;
        signals_.onTimeout(live_rx_ms_ + kLiveTimeoutMs, groups);
        render_dirty_ = true;
        stateChanged();
    }
    if (local_active_ && !within(now_ms, local_rx_ms_)) {
        local_active_ = false;
        if (fed_by_ == SoundSource::Local) {
            signals_.onTimeout(local_rx_ms_ + kLiveTimeoutMs, kLiveSoundGroups);
            render_dirty_ = true;
        }
    }
    if (identify_.active && static_cast<int32_t>(now_ms - identify_.start_ms) >= static_cast<int32_t>(identify_.duration_ms)) {
        identify_.active = false;
        render_dirty_ = true;
    }
}

// ---- Persistence ----------------------------------------------------------------

bool Engine::save() {
    bool ok = true;
    uint8_t buf[kMaxStorageBlob];
    if (settings_dirty_) {
        const size_t n = serializeSettings(settings_, buf, sizeof buf);
        if (n != 0 && storage_.write(kKeySettings, buf, n)) {
            settings_dirty_ = false;
        } else {
            ok = false;
        }
    }
    if (last_dirty_) {
        buf[0] = kLastRecordVersion;
        buf[1] = preset_;
        const size_t n = serializeScene(scene_, buf + 2, sizeof buf - 2);
        if (n != 0 && storage_.write(kKeyLast, buf, n + 2)) {
            last_dirty_ = false;
        } else {
            ok = false;
        }
    }
    return ok;
}

// ---- Rendering ------------------------------------------------------------------

bool Engine::render(uint32_t now_ms, Rgb* left, Rgb* right) {
    tick(now_ms);
    const Reactive& reactive = signals_.advance(now_ms);

    Rgb* const strips[kStripCount] = {left, right};
    for (Rgb* strip : strips) {
        for (uint16_t i = 0; i < kLedsPerStrip; ++i) strip[i] = kBlack;
    }

    bool animating = false;
    if (power_) {
        // Effect time only runs while the picture is being animated; after a
        // pause (power off, static scene) it resumes where it stopped.
        const uint32_t elapsed_ms = clock_running_ ? now_ms - last_render_ms_ : 0;
        for (uint8_t s = 0; s < scene_.segment_count; ++s) {
            const Segment& segment = scene_.segments[s];
            SegmentRuntime& runtime = runtime_[s];
            if (runtime.started) {
                runtime.time_q16 += static_cast<uint64_t>(elapsed_ms) * speedMultiplierQ16(segment.config.speed);
            } else {
                runtime.started = true;
            }

            const uint16_t length = segment.geometry.length;
            for (uint16_t i = 0; i < length; ++i) span_[i] = kBlack;

            const EffectInfo* effect = effectInfo(segment.config.effect);
            if (effect == nullptr) effect = effectInfo(0);
            const Palette palette(segment.config.palette, segment.config.color1, segment.config.color2);
            EffectContext ctx{span_,          length,       static_cast<uint32_t>(runtime.time_q16 >> 16),
                              segment.config, palette,      liveInput(),
                              runtime.state,  &reactive};
            if (effect->render(ctx)) animating = true;

            const bool reversed = segment.geometry.reversed();
            for (uint8_t strip = 0; strip < kStripCount; ++strip) {
                if ((segment.geometry.strip_mask & (1u << strip)) == 0) continue;
                Rgb* out = strips[strip] + segment.geometry.start;
                for (uint16_t i = 0; i < length; ++i) out[reversed ? length - 1 - i : i] = span_[i];
            }
        }
    }
    clock_running_ = power_ && animating;
    last_render_ms_ = now_ms;

    if (fading_) {
        if (!fade_started_) {  // the fade runs from the first frame that shows it
            fade_started_ = true;
            fade_start_ms_ = now_ms;
        }
        const uint32_t elapsed = now_ms - fade_start_ms_;
        if (elapsed >= config_.crossfade_ms) {
            fading_ = false;  // this frame is the new picture, exactly
        } else {
            const uint8_t amount = static_cast<uint8_t>(elapsed * 255u / config_.crossfade_ms);
            for (uint8_t strip = 0; strip < kStripCount; ++strip) {
                for (uint16_t i = 0; i < kLedsPerStrip; ++i) {
                    strips[strip][i] = blend(fade_from_[strip][i], strips[strip][i], amount);
                }
            }
            animating = true;  // keep rendering until the fade is over
        }
    }
    if (config_.crossfade_ms != 0) {
        memcpy(static_cast<void*>(shown_[0]), left, sizeof shown_[0]);
        memcpy(static_cast<void*>(shown_[1]), right, sizeof shown_[1]);
    }

    if (identify_.active) {
        // Dim white whatever the brightness setting: pre-compensate for the
        // scaling the output layer is about to apply.
        const uint8_t brightness = scene_.brightness;
        const uint8_t level = brightness > kIdentifyLevel
                                  ? static_cast<uint8_t>((kIdentifyLevel * 255u + brightness / 2u) / brightness)
                                  : 255;
        for (uint8_t strip = 0; strip < kStripCount; ++strip) {
            if ((identify_.strip_mask & (1u << strip)) == 0) continue;
            Rgb* out = strips[strip] + identify_.start;
            for (uint16_t i = 0; i < identify_.length; ++i) out[i] = Rgb(level, level, level);
        }
        animating = true;  // keep rendering so the overlay is removed on time
    }

    animating_ = animating;
    render_dirty_ = false;
    return animating;
}

// ---- Helpers --------------------------------------------------------------------

void Engine::resetSegment(uint8_t index) {
    runtime_[index] = SegmentRuntime();
    runtime_[index].state.seed = hash32(config_.seed + index + 1u);
}

// Stored scenes may name effects or palettes this firmware does not have.
void Engine::sanitize(Scene& scene) const {
    for (uint8_t i = 0; i < scene.segment_count; ++i) {
        EffectConfig& config = scene.segments[i].config;
        if (effectInfo(config.effect) == nullptr) config.effect = 0;
        if (config.palette >= paletteCount()) config.palette = 0;
    }
}

}  // namespace jacket
