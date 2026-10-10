#include "dev.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <errno.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <lwip/udp.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "log.h"

namespace {

constexpr char kHostname[] = "jacket";
constexpr uint16_t kOtaPort = 3232;
constexpr uint16_t kLogPort = 23;
constexpr uint32_t kOtaRetryMs = 5000;  // after the upload listener failed to start
constexpr uint32_t kOtaPollMs = 100;    // how often upload requests are looked for

volatile bool g_has_ip = false;
volatile bool g_uploading = false;
Dev::Hooks g_hooks;

// ---- Log stream --------------------------------------------------------------------
// logLine() may run on any task, so it only copies into this ring; the loop
// task sends it. The ring keeps the newest text: a reader that connects gets
// the last couple of kilobytes first, a reader that is too slow loses the oldest.
WiFiServer g_log_server(kLogPort, 1);
WiFiClient g_log_client;
portMUX_TYPE g_log_mux = portMUX_INITIALIZER_UNLOCKED;
char g_log_ring[2048];
size_t g_log_head = 0;  // next byte to send
size_t g_log_used = 0;

void logSink(const char* line, size_t len) {
    if (len > sizeof g_log_ring) return;
    portENTER_CRITICAL(&g_log_mux);
    for (size_t i = 0; i < len; ++i) {
        if (g_log_used == sizeof g_log_ring) {  // full: overwrite the oldest byte
            g_log_head = (g_log_head + 1) % sizeof g_log_ring;
            --g_log_used;
        }
        g_log_ring[(g_log_head + g_log_used) % sizeof g_log_ring] = line[i];
        ++g_log_used;
    }
    portEXIT_CRITICAL(&g_log_mux);
}

size_t takeLog(char* out, size_t cap) {
    portENTER_CRITICAL(&g_log_mux);
    size_t n = g_log_used < cap ? g_log_used : cap;
    for (size_t i = 0; i < n; ++i) out[i] = g_log_ring[(g_log_head + i) % sizeof g_log_ring];
    g_log_head = (g_log_head + n) % sizeof g_log_ring;
    g_log_used -= n;
    portEXIT_CRITICAL(&g_log_mux);
    return n;
}

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        g_has_ip = true;
        logLine("[wifi] connected, ip %s", WiFi.localIP().toString().c_str());
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED || event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
        if (g_has_ip) {
            logLine("[wifi] lost (reason %u)", event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED
                                                   ? static_cast<unsigned>(info.wifi_sta_disconnected.reason)
                                                   : 0u);
        }
        g_has_ip = false;
    }
}

// Whether a UDP socket is bound to `port`. ArduinoOTA.begin() returns nothing,
// and gives up without a word when it cannot bind its port; this asks the
// network stack itself.
bool udpPortBound(uint16_t port) {
    bool bound = false;
    LOCK_TCPIP_CORE();
    for (const struct udp_pcb* pcb = udp_pcbs; pcb != nullptr; pcb = pcb->next) {
        if (pcb->local_port == port) {
            bound = true;
            break;
        }
    }
    UNLOCK_TCPIP_CORE();
    return bound;
}

}  // namespace

bool Dev::available() const { return cfg::hasCredentials(); }

bool Dev::wifiConnected() const { return active_ && g_has_ip; }

void Dev::begin(const Hooks& hooks) {
    g_hooks = hooks;
    if (!available()) logLine("[dev] no WiFi credentials in this build: WiFi, OTA and the log stream stay off");
}

void Dev::poll(uint32_t now_ms, bool enabled, bool accept_uploads, bool never_give_up) {
    if (!available()) return;
    if (enabled != wanted_) {  // dev mode switched: also the way back from having given up
        wanted_ = enabled;
        gave_up_ = false;
        if (enabled) {
            start(now_ms);
        } else if (active_) {
            stop();
        }
    }
    if (!active_) return;

    if (!g_has_ip) {
        if (!never_give_up && !ever_had_ip_ && now_ms - started_ms_ >= cfg::kWifiGiveUpMs) {
            logLine("[wifi] no network after %lus: WiFi OFF until the next boot, or until dev mode is switched off and on",
                    static_cast<unsigned long>(cfg::kWifiGiveUpMs / 1000));
            stop();
            gave_up_ = true;
            return;
        }
        // The core is told not to reconnect by itself (it would scan without a
        // pause for as long as the network is away). Try again here, less and
        // less often.
        if (static_cast<int32_t>(now_ms - next_attempt_ms_) >= 0) {
            WiFi.reconnect();
            next_attempt_ms_ = now_ms + retry_ms_;
            retry_ms_ = retry_ms_ * 2 > cfg::kWifiMaxRetryMs ? cfg::kWifiMaxRetryMs : retry_ms_ * 2;
        }
        return;
    }
    ever_had_ip_ = true;
    retry_ms_ = cfg::kWifiFirstRetryMs;  // if the network goes away, start again with short pauses
    next_attempt_ms_ = now_ms;

    if (!ota_started_) {
        if (static_cast<int32_t>(now_ms - next_ota_try_ms_) >= 0) startOta(now_ms);
        return;  // the first look for an upload request is in a later pass
    }

    if (accept_uploads && now_ms - last_ota_poll_ms_ >= kOtaPollMs) {
        last_ota_poll_ms_ = now_ms;
        // handle() can keep this task for seconds: an upload request is
        // authenticated with PBKDF2 (10000 rounds, in software: 3.6 s at
        // 80 MHz, measured) before onStart is called, and the upload itself
        // runs in here. The loop watchdog must not take that for a hang.
        disableLoopWDT();
        ArduinoOTA.handle();
        enableLoopWDT();
        if (g_uploading) {  // handle() came back: the upload failed
            g_uploading = false;
            if (g_hooks.after_failed_upload != nullptr) g_hooks.after_failed_upload();
        }
    }
    serviceLogStream();
}

