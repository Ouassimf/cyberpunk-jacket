// Beat clock: turns the beat fields of the live frames into a smooth local
// beat position that keeps running between frames.
//
// The phone says, about 30 times a second, "at my time t_ms the beat was at
// phase p and the tempo is b". Frames arrive tens of milliseconds late, by a
// different amount each time, and some never arrive. Using the arrival time
// would make the beat jitter by that much, so the clock does three things:
//
//  1. Clock offset. Every frame gives one sample of (local arrival time -
//     phone send time). The smallest sample of the last 16 seconds is the
//     frame that was delayed least; taking it as "no delay" tells how late
//     every other frame was, to the millisecond. Each frame's phase is then
//     applied to the moment it was sent, not the moment it arrived.
//     When the best sample leaves the window the estimate does not jump to
//     the next best: it drops at once but rises only by 4 ms per second.
//     t_ms is only 16 bits wide: it is unwrapped from frame to frame.
//  2. Phase lock. Between frames the position advances at the phone's tempo.
//     Each frame is compared with the prediction: a small error is slewed
//     away (the clock runs up to 25 % fast or slow, it never steps), a large
//     one seen twice in a row is taken over at once, and a single wild frame
//     is ignored.
//  3. Coasting. When frames stop carrying a valid beat, or stop altogether,
//     the clock keeps the last tempo for kCoastMs and only then reports
//     "no beat".
//
// What remains: the clock lags the phone by the smallest delay of the link
// (which nothing here can measure) and wanders by a few milliseconds around
// that. See the bound asserted in test/test_beat.
//
// No heap, integer only. Not thread-safe.
#pragma once

#include <stdint.h>

namespace jacket {

class BeatClock {
public:
    // ---- What is assumed about the phone (PROTOCOL.md "Live input") -----------
    // bpm_x10 outside this range is treated as "no beat in this frame".
    static constexpr uint16_t kMinBpmX10 = 300;   // 30.0 BPM
    static constexpr uint16_t kMaxBpmX10 = 3000;  // 300.0 BPM
    // beat_phase is floor(phase * 256): the true phase is half a step later on average.
    static constexpr uint32_t kPhaseStepQ32 = 1u << 24;
    // The delay of the best frame, if it is ever measured. 0: the jacket lags
    // the phone by exactly that delay.
    static constexpr uint32_t kLinkLatencyMs = 0;

    // ---- Tuning ------------------------------------------------------------------
    static constexpr uint32_t kCoastMs = 3000;           // keep the tempo this long after the last valid beat
    static constexpr uint32_t kFreshMs = 500;            // "locked": a valid beat arrived this recently
    static constexpr uint32_t kBucketMs = 2000;          // the offset window: kBuckets of these
    static constexpr uint8_t kBuckets = 8;
    static constexpr uint32_t kOffsetRiseQ8PerMs = 1;    // the delay estimate rises by at most 1/256 ms per ms
    static constexpr uint32_t kRestartGapMs = 2000;      // after this silence the phone clock is measured afresh
    static constexpr uint32_t kMaxExcessDelayMs = 1000;  // a frame "later" than this means the phone clock jumped
    static constexpr uint32_t kSnapQ32 = 1u << 29;       // errors above 1/8 beat are jumps, not drift
    static constexpr uint32_t kSnapAgreeQ32 = 1u << 28;  // two frames within 1/16 beat of each other confirm a jump
    static constexpr uint8_t kSlewGainShift = 2;         // each frame corrects a quarter of the error it measures ...
    static constexpr uint8_t kSlewRateShift = 2;         // ... at no more than a quarter of the tempo

    BeatClock() { reset(); }
    void reset();

    // One live frame, which arrived at local time `now_ms`. `beat_valid` is
    // the frame's beat-valid flag; t_ms is used from every frame. The arrival
    // may lie a little before the last advance(): a frame is often handled a
    // few milliseconds after the radio delivered it.
    void onFrame(bool beat_valid, uint16_t bpm_x10, uint8_t beat_phase, uint16_t t_ms, uint32_t now_ms);

    // The frames that follow come from another sender than those before (the
    // phone instead of the built-in microphone, or back): their t_ms is
    // another clock, so the offset is measured afresh from the next frame.
    // Tempo and position carry on; the new sender's phase is locked onto as
    // any phase change is.
    void sourceChanged();

    // Moves the clock to local time `now_ms` (times must not go backwards).
    // The result does not depend on how the time is cut into calls.
    void advance(uint32_t now_ms);

    // ---- At the time of the last advance() or onFrame() ------------------------------
    // Beats as 16.16: beat count in the high half, phase in the low half
    // (0 = on the beat). Keeps advancing at the last tempo even when inactive.
    uint32_t position() const { return static_cast<uint32_t>(pos_ >> 16); }
    uint16_t phase() const { return static_cast<uint16_t>(pos_ >> 16); }
    uint32_t beats() const { return static_cast<uint32_t>(pos_ >> 32); }

    bool active() const { return activeAt(now_ms_); }  // locked or coasting: the position means something
    bool locked() const { return lockedAt(now_ms_); }  // a valid beat arrived in the last kFreshMs
    uint16_t bpmX10() const { return bpm_x10_; }       // the tempo in use (0 before the first beat)

    // Whether the clock is (still) active at a time at or after the last advance.
    bool activeAt(uint32_t at_ms) const { return ever_valid_ && at_ms - last_valid_ms_ < kCoastMs; }
    bool lockedAt(uint32_t at_ms) const { return ever_valid_ && at_ms - last_valid_ms_ < kFreshMs; }

    // How late the last frame was compared with the best one of the window, in ms.
    uint16_t lastExcessDelayMs() const { return last_excess_ms_; }

private:
    struct Bucket {
        uint32_t epoch;
        int32_t min;
    };

    void restartOffset(uint16_t t_ms, uint32_t now_ms);
    uint32_t excessDelayQ8(uint16_t t_ms, uint32_t now_ms);

    // Beat position: beats in 32.32 fixed point, and what advances it.
    uint64_t pos_;
    uint32_t rate_q32_;  // beat fraction (Q32) per millisecond
    int64_t slew_q32_;   // correction still to be applied
    uint32_t now_ms_;
    bool started_;

    uint16_t bpm_x10_;
    bool ever_valid_;
    uint32_t last_valid_ms_;
    bool snap_pending_;
    int32_t pending_error_;

    // Phone clock: unwrapped t_ms, and the window of delay samples.
    bool have_phone_;
    uint16_t last_t16_;
    uint32_t phone_ms_;
    uint32_t last_frame_ms_;
    uint32_t delay_ref_;  // delay samples are kept relative to this
    Bucket buckets_[kBuckets];
    bool offset_set_;
    int32_t offset_q8_;   // the delay taken as "no delay", in 1/256 ms relative to delay_ref_
    uint16_t last_excess_ms_;
};

}  // namespace jacket
