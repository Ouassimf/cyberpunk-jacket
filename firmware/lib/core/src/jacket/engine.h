// The engine: the one object device code talks to. It owns all state, speaks
// the BLE protocol at the byte level, persists through a Storage and renders
// the two strips. It knows nothing about BLE stacks, flash or LED drivers.
//
// Typical device code:
//
//     Engine engine(nvsStorage, config);
//     engine.begin();                                   // once, at boot (BootOptions for safe mode)
//
//     // command characteristic written:   engine.handleCommand(data, len, millis(), sink);
//     // live characteristic written:      engine.handleLive(data, len, millis());
//     // the board's own microphone:       engine.handleLocalLive(frame, sample time);
//     // state characteristic read:        engine.encodeState(buf, sizeof buf);
//
//     loop:
//         engine.tick(now);                             // timeouts; cheap, call every pass
//         // needsRender() is also true during a crossfade and while a reactive effect plays
//         if (engine.needsRender()) {
//             engine.render(now, left, right);
//             // show left/right scaled by engine.brightness(), limited to engine.budgetMa()
//         }
//         if (every 100 ms && engine.takeStateChanged())    notify(engine.encodeState(...));
//         if (engine.needsSave() && now - engine.lastChangeMs() > 2000)    engine.save();
//
// Not thread-safe: call everything from one task (queue frames received in
// BLE callbacks), or guard every call with one mutex.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "jacket/color.h"
#include "jacket/config.h"
#include "jacket/effect.h"
#include "jacket/live_signals.h"
#include "jacket/protocol.h"
#include "jacket/scene.h"
#include "jacket/storage.h"

namespace jacket {

struct EngineConfig {
    uint8_t fw_major = 2;
    uint8_t fw_minor = 0;
    uint8_t fw_patch = 0;
    const char* build = "dev";   // INFO.build; must outlive the engine, first 32 bytes are used
    uint32_t seed = 0x4A41434B;  // base of the effects' random sequences
    uint8_t default_brightness = 128;  // brightness of the scene used when storage holds none
    // Crossfade: when the picture would cut (power on or off, a preset loaded,
    // a segment given another effect, the layout changed) the last frame is
    // blended into the new picture over this long. 0 = cut. Brightness,
    // speed, intensity, colour and palette changes always apply at once.
    uint16_t crossfade_ms = 0;
};

// How begin() starts the jacket. The defaults are the normal boot.
struct BootOptions {
    // False: ignore the stored last scene and start from the default one
    // (safe mode after repeated crashes). The stored scene is left alone
    // until something changes.
    bool restore_scene = true;
    // True: dev mode is on after begin() whatever the stored setting says
    // (development builds, safe mode). Nothing is written: the stored
    // setting takes effect again at the next normal boot. SET_DEV_MODE still
    // works for the rest of the session.
    bool force_dev_mode = false;
};

// Where reply frames go. Device code forwards them as notifications on `state`.
class FrameSink {
public:
    virtual ~FrameSink() = default;
    virtual void send(const uint8_t* frame, size_t len) = 0;
};

class Engine {
public:
    explicit Engine(Storage& storage, const EngineConfig& config = EngineConfig());

    // Loads settings, the last scene and the preset names from storage.
    // Anything missing or unreadable falls back to defaults. Power is on
    // after begin(): it is not persisted. `rev` is 0 afterwards.
    void begin(const BootOptions& options = BootOptions());

    // ---- BLE ----------------------------------------------------------------

    // One frame written to `command`. GET_* commands send exactly one reply
    // frame to `reply`; a rejected command sends one ERROR frame and changes
    // nothing. Accepted commands that change something bump `rev` and are
    // announced through takeStateChanged(); they send nothing here, except
    // PRESET_SAVE and PRESET_DELETE, which also send the updated PRESETS list.
    // Returns the error code that was sent, or ErrorCode::None.
    ErrorCode handleCommand(const uint8_t* frame, size_t len, uint32_t now_ms, FrameSink& reply);

    // One frame written to `live`. Short frames are ignored. `now_ms` is when
    // the frame arrived: stamp it as close to the radio as possible, the beat
    // clock measures the link's jitter from it.
    void handleLive(const uint8_t* frame, size_t len, uint32_t now_ms);

