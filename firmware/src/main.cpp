// Jacket firmware: wires the portable engine (lib/core) to the ESP32.
//
// Everything that touches the engine runs here, on the Arduino loop task:
// Bluetooth callbacks only queue frames and wake this task (see ble.h), and
// so does the microphone's task on a board that has one (see mic.h).
//
// loop() is the render loop. While something animates it sends 60 frames a
// second (an effect, a crossfade, or a reactive effect following the phone);
// when the picture is static or the jacket is off it sends the frame once,
// sleeps, and wakes at once when a command arrives. It never spins.

#include <Arduino.h>
#include <jacket.h>

#include "ble.h"
#include "boot_guard.h"
#include "config.h"
#include "dev.h"
#include "led_output.h"
#include "log.h"
#include "mic.h"
#include "storage.h"

namespace {

jacket::EngineConfig engineConfig() {
    jacket::EngineConfig config;
    config.fw_major = JACKET_FW_MAJOR;
    config.fw_minor = JACKET_FW_MINOR;
    config.fw_patch = JACKET_FW_PATCH;
    config.build = JACKET_BUILD " " __DATE__ " " __TIME__;
    config.default_brightness = cfg::kDefaultBrightness;
    config.crossfade_ms = cfg::kCrossfadeMs;
    return config;
}

NvsStorage storage;
jacket::Engine engine(storage, engineConfig());
Saver saver(engine);
BootGuard guard;
LedOutput leds;
Ble ble;
Dev dev;
Mic mic;
jacket::RecoveryHold recovery(cfg::kRecoveryWindowMs, cfg::kRecoveryHoldMs, cfg::kRecoveryReleaseMs);

jacket::Rgb left[jacket::kLedsPerStrip];
jacket::Rgb right[jacket::kLedsPerStrip];

// STATE as last handed to the Bluetooth side.
bool state_published = false;
uint16_t published_rev = 0;
bool state_notify_pending = false;
uint32_t last_state_notify_ms = 0;

// Frame pacing.
uint32_t next_frame_us = 0;
uint32_t last_show_ms = 0;
bool refresh_now = false;
bool ble_ok = false;

// The microphone, and the recovery button's answer on the strips.
bool mic_ok = false;
uint32_t mic_needed_ms = 0;
bool cue_showing = false;
uint32_t cue_until_ms = 0;  // 0: until the button says otherwise
jacket::Rgb cue_pixels[jacket::kLedsPerStrip];

// Since the last heartbeat.
uint32_t last_heartbeat_ms = 0;
uint32_t frames = 0;
uint32_t frame_us_sum = 0;
uint32_t frame_us_max = 0;
uint32_t commands = 0;
uint32_t live_frames = 0;

uint16_t systemMa() { return dev.active() ? cfg::kSystemMaWifi : cfg::kSystemMaBle; }

// Safe mode on a build with credentials: WiFi and the upload listener run
// whatever dev mode says, and never give up looking for the network. On a
// board without a serial port they are the only way to put it right.
bool rescueWifi() { return guard.safeMode() && cfg::hasCredentials(); }
bool wifiWanted() { return engine.devMode() || rescueWifi(); }

void publishState() {
    uint8_t frame[jacket::kMaxFrame];
    const size_t len = engine.encodeState(frame, sizeof frame);
    ble.setState(frame, len);
    published_rev = engine.rev();
    state_published = true;
}

// ---- Bluetooth -> engine -> Bluetooth ---------------------------------------------

// Writing flash stalls the LED signal for a moment (the RMT interrupt is held
// off while the flash is busy), so nothing is written while a frame may still
// be on its way out.
bool frameMayBeOnWire() { return micros() - leds.lastShowUs() <= cfg::kFrameOnWireUs; }

void waitForFrameToLeave() {
    const uint32_t age_us = micros() - leds.lastShowUs();
    if (age_us <= cfg::kFrameOnWireUs) delay((cfg::kFrameOnWireUs - age_us) / 1000u + 1u);
}

// These two write a preset to flash inside handleCommand().
bool writesFlash(uint8_t opcode) {
    return opcode == static_cast<uint8_t>(jacket::Opcode::PresetSave) || opcode == static_cast<uint8_t>(jacket::Opcode::PresetDelete);
}

// May move `now_ms` forward: a live frame can arrive after it was sampled.
void serviceBluetooth(uint32_t& now_ms) {
    ble.poll(now_ms);

    // Commands stay in their queue while the reply queue is full (after a
    // stall there can be more commands waiting than replies fit): flush()
    // makes room and the next pass carries on. No reply is dropped.
    uint8_t frame[jacket::kMaxFrame];
    size_t len = 0;
    while (ble.canQueueReply()) {
        uint8_t opcode = 0;
        if (ble.peekOpcode(opcode) && writesFlash(opcode)) waitForFrameToLeave();
        if (!ble.takeCommand(frame, len)) break;
        engine.handleCommand(frame, len, now_ms, ble);  // replies are queued in `ble`
        ++commands;
    }
    uint32_t arrived_ms = 0;
    if (ble.takeLive(frame, arrived_ms)) {
        // The beat clock gets the time the frame arrived, not the time this pass began.
        engine.handleLive(frame, jacket::kLiveFrameSize, arrived_ms);
        if (static_cast<int32_t>(arrived_ms - now_ms) > 0) now_ms = arrived_ms;
        ++live_frames;
    }
}

// The board's own microphone: runs only while an effect that reacts to sound
// is playing (and a little longer, so that flipping through effects does not
// restart it each time), never in safe mode, never during a firmware upload.
// Its frames reach the engine here, like the phone's above. May move `now_ms`
// forward for the same reason.
void serviceMic(uint32_t& now_ms) {
    if (!JACKET_HAS_MIC || !mic_ok) return;
    const bool allowed = !guard.safeMode() && !leds.suspended();
    if (allowed && engine.soundWanted()) {
        mic_needed_ms = now_ms;
        mic.setWanted(true);
    } else if (mic.wanted() && (!allowed || now_ms - mic_needed_ms >= cfg::kMicLingerMs)) {
        mic.setWanted(false);
    }
    jacket::LiveInput frame;
    uint32_t heard_ms = 0;
    if (mic.take(frame, heard_ms)) {
        engine.handleLocalLive(frame, heard_ms);
        if (static_cast<int32_t>(heard_ms - now_ms) > 0) now_ms = heard_ms;
    }
}

void serviceState(uint32_t now_ms) {
    // Reads of `state` see every change at once; notifications are coalesced.
    if (!state_published || engine.rev() != published_rev) publishState();
    if (engine.takeStateChanged()) state_notify_pending = true;
    if (state_notify_pending && now_ms - last_state_notify_ms >= cfg::kStateNotifyIntervalMs) {
        state_notify_pending = false;
        last_state_notify_ms = now_ms;
        ble.notifyState();
    }
    ble.flush(now_ms);
}

// ---- LEDs -------------------------------------------------------------------------

// Microseconds until the next animation frame may be sent; <= 0 when it is due.
// Half a millisecond early counts as due, so the loop never has to spin for
// less than one scheduler tick.
int32_t usUntilFrame() { return static_cast<int32_t>(next_frame_us - micros()) - 500; }

// Sends the pixel buffers and keeps the heartbeat's statistics. `start_us` is
// when work on this frame began.
void showFrame(uint32_t now_ms, uint32_t start_us) {
    leds.show(left, right, engine.brightness(), engine.budgetMa(), systemMa());
    last_show_ms = now_ms;
    const uint32_t took_us = micros() - start_us;
    ++frames;
    frame_us_sum += took_us;
    if (took_us > frame_us_max) frame_us_max = took_us;
}

void serviceLeds(uint32_t now_ms) {
    if (leds.suspended() || cue_showing) return;
    if (engine.needsRender()) {
        const int32_t early_us = usUntilFrame();
        if (early_us > 0) return;  // 60 frames per second at most
        const uint32_t start_us = micros();
        engine.render(now_ms, left, right);
        showFrame(now_ms, start_us);
        // Keep the rhythm if we are on time; start a new one after a pause.
        const bool on_time = early_us > -static_cast<int32_t>(cfg::kFrameIntervalUs);
        next_frame_us = (on_time ? next_frame_us : start_us) + cfg::kFrameIntervalUs;
        refresh_now = false;
    } else if (refresh_now || now_ms - last_show_ms >= cfg::kStaticRefreshMs) {
        showFrame(now_ms, micros());  // the same picture again
        refresh_now = false;
    }
}

// ---- Firmware upload hooks (dev.h) -------------------------------------------------

// The upload runs inside one loop pass; dev.cpp suspends the loop watchdog for it.
void beforeUpload() {
    mic.setWanted(false);
    mic.waitStopped(300);
    waitForFrameToLeave();
    saver.flush();
    leds.suspend();
}

void afterFailedUpload() {
    leds.resume();
    refresh_now = true;
}

// ---- Recovery button (jacket/boot_policy.h) ------------------------------------------

bool buttonPressed() {
    return cfg::kPinButton != cfg::kNoPin && (digitalRead(cfg::kPinButton) == LOW) == cfg::kButtonActiveLow;
}

// The jacket's answer where there is no serial log to read: the first LEDs of
// both strips, green for "let go and the other firmware starts", red for
// "there is none to go back to". Shown instead of the picture.
void showCue(bool ok, uint32_t until_ms) {
    for (jacket::Rgb& pixel : cue_pixels) pixel = jacket::kBlack;
    for (uint16_t i = 0; i < 12; ++i) cue_pixels[i] = ok ? jacket::Rgb(0, 255, 0) : jacket::Rgb(255, 0, 0);
    waitForFrameToLeave();
    leds.show(cue_pixels, cue_pixels, 48, engine.budgetMa(), systemMa());
    cue_showing = true;
    cue_until_ms = until_ms;
}

void endCue() {
    cue_showing = false;
    refresh_now = true;  // the picture again
}

// The other firmware is selected and the chip is about to be reset into it
// (boot_guard.h): green on the strips, a proper goodbye on the radio.
void leaveForOtherFirmware() {
    showCue(true, 0);
    mic.setWanted(false);
    mic.waitStopped(300);
    for (int i = 0; i < 40; ++i) {  // what is still queued for the phone goes out first
        ble.flush(millis());
        delay(10);
    }
    ble.stop();
    dev.shutdown();
    delay(100);  // the link is closed properly, and the green has been seen
    waitForFrameToLeave();
}

void serviceRecovery(uint32_t now_ms) {
    using State = jacket::RecoveryHold::State;
    if (cfg::kPinButton == cfg::kNoPin) return;
    if (cue_showing && cue_until_ms != 0 && static_cast<int32_t>(now_ms - cue_until_ms) >= 0) endCue();
    const State before = recovery.state();
    if (before == State::Done) return;
    const State state = recovery.sample(buttonPressed(), now_ms);  // millis() counts from the start
    if (state == before) return;

    switch (state) {
        case State::Holding:
            logLine("[boot] button down: hold it for %lus to go back to the firmware in the other slot",
                    static_cast<unsigned long>(cfg::kRecoveryHoldMs / 1000));
            break;
        case State::Watching:
            logLine("[boot] button released too early: nothing changed");
            break;
        case State::Armed:
            if (guard.otherSlotUsable()) {
                logLine("[boot] button held: let go now and the jacket restarts into the firmware in the other slot");
            } else {
                logLine("[boot] button held, but there is no firmware to go back to: nothing will change");
            }
            showCue(guard.otherSlotUsable(), 0);
            break;
        case State::Fire:
            if (guard.otherSlotUsable()) {
                waitForFrameToLeave();
                guard.switchToOtherSlot("recovery button", leaveForOtherFirmware);  // comes back only if it could not
                showCue(false, now_ms + 2000);                                     // it did not verify after all
            } else {
                logLine("[boot] button released: nothing changed");
                endCue();
            }
            break;
        case State::Done:
            if (before == State::Armed) {
                logLine("[boot] button never released: taken for a stuck pin, nothing changed");
                endCue();
            } else if (before == State::Holding) {
                logLine("[boot] button released too early: nothing changed");
            }
            break;
    }
}

// BOOT_OTHER_SLOT: the same way back, asked for over Bluetooth. The engine has
// already refused it if there was nothing to go back to.
void serviceBootOtherSlot() {
    engine.setOtherSlotUsable(guard.otherSlotUsable());
    if (!engine.takeBootOtherSlot()) return;
    logLine("[boot] BOOT_OTHER_SLOT received");
    waitForFrameToLeave();
    guard.switchToOtherSlot("Bluetooth command", leaveForOtherFirmware);  // comes back only if it could not
    // The image did not verify after all: the answer the engine would have given.
    engine.setOtherSlotUsable(false);
    uint8_t frame[3];
    const size_t len = jacket::encodeError(static_cast<uint8_t>(jacket::Opcode::BootOtherSlot), jacket::ErrorCode::NotFound, frame,
                                           sizeof frame);
    ble.send(frame, len);
}

bool recoveryWatching() {
    return cfg::kPinButton != cfg::kNoPin && (recovery.state() != jacket::RecoveryHold::State::Done || cue_showing);
}

// ---- Serial status ------------------------------------------------------------------

void heartbeat(uint32_t now_ms) {
    if (now_ms - last_heartbeat_ms < cfg::kHeartbeatMs) return;
    const uint32_t window_ms = now_ms - last_heartbeat_ms;
    last_heartbeat_ms = now_ms;

    char link[48];
    if (ble.connected()) {
        const unsigned interval = ble.connIntervalUnits() * 125u;  // in 0.01 ms
        snprintf(link, sizeof link, "connected mtu=%u itvl=%u.%02ums%s", static_cast<unsigned>(ble.mtu()), interval / 100u,
                 interval % 100u, ble.subscribed() ? "" : " unsubscribed");
    } else {
        snprintf(link, sizeof link, "advertising");
    }
    char wifi[40];
    dev.describe(wifi, sizeof wifi);

    // Live input: which groups the latest frame carried, how many frames this
    // window, the beat clock's tempo and state, and how late the last frame
    // was compared with the best of the last seconds.
    char live[64];
    const jacket::LiveInput* input = engine.liveInput();
    const jacket::BeatClock& beat = engine.beatClock();
    char groups[4] = "off";
    if (input != nullptr) {
        snprintf(groups, sizeof groups, "%s%s%s", input->flags & jacket::kLiveAudioValid ? "a" : "",
                 input->flags & jacket::kLiveMotionValid ? "m" : "", input->flags & jacket::kLiveBeatValid ? "b" : "");
        if (groups[0] == '\0') snprintf(groups, sizeof groups, "-");
    }
    snprintf(live, sizeof live, "live=%s rx=%lu beat=%u.%ubpm/%s late=%ums", groups, static_cast<unsigned long>(live_frames),
             static_cast<unsigned>(beat.bpmX10() / 10u), static_cast<unsigned>(beat.bpmX10() % 10u),
             beat.lockedAt(now_ms) ? "lock" : beat.activeAt(now_ms) ? "coast" : "none",
             static_cast<unsigned>(beat.lastExcessDelayMs()));

    // The board's own microphone, where there is one: what it costs (mic.h),
    // and who supplies the sound the effects follow right now.
    char heard[128] = "";
    if (JACKET_HAS_MIC) {
        char stats[104];
        mic.describe(stats, sizeof stats, window_ms);
        const jacket::Engine::SoundSource source = engine.soundSource();
        snprintf(heard, sizeof heard, " | %s snd=%s", stats,
                 source == jacket::Engine::SoundSource::Phone ? "phone" : source == jacket::Engine::SoundSource::Local ? "mic" : "none");
    }

    const unsigned fps_x10 = window_ms ? static_cast<unsigned>(frames * 10000u / window_ms) : 0;
    const unsigned avg_us = frames ? static_cast<unsigned>(frame_us_sum / frames) : 0;
    // dropped: commands the queue had no room for / replies given up on / frames too long for the link's MTU.
    logLine("[hb] up=%lus heap=%u min_heap=%u stack=%u | ble=%s cmds=%lu dropped=%lu/%lu/%lu | fps=%u.%u frame=%u/%uus | "
            "power=%s bri=%u out=%u%s budget=%umA est=%umA | wifi=%s | %s%s%s%s%s%s",
            static_cast<unsigned long>(now_ms / 1000), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMinFreeHeap()), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), link,
            static_cast<unsigned long>(commands), static_cast<unsigned long>(ble.droppedCommands()),
            static_cast<unsigned long>(ble.droppedReplies()), static_cast<unsigned long>(ble.oversizeDrops()), fps_x10 / 10u,
            fps_x10 % 10u, avg_us,
            static_cast<unsigned>(frame_us_max), engine.power() ? "on" : "off", static_cast<unsigned>(engine.brightness()),
            static_cast<unsigned>(leds.appliedScale()), leds.limited() ? "(limited)" : "",
            static_cast<unsigned>(engine.budgetMa()), static_cast<unsigned>(leds.estimatedMa()), wifi, live, heard,
            guard.safeMode() ? " SAFE-MODE" : "",
            guard.imagePending() ? " FIRMWARE-UNCONFIRMED" : !guard.imageConfirmed() ? " firmware-not-yet-confirmed" : "",
            leds.suspended() ? " leds-suspended" : "", cue_showing ? " recovery-cue" : "");
    // The same in short for the `status` characteristic (ble.h): what a board
    // without a serial port can still be asked. Only while dev mode is on.
    char status[240];
    if (engine.devMode()) {
        snprintf(status, sizeof status, "up=%lus reset=%s crashes=%u%s fps=%u.%u frame=%u/%uus heap=%u min_heap=%u%s",
                 static_cast<unsigned long>(now_ms / 1000), guard.resetReason(), static_cast<unsigned>(guard.crashCount()),
                 guard.safeMode() ? " SAFE-MODE" : "", fps_x10 / 10u, fps_x10 % 10u, avg_us, static_cast<unsigned>(frame_us_max),
                 static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()), heard);
    } else {
        snprintf(status, sizeof status, "dev mode is off");
    }
    ble.setStatus(status);

    frames = 0;
    frame_us_sum = 0;
    frame_us_max = 0;
    live_frames = 0;
}

