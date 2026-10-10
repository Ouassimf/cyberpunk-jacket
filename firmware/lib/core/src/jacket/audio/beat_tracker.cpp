#include "jacket/audio/beat_tracker.h"

#include <math.h>

#include "jacket/audio/const_math.h"

namespace jacket {

namespace {

constexpr int kMinLag = 15;
constexpr int kMinOverlap = 150;
constexpr int kMinTeeth = 3;

constexpr int kConfirmations = 3;
constexpr int kDropAfter = 6;
constexpr int kForgetAfter = 40;
constexpr float kPeriodGain = 0.5f;
constexpr float kStickiness = 1.1f;
constexpr float kNear = 0.04f;

constexpr float kHalveRatio = 0.93f;
constexpr float kHalveToKeep = 0.85f;
constexpr float kHalveToSwitch = 1.0f;
constexpr int kHalfSearch = 3;

constexpr float kPhaseMemory = 250.0f;
constexpr float kPhaseGain = 0.5f;
constexpr float kPhasePullRange = 0.15f;

constexpr int kLocalMean = 40;
constexpr int kRecent = 200;
constexpr int kRecentSpan = 250;
constexpr int kRecentTeeth = 3;
constexpr float kRecentShare = 0.2f;
constexpr float kSilence = 1e-9f;

constexpr int kCandidates = BeatTracker::kCandidates;
constexpr float kTauMin = BeatTracker::kTauMin;
constexpr float kTauStep = BeatTracker::kTauStep;

// A gentle preference for tempos around 125 BPM, 1.2 octaves wide. Worked out
// by the compiler, with the app's arithmetic, and kept in flash.
struct Prior {
    float weight[kCandidates];
    constexpr Prior() : weight() {
        for (int i = 0; i < kCandidates; ++i) {
            const float bpm = BeatTracker::kFrameRate * 60.0f / (kTauMin + static_cast<float>(i) * kTauStep);
            const float octaves = static_cast<float>(constmath::ln(bpm / 125.0f)) / static_cast<float>(constmath::ln(2.0));
            const float widths = octaves / 1.2f;
            weight[i] = static_cast<float>(constmath::exp(-0.5f * widths * widths));
        }
    }
};
constexpr Prior kPrior;

bool isNear(float a, float b) { return fabsf(a - b) <= kNear * b; }

// Offset, in steps, of the vertex of the parabola through three equally
// spaced values around a maximum.
float vertex(float before, float at, float after) {
    const float curve = before - 2.0f * at + after;
    if (curve >= 0.0f) return 0.0f;
    const float offset = 0.5f * (before - after) / curve;
    return offset < -0.5f ? -0.5f : offset > 0.5f ? 0.5f : offset;
}

}  // namespace

float BeatTracker::prior(int index) { return kPrior.weight[index]; }

void BeatTracker::reset() {
    for (float& value : ring_) value = 0.0f;
    head_ = 0;
    filled_ = 0;
    since_estimate_ = 0;
    max_lag_ = 0;
    period_ = 0.0f;
    oscillator_ = 0.0f;
    pending_period_ = 0.0f;
    pending_count_ = 0;
    unsupported_ = 0;
    phase_misses_ = 0;
    valid_ = false;
    confidence_ = 0.0f;
}

void BeatTracker::add(float onset) {
    ring_[head_] = onset;
    if (++head_ == kHistory) head_ = 0;
    if (filled_ < kHistory) ++filled_;
    if (period_ > 0.0f) {
        oscillator_ += 1.0f / period_;
        if (oscillator_ >= 1.0f) oscillator_ -= 1.0f;
    }
    if (++since_estimate_ >= kEstimateEvery) {
        since_estimate_ = 0;
        if (filled_ >= kMinHistory) estimate();
    }
}

void BeatTracker::estimate() {
    const int n = filled_;
    int oldest = head_ - n;
    if (oldest < 0) oldest += kHistory;

    // Keep what stands out of its surroundings: each value less the mean of
    // the 0.8 s around it, negatives dropped. A level that slowly rises or
    // falls would otherwise correlate with itself at every lag and pass for a
    // beat of any tempo.
    float* const prefix = work_;  // prefix[i] is the sum of the i oldest onsets
    prefix[0] = 0.0f;
    for (int i = 0, index = oldest; i < n; ++i) {
        prefix[i + 1] = prefix[i] + ring_[index];
        if (++index == kHistory) index = 0;
    }
    float peak_sum = 0.0f;
    for (int i = 0, index = oldest; i < n; ++i) {
        const int from = i > kLocalMean ? i - kLocalMean : 0;
        const int to = i + kLocalMean + 1 < n ? i + kLocalMean + 1 : n;
        float peak = ring_[index] - (prefix[to] - prefix[from]) / static_cast<float>(to - from);
        if (!(peak > 0.0f)) peak = 0.0f;
        peaks_[i] = peak;
        peak_sum += peak;
        if (++index == kHistory) index = 0;
    }
    const float peak_mean = peak_sum / static_cast<float>(n);

    // Smoothed over five frames (1 2 3 2 1) for the autocorrelation: its
    // peaks become wide enough to read between whole lags, and a drummer who
    // is a frame early or late still counts.
    // The running sum is done with; its array now holds these values.
    float* const centred = work_;
    float power = 0.0f;
    for (int i = 0; i < n; ++i) {
        const int before2 = i >= 2 ? i - 2 : 0;
        const int before1 = i >= 1 ? i - 1 : 0;
        const int after1 = i + 1 < n ? i + 1 : n - 1;
        const int after2 = i + 2 < n ? i + 2 : n - 1;
        const float c = (peaks_[before2] + peaks_[after2] + 2.0f * (peaks_[before1] + peaks_[after1]) + 3.0f * peaks_[i]) *
                            (1.0f / 9.0f) -
                        peak_mean;
        centred[i] = c;
        power += c * c;
    }
    power /= static_cast<float>(n);
    float recent = 0.0f;
    for (int i = n - kRecent; i < n; ++i) recent += peaks_[i];
    recent /= static_cast<float>(kRecent);

    // Silence, or the music stopped: the six seconds still hold a beat, but
    // nobody is playing it any more. Judged by what stands out, like
    // everything else here: a background that merely got quieter is not it.
    if (power < kSilence || recent < kRecentShare * peak_mean) {
        confidence_ = 0.0f;
        pending_count_ = 0;
        noSupport();
        valid_ = false;
        return;
    }

    // This loop is nearly all of the cost of an estimate.
    max_lag_ = static_cast<int16_t>(kMaxLag < n - kMinOverlap ? kMaxLag : n - kMinOverlap);
    for (int lag = kMinLag; lag <= max_lag_ + 1; ++lag) {
        float s = 0.0f;
        for (int i = lag; i < n; ++i) s += centred[i] * centred[i - lag];
        acf_[lag] = s / (static_cast<float>(n - lag) * power);  // unbiased, so a long lag is not at a disadvantage
    }

    int best = 0;
    float best_weighted = 0.0f;
    float score_sum = 0.0f;
    for (int i = 0; i < kCandidates; ++i) {
        const float tau = kTauMin + static_cast<float>(i) * kTauStep;
        const float score = comb(tau);
        scores_[i] = score;
        score_sum += score;
        float weighted = score * kPrior.weight[i];
        if (period_ > 0.0f && isNear(tau, period_)) weighted *= kStickiness;
        if (i == 0 || weighted > best_weighted) {
            best_weighted = weighted;
            best = i;
        }
    }

    // The fastest level that repeats as well wins. With hysteresis, so a lock
    // does not flip between the two.
    int chosen = best;
    const float best_tau = kTauMin + static_cast<float>(best) * kTauStep;
    const float half_tau = best_tau / 2.0f;
    if (half_tau >= kTauMin) {
        const int half = peakNear(static_cast<int>((half_tau - kTauMin) / kTauStep + 0.5f));
        const float h = kTauMin + static_cast<float>(half) * kTauStep;
        float needed = kHalveRatio;
        if (period_ > 0.0f && isNear(h, period_)) {
            needed = kHalveToKeep;
        } else if (period_ > 0.0f && isNear(best_tau, period_)) {
            needed = kHalveToSwitch;
        }
        // The pulses in between show at odd multiples of the half period, the
        // beats themselves at even ones.
        const bool four_fit = 4.0f * h <= static_cast<float>(max_lag_);
        const float between = four_fit ? at(h) + at(3.0f * h) : at(h);
        const float on_beat = four_fit ? at(2.0f * h) + at(4.0f * h) : at(2.0f * h);
        if (between >= needed * on_beat) chosen = half;
    }

    float tau = kTauMin + static_cast<float>(chosen) * kTauStep;
    const bool is_peak = chosen >= 1 && chosen < kCandidates - 1 && scores_[chosen] >= scores_[chosen - 1] &&
                         scores_[chosen] >= scores_[chosen + 1];
    if (is_peak) tau += vertex(scores_[chosen - 1], scores_[chosen], scores_[chosen + 1]) * kTauStep;
    // Both must hold: the beat repeats across the whole history, and the last
    // seconds still belong to it. And it must do so better than a period
    // picked blindly would; the end of the range is not a tempo.
    if (is_peak) {
        const float whole = support(tau);
        const float lately = recentSupport(tau, n);
        const float blind = score_sum / static_cast<float>(kCandidates);
        confidence_ = (whole < lately ? whole : lately) - (blind > 0.0f ? blind : 0.0f);
    } else {
        confidence_ = 0.0f;
    }

    bool relocked = false;
    if (confidence_ >= kMinConfidence && period_ > 0.0f && isNear(tau, period_)) {
        period_ += kPeriodGain * (tau - period_);
        unsupported_ = 0;
        pending_count_ = 0;
    } else if (confidence_ >= kMinConfidence) {
        pending_count_ = pending_count_ > 0 && isNear(tau, pending_period_) ? static_cast<uint8_t>(pending_count_ + 1) : 1;
        pending_period_ = tau;
        if (pending_count_ >= kConfirmations) {
            period_ = tau;
            pending_count_ = 0;
            unsupported_ = 0;
            relocked = true;
        } else {
            noSupport();
        }
    } else {
        pending_count_ = 0;
        noSupport();
    }

    if (period_ > 0.0f && unsupported_ == 0) {
        trackPhase(n, relocked);
        valid_ = true;
    }
}

// One more estimate that did not back the current lock: after a while it stops
// counting, later it is forgotten.
void BeatTracker::noSupport() {
    if (period_ == 0.0f) {
        valid_ = false;
        return;
    }
    ++unsupported_;
    if (unsupported_ >= kDropAfter) valid_ = false;
    if (unsupported_ >= kForgetAfter) {
        period_ = 0.0f;
        oscillator_ = 0.0f;
        unsupported_ = 0;
    }
}

// The autocorrelation at a lag between two whole ones.
float BeatTracker::at(float lag) const {
    const int i = static_cast<int>(lag);
    return acf_[i] + (acf_[i + 1] - acf_[i]) * (lag - static_cast<float>(i));
}

// Mean autocorrelation at the first kCombTeeth multiples of tau that the
// history is long enough for.
float BeatTracker::comb(float tau) const {
    const float limit = static_cast<float>(max_lag_);
    float total = 0.0f;
    int used = 0;
    for (float lag = tau; used < kCombTeeth && lag <= limit; lag += tau) {
        total += at(lag);
        ++used;
    }
    return used == 0 ? 0.0f : total / static_cast<float>(used);
}

// The comb again, without its best tooth: two sounds that happen to be one
// period apart make one tall tooth, a beat makes all of them. Zero until the
// history is long enough for three teeth.
float BeatTracker::support(float tau) const {
    const float limit = static_cast<float>(max_lag_);
    float total = 0.0f;
    float highest = 0.0f;
    int used = 0;
    for (float lag = tau; used < kCombTeeth && lag <= limit; lag += tau) {
        const float value = at(lag);
        total += value;
        if (used == 0 || value > highest) highest = value;
        ++used;
    }
    return used < kMinTeeth ? 0.0f : (total - highest) / static_cast<float>(used - 1);
}

// How well the last kRecentSpan frames match the frames one, two and three
// periods before them, as a correlation coefficient. When the music stops and
// room noise takes over, the whole history still looks periodic for seconds;
// this does not.
float BeatTracker::recentSupport(float tau, int n) const {
    const float* const centred = work_;
    const int start = n - kRecentSpan;
    float total = 0.0f;
    int used = 0;
    for (float lag = tau; used < kRecentTeeth && static_cast<float>(start) - lag >= 1.0f; lag += tau) {
        const int whole = static_cast<int>(lag);
        const float part = lag - static_cast<float>(whole);
        float ab = 0.0f;
        float aa = 0.0f;
        float bb = 0.0f;
        for (int i = start; i < n; ++i) {
            const float a = centred[i];
            const float b = centred[i - whole] + (centred[i - whole - 1] - centred[i - whole]) * part;
            ab += a * b;
            aa += a * a;
            bb += b * b;
        }
        if (aa > 0.0f && bb > 0.0f) total += ab / sqrtf(aa * bb);
        ++used;
    }
    return used == 0 ? 0.0f : total / static_cast<float>(used);
}

int BeatTracker::peakNear(int index) const {
    int best = index < 0 ? 0 : index > kCandidates - 1 ? kCandidates - 1 : index;
    const int from = index - kHalfSearch > 0 ? index - kHalfSearch : 0;
    const int to = index + kHalfSearch < kCandidates - 1 ? index + kHalfSearch : kCandidates - 1;
    for (int i = from; i <= to; ++i) {
        if (scores_[i] > scores_[best]) best = i;
    }
    return best;
}

// Lines a pulse train of the locked period up with the onsets and pulls the
// oscillator towards it.
void BeatTracker::trackPhase(int n, bool jump) {
    const float tau = period_;
    const float last = static_cast<float>(n - 1);
    const float fade = expf(-tau / kPhaseMemory);
    int count = static_cast<int>(tau / kPhaseStep) + 1;
    if (count > kPhaseSlots) count = kPhaseSlots;
    int best = 0;
    for (int j = 0; j < count; ++j) {
        float position = last - static_cast<float>(j) * kPhaseStep;
        float weight = 1.0f;
        float s = 0.0f;
        while (position >= 0.0f) {
            const int i = static_cast<int>(position);
            const float next = i + 1 < n ? peaks_[i + 1] : peaks_[i];
            s += weight * (peaks_[i] + (next - peaks_[i]) * (position - static_cast<float>(i)));
            weight *= fade;
            position -= tau;
        }
        phase_scores_[j] = s;
        if (s > phase_scores_[best]) best = j;
    }
    const float before = phase_scores_[best == 0 ? count - 1 : best - 1];
    const float after = phase_scores_[best == count - 1 ? 0 : best + 1];
    const float since_beat = (static_cast<float>(best) + vertex(before, phase_scores_[best], after)) * kPhaseStep;
    float measured = since_beat / tau;
    measured -= floorf(measured);

    float error = measured - oscillator_;
    error -= floorf(error + 0.5f);  // the short way round
    if (jump) {
        oscillator_ = measured;
        phase_misses_ = 0;
    } else if (fabsf(error) <= kPhasePullRange) {
        oscillator_ += kPhaseGain * error;
        oscillator_ -= floorf(oscillator_);
        phase_misses_ = 0;
    } else if (++phase_misses_ >= kConfirmations) {
        // Far off: one such measurement is an accident (a fill, a clap),
        // several in a row are the truth.
        oscillator_ = measured;
        phase_misses_ = 0;
    }
}

}  // namespace jacket
