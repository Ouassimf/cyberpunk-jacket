#include "jacket/boot_policy.h"

#include <string.h>

namespace jacket {

namespace {

constexpr uint8_t kRecordVersion = 1;  // [1][crashes][flags][0][image id]
constexpr uint8_t kFlagConfirmed = 0x01;
constexpr uint8_t kFlagArrivedBySwitch = 0x02;
constexpr uint8_t kFlagExpectReset = 0x04;

}  // namespace

size_t serializeBootRecord(const BootRecord& record, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kBootRecordSize) return 0;
    out[0] = kRecordVersion;
    out[1] = record.crashes;
    out[2] = static_cast<uint8_t>((record.confirmed ? kFlagConfirmed : 0) | (record.arrived_by_switch ? kFlagArrivedBySwitch : 0) |
                                  (record.expect_reset ? kFlagExpectReset : 0));
    out[3] = 0;
    memcpy(out + 4, record.image, kImageIdSize);
    return kBootRecordSize;
}

bool deserializeBootRecord(const uint8_t* data, size_t len, BootRecord& out) {
    if (data == nullptr || len != kBootRecordSize || data[0] != kRecordVersion) return false;
    out.crashes = data[1];
    out.confirmed = (data[2] & kFlagConfirmed) != 0;
    out.arrived_by_switch = (data[2] & kFlagArrivedBySwitch) != 0;
    out.expect_reset = (data[2] & kFlagExpectReset) != 0;
    memcpy(out.image, data + 4, kImageIdSize);
    return true;
}

BootDecision decideBoot(const BootRecord* stored, const BootFacts& facts) {
    BootDecision decision;
    // The reset before this start was announced: whatever it looks like, it
    // was not a crash. The announcement is good for one start.
    decision.handed_over = stored != nullptr && stored->expect_reset;

    const bool same_image = stored != nullptr && facts.image != nullptr && memcmp(stored->image, facts.image, kImageIdSize) == 0;
    if (same_image) {
        decision.record = *stored;
        if (decision.record.expect_reset) {
            decision.record.expect_reset = false;
            decision.changed = true;
        }
    } else {
        // An image this record has not seen: nothing is known about it, and
        // the crashes counted so far were another image's.
        decision.new_image = true;
        decision.changed = true;
        if (facts.image != nullptr) memcpy(decision.record.image, facts.image, kImageIdSize);
    }
    if (facts.crashed && !decision.handed_over) {
        if (decision.record.crashes < 255) ++decision.record.crashes;
        decision.changed = true;
    }

    if (decision.record.crashes >= facts.crash_threshold && facts.crash_threshold != 0) {
        const bool may_switch = !decision.record.confirmed && !decision.record.arrived_by_switch && facts.other_usable;
        decision.action = may_switch ? BootAction::SwitchSlot : BootAction::SafeMode;
    }
    return decision;
}

BootRecord recordForSwitch(const uint8_t* other) {
    BootRecord record;
    if (other != nullptr) memcpy(record.image, other, kImageIdSize);
    record.arrived_by_switch = true;
    record.expect_reset = true;
    return record;
}

bool confirmImage(BootRecord& record) {
    if (record.confirmed && !record.arrived_by_switch) return false;
    record.confirmed = true;
    record.arrived_by_switch = false;
    return true;
}

bool clearCrashes(BootRecord& record) {
    if (record.crashes == 0) return false;
    record.crashes = 0;
    return true;
}

RecoveryHold::State RecoveryHold::sample(bool pressed, uint32_t since_boot_ms) {
    switch (state_) {
        case State::Watching:
            if (since_boot_ms > window_ms_) {
                state_ = State::Done;
            } else if (pressed) {
                state_ = State::Holding;
                since_ms_ = since_boot_ms;
            }
            break;
        case State::Holding:
            if (!pressed) {
                // Too short. Another try is welcome while the window lasts.
                state_ = since_boot_ms > window_ms_ ? State::Done : State::Watching;
            } else if (since_boot_ms - since_ms_ >= hold_ms_) {
                state_ = State::Armed;
                since_ms_ = since_boot_ms;
            }
            break;
        case State::Armed:
            if (!pressed) {
                state_ = State::Fire;
            } else if (since_boot_ms - since_ms_ >= release_ms_) {
                state_ = State::Done;  // it never came up: not a finger
            }
            break;
        case State::Fire:
            state_ = State::Done;
            break;
        case State::Done:
            break;
    }
    return state_;
}

}  // namespace jacket
