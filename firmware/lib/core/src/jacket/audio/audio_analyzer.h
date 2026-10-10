// Audio analyser: turns microphone samples into what the reactive effects
// follow: how loud it is overall and in three bands, where the onsets are,
// and the tempo and phase of the beat.
//
// This is AudioAnalyzer.kt of the Android app (with BeatTracker.kt and Fft.kt
// behind it), ported so that a board with its own microphone hears the music
// the way the phone does. Same algorithm, same constants: change one and the
// other must follow. Only the FFT is done differently (fft.h), which moves the
// spectrum by rounding errors and nothing else.
//
// The rest of lib/core is integer only; jacket/audio is the exception and uses
// single-precision float throughout. Never double: the ESP32's FPU cannot do
// it, and double arithmetic in software is many times slower.
//
// Input is mono 16-bit PCM at kSampleRate in blocks of any size. Every kHop
// samples (10 ms) one analysis frame is made, and the outputs change.
//
// The choices, briefly:
//  - 16 kHz reaches 8 kHz, which covers kick to hi-hat. A higher rate would
//    cost more for nothing an LED can show.
//  - One 512-point FFT per frame (32 ms Hann window, 69 % overlap) serves
//    everything: its bins are summed into 17 roughly logarithmic bands, and
//    those into bass (60 to 265 Hz), mid (to 2 kHz) and treble (to 8 kHz).
//  - Automatic gain works in decibels. A ceiling per output follows the
//    loudest recent sound. It rises within a few frames; while sound keeps
//    coming within 3 dB of it, it stays (creeping down 2 dB a second, to
//    settle exactly on a steady level); 1.5 s after the last such sound it
//    sinks 8 dB a second until sound meets it again. So both a quiet room and
//    a club end up using the whole range within a few seconds, and between
//    the hits of a steady beat the ceiling does not move, which keeps a hit
//    half as loud at half the height. The outputs are linear in sound
//    pressure below that ceiling (255 at the ceiling, 128 about 6 dB under, 0
//    from 30 dB under). The ceiling never sinks below an absolute floor, so
//    the hiss of a silent room is not blown up into a light show; each band's
//    ceiling stays above that floor too and within 24 dB of the overall one,
//    so a band holding only leakage or noise stays dark.
//  - Smoothing is left to LiveSignals, which does its own (25 ms up, 180 ms
//    down). The outputs here follow the sound: up at once, down with a 30 ms
//    time constant, which only irons out the frame-to-frame wobble of a 32 ms
//    window. Whoever reads them more slowly than every frame should take the
//    peak since the last reading, as MicLive does, so that a short hit is
//    never missed.
//  - Onsets are the summed rise of the log energy of the 17 bands from one
//    frame to the next (spectral flux), with the bass bands counted double
//    because the kick carries the beat. The log makes a soft hi-hat count
//    next to a loud bass line; a floor 42 dB under the ceiling keeps noise
//    out of it.
//  - Tempo and beat phase come from that onset stream, see beat_tracker.h.
//
// Cost of process(). Counted in single-precision operations; the times are
// estimates from the code compiled for the ESP32 and the ratios measured on a
// PC, for a classic ESP32 at 80 MHz. Nobody has measured them on a board.
//  - Per sample: one store.
//  - Per frame, so once every 160 samples: 512 multiplications for the window,
//    one FFT (about 4 100 multiplications and 6 900 additions), 510
//    multiplications for the bin energies, 21 logf and 5 expf calls and 6
//    divisions. Something like 0.5 to 1 ms.
//  - Every 25th frame, so every 250 ms, on top of that one tempo estimate, all
//    inside that one call: about 150 000 multiply-adds and 1 300 divisions
//    (which the ESP32 does in software). This is the worst case, 30 to 50
//    times an ordinary frame on a PC: something like 20 to 35 ms, which is
//    longer than the 10 ms a frame lasts and longer than one picture at 60 fps.
//  - Over a second: a hundred frames and four estimates, the estimates being
//    the larger half. Something like 15 to 20 % of one core.
// So the task that calls process() must be one the render loop can interrupt
// (lower priority, or the other core), and the capture buffer ahead of it must
// hold what arrives while an estimate runs and the render loop cuts in: allow
// 64 ms, 1024 samples.
// test/test_audio prints the PC's times with -v.
//
// Memory: 13 816 bytes (sizeof, checked in test/test_audio): the beat tracker
// 10 608, the last 512 samples 1 024, the FFT's work space 2 048. All of it is
// in the object: make it static, never a local variable of a task. Nothing is
// allocated, and process() needs little stack (nothing large, no recursion). The
// constant tables (window, twiddles, tempo prior) are 5.5 KB of flash.
//
// Tests: test/test_audio holds this code to the app's own unit tests, with
// the app's signals, on a PC. When it was ported its outputs were also laid
// next to the Kotlin original's for the same sound, once, by hand: levels,
// onsets and beat flag were the same in every frame, tempo and phase to six
// digits. On the ESP32 the compiler fuses multiplications into additions
// (madd.s), so the last bits differ from the PC's; nothing has been run there.
//
// Not thread-safe: one task feeds it and reads it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "jacket/audio/beat_tracker.h"
#include "jacket/audio/fft.h"

