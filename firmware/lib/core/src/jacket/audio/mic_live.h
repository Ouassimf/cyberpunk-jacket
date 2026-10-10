// The built-in microphone as a source of live frames.
//
// A phone in live mode analyses what its microphone hears and sends the jacket
// a live frame about 30 times a second (PROTOCOL.md "Live input"). A board
// with a microphone of its own does that for itself: MicLive takes the
// captured samples, runs the analyser on them and describes the sound as a
// LiveInput, the way the phone would in a frame, so that nothing downstream
// (LiveSignals, the beat clock, the effects) can tell the two apart. It is the
// firmware's counterpart of the app's LiveFeatures and LiveFrameBuilder for
// the audio and beat groups. There is no motion here.
//
//     mic.process(samples, count);         // whenever captured audio arrives
//     if (mic.snapshot(t_ms, frame)) ...   // about 30 times a second: the frame goes
//                                          // to the engine, with t_ms as its time
//
// What a frame holds:
//  - flags: the audio bit, and the beat bit while a beat is tracked. Never
//    the motion bit; the motion fields are 0.
//  - level, bass, mid, treble: the highest value of the analysis frames (one
//    per 10 ms) since the previous snapshot, so a drum hit that fell between
//    two snapshots is still there at full height. With audio fed but no
//    analysis frame finished since, they are the analyser's current values.
//  - bpm_x10: tenths of a BPM, rounded; beat_phase: floor(phase * 256). Both 0
//    without the beat bit. The analyser tracks 66 to 186 BPM, always inside
//    what BeatClock accepts.
//  - seq counts snapshots, wrapping at 256; t_ms is the low 16 bits of the
//    time given.
//
// What `t_ms` must be: the local millisecond time at which the newest sample
// given to process() was captured. beat_phase is the phase of that sample,
// and BeatClock takes a frame's phase as belonging to its t_ms. A snapshot
// stamped later than its samples makes the beat late by the difference, so
// when the capture task only knows when a read returned, it should subtract
// what it knows of the buffering in between rather than add its own delay.
// Unlike the phone, the board has no link to compensate for (the app's
// latency offset has no counterpart): hand the frame to the engine with that
// same time as the time it arrived, and the beat clock sees a link without
// delay or jitter. That time may lie a little before the engine's last render.
//
// When the microphone stalls, snapshot() returns false; with no frame handed
// on, the engine's own 500 ms timeout ends live input, as it does for a phone
// that went away.
//
// Cost and memory are the analyser's (audio_analyzer.h): process() is its
// process(), with its worst case of tens of milliseconds every 250 ms;
// snapshot() is a few operations. 13 828 bytes, in the object: make it static.
// Single-precision float, like the rest of jacket/audio.
//
// Not thread-safe. process() and snapshot() belong to one task, the one that
// captures; it hands the finished frame to whoever feeds the engine. A lock
// around the object instead would make the render loop wait out a whole
// tempo estimate.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "jacket/audio/audio_analyzer.h"
#include "jacket/protocol.h"

namespace jacket {

class MicLive {
public:
    MicLive() { reset(); }

    // Forgets everything heard; the next frame starts from silence. The
    // sensitivity stays.
    void reset();

    // 0 to 100, 50 is neutral: see AudioAnalyzer::setSensitivity.
    void setSensitivity(int percent) { analyzer_.setSensitivity(percent); }

    // Captured audio: mono 16-bit PCM at AudioAnalyzer::kSampleRate, any
    // block size. See audio_analyzer.h for what a call can cost.
    void process(const int16_t* pcm, size_t count);

    // Describes the sound up to the newest sample fed, see above. Returns
    // false, and leaves `out` alone, if no audio was fed since the previous
    // snapshot.
    bool snapshot(uint32_t t_ms, LiveInput& out);

    const AudioAnalyzer& analyzer() const { return analyzer_; }

    // The protocol's encodings, as the app does them.
    static uint16_t bpmX10(float bpm);      // tenths of a BPM, rounded, half up
    static uint8_t phaseByte(float phase);  // 0 on the beat, 128 half-way, 255 just before the next

private:
    AudioAnalyzer analyzer_;
    uint16_t to_frame_;  // samples until the analyser finishes its next frame
    bool fed_;           // audio arrived since the last snapshot ...
    bool analysed_;      // ... and completed at least one analysis frame
    uint8_t peak_[4];    // level, bass, mid, treble: highest since the last snapshot
    uint8_t seq_;
};

}  // namespace jacket
