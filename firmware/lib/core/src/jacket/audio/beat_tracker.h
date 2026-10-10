// Beat tracker: finds the tempo and the beat phase in a stream of onset
// strengths, one value per analysis frame (kFrameRate per second). It never
// hears audio, so it is tested with hand-made onset trains.
//
// This is BeatTracker.kt of the Android app, statement for statement: the same
// algorithm, the same constants, the same order of arithmetic. Change one and
// the other must follow, or the jacket's own microphone and the phone's will
// disagree about the same music.
//
// How, and why this way:
//  - Tempo comes from the autocorrelation of the last kHistory frames (6 s),
//    recomputed four times a second. Every candidate beat period is scored by
//    a comb: the mean autocorrelation at 1, 2, 3 and 4 periods. A period the
//    music really repeats at scores high at all four, which also gives
//    sub-frame resolution (an error of a quarter frame is a whole frame at the
//    fourth tooth).
//  - Half or double? A beat and its half or double all repeat, so the comb
//    alone cannot choose. Two rules do: a broad preference for tempos around
//    125 BPM, and "the fastest level that repeats as well wins": when the
//    period half as long scores (nearly) the same, the pulses in between are
//    as strong as the ones on the beat, so they are beats too. A plain click
//    track at 174 BPM is therefore 174, not 87; a 90 BPM track whose off-beats
//    are clearly weaker stays 90.
//  - Lock needs the same answer three times in a row with enough confidence;
//    small changes are followed smoothly, a different tempo replaces the old
//    one once it has been seen three times in a row.
//  - Phase is where a pulse train of that period best lines up with the
//    recent onsets (recent beats count more). A free-running oscillator
//    carries the phase from frame to frame and is nudged towards that
//    measurement, so the phase is continuous and survives a bar without drums.
//  - No beat is an answer: valid() is false when the comb is weak (noise,
//    speech), when the tempo keeps changing, and when the last seconds were
//    quiet compared to the rest or no longer match what came before (the
//    music stopped).
//
// Cost. add() is a handful of operations, except that every kEstimateEvery-th
// call (every 250 ms) also runs the tempo estimate, all at once: about 144 000
// multiply-adds for the autocorrelation (lags 15 to 365 over 600 frames),
// some 6 000 more for the comb, the support checks and the phase, and about
// 1 300 divisions. That one call is the worst case of the whole audio
// analysis; see audio_analyzer.h for what it means in time.
//
// Memory: 10 608 bytes, all of it in the object. All but the 600 onsets is
// work space of the estimate, which is too large for a task stack. Single-
// precision float, like the rest of jacket/audio: see audio_analyzer.h. No
// heap. Not thread-safe.
#pragma once

#include <stdint.h>

namespace jacket {

class BeatTracker {
public:
    static constexpr int kFrameRate = 100;

    // Tempos reported. The search goes a little further out (66 to 186 BPM)
    // so that both ends are proper peaks.
    static constexpr float kMinBpm = 70.0f;
    static constexpr float kMaxBpm = 180.0f;

    // Six seconds of onsets: enough for four beats at the slowest tempo,
    // short enough to follow a change.
    static constexpr int kHistory = 600;
    static constexpr int kMinHistory = 350;
    static constexpr int kEstimateEvery = 25;

    // A lock needs this much of the onset energy to repeat at the beat. Noise
    // stays under 0.1.
    static constexpr float kMinConfidence = 0.3f;

    BeatTracker() { reset(); }
    void reset();

    // The onset strength of the next frame.
    void add(float onset);

    // True while a beat is being tracked. When false, bpm() and phase() are
    // not to be used.
    bool valid() const { return valid_; }

    // How periodic the onsets are at the chosen tempo: about 1 for a
    // metronome, below 0.1 for noise.
    float confidence() const { return confidence_; }

    // The tracked beat period in frames, 0 when there has never been a lock
    // or it was forgotten.
    float periodFrames() const { return period_; }
    float bpm() const { return period_ > 0.0f ? kFrameRate * 60.0f / period_ : 0.0f; }

    // Where the latest frame lies in its beat, 0 until just under 1; 0 is the
    // beat itself. This is the phase of the onset stream: if onsets reach
    // add() late, the caller adds that delay.
    float phase() const { return oscillator_; }

    // ---- The search, public for the tests ----------------------------------------
    static constexpr float kTauMin = kFrameRate * 60.0f / 186.0f;  // candidate beat periods, in frames
    static constexpr float kTauMax = kFrameRate * 60.0f / 66.0f;
    static constexpr float kTauStep = 0.2f;
    static constexpr int kCandidates = static_cast<int>((kTauMax - kTauMin) / kTauStep) + 1;
    // The preference for candidate `index`: 1 at 125 BPM, falling off 1.2
    // octaves wide. It only decides near-ties.
    static float prior(int index);

private:
    static constexpr int kCombTeeth = 4;
    static constexpr int kMaxLag = static_cast<int>(kCombTeeth * kTauMax) + 1;
    static constexpr float kPhaseStep = 0.5f;  // frames
    static constexpr int kPhaseSlots = static_cast<int>(kTauMax / kPhaseStep) + 2;

    void estimate();
    void noSupport();
    float at(float lag) const;
    float comb(float tau) const;
    float support(float tau) const;
    float recentSupport(float tau, int n) const;
    int peakNear(int index) const;
    void trackPhase(int n, bool jump);

    // The onsets: the only part of the arrays that lasts from call to call.
    float ring_[kHistory];
    int16_t head_;
    int16_t filled_;
    int16_t since_estimate_;

    // Work space of estimate().
    float peaks_[kHistory];     // what stands out of its surroundings, oldest first
    float work_[kHistory + 1];  // first the running sum of the onsets, then the smoothed, centred peaks
    float acf_[kMaxLag + 2];
    float scores_[kCandidates];
    float phase_scores_[kPhaseSlots];
    int16_t max_lag_;

    float period_;
    float oscillator_;
    float pending_period_;
    uint8_t pending_count_;
    uint8_t unsupported_;
    uint8_t phase_misses_;
    bool valid_;
    float confidence_;
};

}  // namespace jacket