// Sleeps until something is due, or until Bluetooth wakes the task.
void waitForWork(uint32_t now_ms) {
    uint32_t wait_ms = cfg::kIdleWaitMs;
    if (!leds.suspended() && engine.needsRender()) {
        const int32_t early_us = usUntilFrame();
        if (early_us <= 0) return;
        const uint32_t frame_ms = (static_cast<uint32_t>(early_us) + 999u) / 1000u;
        if (frame_ms < wait_ms) wait_ms = frame_ms;
    }
    if (state_notify_pending) {
        const uint32_t since_ms = now_ms - last_state_notify_ms;
        const uint32_t due_ms = since_ms < cfg::kStateNotifyIntervalMs ? cfg::kStateNotifyIntervalMs - since_ms : 1;
        if (due_ms < wait_ms) wait_ms = due_ms;
    }
    if (ble.busy() && wait_ms > 5) wait_ms = 5;                        // a reply the stack could not take yet
    if (ble.commandsWaiting() && wait_ms > 2) wait_ms = 2;             // commands held back until replies have gone
    if ((engine.liveInput() != nullptr || engine.localLiveActive()) && wait_ms > 20) wait_ms = 20;  // their 500 ms timeout
    if (recoveryWatching() && wait_ms > 50) wait_ms = 50;              // the button, shortly after the start
    if (dev.active() && wait_ms > 20) wait_ms = 20;                    // upload invitations, the log stream
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
}

}  // namespace