void Dev::shutdown() {
    if (available() && active_) stop();
}

void Dev::start(uint32_t now_ms) {
    if (!events_registered_) {
        WiFi.onEvent(onWifiEvent);
        setLogSink(logSink);
        events_registered_ = true;
    }
    logLine("[wifi] dev mode on: joining the network");
    WiFi.persistent(false);       // the credentials stay in this image: the driver must not copy them to flash
    WiFi.setHostname(kHostname);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);  // see poll()
    WiFi.begin(JACKET_WIFI_SSID, JACKET_WIFI_PASSWORD);
    started_ms_ = now_ms;
    retry_ms_ = cfg::kWifiFirstRetryMs;
    next_attempt_ms_ = now_ms + retry_ms_;
    ever_had_ip_ = false;
    active_ = true;
}

void Dev::stop() {
    logLine("[wifi] WiFi off");
    if (ota_started_) {
        ArduinoOTA.end();
        g_log_client.stop();
        g_log_server.end();
        ota_started_ = false;
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    g_has_ip = false;
    active_ = false;
}

void Dev::startOta(uint32_t now_ms) {
    ArduinoOTA.setPort(kOtaPort);
    ArduinoOTA.setHostname(kHostname);
    ArduinoOTA.setPassword(JACKET_OTA_PASSWORD);
    ArduinoOTA.onStart([] {
        g_uploading = true;
        logLine("[ota] upload starting");
        if (g_hooks.before_upload != nullptr) g_hooks.before_upload();
    });
    ArduinoOTA.onEnd([] { logLine("[ota] upload complete, rebooting"); });
    ArduinoOTA.onError([](ota_error_t error) { logLine("[ota] upload FAILED (error %u)", static_cast<unsigned>(error)); });
    ArduinoOTA.begin();
    if (!udpPortBound(kOtaPort)) {
        // Not ready: a new firmware must not be confirmed on the strength of this.
        if (!ota_failure_logged_) logLine("[ota] could NOT open port %u: uploads unavailable, retrying", static_cast<unsigned>(kOtaPort));
        ota_failure_logged_ = true;
        next_ota_try_ms_ = now_ms + kOtaRetryMs;
        return;
    }
    g_log_server.begin();
    g_log_server.setNoDelay(true);
    ota_started_ = true;
    ota_failure_logged_ = false;
    logLine("[ota] ready on port %u, log stream on port %u", static_cast<unsigned>(kOtaPort), static_cast<unsigned>(kLogPort));
}

void Dev::serviceLogStream() {
    if (g_log_server.hasClient()) {  // the newest reader replaces the previous one
        g_log_client.stop();
        g_log_client = g_log_server.accept();
        g_log_client.setNoDelay(true);
        logLine("[dev] log reader connected: jacket fw %s (%s)", JACKET_FW_VERSION, JACKET_BUILD);
    }
    if (!g_log_client.connected()) return;
    char chunk[256];
    const size_t n = takeLog(chunk, sizeof chunk);
    if (n == 0) return;
    // Never wait for a slow reader: the loop also drives the LEDs.
    const int sent = ::send(g_log_client.fd(), chunk, n, MSG_DONTWAIT);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) g_log_client.stop();
}

void Dev::describe(char* out, unsigned cap) const {
    if (!available()) {
        snprintf(out, cap, "none");
    } else if (gave_up_) {
        snprintf(out, cap, "gave-up");
    } else if (!active_) {
        snprintf(out, cap, "off");
    } else if (!g_has_ip) {
        snprintf(out, cap, "connecting");
    } else {
        snprintf(out, cap, "%s %ddBm", WiFi.localIP().toString().c_str(), static_cast<int>(WiFi.RSSI()));
    }
}
