#include "jacket/audio/audio_analyzer.h"

#include <math.h>

#include "jacket/audio/const_math.h"

namespace jacket {

namespace {

constexpr int kWindow = RealFft::kSize;

// First FFT bin of each band; a bin is 31.25 Hz wide. Bin 0 and 1 are left
// out: DC and rumble.
constexpr uint16_t kBandStart[] = {2, 3, 4, 5, 7, 9, 12, 16, 22, 30, 41, 52, 65, 85, 110, 145, 195, 256};
constexpr int kBassBands = 5;  // bins 2 to 8: 47 to 265 Hz
constexpr int kMidBands = 7;   // bins 9 to 64: up to 2 kHz; the rest, up to 8 kHz, is treble

// Makes the summed bins the mean square of the signal (full-scale sine: 0.5,
// which is -3 dB).
constexpr float kPowerScale = 16.0f / (3.0f * kWindow * kWindow);

constexpr float kDbToLnPower = 0.2302585f;      // ln(10) / 10
constexpr float kDbToLnAmplitude = 0.1151293f;  // ln(10) / 20
constexpr float kLnToDb = 4.3429448f;           // 10 / ln(10)

// The lowest the level ceiling goes at neutral sensitivity, in dB below full
// scale: a quiet conversation.
constexpr float kCeilingFloor = -50.0f;
constexpr float kGainPerStep = 0.24f;  // 12 dB over 50 steps of sensitivity
constexpr float kPushShare = 0.5f;
constexpr float kCeilingAttack = 0.3f;  // per frame: a hit of a few frames takes it most of the way
constexpr uint8_t kCeilingHold = 150;   // frames
constexpr float kCeilingNear = 3.0f;    // dB
constexpr float kCeilingCreep = 0.02f;  // dB per frame: 2 dB a second
constexpr float kCeilingDecay = 0.08f;  // dB per frame: 8 dB a second
constexpr float kBandSpread = 24.0f;
// A ceiling that has not heard anything yet: far below any sound, so the
// first frame puts it on its floor (the app starts from minus infinity, to
// the same end).
constexpr float kNoCeiling = -1000.0f;

// Sound this far under the ceiling, as an amplitude ratio (30 dB), reads 0.
constexpr float kGate = 0.0316f;
constexpr float kOutputAttack = 1.0f;     // per frame: at once
constexpr float kOutputRelease = 0.283f;  // per frame: 30 ms

constexpr float kFluxRange = 42.0f;
constexpr float kFluxReferenceRate = 0.01f;
constexpr float kBassWeight = 2.0f;
constexpr float kFluxStatsRate = 0.01f;
constexpr float kOnsetDeviations = 3.5f;
constexpr float kOnsetMinimum = 1.5f;
constexpr uint32_t kOnsetWarmUp = 50;
constexpr float kFluxMinimum = 1.0f;
constexpr uint8_t kRefractory = 6;

// The Hann window, with the step from 16-bit samples to +-1.0 folded in (a
// power of two, so the products are the same to the last bit).
struct Hann {
    float value[kWindow];
    constexpr Hann() : value() {
        for (int i = 0; i < kWindow; ++i) {
            value[i] = static_cast<float>(0.5 - 0.5 * constmath::cosTurn(i, kWindow)) * (1.0f / 32768.0f);
        }
    }
};
constexpr Hann kHann;

float toDecibels(float power) { return logf(power + 1e-12f) * kLnToDb; }

uint8_t toByte(float value) {
    const int rounded = static_cast<int>(value * 255.0f + 0.5f);
    return static_cast<uint8_t>(rounded < 0 ? 0 : rounded > 255 ? 255 : rounded);
}

}  // namespace

AudioAnalyzer::AudioAnalyzer() : sensitivity_(50) { reset(); }

void AudioAnalyzer::setSensitivity(int percent) {
    sensitivity_ = static_cast<int8_t>(percent < 0 ? 0 : percent > 100 ? 100 : percent);
}

void AudioAnalyzer::reset() {
    for (int16_t& sample : samples_) sample = 0;
    write_ = 0;
    pending_ = 0;
    primed_ = false;
    flux_reference_ = 0.0f;
    flux_mean_ = 0.0f;
    flux_deviation_ = 0.0f;
    since_onset_ = kRefractory;
    for (int i = 0; i < 4; ++i) {
        ceiling_[i] = kNoCeiling;
        hold_[i] = 0;
        smoothed_[i] = 0.0f;
        level_[i] = 0;
    }
    onset_ = false;
    onsets_ = 0;
    frames_ = 0;
    tracker_.reset();
}

void AudioAnalyzer::process(const int16_t* pcm, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        samples_[write_] = pcm[i];
        write_ = static_cast<uint16_t>((write_ + 1) & (kWindow - 1));
        if (++pending_ == kHop) {
            pending_ = 0;
            analyse();
        }
    }
}