void setup() {
    Serial.setTxBufferSize(1024);  // a heartbeat line is queued, not waited for (it took 15 ms of the loop)
    Serial.begin(115200);
    // From here on: a start-up that hangs for 5 s resets the chip, which counts
    // as a crash, so a firmware that never gets as far as the loop is caught
    // by the boot guard like one that panics.
    enableLoopWDT();
    if (cfg::kPinStatusLed != cfg::kNoPin) pinMode(cfg::kPinStatusLed, OUTPUT);
    if (cfg::kPinButton != cfg::kNoPin) pinMode(cfg::kPinButton, cfg::kButtonActiveLow ? INPUT_PULLUP : INPUT_PULLDOWN);

    // Before anything else is started: this is where a firmware that keeps
    // crashing gives way to the one in the other slot (and does not return).
    const bool storage_ok = storage.begin();
    guard.begin(storage);
    feedLoopWDT();
    logLine(" ");
    logLine("[boot] jacket fw=%s build=\"%s\" slot=%s%s reset=%s crashes=%u safe_mode=%s", JACKET_FW_VERSION,
            JACKET_BUILD " " __DATE__ " " __TIME__, guard.slot(), guard.imagePending() ? " (unconfirmed)" : "",
            guard.resetReason(), static_cast<unsigned>(guard.crashCount()), guard.safeMode() ? "YES" : "no");
    logLine("[boot] board=%s core=%s idf=%s cpu=%uMHz storage=%s", cfg::kBoardName, ESP_ARDUINO_VERSION_STR, ESP.getSdkVersion(),
            static_cast<unsigned>(getCpuFrequencyMhz()), storage_ok ? "ok" : "FAILED");
    guard.logSlots();
#if JACKET_TEST_CRASH_AFTER_MS > 0
    logLine("[test] CRASH TEST BUILD: crashing on purpose in %d ms, at this and every boot", JACKET_TEST_CRASH_AFTER_MS);
    Serial.flush();
    delay(JACKET_TEST_CRASH_AFTER_MS);
#if JACKET_TEST_CRASH_BY_HANGING
    for (;;) {}  // the loop watchdog armed at the top of setup() must end this
#else
    abort();
#endif
#endif

    jacket::BootOptions options;
    options.restore_scene = !guard.safeMode();
    options.force_dev_mode = cfg::hasCredentials() && (JACKET_FORCE_DEV_MODE != 0 || guard.safeMode());
    engine.begin(options);
    engine.setOtherSlotUsable(guard.otherSlotUsable());
    if (guard.safeMode()) {
        logLine("[boot] SAFE MODE: the stored scene was not restored%s%s", JACKET_HAS_MIC ? ", the microphone stays off" : "",
                rescueWifi() ? ", WiFi and uploads stay on whatever dev mode says" : "");
    }

    leds.begin();
    feedLoopWDT();
    ble_ok = ble.begin(xTaskGetCurrentTaskHandle());
    feedLoopWDT();
    mic_ok = mic.begin(xTaskGetCurrentTaskHandle());
    publishState();

    Dev::Hooks hooks;
    hooks.before_upload = beforeUpload;
    hooks.after_failed_upload = afterFailedUpload;
    dev.begin(hooks);

    feedLoopWDT();  // from now on a loop pass that hangs for 5 s resets the chip
    next_frame_us = micros();
    last_heartbeat_ms = millis();
    logLine("[boot] ready: brightness=%u budget=%umA (standby about %umA) segments=%u preset=%d dev_mode=%s heap=%u",
            static_cast<unsigned>(engine.brightness()), static_cast<unsigned>(engine.budgetMa()),
            static_cast<unsigned>(LedOutput::standbyMa(systemMa())), static_cast<unsigned>(engine.scene().segment_count),
            engine.preset() == jacket::kNoPreset ? -1 : static_cast<int>(engine.preset()), engine.devMode() ? "on" : "off",
            static_cast<unsigned>(ESP.getFreeHeap()));
}

