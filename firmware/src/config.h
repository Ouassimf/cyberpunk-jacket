// Board wiring, timing and build-time options of the jacket firmware.
#pragma once

#include <stdint.h>

// ---- Version -------------------------------------------------------------------
// The one place the firmware version is written. INFO reports the numbers and
// "<build> <compile date>" as its build string.
#define JACKET_FW_MAJOR 2
#define JACKET_FW_MINOR 0
#define JACKET_FW_PATCH 0

#define JACKET_STR2(x) #x
#define JACKET_STR(x) JACKET_STR2(x)
#define JACKET_FW_VERSION JACKET_STR(JACKET_FW_MAJOR) "." JACKET_STR(JACKET_FW_MINOR) "." JACKET_STR(JACKET_FW_PATCH)

// ---- Set per environment in platformio.ini ---------------------------------------
#ifndef JACKET_BUILD
#define JACKET_BUILD "dev"  // which environment built this image: usb, ota, bench, ci
#endif
// WiFi and OTA exist only in builds that carry all three; empty keeps them off.
#ifndef JACKET_WIFI_SSID
#define JACKET_WIFI_SSID ""
#endif
#ifndef JACKET_WIFI_PASSWORD
#define JACKET_WIFI_PASSWORD ""
#endif
#ifndef JACKET_OTA_PASSWORD
#define JACKET_OTA_PASSWORD ""
#endif
// 1: dev mode is on at every boot whatever the stored setting says
// (development builds). The app can still switch it off until the next boot.
#ifndef JACKET_FORCE_DEV_MODE
#define JACKET_FORCE_DEV_MODE 0
#endif
// TEST ONLY, never in a build that is worn: N > 0 makes the firmware crash on
// purpose N milliseconds after the boot guard has run, at every boot, to
// prove the way back to the previous firmware (environment gledopto_crashtest).
#ifndef JACKET_TEST_CRASH_AFTER_MS
#define JACKET_TEST_CRASH_AFTER_MS 0
#endif
// With it, 1: hang instead of crashing, to prove that a start-up that never
// finishes is ended by the watchdog and counted like a crash.
#ifndef JACKET_TEST_CRASH_BY_HANGING
#define JACKET_TEST_CRASH_BY_HANGING 0
#endif

// TEST ONLY: 1 replaces what the microphone delivers by a drum machine (a
// beat at 120 BPM over a little noise), so that the analysis has real work
// to do on a bench where the microphone's pins are not connected to anything.
#ifndef JACKET_TEST_MIC_SYNTH
#define JACKET_TEST_MIC_SYNTH 0
#endif

// ---- Board ---------------------------------------------------------------------
// One of these, set per environment in platformio.ini; the devkit if none is.
//   JACKET_BOARD_DEVKIT     ESP32-WROOM-32 devkit, wired by hand
//   JACKET_BOARD_GLEDOPTO   Gledopto GL-C-010WL, "ESP32 WLED Digital LED Controller with Mic"
#if defined(JACKET_BOARD_DEVKIT) && defined(JACKET_BOARD_GLEDOPTO)
#error "Two boards selected: define JACKET_BOARD_DEVKIT or JACKET_BOARD_GLEDOPTO, not both"
#endif
#if !defined(JACKET_BOARD_DEVKIT) && !defined(JACKET_BOARD_GLEDOPTO)
#define JACKET_BOARD_DEVKIT
#endif