    // ---- Sound heard by the jacket itself -------------------------------------

    // A live frame made on the board from its own microphone (MicLive), about
    // 30 times a second while soundWanted(). `now_ms` is the local time the
    // frame describes. Only its audio and beat groups are used, and only while
    // the phone is not supplying them:
    //
    //   - A phone frame with the audio bit makes the phone the source of sound
    //     for the next kLiveTimeoutMs: local frames are dropped meanwhile.
    //   - Otherwise local frames supply audio and beat. The phone's frames
    //     then speak for motion and tilt only, so "the phone sends motion, the
    //     jacket hears the music itself" works.
    //   - With no local frames (no microphone, or it is not running) a phone
    //     frame speaks for every group, exactly as without this call.
    //
    // Local frames are not live input in the protocol's sense: they do not
    // set the live-input flag of STATE, do not bump `rev` and do not show in
    // liveInput(). They go silent like a phone does: kLiveTimeoutMs without
    // one and their groups fade out.
    void handleLocalLive(const LiveInput& in, uint32_t now_ms);

    // True while some segment plays an effect whose catalogue flags say it
    // reacts to sound and the jacket is on: the only time a microphone needs
    // to run.
    bool soundWanted() const;

    // Who supplies audio and beat right now.
    enum class SoundSource : uint8_t { None, Phone, Local };
    SoundSource soundSource() const;
    bool localLiveActive() const { return local_active_; }

    // The current STATE frame (at most 123 bytes). Returns its length.
    size_t encodeState(uint8_t* out, size_t cap) const;

    // True if STATE changed since the last call; clears the mark. Poll it at
    // the notification rate (about 10 Hz) to coalesce bursts of changes.
    bool takeStateChanged();

    // ---- Status owned by device code ------------------------------------------

    void setWifiConnected(bool connected);

    // BOOT_OTHER_SLOT ("restart into the firmware in the other app slot") is
    // carried out by device code; the engine only answers for it. Device code
    // says whether there is a firmware to go to (false until told): without
    // one the command is refused with ERROR code 4. An accepted command sends
    // nothing and changes nothing in STATE; takeBootOtherSlot() then returns
    // true once, and device code verifies the image, switches and restarts
    // (or, if the image does not verify after all, sends that ERROR itself).
    void setOtherSlotUsable(bool usable) { other_slot_usable_ = usable; }
    bool takeBootOtherSlot();

    // ---- Housekeeping ---------------------------------------------------------

    // Expires live input (500 ms without a frame) and IDENTIFY. render()
    // calls it too, but render() may not run while the picture is static.
    void tick(uint32_t now_ms);

    // Debounced persistence: something that is persisted (scene, preset
    // marker, budget, dev mode) changed at lastChangeMs() and is not yet
    // stored. save() writes it; false means the storage refused and the data
    // is still pending. Presets themselves are written immediately by
    // PRESET_SAVE / PRESET_DELETE, not here.
    bool needsSave() const { return last_dirty_ || settings_dirty_; }
    uint32_t lastChangeMs() const { return last_change_ms_; }
    bool save();

    // ---- Rendering ------------------------------------------------------------

    // True when render() would produce something new: the picture is
    // animating, or something changed since the last render().
    bool needsRender() const { return animating_ || render_dirty_ || fading_; }

    // Fills both strips (kLedsPerStrip pixels each) for time `now_ms`.
    // Pixels are full range: the output layer must scale them by brightness()
    // and enforce budgetMa(). Returns true while the picture keeps changing,
    // false when it is static (including "power off": all black) and need not
    // be refreshed until needsRender() says so.
    bool render(uint32_t now_ms, Rgb* left, Rgb* right);

    // ---- State ----------------------------------------------------------------

    bool power() const { return power_; }
    uint8_t brightness() const { return scene_.brightness; }
    uint16_t budgetMa() const { return settings_.budget_ma; }
    bool devMode() const { return settings_.dev_mode || dev_forced_; }
    bool wifiConnected() const { return wifi_connected_; }
    bool identifyActive() const { return identify_.active; }
    uint16_t rev() const { return rev_; }
    uint8_t preset() const { return preset_; }  // kNoPreset or a slot
    const Scene& scene() const { return scene_; }
    // The most recent live frame, or nullptr while live input is not active.
    const LiveInput* liveInput() const { return live_active_ ? &live_ : nullptr; }
    // Live input as the reactive effects get it, as of the last render() or
    // handleLive(); and the beat clock behind it.
    const Reactive& reactive() const { return signals_.current(); }
    const BeatClock& beatClock() const { return signals_.beatClock(); }
    bool fading() const { return fading_; }  // a crossfade is in progress