void loop() {
    uint32_t now_ms = millis();

    serviceBluetooth(now_ms);
    serviceMic(now_ms);
    engine.tick(now_ms);

    // Checking an upload's password, and the upload itself, happen in here and
    // can take seconds: look at the clock again afterwards. No second upload
    // while the bootloader still waits for the running firmware to be confirmed.
    dev.poll(now_ms, wifiWanted(), !guard.imagePending(), rescueWifi());
    now_ms = millis();
    engine.setWifiConnected(dev.wifiConnected());

    serviceState(now_ms);

    // Everything that writes flash by itself: only between frames.
    if (!frameMayBeOnWire()) {
        saver.poll(now_ms);
        // A build that takes uploads over WiFi confirms a new firmware only
        // once that path works again.
        const bool update_path_ok = !dev.available() || !wifiWanted() || dev.otaReady();
        const char* waiting_for = dev.gaveUp()            ? "WiFi, which found no network and is OFF (reboot within reach of it)"
                                  : !dev.wifiConnected() ? "WiFi to connect"
                                                         : "the upload listener to start";
        guard.poll(now_ms, ble_ok, update_path_ok, waiting_for);
    }

    serviceRecovery(now_ms);
    serviceBootOtherSlot();
    serviceLeds(now_ms);

    if (cfg::kPinStatusLed != cfg::kNoPin) {
        // On while a phone is connected; flickering while the recovery button is being held.
        const jacket::RecoveryHold::State hold = recovery.state();
        const bool lit = hold == jacket::RecoveryHold::State::Holding  ? (now_ms / 100u) % 2u == 0
                         : hold == jacket::RecoveryHold::State::Armed ? true
                                                                      : ble.connected();
        digitalWrite(cfg::kPinStatusLed, lit ? HIGH : LOW);
    }
    heartbeat(now_ms);

    waitForWork(now_ms);
}
