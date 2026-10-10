// Synthetic sound for the audio tests: the same signals the Android app's
// analyser tests use (android/.../analysis/Signals.kt), sample for sample, so
// the C++ port is held to the same expectations on the same input.
//
// That takes the same random numbers too. KotlinRandom is kotlin.random.Random
// with a seed; test_audio checks it against values taken from the Kotlin
// library itself.
//
// Host only: double arithmetic and std::vector are fine here.
#pragma once

#include <cmath>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace audio_sim {

constexpr int kRate = 16000;  // AudioAnalyzer::kSampleRate

// kotlin.random.Random(seed): Marsaglia's xorwow, as the Kotlin standard
// library implements it on every platform.
class KotlinRandom {
public:
    explicit KotlinRandom(int32_t seed)
        : x_(static_cast<uint32_t>(seed)), y_(seed < 0 ? 0xFFFFFFFFu : 0u), z_(0), w_(0), v_(~x_), addend_((x_ << 10) ^ (y_ >> 4)) {
        for (int i = 0; i < 64; ++i) nextInt();
    }

    int32_t nextInt() {
        uint32_t t = x_;
        t ^= t >> 2;
        x_ = y_;
        y_ = z_;
        z_ = w_;
        const uint32_t v0 = v_;
        w_ = v0;
        t = (t ^ (t << 1)) ^ v0 ^ (v0 << 4);
        v_ = t;
        addend_ += 362437u;
        return static_cast<int32_t>(t + addend_);
    }

    // 0 until `until`, which must be positive.
    int32_t nextInt(int32_t until) {
        if ((until & -until) == until) {
            int bits = 0;
            while ((1 << bits) < until) ++bits;
            const uint32_t raw = static_cast<uint32_t>(nextInt());
            return bits == 0 ? 0 : static_cast<int32_t>(raw >> (32 - bits));
        }
        for (;;) {
            const int32_t bits = static_cast<int32_t>(static_cast<uint32_t>(nextInt()) >> 1);
            const int32_t value = bits % until;
            // Kotlin's test is "bits - value + (until - 1) < 0" in wrapping 32-bit arithmetic.
            if (static_cast<int64_t>(bits) - value + (until - 1) <= INT32_MAX) return value;
        }
    }

    double nextDouble() {  // [0, 1)
        const uint64_t high = nextBits(26);
        const uint64_t low = nextBits(27);
        return static_cast<double>((high << 27) + low) / 9007199254740992.0;
    }

    float nextFloat() { return static_cast<float>(nextBits(24)) / 16777216.0f; }  // [0, 1)

private:
    uint32_t nextBits(int count) { return static_cast<uint32_t>(nextInt()) >> (32 - count); }

    uint32_t x_, y_, z_, w_, v_, addend_;
};

// Kotlin's roundToInt(): to the nearest whole number, halves up.
inline int roundToInt(double value) { return static_cast<int>(std::floor(value + 0.5)); }

// What Mix::beat() takes besides the tempo; the defaults are the app's.
struct BeatOptions {
    double first = 0.2;      // seconds: the first kick
    double from = 0.0;       // only kicks from here ...
    double to = -1.0;        // ... to here (negative: the end)
    double amplitude = 0.5;
    double hats = 0.0;       // amplitude of a hat half-way between kicks, 0 for none
    double jitter = 0.0;     // seconds each hit may be early or late
};

// A mix is built in floats (1.0 is full scale) and rendered to PCM.
class Mix {
public:
    static constexpr double kPi = 3.141592653589793;

    explicit Mix(double length) : seconds(length), data(static_cast<size_t>(roundToInt(length * kRate)), 0.0f) {}

    const double seconds;
    std::vector<float> data;

    Mix& tone(double hz, double amplitude, double from = 0.0, double to = -1.0) {
        const int end = index(to < 0 ? seconds : to);
        for (int i = index(from); i < end; ++i) data[i] += static_cast<float>(amplitude * std::sin(2 * kPi * hz * i / kRate));
        return *this;
    }

    Mix& noise(double amplitude, KotlinRandom& random, double from = 0.0, double to = -1.0) {
        const int end = index(to < 0 ? seconds : to);
        for (int i = index(from); i < end; ++i) data[i] += static_cast<float>(amplitude * (random.nextDouble() * 2 - 1));
        return *this;
    }

    // A kick drum at `at` seconds: a sine sweeping down from 150 to 50 Hz
    // under a short click, about 150 ms long.
    Mix& kick(double at, double amplitude, KotlinRandom& random) {
        const double start = at * kRate;
        int i = static_cast<int>(std::ceil(start));
        if (i < 0) i = 0;
        double phase = 0.0;
        for (; i < static_cast<int>(data.size()); ++i) {
            const double t = (i - start) / kRate;
            if (t > 0.25) break;
            phase += 2 * kPi * (50 + 100 * std::exp(-t / 0.02)) / kRate;
            const double body = std::exp(-t / 0.05) * std::sin(phase);
            const double click = 0.5 * std::exp(-t / 0.003) * (random.nextDouble() * 2 - 1);
            data[i] += static_cast<float>(amplitude * (body + click));
        }
        return *this;
    }

    // A closed hi-hat at `at` seconds: 30 ms of bright noise.
    Mix& hat(double at, double amplitude, KotlinRandom& random) {
        const double start = at * kRate;
        int i = static_cast<int>(std::ceil(start));
        if (i < 0) i = 0;
        double last = 0.0;
        for (; i < static_cast<int>(data.size()); ++i) {
            const double t = (i - start) / kRate;
            if (t > 0.1) break;
            const double white = random.nextDouble() * 2 - 1;
            data[i] += static_cast<float>(amplitude * std::exp(-t / 0.012) * (white - last));  // differenced: mostly treble
            last = white;
        }
        return *this;
    }

    using Beat = BeatOptions;

    // Kicks on every beat of `bpm`, optionally with hats half-way between them.
    Mix& beat(double bpm, KotlinRandom& random, const Beat& b = Beat()) {
        const double to = b.to < 0 ? seconds : b.to;
        const double period = 60 / bpm;
        for (double at = b.first; at < to; at += period) {
            if (at < b.from) continue;
            // One random number per hit even without jitter, as in the app.
            const double kick_at = at + b.jitter * (random.nextDouble() * 2 - 1);
            kick(kick_at, b.amplitude, random);
            if (b.hats > 0 && at + period / 2 < to) {
                const double hat_at = at + period / 2 + b.jitter * (random.nextDouble() * 2 - 1);
                hat(hat_at, b.hats, random);
            }
        }
        return *this;
    }

    Mix& scale(double factor, double from = 0.0, double to = -1.0) {
        const int end = index(to < 0 ? seconds : to);
        for (int i = index(from); i < end; ++i) data[i] = static_cast<float>(static_cast<double>(data[i]) * factor);
        return *this;
    }

    std::vector<int16_t> pcm() const {
        std::vector<int16_t> out(data.size());
        for (size_t i = 0; i < data.size(); ++i) {
            const int value = roundToInt(static_cast<double>(data[i] * 32767.0f));
            out[i] = static_cast<int16_t>(value < -32768 ? -32768 : value > 32767 ? 32767 : value);
        }
        return out;
    }

private:
    int index(double at) const {
        const int i = roundToInt(at * kRate);
        const int size = static_cast<int>(data.size());
        return i < 0 ? 0 : i > size ? size : i;
    }
};

}  // namespace audio_sim