namespace jacket {

class AudioAnalyzer {
public:
    static constexpr int kSampleRate = 16000;
    static constexpr int kHop = 160;  // samples per analysis frame: 10 ms
    static constexpr int kFrameRate = BeatTracker::kFrameRate;

    // Frames between a hit and the frame whose flux shows it most: the hit
    // enters at the tapered end of the window and counts fully about a frame
    // later. Measured with clicks and kicks, see the tests.
    static constexpr float kOnsetDelay = 1.3f;

    AudioAnalyzer();

    // Forgets everything heard. The sensitivity stays.
    void reset();

    // 0 to 100 (clamped), 50 is neutral. Higher lets quieter sound through
    // (the absolute floor drops by up to 12 dB) and drives the outputs harder
    // (up to 6 dB); lower does the opposite.
    void setSensitivity(int percent);
    int sensitivity() const { return sensitivity_; }

    // Mono 16-bit PCM at kSampleRate, any block size.
    void process(const int16_t* pcm, size_t count);

    // ---- As of the frame analysed last ------------------------------------------
    // Loudness overall and per band, 0 to 255, after gain and smoothing.
    uint8_t level() const { return level_[0]; }
    uint8_t bass() const { return level_[1]; }
    uint8_t mid() const { return level_[2]; }
    uint8_t treble() const { return level_[3]; }

    // True when that frame started a new sound (a drum hit, a clap).
    bool onset() const { return onset_; }
    uint32_t onsets() const { return onsets_; }  // since construction or reset()
    uint32_t frames() const { return frames_; }  // frames analysed since construction or reset()

    // ---- The beat ---------------------------------------------------------------
    // True while a beat is tracked; false in silence and for sound without a
    // steady pulse.
    bool beatValid() const { return tracker_.valid(); }
    // Beats per minute and seconds per beat, 0 while beatValid() is false.
    float bpm() const { return tracker_.valid() ? tracker_.bpm() : 0.0f; }
    float beatPeriod() const { return tracker_.valid() ? tracker_.periodFrames() / kFrameRate : 0.0f; }
    float beatConfidence() const { return tracker_.confidence(); }
    // Where the last sample given to process() lies in its beat: 0 on the
    // beat, rising to just under 1 before the next. It moves with every
    // sample, not only with every frame. Only meaningful while beatValid().
    float beatPhase() const;

private:
    static constexpr int kWindow = RealFft::kSize;
    static constexpr int kBands = 17;

    void analyse();

    BeatTracker tracker_;
    int16_t samples_[kWindow];  // the last kWindow samples, a ring
    float re_[RealFft::kBins];  // work space of analyse()
    float im_[RealFft::kBins];
    uint16_t write_;
    uint16_t pending_;  // samples since the last frame

    int8_t sensitivity_;
    bool primed_;  // a frame has been analysed since reset()
    bool onset_;
    uint8_t since_onset_;  // frames, counted up to the refractory time
    float band_log_[kBands];
    float flux_reference_;
    float flux_mean_;
    float flux_deviation_;

    // Index 0 is the overall level, 1 to 3 are bass, mid and treble.
    float ceiling_[4];
    uint8_t hold_[4];
    float smoothed_[4];
    uint8_t level_[4];

    uint32_t onsets_;
    uint32_t frames_;
};

}  // namespace jacket