namespace cfg {

constexpr int8_t kNoPin = -1;  // "this board does not have it"

// ---- Wiring ----------------------------------------------------------------------
// Every pin of a board is one constant here and is used nowhere else as a
// number. The microphone's data pin is a macro because its absence removes
// code at compile time (JACKET_HAS_MIC): -1 there means no microphone.
#if defined(JACKET_BOARD_DEVKIT)

constexpr char kBoardName[] = "devkit";
constexpr int8_t kPinLeft = 14;          // WS2812B data, left strip
constexpr int8_t kPinRight = 12;         // WS2812B data, right strip (a boot strapping pin: must not be pulled high at reset)
constexpr int8_t kPinStatusLed = 2;      // devkit LED: on while a phone is connected
constexpr int8_t kPinButton = 0;         // the BOOT button
constexpr bool kButtonActiveLow = true;
// No microphone on the devkit. JACKET_DEVKIT_MIC (environment devkit_mic)
// runs the microphone code on three free pins anyway, to measure what it
// costs: with nothing wired the data pin floats and the "sound" is noise.
#ifdef JACKET_DEVKIT_MIC
#define JACKET_PIN_MIC_SD 26
#else
#define JACKET_PIN_MIC_SD -1
#endif
constexpr int8_t kPinMicWs = 5;
constexpr int8_t kPinMicSck = 21;
constexpr bool kMicRightSlot = false;
constexpr uint16_t kSystemMaBle = 80;    // the board at 80 MHz with Bluetooth, mA at the 5 V rail (datasheet estimate, not measured)
constexpr uint16_t kSystemMaWifi = 180;  // the same with WiFi up (dev mode)

#elif defined(JACKET_BOARD_GLEDOPTO)

// As read from this unit's own WLED configuration (cfg.json) before the first
// flash: two LED outputs, one button, an I2S microphone without MCLK, and no
// relay or LED power switch (WLED had none configured and the strips lit).
// The board has no status LED and no USB or serial port.
constexpr char kBoardName[] = "gledopto";
constexpr int8_t kPinLeft = 16;          // WS2812B data, first output ("GPIO16" on the case)
constexpr int8_t kPinRight = 2;          // WS2812B data, second output ("GPIO2" on the case)
constexpr int8_t kPinStatusLed = kNoPin; // GPIO 2 is a data output here
constexpr int8_t kPinButton = 0;         // the button marked OPT. GPIO0: see RecoveryHold in jacket/boot_policy.h
constexpr bool kButtonActiveLow = true;
// The I2S microphone is on GPIO 26 (data), 5 (word select) and 21 (bit
// clock). Until its code has run on this board it is built in only on request
// (JACKET_GLEDOPTO_MIC): the first firmware installed must be the plainest.
#ifdef JACKET_GLEDOPTO_MIC
#define JACKET_PIN_MIC_SD 26
#else
#define JACKET_PIN_MIC_SD -1
#endif
constexpr int8_t kPinMicWs = 5;
constexpr int8_t kPinMicSck = 21;
// Which half of the I2S frame the microphone answers in depends on how its
// L/R pin is strapped, which nothing reports. This is where the task starts;
// if it is wrong the heartbeat shows it (mic ... pk=0/NNNN: silence in the
// slot read, signal in the other) and the task changes over by itself (mic.h).
constexpr bool kMicRightSlot = false;
constexpr uint16_t kSystemMaBle = 80;    // assumed the same as the devkit: not measured on this board
constexpr uint16_t kSystemMaWifi = 180;

#endif

constexpr int8_t kPinMicSd = JACKET_PIN_MIC_SD;
#define JACKET_HAS_MIC (JACKET_PIN_MIC_SD >= 0)

// ---- Microphone (mic.h) -------------------------------------------------------------
// An I2S MEMS microphone delivers 24 bits; the analyser wants 16. Dropping
// the low 8 keeps the microphone's whole range and clips only where it does
// (120 dB SPL for the usual parts), but leaves music at ordinary loudness some
// 8 dB quieter than a phone's 16-bit samples, which the analyser's absolute
// floor was tuned to. One bit less is a gain of 2: about a phone's level (a
// 90 dB SPL tone gives an RMS near 2000), clipping from 114 dB SPL.
constexpr uint8_t kMicDropBits = 7;
constexpr uint32_t kMicLingerMs = 5000;  // keep listening this long after the last effect that needs it went

// ---- Rendering -------------------------------------------------------------------
constexpr uint32_t kFrameIntervalUs = 16667;      // 60 frames per second while something animates
constexpr uint32_t kStaticRefreshMs = 2000;       // a static picture is sent again this often, so a data glitch heals
constexpr uint32_t kFrameOnWireUs = 8000;         // a frame takes 4.4 ms to leave; flash is not written before this has passed
constexpr uint32_t kIdleWaitMs = 100;             // longest sleep of the loop when nothing is due
constexpr uint8_t kDefaultBrightness = 64;        // fresh board, no stored scene
constexpr uint16_t kCrossfadeMs = 350;            // power, preset, effect and layout changes blend over this long

// ---- Power model (see led_output.h) -----------------------------------------------
// What the jacket draws besides the light itself, in mA at the 5 V rail.
// Estimates from datasheets, not yet measured on this jacket.
// kSystemMaBle and kSystemMaWifi, what the board itself draws, are with its wiring above.
constexpr uint16_t kLedIdleMaX10 = 10;            // per LED with all channels off, in 0.1 mA (WS2812B: about 1 mA)

// ---- Bluetooth -------------------------------------------------------------------
constexpr uint32_t kStateNotifyIntervalMs = 100;  // STATE notifications are coalesced to 10 per second
constexpr uint32_t kAdvFastMs = 30000;            // after boot or a disconnect: advertise fast for this long ...
constexpr uint16_t kAdvFastInterval = 244;        // ... every 152.5 ms (unit 0.625 ms)
constexpr uint16_t kAdvSlowInterval = 874;        // then every 546.25 ms: found by a scanning phone within a second or two
constexpr int8_t kTxPowerDbm = 9;                 // worn on the body with the phone in a pocket: use the full power

// ---- WiFi (dev mode) ---------------------------------------------------------------
constexpr uint32_t kWifiFirstRetryMs = 10000;     // pause before joining is tried again; doubles each time ...
constexpr uint32_t kWifiMaxRetryMs = 60000;       // ... up to this
constexpr uint32_t kWifiGiveUpMs = 180000;        // never got an address in this long: WiFi off until the next boot

// ---- Persistence -----------------------------------------------------------------
constexpr uint32_t kSaveDelayMs = 5000;           // write to flash this long after the last change
constexpr uint32_t kSaveRetryMs = 10000;          // after a failed write

// ---- Health ----------------------------------------------------------------------
constexpr uint32_t kHealthyAfterMs = 20000;       // running this long counts as a good boot
constexpr uint8_t kSafeModeCrashes = 3;           // abnormal resets in a row before safe mode
constexpr uint32_t kHeartbeatMs = 5000;           // serial status line

// ---- Recovery button (jacket/boot_policy.h) ---------------------------------------
constexpr uint32_t kRecoveryWindowMs = 10000;     // a press must begin this soon after the start ...
constexpr uint32_t kRecoveryHoldMs = 3000;        // ... and last this long ...
constexpr uint32_t kRecoveryReleaseMs = 10000;    // ... and end within this, or it was not a finger

inline bool hasCredentials() { return JACKET_WIFI_SSID[0] != '\0' && JACKET_OTA_PASSWORD[0] != '\0'; }

}  // namespace cfg
