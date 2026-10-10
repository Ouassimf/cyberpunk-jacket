#include "ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/queue.h>
#include <string.h>

#include "config.h"
#include "log.h"

namespace {

constexpr char kDeviceName[] = "Jacket";
constexpr char kServiceUuid[] = "89960001-beff-438f-8710-5e7d3dff8891";
constexpr char kCommandUuid[] = "89960002-beff-438f-8710-5e7d3dff8891";
constexpr char kStateUuid[] = "89960003-beff-438f-8710-5e7d3dff8891";
constexpr char kLiveUuid[] = "89960004-beff-438f-8710-5e7d3dff8891";
constexpr char kDebugServiceUuid[] = "8996d001-beff-438f-8710-5e7d3dff8891";
constexpr char kStatusUuid[] = "8996d002-beff-438f-8710-5e7d3dff8891";
constexpr size_t kMaxStatus = 240;  // fits one read at the MTU the jacket asks for

constexpr uint16_t kPreferredMtu = 247;
constexpr UBaseType_t kCommandQueueLength = 32;
constexpr uint32_t kReplyGiveUpMs = 1000;
constexpr uint32_t kAdvCheckMs = 1000;

// Asked of the central after it connects: 15-30 ms interval so a dragged
// slider feels direct, no latency, link loss noticed after 4 s.
constexpr uint16_t kConnIntervalMin = 12;  // x 1.25 ms
constexpr uint16_t kConnIntervalMax = 24;
constexpr uint16_t kConnTimeout = 400;     // x 10 ms

struct CommandItem {
    uint8_t len;
    uint8_t data[jacket::kMaxFrame];
};
static_assert(jacket::kMaxFrame <= 255, "frame lengths are kept in a byte");

Ble* g_ble = nullptr;
TaskHandle_t g_wake_task = nullptr;
QueueHandle_t g_commands = nullptr;
NimBLECharacteristic* g_state_char = nullptr;
NimBLECharacteristic* g_status_char = nullptr;

// The newest live frame, written by the NimBLE task and taken by the loop.
portMUX_TYPE g_live_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t g_live[jacket::kLiveFrameSize];
uint32_t g_live_arrived_ms = 0;
bool g_live_fresh = false;

// The STATE frame reads are answered with, written by the loop.
portMUX_TYPE g_state_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t g_state[jacket::kMaxFrame];
size_t g_state_len = 0;

// The `status` text, written by the loop. Shares the mutex above.
char g_status[kMaxStatus];
size_t g_status_len = 0;

void wakeLoop() {
    if (g_wake_task != nullptr) xTaskNotifyGive(g_wake_task);
}

}  // namespace

// The NimBLE task's side. Nothing here touches the engine.
class BleCallbacks : public NimBLEServerCallbacks, public NimBLECharacteristicCallbacks {
public:
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        if (g_ble->connected_) {  // one central at a time
            server->disconnect(info.getConnHandle());
            return;
        }
        g_ble->conn_handle_ = info.getConnHandle();
        g_ble->mtu_ = info.getMTU();
        g_ble->conn_interval_ = info.getConnInterval();
        g_ble->subscribed_ = false;
        g_ble->connected_ = true;
        g_ble->link_changed_ = true;
        logLine("[ble] connected %s interval=%u.%02ums", info.getAddress().toString().c_str(),
                static_cast<unsigned>(info.getConnInterval() * 125u / 100u),
                static_cast<unsigned>(info.getConnInterval() * 125u % 100u));
        server->updateConnParams(info.getConnHandle(), kConnIntervalMin, kConnIntervalMax, 0, kConnTimeout);
        wakeLoop();
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
        if (!g_ble->connected_ || info.getConnHandle() != g_ble->conn_handle_) return;  // a central we turned away
        g_ble->connected_ = false;
        g_ble->subscribed_ = false;
        g_ble->mtu_ = 0;
        g_ble->conn_interval_ = 0;
        g_ble->link_changed_ = true;
        logLine("[ble] disconnected reason=0x%x", reason);
        wakeLoop();
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
        if (info.getConnHandle() != g_ble->conn_handle_) return;
        g_ble->mtu_ = mtu;
        logLine("[ble] mtu=%u", static_cast<unsigned>(mtu));
    }

    void onConnParamsUpdate(NimBLEConnInfo& info) override {
        if (info.getConnHandle() != g_ble->conn_handle_) return;
        g_ble->conn_interval_ = info.getConnInterval();
        logLine("[ble] params interval=%u.%02ums latency=%u timeout=%ums",
                static_cast<unsigned>(info.getConnInterval() * 125u / 100u),
                static_cast<unsigned>(info.getConnInterval() * 125u % 100u), static_cast<unsigned>(info.getConnLatency()),
                static_cast<unsigned>(info.getConnTimeout() * 10u));
    }

    // `state` only: the other two characteristics cannot be subscribed to.
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t sub_value) override {
        if (info.getConnHandle() != g_ble->conn_handle_) return;
        g_ble->subscribed_ = (sub_value & 0x0001) != 0;
        logLine("[ble] state notifications %s", g_ble->subscribed_ ? "on" : "off");
        wakeLoop();
    }

    // `state` and `status`: hand NimBLE the loop's latest copy just before it answers.
    void onRead(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        uint8_t value[kMaxStatus > jacket::kMaxFrame ? kMaxStatus : jacket::kMaxFrame];
        const bool status = characteristic == g_status_char;
        portENTER_CRITICAL(&g_state_mux);
        const size_t len = status ? g_status_len : g_state_len;
        memcpy(value, status ? reinterpret_cast<const uint8_t*>(g_status) : g_state, len);
        portEXIT_CRITICAL(&g_state_mux);
        characteristic->setValue(value, len);
    }

    // `command` and `live`.
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        const NimBLEAttValue value = characteristic->getValue();
        if (characteristic == live_char) {
            if (value.size() < jacket::kLiveFrameSize) return;  // "shorter frames are dropped silently"
            const uint32_t arrived_ms = millis();  // the beat clock wants the arrival, not when the loop got round to it
            portENTER_CRITICAL(&g_live_mux);
            memcpy(g_live, value.data(), jacket::kLiveFrameSize);
            g_live_arrived_ms = arrived_ms;
            g_live_fresh = true;
            portEXIT_CRITICAL(&g_live_mux);
        } else {
            CommandItem item;
            item.len = static_cast<uint8_t>(value.size() < jacket::kMaxFrame ? value.size() : jacket::kMaxFrame);
            memcpy(item.data, value.data(), item.len);
            if (xQueueSend(g_commands, &item, 0) != pdTRUE) g_ble->dropped_commands_ = g_ble->dropped_commands_ + 1;
        }
        wakeLoop();
    }

    NimBLECharacteristic* live_char = nullptr;
};

