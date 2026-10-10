// Bluetooth LE peripheral, as PROTOCOL.md "Transport" describes it.
//
// The engine is not thread-safe and NimBLE calls back on its own task, so the
// callbacks only store what arrived and wake the loop task:
//
//   command  -> a FreeRTOS queue; the loop takes frames with takeCommand()
//   live     -> one slot, the newest frame wins, stamped with its arrival
//               time; the loop takes it with takeLive()
//   state    -> reads are answered from a copy the loop refreshes with setState()
//
// Beside the protocol's service there is a second one for diagnosis, with one
// readable characteristic, `status`: a line of text the loop refreshes with
// setStatus() (PROTOCOL.md "Diagnostics"). It is how a board without a serial
// port says what its heartbeat would. The protocol's own service is unchanged.
//
// Everything the jacket sends (replies and STATE) is sent from the loop task
// through this class, as notifications on `state`.
#pragma once

#include <jacket.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class Ble : public jacket::FrameSink {
public:
    // Starts the stack and advertising; false if any of it failed. `wake_task`
    // is notified (xTaskNotifyGive) whenever a frame arrives or the link changes.
    bool begin(TaskHandle_t wake_task);

    // Stops advertising and drops the central, for good: the jacket is about
    // to restart into another firmware.
    void stop();

    // ---- Called from the loop task ---------------------------------------------

    // Next queued command frame; false when the queue is empty. `out` holds
    // jacket::kMaxFrame bytes. An empty write is a frame of length 0.
    bool takeCommand(uint8_t* out, size_t& len);

    // The opcode of the command takeCommand() would return next; false when
    // there is none (or it is an empty write).
    bool peekOpcode(uint8_t& opcode) const;
    bool commandsWaiting() const;

    // False while the reply queue is full. A command's reply would be lost:
    // leave the command queued and flush() first.
    bool canQueueReply() const { return reply_count_ < kReplySlots; }

    // The newest live frame since the last call (jacket::kLiveFrameSize bytes)
    // and the millis() at which the Bluetooth stack delivered it.
    bool takeLive(uint8_t* out, uint32_t& arrived_ms);

    // The STATE frame a read of `state` returns from now on.
    void setState(const uint8_t* frame, size_t len);

    // The text a read of `status` returns from now on (cut at 240 bytes).
    void setStatus(const char* text);

    // Asks for the current STATE frame to be notified at the next flush().
    void notifyState() { state_due_ = true; }

    // FrameSink: queues one reply frame for notification. Dropped when nobody
    // is subscribed. A frame that does not fit the negotiated MTU is not sent
    // at all (the stack would cut it short and call that a success): it is
    // counted in oversizeDrops().
    void send(const uint8_t* frame, size_t len) override;

    // Link housekeeping, first thing in every loop pass: forgets what was
    // queued for a central that has gone, and keeps the jacket advertising
    // (fast at first, then slowly) while nobody is connected.
    void poll(uint32_t now_ms);

    // Sends the queued replies, then STATE if it is due. Whatever the stack
    // cannot take yet stays queued for the next call.
    void flush(uint32_t now_ms);

    // True while something is waiting to be sent: flush() again soon.
    bool busy() const { return reply_count_ != 0 || state_due_; }

    // ---- Status (any task) ---------------------------------------------------------
    bool connected() const { return connected_; }
    bool subscribed() const { return subscribed_; }
    uint16_t mtu() const { return mtu_; }
    uint16_t connIntervalUnits() const { return conn_interval_; }  // 1.25 ms units, 0 when not connected
    uint32_t droppedCommands() const { return dropped_commands_; }
    uint32_t droppedReplies() const { return dropped_replies_; }
    uint32_t oversizeDrops() const { return oversize_drops_; }
    uint32_t notifications() const { return notifications_; }

private:
    friend class BleCallbacks;

    static constexpr uint8_t kReplySlots = 8;
    struct Reply {
        uint8_t len = 0;
        uint8_t data[jacket::kMaxFrame];
    };

    bool notifyFrame(const uint8_t* frame, size_t len);
    bool advertise(bool slow);
    void forgetQueued();

    // Shared with the NimBLE task.
    volatile bool connected_ = false;
    volatile bool subscribed_ = false;
    volatile bool link_changed_ = false;  // a connect or disconnect the loop has not handled yet
    volatile uint16_t conn_handle_ = 0;
    volatile uint16_t mtu_ = 0;
    volatile uint16_t conn_interval_ = 0;
    volatile uint32_t dropped_commands_ = 0;

    // Loop task only.
    Reply replies_[kReplySlots];
    uint8_t reply_head_ = 0;
    uint8_t reply_count_ = 0;
    uint32_t reply_since_ms_ = 0;  // when the frame at the head was first tried
    bool reply_waiting_ = false;
    bool state_due_ = false;
    uint32_t dropped_replies_ = 0;
    uint32_t oversize_drops_ = 0;
    uint32_t notifications_ = 0;
    bool adv_slow_ = false;
    bool adv_running_ = false;  // what the last attempt to start it gave
    bool adv_logged_ = false;
    uint32_t adv_fast_until_ms_ = 0;
    uint32_t adv_checked_ms_ = 0;
};
