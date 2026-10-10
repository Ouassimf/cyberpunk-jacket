// Development services: WiFi (station), ArduinoOTA firmware upload and a
// read-only log stream on TCP port 23.
//
// They run only when both hold:
//   - the build carries WiFi credentials and an OTA password (cfg::hasCredentials);
//     without an OTA password nothing is ever started, so uploads are never
//     open to the network unauthenticated;
//   - dev mode is on (SET_DEV_MODE, the stored setting, or forced at boot).
// Otherwise the WiFi driver is not even initialised.
//
// A jacket worn away from its network must not search for it all evening:
// joining is retried with a growing pause, and if no address was ever obtained
// within cfg::kWifiGiveUpMs the radio is switched off until the next boot, or
// until dev mode is switched off and on again.
#pragma once

#include <stdint.h>

class Dev {
public:
    struct Hooks {
        void (*before_upload)() = nullptr;        // save state, stop the LEDs
        void (*after_failed_upload)() = nullptr;  // carry on as before
    };

    void begin(const Hooks& hooks);

    // Every loop pass, from the loop task. `enabled`: the engine's dev mode,
    // or safe mode on a build with credentials. `accept_uploads` is false
    // while the running firmware is still unconfirmed: a second upload then
    // would overwrite the firmware the bootloader falls back to.
    // `never_give_up` (safe mode): keep trying to join for as long as it
    // takes, with the same growing pauses; a jacket that needs a new firmware
    // must be reachable when it comes home, without anyone knowing to reboot it.
    //
    // This call can take seconds: checking the password of an upload request
    // is slow, and a firmware upload runs inside it (and, when it succeeds,
    // reboots). The loop watchdog is suspended for that part. Sample the
    // clock again afterwards.
    void poll(uint32_t now_ms, bool enabled, bool accept_uploads, bool never_give_up = false);

    // WiFi off now, if it is on: the jacket is about to restart into another firmware.
    void shutdown();

    bool available() const;                         // the build has credentials
    bool active() const { return active_; }         // the WiFi radio is on
    bool wifiConnected() const;                     // and has an address
    bool otaReady() const { return ota_started_; }  // and the upload listener is bound to its port
    bool gaveUp() const { return gave_up_; }        // dev mode is on, but WiFi was switched off: no network
    void describe(char* out, unsigned cap) const;   // for the heartbeat: "off", "connecting", "gave-up", "192.168.1.50 -61dBm"

private:
    void start(uint32_t now_ms);
    void stop();
    void startOta(uint32_t now_ms);
    void serviceLogStream();

    bool wanted_ = false;       // dev mode as last seen
    bool active_ = false;
    bool gave_up_ = false;
    bool ever_had_ip_ = false;  // since the radio was last switched on
    bool ota_started_ = false;
    bool ota_failure_logged_ = false;
    bool events_registered_ = false;
    uint32_t started_ms_ = 0;
    uint32_t next_attempt_ms_ = 0;
    uint32_t retry_ms_ = 0;
    uint32_t next_ota_try_ms_ = 0;
    uint32_t last_ota_poll_ms_ = 0;
};