namespace {
BleCallbacks g_callbacks;
}

bool Ble::begin(TaskHandle_t wake_task) {
    g_ble = this;
    g_wake_task = wake_task;
    g_commands = xQueueCreate(kCommandQueueLength, sizeof(CommandItem));
    if (g_commands == nullptr || !NimBLEDevice::init(kDeviceName)) {
        logLine("[ble] stack FAILED to start");
        return false;
    }
    NimBLEDevice::setPower(cfg::kTxPowerDbm);
    NimBLEDevice::setMTU(kPreferredMtu);

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(&g_callbacks);
    // The loop chooses the advertising interval, but if it is ever stuck (a
    // firmware upload) the stack still makes the jacket discoverable again.
    server->advertiseOnDisconnect(true);

    // No value is longer than a frame, so longer writes are refused by the stack.
    NimBLEService* service = server->createService(kServiceUuid);
    NimBLECharacteristic* command = service->createCharacteristic(
        kCommandUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, jacket::kMaxFrame);
    g_state_char = service->createCharacteristic(kStateUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
                                                 jacket::kMaxFrame);
    NimBLECharacteristic* live = service->createCharacteristic(kLiveUuid, NIMBLE_PROPERTY::WRITE_NR, jacket::kMaxFrame);
    g_callbacks.live_char = live;
    command->setCallbacks(&g_callbacks);
    g_state_char->setCallbacks(&g_callbacks);
    live->setCallbacks(&g_callbacks);

    // Diagnosis, in a service of its own so that the protocol's stays as
    // PROTOCOL.md "Transport" lists it. Created after it: its handles follow.
    NimBLEService* debug = server->createService(kDebugServiceUuid);
    g_status_char = debug->createCharacteristic(kStatusUuid, NIMBLE_PROPERTY::READ, kMaxStatus);
    g_status_char->setCallbacks(&g_callbacks);
    bool ok = server->start();

    // Advertisement: flags + the 128-bit service UUID. Scan response: the name.
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->enableScanResponse(true);
    advertising->addServiceUUID(kServiceUuid);
    advertising->setName(kDeviceName);

    logLine("[ble] \"%s\" addr %s, tx %ddBm", kDeviceName, NimBLEDevice::getAddress().toString().c_str(),
            NimBLEDevice::getPower());
    adv_fast_until_ms_ = millis() + cfg::kAdvFastMs;
    ok = advertise(false) && ok;
    return ok;
}

void Ble::stop() {
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server == nullptr) return;
    server->advertiseOnDisconnect(false);
    NimBLEDevice::getAdvertising()->stop();
    if (connected_) server->disconnect(conn_handle_);
}

bool Ble::takeCommand(uint8_t* out, size_t& len) {
    CommandItem item;
    if (g_commands == nullptr || xQueueReceive(g_commands, &item, 0) != pdTRUE) return false;
    memcpy(out, item.data, item.len);
    len = item.len;
    return true;
}

bool Ble::peekOpcode(uint8_t& opcode) const {
    CommandItem item;
    if (g_commands == nullptr || xQueuePeek(g_commands, &item, 0) != pdTRUE || item.len == 0) return false;
    opcode = item.data[0];
    return true;
}