float AudioAnalyzer::beatPhase() const {
    const float period = tracker_.periodFrames();
    if (period <= 0.0f) return 0.0f;
    // The tracker's phase belongs to the onset stream, which sees a hit
    // kOnsetDelay frames after it sounded.
    const float phase = tracker_.phase() + (kOnsetDelay + static_cast<float>(pending_) / kHop) / period;
    return phase - floorf(phase);
}

void AudioAnalyzer::analyse() {
    // The window starts at the oldest sample, which is the one written next.
    const uint8_t* const slots = RealFft::slots();
    unsigned read = write_;
    for (int pair = 0; pair < RealFft::kBins; ++pair) {
        const int slot = slots[pair];
        re_[slot] = static_cast<float>(samples_[read]) * kHann.value[2 * pair];
        read = (read + 1) & (kWindow - 1);
        im_[slot] = static_cast<float>(samples_[read]) * kHann.value[2 * pair + 1];
        read = (read + 1) & (kWindow - 1);
    }
    RealFft::transform(re_, im_);

    const float gain = static_cast<float>(sensitivity_ - 50) * kGainPerStep;
    const float floor_db = kCeilingFloor - gain;
    // The onset floor follows the level ceiling slowly, so the ceiling's own
    // movements are not onsets.
    const float reference = ceiling_[0] > floor_db ? ceiling_[0] : floor_db;
    flux_reference_ = primed_ ? flux_reference_ + (reference - flux_reference_) * kFluxReferenceRate : reference;
    const float floor_power = expf((flux_reference_ - kFluxRange) * kDbToLnPower);

    static_assert(sizeof kBandStart / sizeof kBandStart[0] == kBands + 1, "one start per band, and the end");
    float bass_power = 0.0f;
    float mid_power = 0.0f;
    float treble_power = 0.0f;
    float flux = 0.0f;
    for (int band = 0; band < kBands; ++band) {
        float energy = 0.0f;
        for (int k = kBandStart[band]; k < kBandStart[band + 1]; ++k) energy += re_[k] * re_[k] + im_[k] * im_[k];
        energy *= kPowerScale;
        if (band < kBassBands) {
            bass_power += energy;
        } else if (band < kBassBands + kMidBands) {
            mid_power += energy;
        } else {
            treble_power += energy;
        }
        const float log = logf(energy + floor_power);
        if (primed_) {
            const float rise = log - band_log_[band];
            if (rise > 0.0f) flux += band < kBassBands ? kBassWeight * rise : rise;
        }
        band_log_[band] = log;
    }
    primed_ = true;

    const float decibels[4] = {toDecibels(bass_power + mid_power + treble_power), toDecibels(bass_power),
                               toDecibels(mid_power), toDecibels(treble_power)};
    for (int i = 0; i < 4; ++i) {
        const float value = decibels[i];
        float top = ceiling_[i];
        if (value > top) {
            top += (value - top) * kCeilingAttack;
            hold_[i] = kCeilingHold;
        } else if (value > top - kCeilingNear) {
            // As loud as it gets, again: this is still the level to measure
            // against. It only creeps down, so that it ends up exactly on a
            // sound that stays just under it.
            hold_[i] = kCeilingHold;
            top -= kCeilingCreep;
        } else if (hold_[i] > 0) {
            --hold_[i];
        } else {
            top -= kCeilingDecay;
        }
        float lowest = floor_db;
        if (i != 0 && ceiling_[0] - kBandSpread > floor_db) lowest = ceiling_[0] - kBandSpread;
        if (top < lowest) top = lowest;
        ceiling_[i] = top;

        const float amplitude = expf((value - top + gain * kPushShare) * kDbToLnAmplitude);
        float target = (amplitude - kGate) / (1.0f - kGate);
        if (target < 0.0f) target = 0.0f;
        if (target > 1.0f) target = 1.0f;
        smoothed_[i] += (target - smoothed_[i]) * (target > smoothed_[i] ? kOutputAttack : kOutputRelease);
        level_[i] = toByte(smoothed_[i]);
    }

    // An onset is a frame whose flux stands out from the last second or so,
    // at most one per 60 ms. Not in the first half second: until then there
    // is no "usual" to stand out from.
    if (since_onset_ < kRefractory) ++since_onset_;
    onset_ = frames_ >= kOnsetWarmUp && since_onset_ >= kRefractory &&
             flux > flux_mean_ + kOnsetDeviations * flux_deviation_ + kOnsetMinimum;
    if (onset_) {
        since_onset_ = 0;
        ++onsets_;
    }
    // A plain average at first, so it settles at once.
    const float settling = 1.0f / (static_cast<float>(frames_) + 1.0f);
    const float rate = settling > kFluxStatsRate ? settling : kFluxStatsRate;
    flux_deviation_ += (fabsf(flux - flux_mean_) - flux_deviation_) * rate;
    flux_mean_ += (flux - flux_mean_) * rate;

    // Anything smaller is arithmetic, not sound: the ripple a pure tone
    // leaves in the bands next to it repeats exactly, and the tracker, which
    // only looks at the shape, would call that a beat.
    tracker_.add(flux >= kFluxMinimum ? flux : 0.0f);
    ++frames_;
}

}  // namespace jacket