    // The overlay IDENTIFY draws, after the output layer has applied brightness().
    static constexpr uint8_t kIdentifyLevel = 40;

private:
    struct SegmentRuntime {
        uint64_t time_q16 = 0;  // effect time in ms, 48.16 fixed point
        bool started = false;   // false until first rendered: time starts at 0 then
        EffectState state;
    };
    struct Identify {
        bool active = false;
        uint8_t strip_mask = 0;
        uint8_t start = 0;
        uint8_t length = 0;
        uint32_t start_ms = 0;
        uint32_t duration_ms = 0;
    };

    ErrorCode apply(const Command& cmd, uint32_t now_ms, FrameSink& reply);
    ErrorCode setEffect(uint8_t segment, const EffectConfig& config);
    ErrorCode setParam(const Command& cmd);
    void setSegments(const Command& cmd);
    ErrorCode presetSave(uint8_t slot, const Name& name);
    ErrorCode presetLoad(uint8_t slot);
    ErrorCode presetDelete(uint8_t slot);
    void sendPresets(FrameSink& reply) const;
    void sendInfo(FrameSink& reply) const;
    void sendCatalog(uint8_t kind, uint8_t start, FrameSink& reply) const;

    void committed(const Scene& before, uint32_t now_ms);
    void stateChanged();
    void startFade();
    void resetSegment(uint8_t index);
    void sanitize(Scene& scene) const;

    Storage& storage_;
    EngineConfig config_;

    Scene scene_;
    Settings settings_;
    bool power_ = true;
    bool wifi_connected_ = false;
    bool dev_forced_ = false;  // BootOptions::force_dev_mode, until SET_DEV_MODE says otherwise
    bool other_slot_usable_ = false;
    bool boot_other_slot_ = false;  // BOOT_OTHER_SLOT accepted, not yet taken by device code
    uint8_t preset_ = kNoPreset;
    uint8_t pending_preset_ = kNoPreset;  // set by PRESET_SAVE / PRESET_LOAD, applied in committed()
    uint16_t rev_ = 0;

    bool preset_used_[kPresetSlots] = {};
    Name preset_names_[kPresetSlots];

    LiveInput live_;
    bool live_active_ = false;
    uint32_t live_rx_ms_ = 0;
    LiveSignals signals_;

    // Who supplies the sound groups (see handleLocalLive).
    bool phoneSoundAt(uint32_t now_ms) const;
    void soundFrom(SoundSource source);
    bool phone_sound_ = false;    // a phone frame with the audio bit arrived at phone_sound_ms_ ...
    uint32_t phone_sound_ms_ = 0;
    bool local_active_ = false;   // ... a local frame at local_rx_ms_; each lapses after kLiveTimeoutMs
    uint32_t local_rx_ms_ = 0;
    SoundSource fed_by_ = SoundSource::None;  // whose frames LiveSignals got last for the sound groups

    Identify identify_;

    bool state_changed_ = false;
    bool last_dirty_ = false;
    bool settings_dirty_ = false;
    uint32_t last_change_ms_ = 0;

    bool render_dirty_ = true;
    bool animating_ = false;
    bool clock_running_ = false;
    uint32_t last_render_ms_ = 0;
    SegmentRuntime runtime_[kMaxSegments];
    Rgb span_[kLedsPerStrip];  // the virtual pixels of the segment being rendered

    // Crossfade: the frame last produced (without the IDENTIFY overlay), and
    // a copy of it taken when a fade begins.
    Rgb shown_[kStripCount][kLedsPerStrip];
    Rgb fade_from_[kStripCount][kLedsPerStrip];
    bool fading_ = false;
    bool fade_started_ = false;  // false until the first frame of the fade is rendered
    uint32_t fade_start_ms_ = 0;
};

}  // namespace jacket
