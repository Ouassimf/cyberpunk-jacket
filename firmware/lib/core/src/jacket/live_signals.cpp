#include "jacket/live_signals.h"

namespace jacket {

namespace {

// Per-millisecond share of the remaining distance for a time constant:
// 65536 * (1 - e^(-1/tau)), which is 65536 / (tau + 0.5) to well under 1 %.
constexpr uint32_t perMs(uint16_t tau_ms) { return 131072u / (2u * tau_ms + 1u); }

constexpr uint32_t kAudioRise = perMs(kAudioAttackMs);
constexpr uint32_t kAudioFall = perMs(kAudioReleaseMs);
constexpr uint32_t kMotionRise = perMs(kMotionAttackMs);
constexpr uint32_t kMotionFall = perMs(kMotionReleaseMs);
constexpr uint32_t kTiltRate = perMs(kTiltSmoothMs);

constexpr int32_t kMixFull = 255 * 65536;
constexpr int32_t kMixStep = kMixFull / kReactiveRampMs;  // per millisecond

// After a pause this long nothing is worth smoothing: jump to the targets.
constexpr uint32_t kCatchUpMs = 1000;

void ramp(int32_t& mix, bool present) {
    mix += present ? kMixStep : -kMixStep;
    if (mix > kMixFull) mix = kMixFull;
    if (mix < 0) mix = 0;
}

int32_t clampTilt(int8_t tilt) {
    if (tilt > kTiltFull) return kTiltFull;
    if (tilt < -kTiltFull) return -kTiltFull;
    return tilt;
}

}  // namespace

void LiveSignals::Follower::step(uint32_t rise_q16, uint32_t fall_q16) {
    const int32_t gap = target - value;
    if (gap == 0) return;
    const uint32_t rate = gap > 0 ? rise_q16 : fall_q16;
    int32_t move = static_cast<int32_t>((static_cast<int64_t>(gap) * rate) / 65536);
    if (move == 0) move = gap > 0 ? 1 : -1;  // always arrive
    value += move;
}

void LiveSignals::reset() {
    beat_.reset();
    out_ = Reactive();
    started_ = false;
    now_ms_ = 0;
    audio_present_ = false;
    motion_present_ = false;
    audio_mix_q16_ = 0;
    motion_mix_q16_ = 0;
    beat_mix_q16_ = 0;
    level_ = bass_ = mid_ = treble_ = Follower();
    motion_ = tilt_x_ = tilt_y_ = Follower();
}

void LiveSignals::stepOneMs(uint32_t at_ms) {
    ramp(audio_mix_q16_, audio_present_);
    ramp(motion_mix_q16_, motion_present_);
    ramp(beat_mix_q16_, beat_.activeAt(at_ms));
    level_.step(kAudioRise, kAudioFall);
    bass_.step(kAudioRise, kAudioFall);
    mid_.step(kAudioRise, kAudioFall);
    treble_.step(kAudioRise, kAudioFall);
    motion_.step(kMotionRise, kMotionFall);
    tilt_x_.step(kTiltRate, kTiltRate);
    tilt_y_.step(kTiltRate, kTiltRate);
}

const Reactive& LiveSignals::advance(uint32_t now_ms) {
    if (!started_) {
        started_ = true;
        now_ms_ = now_ms;
    }
    const uint32_t dt = now_ms - now_ms_;
    if (dt != 0 && dt <= 0x7FFFFFFFu) {
        if (dt > kCatchUpMs) {
            audio_mix_q16_ = audio_present_ ? kMixFull : 0;
            motion_mix_q16_ = motion_present_ ? kMixFull : 0;
            beat_mix_q16_ = beat_.activeAt(now_ms) ? kMixFull : 0;
            level_.snap();
            bass_.snap();
            mid_.snap();
            treble_.snap();
            motion_.snap();
            tilt_x_.snap();
            tilt_y_.snap();
        } else {
            for (uint32_t i = 1; i <= dt; ++i) stepOneMs(now_ms_ + i);
        }
        now_ms_ = now_ms;
    }
    beat_.advance(now_ms);
    publish();
    return out_;
}

void LiveSignals::onFrame(const LiveInput& in, uint32_t now_ms, uint8_t groups) {
    advance(now_ms);  // what happened up to this frame used the old targets

    // A group without its valid bit keeps what it had and fades out. One that
    // (re)appears from nothing starts at its values instead of gliding to them.
    if ((groups & kLiveAudioValid) != 0) {
        audio_present_ = (in.flags & kLiveAudioValid) != 0;
        if (audio_present_) {
            level_.set(in.level);
            bass_.set(in.bass);
            mid_.set(in.mid);
            treble_.set(in.treble);
            if (audio_mix_q16_ == 0) {
                level_.snap();
                bass_.snap();
                mid_.snap();
                treble_.snap();
            }
        }
    }
    if ((groups & kLiveMotionValid) != 0) {
        motion_present_ = (in.flags & kLiveMotionValid) != 0;
        if (motion_present_) {
            motion_.set(in.motion);
            tilt_x_.set(clampTilt(in.tilt_x));
            tilt_y_.set(clampTilt(in.tilt_y));
            if (motion_mix_q16_ == 0) {
                motion_.snap();
                tilt_x_.snap();
                tilt_y_.snap();
            }
        }
    }

    // The beat clock measures its sender's clock from t_ms: only frames of the
    // sender it follows may reach it.
    if ((groups & kLiveBeatValid) != 0) {
        beat_.onFrame((in.flags & kLiveBeatValid) != 0, in.bpm_x10, in.beat_phase, in.t_ms, now_ms);
    }
    publish();
}

void LiveSignals::onTimeout(uint32_t at_ms, uint8_t groups) {
    if (started_ && at_ms - now_ms_ > 0x7FFFFFFFu) at_ms = now_ms_;  // already past it
    advance(at_ms);
    if ((groups & kLiveAudioValid) != 0) audio_present_ = false;
    if ((groups & kLiveMotionValid) != 0) motion_present_ = false;
}

void LiveSignals::publish() {
    out_.audio_mix = static_cast<uint8_t>(audio_mix_q16_ >> 16);
    out_.motion_mix = static_cast<uint8_t>(motion_mix_q16_ >> 16);
    out_.beat_mix = static_cast<uint8_t>(beat_mix_q16_ >> 16);
    out_.level = static_cast<uint8_t>(level_.rounded());
    out_.bass = static_cast<uint8_t>(bass_.rounded());
    out_.mid = static_cast<uint8_t>(mid_.rounded());
    out_.treble = static_cast<uint8_t>(treble_.rounded());
    out_.motion = static_cast<uint8_t>(motion_.rounded());
    out_.tilt_x = static_cast<int8_t>(tilt_x_.rounded());
    out_.tilt_y = static_cast<int8_t>(tilt_y_.rounded());
    out_.beat = beat_.position();
}

}  // namespace jacket
