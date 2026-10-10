#include "jacket/beat_clock.h"

namespace jacket {

namespace {

// Beat fraction (Q32) per millisecond at a tempo: bpm / 60000.
uint32_t rateFor(uint16_t bpm_x10) { return static_cast<uint32_t>((static_cast<uint64_t>(bpm_x10) << 32) / 600000u); }

uint32_t magnitude(int32_t v) { return v < 0 ? 0u - static_cast<uint32_t>(v) : static_cast<uint32_t>(v); }

}  // namespace

void BeatClock::reset() {
    pos_ = 0;
    rate_q32_ = 0;
    slew_q32_ = 0;
    now_ms_ = 0;
    started_ = false;
    bpm_x10_ = 0;
    ever_valid_ = false;
    last_valid_ms_ = 0;
    snap_pending_ = false;
    pending_error_ = 0;
    have_phone_ = false;
    last_t16_ = 0;
    phone_ms_ = 0;
    last_frame_ms_ = 0;
    delay_ref_ = 0;
    for (Bucket& b : buckets_) b = Bucket{0, 0};
    offset_set_ = false;
    offset_q8_ = 0;
    last_excess_ms_ = 0;
}

void BeatClock::sourceChanged() { have_phone_ = false; }

void BeatClock::advance(uint32_t now_ms) {
    if (!started_) {
        started_ = true;
        now_ms_ = now_ms;
        return;
    }
    const uint32_t dt = now_ms - now_ms_;
    if (dt == 0 || dt > 0x7FFFFFFFu) return;  // time did not move, or went backwards
    now_ms_ = now_ms;

    pos_ += static_cast<uint64_t>(rate_q32_) * dt;
    if (slew_q32_ != 0) {
        // Spread the correction: at most a quarter of the tempo, so the phase
        // always moves forward and never steps. Linear in dt, hence
        // independent of how often this is called.
        const int64_t limit = static_cast<int64_t>(rate_q32_ >> kSlewRateShift) * dt;
        int64_t step = slew_q32_;
        if (step > limit) step = limit;
        if (step < -limit) step = -limit;
        pos_ += static_cast<uint64_t>(step);
        slew_q32_ -= step;
    }
}

void BeatClock::restartOffset(uint16_t t_ms, uint32_t now_ms) {
    have_phone_ = true;
    phone_ms_ = t_ms;
    delay_ref_ = now_ms - phone_ms_;
    const uint32_t epoch = now_ms / kBucketMs;
    for (Bucket& b : buckets_) b = Bucket{epoch - kBuckets, 0};  // all outside the window
    offset_set_ = false;
}

// How much later than the best frame of the window this one arrived, in 1/256 ms.
uint32_t BeatClock::excessDelayQ8(uint16_t t_ms, uint32_t now_ms) {
    // Signed: a stamp a few milliseconds before the previous one is not a gap.
    const int32_t gap_ms = static_cast<int32_t>(now_ms - last_frame_ms_);
    const bool fresh = !have_phone_ || gap_ms > static_cast<int32_t>(kRestartGapMs) || gap_ms < -static_cast<int32_t>(kRestartGapMs);
    const uint32_t since_ms = fresh || gap_ms < 0 ? 0 : static_cast<uint32_t>(gap_ms);
    if (fresh) {
        restartOffset(t_ms, now_ms);
    } else {
        // Unwrap the 16-bit phone clock: frames are far less than 32 s apart.
        phone_ms_ += static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(t_ms - last_t16_))));
    }
    last_t16_ = t_ms;
    if (fresh || gap_ms > 0) last_frame_ms_ = now_ms;

    const uint32_t epoch = now_ms / kBucketMs;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int32_t delay = static_cast<int32_t>((now_ms - phone_ms_) - delay_ref_);
        Bucket& mine = buckets_[epoch % kBuckets];
        if (mine.epoch != epoch) mine = Bucket{epoch, delay};
        if (delay < mine.min) mine.min = delay;

        int32_t best = delay;
        for (const Bucket& b : buckets_) {
            if (epoch - b.epoch < kBuckets && b.min < best) best = b.min;
        }
        // Down at once, up slowly: losing the best sample must not move the beat.
        const int32_t best_q8 = best * 256;
        if (!offset_set_ || best_q8 <= offset_q8_) {
            offset_q8_ = best_q8;
        } else {
            const int32_t risen = offset_q8_ + static_cast<int32_t>(since_ms * kOffsetRiseQ8PerMs);
            offset_q8_ = risen < best_q8 ? risen : best_q8;
        }
        offset_set_ = true;
        const uint32_t excess_q8 = static_cast<uint32_t>(delay * 256 - offset_q8_);
        if (excess_q8 <= kMaxExcessDelayMs * 256u) return excess_q8;
        restartOffset(t_ms, now_ms);  // the phone's clock jumped back: measure it afresh
    }
    return 0;
}

void BeatClock::onFrame(bool beat_valid, uint16_t bpm_x10, uint8_t beat_phase, uint16_t t_ms, uint32_t now_ms) {
    advance(now_ms);
    const uint32_t excess_q8 = excessDelayQ8(t_ms, now_ms);
    last_excess_ms_ = static_cast<uint16_t>(excess_q8 >> 8);
    if (!beat_valid || bpm_x10 < kMinBpmX10 || bpm_x10 > kMaxBpmX10) return;

    const bool was_active = active();
    bpm_x10_ = bpm_x10;
    rate_q32_ = rateFor(bpm_x10);

    // The phase the phone reported, carried forward from when it was sent to now.
    // If the clock has already been moved past the arrival, the frame is that much older.
    uint32_t handled_late_ms = now_ms_ - now_ms;
    if (handled_late_ms > kMaxExcessDelayMs) handled_late_ms = 0;
    const uint32_t age_q8 = excess_q8 + (kLinkLatencyMs + handled_late_ms) * 256u;
    const uint32_t observed = (static_cast<uint32_t>(beat_phase) << 24) + kPhaseStepQ32 / 2 +
                              static_cast<uint32_t>((static_cast<uint64_t>(rate_q32_) * age_q8) >> 8);
    const uint32_t predicted = static_cast<uint32_t>(pos_);
    const int32_t error = static_cast<int32_t>(observed - predicted);  // +-half a beat, Q32

    if (!was_active) {
        // Nothing to disturb: take the phone's phase, moving forward only.
        pos_ += static_cast<uint32_t>(observed - predicted);
        slew_q32_ = 0;
        snap_pending_ = false;
    } else if (magnitude(error) > kSnapQ32) {
        if (snap_pending_ && magnitude(error - pending_error_) < kSnapAgreeQ32) {
            // The same big error twice: the phone's beat really moved.
            if (error < 0 && pos_ < magnitude(error)) pos_ += 1ull << 32;  // never below beat 0
            pos_ += static_cast<uint64_t>(static_cast<int64_t>(error));
            slew_q32_ = 0;
            snap_pending_ = false;
        } else {
            snap_pending_ = true;  // might be one bad frame: wait for the next
            pending_error_ = error;
        }
    } else {
        slew_q32_ = error / (1 << kSlewGainShift);
        snap_pending_ = false;
    }
    ever_valid_ = true;
    last_valid_ms_ = now_ms;
}

}  // namespace jacket