bool Ble::commandsWaiting() const { return g_commands != nullptr && uxQueueMessagesWaiting(g_commands) != 0; }

bool Ble::takeLive(uint8_t* out, uint32_t& arrived_ms) {
    portENTER_CRITICAL(&g_live_mux);
    const bool fresh = g_live_fresh;
    if (fresh) {
        memcpy(out, g_live, jacket::kLiveFrameSize);
        arrived_ms = g_live_arrived_ms;
    }
    g_live_fresh = false;
    portEXIT_CRITICAL(&g_live_mux);
    return fresh;
}

void Ble::setState(const uint8_t* frame, size_t len) {
    if (len > sizeof g_state) return;
    portENTER_CRITICAL(&g_state_mux);
    memcpy(g_state, frame, len);
    g_state_len = len;
    portEXIT_CRITICAL(&g_state_mux);
}

void Ble::setStatus(const char* text) {
    size_t len = strlen(text);
    if (len > sizeof g_status) len = sizeof g_status;
    portENTER_CRITICAL(&g_state_mux);
    memcpy(g_status, text, len);
    g_status_len = len;
    portEXIT_CRITICAL(&g_state_mux);
}

void Ble::send(const uint8_t* frame, size_t len) {
    if (!connected_ || !subscribed_ || len == 0 || len > jacket::kMaxFrame) return;
    if (reply_count_ == kReplySlots) {  // the central is not taking them: keep the oldest, they were asked for first
        ++dropped_replies_;
        return;
    }
    Reply& slot = replies_[(reply_head_ + reply_count_) % kReplySlots];
    slot.len = static_cast<uint8_t>(len);
    memcpy(slot.data, frame, len);
    ++reply_count_;
}

void Ble::forgetQueued() {
    reply_count_ = 0;
    reply_waiting_ = false;
    state_due_ = false;
}

void Ble::poll(uint32_t now_ms) {
    if (link_changed_) {
        link_changed_ = false;
        forgetQueued();
        if (!connected_) {
            adv_fast_until_ms_ = now_ms + cfg::kAdvFastMs;
            advertise(false);
        }
    }
    if (!connected_ && !adv_slow_ && static_cast<int32_t>(now_ms - adv_fast_until_ms_) >= 0) {
        advertise(true);
        return;
    }
    // Once a second, put right what the two tasks may have got wrong between
    // them: never advertise to a second central, never stay undiscoverable.
    if (now_ms - adv_checked_ms_ < kAdvCheckMs) return;
    adv_checked_ms_ = now_ms;
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (connected_) {
        if (advertising->isAdvertising()) advertising->stop();
    } else if (!advertising->isAdvertising()) {
        advertise(adv_slow_);
    }
}

void Ble::flush(uint32_t now_ms) {
    if (!connected_ || !subscribed_) {
        forgetQueued();
        return;
    }
    while (reply_count_ != 0) {
        const Reply& reply = replies_[reply_head_];
        if (!reply_waiting_) {
            reply_waiting_ = true;
            reply_since_ms_ = now_ms;
        }
        if (!notifyFrame(reply.data, reply.len)) {
            if (now_ms - reply_since_ms_ < kReplyGiveUpMs) return;  // the stack is full: try again next pass
            ++dropped_replies_;
        }
        reply_head_ = static_cast<uint8_t>((reply_head_ + 1) % kReplySlots);
        --reply_count_;
        reply_waiting_ = false;
    }
    if (state_due_) {
        uint8_t frame[jacket::kMaxFrame];
        portENTER_CRITICAL(&g_state_mux);
        const size_t len = g_state_len;
        memcpy(frame, g_state, len);
        portEXIT_CRITICAL(&g_state_mux);
        if (notifyFrame(frame, len)) state_due_ = false;
    }
}

// False: the stack could not take it now, try again. True: done with this
// frame, sent or (too long for this link) dropped.
bool Ble::notifyFrame(const uint8_t* frame, size_t len) {
    if (g_state_char == nullptr || len == 0) return false;
    // A notification carries at most MTU - 3 bytes. NimBLE cuts a longer one
    // short and reports success; a cut frame is worse than none.
    const uint16_t mtu = mtu_;
    if (len + 3u > mtu) {
        ++oversize_drops_;
        return true;
    }
    if (!g_state_char->notify(frame, len, conn_handle_)) return false;
    ++notifications_;
    return true;
}

bool Ble::advertise(bool slow) {
    if (connected_) return true;
    const uint16_t interval = slow ? cfg::kAdvSlowInterval : cfg::kAdvFastInterval;
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->stop();
    advertising->setMinInterval(interval);
    advertising->setMaxInterval(interval);
    const bool running = advertising->start();
    if (!adv_logged_ || running != adv_running_ || slow != adv_slow_) {  // changes only, not every retry
        if (running) {
            logLine("[ble] advertising every %u ms", static_cast<unsigned>(interval * 625u / 1000u));
        } else {
            logLine("[ble] advertising FAILED to start");
        }
        adv_logged_ = true;
    }
    adv_running_ = running;
    adv_slow_ = slow;
    return running;
}
