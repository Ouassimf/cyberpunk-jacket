// Live input as the reactive effects see it.
//
// The phone sends raw values about 30 times a second; the strips are redrawn
// 60 times a second. LiveSignals sits between the two: it smooths every value
// in real time, runs the beat clock, and fades each group of values in and out
// when it appears or disappears, so an effect can blend between its reactive
// and its fallback look without a visible step.
//
// Everything this firmware assumes about the meaning and scale of the live
// frame's fields is written in this file and in beat_clock.h, nowhere else.
#pragma once

#include <stdint.h>

#include "jacket/beat_clock.h"
#include "jacket/protocol.h"

namespace jacket {

// ---- Assumptions about the live frame (PROTOCOL.md "Live input") ---------------
//
//   flags     bit 0: level/bass/mid/treble mean something; bit 1: motion and
//             tilt do; bit 2: bpm_x10 and beat_phase do. Fields whose bit is
//             clear are ignored whatever they contain.
//   level, bass, mid, treble, motion
//             0..255, already auto-ranged on the phone: 0 = silence / still,
//             255 = as loud / as vigorous as it gets. Used linearly.
//   tilt_x, tilt_y
//             signed bytes from the direction of gravity: 0 = neutral,
//             +-kTiltFull = fully tipped to one side. Larger values are clamped.
//   bpm_x10, beat_phase, t_ms
//             see beat_clock.h. t_ms is taken from every frame, valid beat or not.
//   seq       not used.
constexpr int8_t kTiltFull = 127;

// ---- Who a frame speaks for ---------------------------------------------------------
// Live input can come from two places at once: the phone, and a microphone on
// the board itself. A frame is then handed over together with the groups its
// sender is responsible for; the other groups are left exactly as they are,
// neither updated nor counted as absent. Sound is one unit: the beat is heard
// in the audio, so the two always come from the same sender.
constexpr uint8_t kLiveSoundGroups = kLiveAudioValid | kLiveBeatValid;
constexpr uint8_t kLiveAllGroups = kLiveAudioValid | kLiveMotionValid | kLiveBeatValid;

// ---- Smoothing, in real time -------------------------------------------------------
// Time constants of the followers (ms): fast up, slower down, like a VU meter.
constexpr uint16_t kAudioAttackMs = 25;
constexpr uint16_t kAudioReleaseMs = 180;
constexpr uint16_t kMotionAttackMs = 60;
constexpr uint16_t kMotionReleaseMs = 400;
constexpr uint16_t kTiltSmoothMs = 120;
// How long a group of values takes to fade in when it appears and out when it goes.
constexpr uint16_t kReactiveRampMs = 300;

// What a reactive effect works from. All values are smoothed and change a
// little every millisecond, never in 30 Hz steps.
struct Reactive {
    // How much each group counts: 0 = absent, play the fallback; 255 = fully
    // live. In between, blend: the group is fading in or out.
    uint8_t audio_mix = 0;
    uint8_t motion_mix = 0;
    uint8_t beat_mix = 0;

    // A group that goes away keeps its last values while its weight fades
    // out, so the fade is between two steady pictures.
    uint8_t level = 0;   // 0..255
    uint8_t bass = 0;
    uint8_t mid = 0;
    uint8_t treble = 0;

    uint8_t motion = 0;  // 0..255
    int8_t tilt_x = 0;   // -kTiltFull..kTiltFull
    int8_t tilt_y = 0;

    uint32_t beat = 0;   // beats as 16.16 (count . phase); meaningful while beat_mix != 0
};

class LiveSignals {
public:
    LiveSignals() { reset(); }
    void reset();

    // A live frame that arrived at local time `now_ms`, speaking for `groups`
    // (see above). Within those, a group whose valid bit is clear fades out.
    void onFrame(const LiveInput& in, uint32_t now_ms, uint8_t groups = kLiveAllGroups);

    // Whoever supplied `groups` went silent at `at_ms` (the 500 ms timeout):
    // audio and motion fade out, holding their last values. The beat clock
    // coasts on by itself.
    void onTimeout(uint32_t at_ms, uint8_t groups = kLiveAllGroups);

    // The sound groups are about to come from another sender: see
    // BeatClock::sourceChanged().
    void soundSourceChanged() { beat_.sourceChanged(); }

    // Brings everything up to `now_ms` and returns it. Smoothing advances in
    // whole milliseconds, so the result does not depend on how often this is
    // called. Times must not go backwards.
    const Reactive& advance(uint32_t now_ms);

    const Reactive& current() const { return out_; }
    const BeatClock& beatClock() const { return beat_; }

private:
    // One smoothed value: Q16, chasing a target with separate rise and fall rates.
    struct Follower {
        int32_t value = 0;
        int32_t target = 0;
        void set(int32_t t) { target = t * 65536; }
        void snap() { value = target; }
        void step(uint32_t rise_q16, uint32_t fall_q16);
        int32_t rounded() const { return (value + 0x8000) >> 16; }
    };

    void stepOneMs(uint32_t at_ms);
    void publish();

    BeatClock beat_;
    Reactive out_;
    bool started_;
    uint32_t now_ms_;

    bool audio_present_;
    bool motion_present_;
    int32_t audio_mix_q16_;
    int32_t motion_mix_q16_;
    int32_t beat_mix_q16_;
    Follower level_, bass_, mid_, treble_;
    Follower motion_, tilt_x_, tilt_y_;
};

}  // namespace jacket
