// The audio analysis (jacket/audio) against sound made for the purpose, where
// the right answer is known exactly.
//
// These are the Android app's tests for the same code, ported with it:
// BeatTrackerTest.kt, AudioAnalyzerTest.kt and the frame part of
// LiveFrameBuilderTest.kt. The signals are the app's, sample for sample
// (support/audio_signals.h), and so are the expected values and tolerances:
// the C++ port has to hear what the Kotlin original hears. Where a test here
// has no counterpart in the app, its comment says so.
//
// What the tests state about the beat: within kConvergence seconds of a steady
// beat starting, the tempo is within kTempoTolerance BPM and the phase within
// kPhaseToleranceMs of the truth, and stays there. They say nothing about real
// music through a real microphone; that takes ears and a jacket.
//
// pio test -e native -f test_audio -v prints the sizes and the host timings.

#include <jacket.h>
#include <jacket/audio/audio_analyzer.h>
#include <jacket/audio/beat_tracker.h>
#include <jacket/audio/const_math.h>
#include <jacket/audio/fft.h>
#include <jacket/audio/mic_live.h>
#include <unity.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <stdint.h>
#include <stdio.h>
#include <vector>

#include "../support/audio_signals.h"

using namespace jacket;
using audio_sim::KotlinRandom;
using audio_sim::Mix;

// Counts what the program allocates with new, for the test that the analysis
// allocates nothing.
static size_t g_allocations = 0;
void* operator new(size_t size) {
    ++g_allocations;
    void* p = std::malloc(size != 0 ? size : 1);
    if (p == nullptr) std::abort();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

void setUp() {}
void tearDown() {}

namespace {

// Seconds from the first beat until tempo and phase are right, and from the
// last until the flag is off.
constexpr double kConvergence = 6.0;
constexpr double kTempoTolerance = 1.0;
constexpr double kPhaseToleranceMs = 12.0;
// For a beat played with 8 ms of looseness in every hit.
constexpr double kLoosePhaseToleranceMs = 20.0;

constexpr int kHop = AudioAnalyzer::kHop;
constexpr int kRate = AudioAnalyzer::kSampleRate;

// Fails with a message that says the numbers.
#define CHECK(condition, ...)                      \
    do {                                           \
        if (!(condition)) {                        \
            char m_[240];                          \
            snprintf(m_, sizeof m_, __VA_ARGS__);  \
            TEST_FAIL_MESSAGE(m_);                 \
        }                                          \
    } while (0)

#define CHECK_NEAR(expected, actual, tolerance, what)                                                    \
    CHECK(std::fabs(static_cast<double>(actual) - static_cast<double>(expected)) <= (tolerance),         \
          "%s: expected %.4f, was %.4f (tolerance %.4f)", what, static_cast<double>(expected),           \
          static_cast<double>(actual), static_cast<double>(tolerance))

// ================================================================================================
// The tools: random numbers, tables, FFT. No counterpart in the app.
// ================================================================================================

// The test signals are only the app's if the random numbers are Kotlin's.
// Expected values printed by kotlin.random.Random(seed) of kotlin-stdlib 2.2.10.
void test_random_numbers_are_kotlins() {
    struct Golden {
        int32_t seed;
        int32_t ints[3];
        uint64_t doubles[3];  // raw bits
        uint32_t floats[3];   // raw bits
        int32_t below_six[12];
        int32_t then;
    };
    const Golden golden[] = {
        {1,
         {600123930, -1531902544, -527218591},
         {4603829760600953426u, 4598468828331043702u, 4602940123382187957u},
         {1046957332u, 1050574182u, 1062219340u},
         {0, 5, 4, 3, 4, 2, 5, 5, 5, 4, 0, 1},
         821469387},
        {7,
         {-182312124, 11901178, -1941452650},
         {4600383045433856462u, 4604517863620343359u, 4605573456676814403u},
         {1058412492u, 1055216820u, 1051970006u},
         {0, 0, 3, 2, 3, 1, 2, 3, 5, 0, 4, 1},
         -620565081},
        {18,
         {1882206267, 1349293640, -212329233},
         {4605342224289009236u, 4602460902510462358u, 4605752545201294711u},
         {1041915812u, 1061647417u, 1064989412u},
         {5, 0, 5, 4, 0, 5, 4, 5, 2, 1, 4, 4},
         -1882380485},
        {-5,
         {1661948647, 2003040869, -413108419},
         {4606499341730668398u, 4595093949504410664u, 4604893037119993508u},
         {1062629185u, 1063512041u, 1065081021u},
         {1, 2, 2, 3, 2, 0, 0, 0, 4, 3, 3, 0},
         -1876407064},
    };
    for (const Golden& g : golden) {
        KotlinRandom random(g.seed);
        for (int32_t expected : g.ints) TEST_ASSERT_EQUAL_INT32(expected, random.nextInt());
        for (uint64_t expected : g.doubles) {
            const double value = random.nextDouble();
            uint64_t bits;
            memcpy(&bits, &value, sizeof bits);
            TEST_ASSERT_TRUE(expected == bits);
        }
        for (uint32_t expected : g.floats) {
            const float value = random.nextFloat();
            uint32_t bits;
            memcpy(&bits, &value, sizeof bits);
            TEST_ASSERT_EQUAL_UINT32(expected, bits);
        }
        for (int32_t expected : g.below_six) TEST_ASSERT_EQUAL_INT32(expected, random.nextInt(6));
        TEST_ASSERT_EQUAL_INT32(g.then, random.nextInt());
    }
}

// The tables in flash are made by the compiler with const_math.h; it must
// agree with the maths library to the last bits of a double (the angles below
// are themselves rounded, which is most of the tolerance).
void test_compile_time_maths() {
    for (int i = -600; i <= 600; ++i) {
        const double angle = 2 * Mix::kPi * i / 512;
        CHECK_NEAR(std::cos(angle), constmath::cosTurn(i, 512), 2e-15, "cos");
        CHECK_NEAR(std::sin(angle), constmath::sinTurn(i, 512), 2e-15, "sin");
    }
    TEST_ASSERT_TRUE(constmath::cosTurn(128, 512) == 0.0);
    TEST_ASSERT_TRUE(constmath::sinTurn(128, 512) == 1.0);
    for (double x = 0.01; x < 400; x *= 1.07) {
        CHECK_NEAR(1.0, constmath::ln(x) / std::log(x), x > 0.9 && x < 1.1 ? 1e-13 : 1e-15, "ln");
    }
    for (double x = -8; x < 8; x += 0.0625) CHECK_NEAR(1.0, constmath::exp(x) / std::exp(x), 1e-14, "exp");
    static_assert(constmath::ln(1.0) == 0.0 && constmath::exp(0.0) == 1.0, "usable in constant expressions");
}

// The tempo prior is such a table: BeatTracker.kt's PRIOR, with its arithmetic.
void test_tempo_prior_is_the_apps() {
    TEST_ASSERT_EQUAL(294, BeatTracker::kCandidates);
    float highest = 0;
    int at = 0;
    for (int i = 0; i < BeatTracker::kCandidates; ++i) {
        const float tau = BeatTracker::kTauMin + static_cast<float>(i) * BeatTracker::kTauStep;
        const float ratio = (BeatTracker::kFrameRate * 60.0f / tau) / 125.0f;
        const float octaves = static_cast<float>(std::log(static_cast<double>(ratio))) / static_cast<float>(std::log(2.0));
        const float expected = static_cast<float>(std::exp(static_cast<double>(-0.5f * (octaves / 1.2f) * (octaves / 1.2f))));
        CHECK_NEAR(expected, BeatTracker::prior(i), 1.2e-7, "prior");
        if (BeatTracker::prior(i) > highest) {
            highest = BeatTracker::prior(i);
            at = i;
        }
    }
    CHECK_NEAR(125.0, 6000.0 / (BeatTracker::kTauMin + at * BeatTracker::kTauStep), 0.3, "the favourite tempo");
    CHECK_NEAR(1.0, highest, 1e-4, "its weight");
    CHECK_NEAR(0.892, BeatTracker::prior(0), 0.01, "186 BPM is 0.57 octaves up");
}

// The half-size FFT against the definition of the transform, in double.
void test_fft_is_the_transform_of_its_input() {
    KotlinRandom random(21);
    for (int round = 0; round < 4; ++round) {
        double x[RealFft::kSize];
        for (int n = 0; n < RealFft::kSize; ++n) {
            switch (round) {
                case 0: x[n] = random.nextDouble() * 2 - 1; break;                         // noise
                case 1: x[n] = n == 37 ? 1.0 : 0.0; break;                                 // one click
                case 2: x[n] = std::sin(2 * Mix::kPi * 100.3 * n / 512) + 0.25; break;     // a tone between two bins, on DC
                default: x[n] = (n % 2 ? -1.0 : 1.0) * (0.5 + 0.5 * random.nextDouble());  // nearly all at the top bin
            }
        }
        float re[RealFft::kBins];
        float im[RealFft::kBins];
        const uint8_t* slots = RealFft::slots();
        bool used[RealFft::kBins] = {};
        for (int n = 0; n < RealFft::kBins; ++n) {
            TEST_ASSERT_FALSE(used[slots[n]]);  // every slot exactly once
            used[slots[n]] = true;
            re[slots[n]] = static_cast<float>(x[2 * n]);
            im[slots[n]] = static_cast<float>(x[2 * n + 1]);
        }
        RealFft::transform(re, im);

        double scale = 0;
        for (double v : x) scale += std::fabs(static_cast<double>(static_cast<float>(v)));
        for (int k = 0; k < RealFft::kBins; ++k) {
            double sum_re = 0;
            double sum_im = 0;
            for (int n = 0; n < RealFft::kSize; ++n) {
                const double angle = -2 * Mix::kPi * ((k * n) % RealFft::kSize) / RealFft::kSize;
                sum_re += static_cast<float>(x[n]) * std::cos(angle);
                sum_im += static_cast<float>(x[n]) * std::sin(angle);
            }
            // Float rounding only: a millionth of the largest value a bin can have.
            CHECK(std::fabs(re[k] - sum_re) <= 1e-6 * scale && std::fabs(im[k] - sum_im) <= 1e-6 * scale,
                  "round %d, bin %d: (%.6f, %.6f) instead of (%.6f, %.6f)", round, k, static_cast<double>(re[k]),
                  static_cast<double>(im[k]), sum_re, sum_im);
        }
    }
}

// What the headers say about memory, to the byte. The objects hold no
// pointers, so the sizes are the same on the ESP32.
void test_memory_is_what_the_headers_say() {
    printf("SIZE   BeatTracker %u bytes, AudioAnalyzer %u bytes, MicLive %u bytes\n", static_cast<unsigned>(sizeof(BeatTracker)),
           static_cast<unsigned>(sizeof(AudioAnalyzer)), static_cast<unsigned>(sizeof(MicLive)));
    TEST_ASSERT_EQUAL(10608, sizeof(BeatTracker));
    TEST_ASSERT_EQUAL(13816, sizeof(AudioAnalyzer));
    TEST_ASSERT_EQUAL(13828, sizeof(MicLive));
}

// ================================================================================================
// BeatTracker alone, on onset trains written by hand: one value per frame, 100 frames a second.
// (BeatTrackerTest.kt)
// ================================================================================================

// `seconds` of onsets; pulses are placed at fractional frame positions by
// splitting them over two frames.
struct Train {
    std::vector<float> frames;

    explicit Train(int seconds) : frames(static_cast<size_t>(seconds * BeatTracker::kFrameRate), 0.0f) {}
    int size() const { return static_cast<int>(frames.size()); }

    Train& pulse(double at, float strength = 1.0f) {
        const int i = static_cast<int>(std::floor(at));
        const float part = static_cast<float>(at - i);
        if (i >= 0 && i < size()) frames[i] += strength * (1 - part);
        if (i + 1 >= 0 && i + 1 < size()) frames[i + 1] += strength * part;
        return *this;
    }

    // Pulses every beat of `bpm` from frame `first`; `strengths` are used in turn.
    Train& beat(double bpm, double first = 12.3, int from = 0, int to = -1, std::initializer_list<float> strengths = {1.0f}) {
        if (to < 0) to = size();
        const double period = 6000 / bpm;
        int n = 0;
        for (double at = first; at < to; at += period, ++n) {
            if (at >= from) pulse(at, strengths.begin()[n % strengths.size()]);
        }
        return *this;
    }

    Train& noise(float amount, KotlinRandom& random) {
        for (float& frame : frames) frame += amount * random.nextFloat();
        return *this;
    }
};

template <typename Each>
void feed(BeatTracker& tracker, const Train& train, int from, int to, Each each) {
    for (int i = from; i < to; ++i) {
        tracker.add(train.frames[i]);
        each(i);
    }
}
void feed(BeatTracker& tracker, const Train& train) {
    feed(tracker, train, 0, train.size(), [](int) {});
}

// Phase error at the latest frame `frame`, in frames, against a beat of `bpm`
// that has a pulse at `first`.
double phaseError(const BeatTracker& tracker, int frame, double bpm, double first) {
    const double period = 6000 / bpm;
    const double truth = (frame - first) / period;
    double error = tracker.phase() - (truth - std::floor(truth));
    error -= std::floor(error + 0.5);
    return error * period;
}

void test_tracker_follows_a_metronome_at_every_tempo() {
    for (double bpm : {70.0, 77.7, 90.0, 100.0, 113.0, 120.0, 128.0, 140.0, 150.0, 163.5, 174.0, 180.0}) {
        BeatTracker tracker;
        const Train train = Train(20).beat(bpm);
        int locked_at = -1;
        feed(tracker, train, 0, train.size(), [&](int frame) {
            if (locked_at < 0 && tracker.valid()) {
                locked_at = frame;
                CHECK(std::fabs(phaseError(tracker, frame, bpm, 12.3)) < 1.0,
                      "%.1f BPM: phase off by %.3f frames when the beat is first announced", bpm,
                      phaseError(tracker, frame, bpm, 12.3));
            }
            if (frame >= 600) {
                CHECK(tracker.valid(), "%.1f BPM lost at frame %d", bpm, frame);
                CHECK_NEAR(bpm, tracker.bpm(), 0.5, "tempo");
                CHECK(std::fabs(phaseError(tracker, frame, bpm, 12.3)) < 0.5, "%.1f BPM: phase off by %.3f frames at %d", bpm,
                      phaseError(tracker, frame, bpm, 12.3), frame);
            }
        });
        CHECK(locked_at >= 349 && locked_at <= 550, "%.1f BPM locked at frame %d", bpm, locked_at);
        CHECK(tracker.confidence() > 0.7f, "%.1f BPM: confidence %.3f", bpm, static_cast<double>(tracker.confidence()));
    }
}

void test_tracker_phase_zero_is_the_beat_itself() {
    BeatTracker tracker;
    // Pulses on whole frames, every 50: at a pulse frame the phase must be 0
    // (or just under 1), half-way 0.5.
    const Train train = Train(12).beat(120.0, 20.0);
    std::vector<float> phases(train.frames.size());
    feed(tracker, train, 0, train.size(), [&](int frame) { phases[frame] = tracker.phase(); });
    auto distance = [](float a, float b) {
        const float d = std::fabs(a - b);
        return d < 1 - d ? d : 1 - d;
    };
    for (int pulse : {820, 870, 1120}) {
        CHECK(distance(phases[pulse], 0.0f) < 0.01f, "phase %.4f on the beat", static_cast<double>(phases[pulse]));
        CHECK(distance(phases[pulse + 25], 0.5f) < 0.01f, "phase %.4f half-way", static_cast<double>(phases[pulse + 25]));
        CHECK(distance(phases[pulse + 49], 0.98f) < 0.01f, "phase %.4f just before the next",
              static_cast<double>(phases[pulse + 49]));
    }
}

void test_tracker_claims_nothing_before_there_is_enough_to_go_on() {
    BeatTracker tracker;
    const Train train = Train(10).beat(120.0);
    feed(tracker, train, 0, 340, [&](int) {
        TEST_ASSERT_FALSE(tracker.valid());
        TEST_ASSERT_TRUE(tracker.bpm() == 0.0f);
    });
}

void test_tracker_equal_pulses_are_the_beat_weaker_ones_in_between_are_not() {
    // Every 34.5 frames, all the same: 174 BPM.
    BeatTracker equal;
    feed(equal, Train(15).beat(174.0));
    CHECK_NEAR(174.0, equal.bpm(), 0.5, "equal pulses");

    // The same pulses, every second one at 40 %: the strong ones are the
    // beat, 87 BPM, and phase 0 is on them.
    BeatTracker tracker;
    feed(tracker, Train(15).beat(174.0, 12.3, 0, -1, {1.0f, 0.4f}));
    TEST_ASSERT_TRUE(tracker.valid());
    CHECK_NEAR(87.0, tracker.bpm(), 0.5, "every second pulse weaker");
    CHECK(std::fabs(phaseError(tracker, 1499, 87.0, 12.3)) < 0.5, "phase off by %.3f frames",
          phaseError(tracker, 1499, 87.0, 12.3));

    // Nearly as strong (90 %) counts as equal.
    BeatTracker nearly;
    feed(nearly, Train(15).beat(174.0, 12.3, 0, -1, {1.0f, 0.9f}));
    CHECK_NEAR(174.0, nearly.bpm(), 0.5, "nearly equal pulses");
}

void test_tracker_does_not_double_a_slow_beat_with_weak_off_beats() {
    // At either end of the range.
    for (double bpm : {72.0, 90.0}) {
        BeatTracker tracker;
        feed(tracker, Train(15).beat(bpm).beat(bpm, 12.3 + 3000 / bpm, 0, -1, {0.35f}));
        CHECK_NEAR(bpm, tracker.bpm(), 0.5, "tempo");
    }
}

void test_tracker_gives_tempos_outside_the_range_as_half_or_double() {
    BeatTracker fast;
    feed(fast, Train(15).beat(220.0));
    TEST_ASSERT_TRUE(fast.valid());
    CHECK_NEAR(110.0, fast.bpm(), 0.5, "220 BPM");
    BeatTracker slow;
    feed(slow, Train(15).beat(55.0));
    TEST_ASSERT_TRUE(slow.valid());
    CHECK_NEAR(110.0, slow.bpm(), 0.5, "55 BPM");
}

void test_tracker_keeps_the_beat_in_a_three_against_two_pattern() {
    // Beats at 120 with a pulse on every third one twice as strong: still
    // 120, not 80 or 40.
    BeatTracker tracker;
    feed(tracker, Train(15).beat(120.0, 12.3, 0, -1, {2.0f, 1.0f, 1.0f}));
    CHECK_NEAR(120.0, tracker.bpm(), 0.5, "tempo");
}

void test_tracker_holds_a_lock_through_small_noise_and_loose_timing() {
    KotlinRandom random(1);
    Train train = Train(30).noise(0.15f, random);
    for (double at = 9.0; at < 3000; at += 6000 / 126.0) {
        const double where = at + random.nextDouble() * 1.6 - 0.8;
        const float strength = 0.7f + 0.6f * random.nextFloat();
        train.pulse(where, strength);
    }
    BeatTracker tracker;
    feed(tracker, train, 0, train.size(), [&](int frame) {
        if (frame >= 600) {
            CHECK(tracker.valid(), "lost at frame %d", frame);
            CHECK_NEAR(126.0, tracker.bpm(), 1.0, "tempo");
            CHECK(std::fabs(phaseError(tracker, frame, 126.0, 9.0)) < 1.5, "phase off by %.3f frames at %d",
                  phaseError(tracker, frame, 126.0, 9.0), frame);
        }
    });
}

void test_tracker_follows_a_tempo_that_drifts_all_the_way() {
    // A band that speeds up from 120 to 127 BPM over twenty seconds, never by
    // enough at once to be a new tempo.
    std::vector<double> beats;
    for (double at = 10.0; at < 4000;) {
        beats.push_back(at);
        const double progress = (at - 1000) / 2000;
        const double bpm = 120 + 7 * (progress < 0 ? 0.0 : progress > 1 ? 1.0 : progress);
        at += 6000 / bpm;
    }
    Train train(40);
    for (double beat : beats) train.pulse(beat);
    BeatTracker tracker;
    feed(tracker, train, 0, train.size(), [&](int frame) {
        if (frame < 600) return;
        CHECK(tracker.valid(), "lost at frame %d", frame);
        size_t next = 0;
        while (next < beats.size() && !(beats[next] > frame)) ++next;
        if (next == 0 || next == beats.size()) return;
        const double period = beats[next] - beats[next - 1];
        // The tempo heard over the last six seconds is that of about three
        // seconds ago: at most 1.1 BPM behind.
        CHECK_NEAR(6000 / period, tracker.bpm(), 1.5, "tempo");
        double error = tracker.phase() - (frame - beats[next - 1]) / period;
        error -= std::floor(error + 0.5);
        // While the tempo keeps rising the oscillator trails the beat a
        // little: under 20 ms.
        CHECK(std::fabs(error * period) < 2.0, "phase off by %.3f frames at %d", error * period, frame);
    });
    CHECK_NEAR(127.0, tracker.bpm(), 0.5, "the tempo it ends on");
}

void test_tracker_finds_a_beat_on_top_of_a_level_that_swells_and_sinks() {
    // Onsets riding on a busy, slowly changing background five times their
    // size: only what stands out of its surroundings may count, or the
    // background's own shape drowns the beat.
    KotlinRandom random(6);
    Train train = Train(30).beat(120.0).noise(0.2f, random);
    for (int i = 0; i < train.size(); ++i) {
        train.frames[i] += 2.5f + 2.5f * static_cast<float>(std::sin(static_cast<double>(static_cast<float>(i) / 100.0f)));
    }
    BeatTracker tracker;
    feed(tracker, train, 0, train.size(), [&](int frame) {
        if (frame >= 600) {
            CHECK(tracker.valid(), "lost at frame %d", frame);
            CHECK_NEAR(120.0, tracker.bpm(), 0.5, "tempo");
            CHECK(std::fabs(phaseError(tracker, frame, 120.0, 12.3)) < 1.0, "phase off by %.3f frames at %d",
                  phaseError(tracker, frame, 120.0, 12.3), frame);
        }
    });
}

void test_tracker_drops_the_beat_within_three_seconds_when_noise_takes_over() {
    // The music stops and something loud but shapeless goes on: the six
    // seconds of history still hold more beat than noise for a while. The
    // last seconds on their own do not.
    KotlinRandom random(7);
    Train train = Train(30).beat(120.0, 12.3, 0, 1500);
    for (int i = 1500; i < train.size(); ++i) train.frames[i] = random.nextFloat();
    BeatTracker tracker;
    int last_valid = 0;
    feed(tracker, train, 0, train.size(), [&](int frame) {
        if (tracker.valid()) last_valid = frame;
    });
    CHECK(last_valid >= 1500 && last_valid <= 1800, "valid until frame %d", last_valid);
}

void test_tracker_replaces_the_old_tempo_and_shows_no_third_on_the_way() {
    const Train train = Train(40).beat(100.0, 12.3, 0, 2000).beat(150.0, 2004.0, 2000);
    BeatTracker tracker;
    feed(tracker, train, 0, 2000, [](int) {});
    CHECK_NEAR(100.0, tracker.bpm(), 0.5, "the first tempo");
    int switched_at = -1;
    feed(tracker, train, 2000, train.size(), [&](int frame) {
        if (tracker.valid()) {
            CHECK(std::fabs(tracker.bpm() - 100) < 2 || std::fabs(tracker.bpm() - 150) < 2, "%.2f BPM at frame %d",
                  static_cast<double>(tracker.bpm()), frame);
        }
        if (switched_at < 0 && tracker.valid() && std::fabs(tracker.bpm() - 150) < 2) switched_at = frame;
        if (frame >= 2600) {
            TEST_ASSERT_TRUE(tracker.valid());
            CHECK_NEAR(150.0, tracker.bpm(), 0.5, "the second tempo");
            CHECK(std::fabs(phaseError(tracker, frame, 150.0, 2004.0)) < 0.5, "phase off by %.3f frames at %d",
                  phaseError(tracker, frame, 150.0, 2004.0), frame);
        }
    });
    CHECK(switched_at >= 2200 && switched_at <= 2600, "switched at frame %d", switched_at);
}

void test_tracker_follows_a_jump_in_phase_after_a_moment_not_at_once() {
    // The same tempo, but from frame 1500 on the beats come 20 frames later
    // (40 % of a beat).
    const Train train = Train(30).beat(120.0, 10.0, 0, 1500).beat(120.0, 1530.0, 1500);
    BeatTracker tracker;
    feed(tracker, train, 0, 1500, [](int) {});
    TEST_ASSERT_TRUE(std::fabs(phaseError(tracker, 1499, 120.0, 10.0)) < 0.5);
    int followed_at = -1;
    feed(tracker, train, 1500, train.size(), [&](int frame) {
        if (followed_at < 0 && tracker.valid() && std::fabs(phaseError(tracker, frame, 120.0, 1530.0)) < 1.0) followed_at = frame;
        if (frame >= 2100) {
            CHECK(tracker.valid() && std::fabs(phaseError(tracker, frame, 120.0, 1530.0)) < 0.5, "phase at frame %d", frame);
        }
    });
    CHECK(followed_at >= 1600 && followed_at <= 2100, "followed at frame %d", followed_at);
}

void test_tracker_loses_the_beat_when_the_pulses_stop_and_finds_it_again() {
    const Train train = Train(30).beat(120.0, 12.3, 0, 1000).beat(120.0, 2012.3, 2000);
    BeatTracker tracker;
    int last_valid = 0;
    feed(tracker, train, 0, 2000, [&](int frame) {
        if (tracker.valid()) last_valid = frame;
    });
    CHECK(last_valid >= 1000 && last_valid <= 1300, "valid until frame %d", last_valid);
    TEST_ASSERT_FALSE(tracker.valid());
    CHECK_NEAR(120.0, tracker.bpm(), 0.5, "the tempo is remembered for a while, for a quicker lock");

    int back_at = -1;
    feed(tracker, train, 2000, train.size(), [&](int frame) {
        if (back_at < 0 && tracker.valid()) back_at = frame;
    });
    CHECK(back_at >= 2000 && back_at <= 2500, "back at frame %d", back_at);
}

void test_tracker_forgets_a_beat_that_stays_away() {
    BeatTracker tracker;
    feed(tracker, Train(30).beat(120.0, 12.3, 0, 1000));
    TEST_ASSERT_FALSE(tracker.valid());
    TEST_ASSERT_TRUE(tracker.bpm() == 0.0f);
    TEST_ASSERT_TRUE(tracker.periodFrames() == 0.0f);
}

void assertTrainHasNoBeat(const char* name, const Train& train) {
    BeatTracker tracker;
    float highest = 0;
    feed(tracker, train, 0, train.size(), [&](int frame) {
        CHECK(!tracker.valid(), "%s got %.2f BPM at frame %d", name, static_cast<double>(tracker.bpm()), frame);
        if (tracker.confidence() > highest) highest = tracker.confidence();
    });
    CHECK(highest < BeatTracker::kMinConfidence, "%s: confidence %.3f", name, static_cast<double>(highest));
}

void test_tracker_finds_no_beat_in_silence_a_constant_noise_or_a_ramp() {
    assertTrainHasNoBeat("silence", Train(20));
    Train constant(20);
    for (float& frame : constant.frames) frame = 3.0f;
    assertTrainHasNoBeat("a constant", constant);
    KotlinRandom noise(2);
    assertTrainHasNoBeat("noise", Train(40).noise(1.0f, noise));
    // A level that only rises correlates with itself at every lag; it must
    // not pass for a beat.
    KotlinRandom rough(3);
    Train ramp = Train(40).noise(0.3f, rough);
    for (int i = 0; i < ramp.size(); ++i) ramp.frames[i] += static_cast<float>(i) * 0.01f;
    assertTrainHasNoBeat("a ramp", ramp);
    Train restarting(40);
    for (int i = 0; i < restarting.size(); ++i) restarting.frames[i] = static_cast<float>(i % 700) * 0.01f;
    assertTrainHasNoBeat("a ramp that restarts", restarting);
}

void test_tracker_almost_never_takes_random_hits_for_a_beat() {
    // Random hits do line up by accident now and then, and for a second or
    // two that is a beat to anyone's ear. What can be asked is that it is
    // rare: here, under a third of a percent of the time over 100 runs of 40 s.
    int valid = 0;
    int total = 0;
    for (int seed = 1; seed <= 100; ++seed) {
        KotlinRandom random(seed);
        Train train(40);
        for (double at = 5.0; at < 4000; at += 15 + random.nextDouble() * 90) train.pulse(at);  // 0.15 to 1.05 s apart
        BeatTracker tracker;
        feed(tracker, train, 0, train.size(), [&](int) {
            ++total;
            if (tracker.valid()) ++valid;
        });
    }
    CHECK(valid < total / 300, "a beat was claimed in %d of %d frames", valid, total);
}

void test_tracker_takes_roughly_regular_hits_for_a_beat() {
    // Every 0.4 to 0.5 s, at random: sloppy, but a pulse of about 133 BPM all
    // the same.
    KotlinRandom random(4);
    Train train(40);
    for (double at = 5.0; at < 4000; at += 40 + random.nextDouble() * 10) train.pulse(at);
    BeatTracker tracker;
    int valid = 0;
    feed(tracker, train, 0, train.size(), [&](int) {
        if (tracker.valid()) {
            ++valid;
            CHECK_NEAR(133.0, tracker.bpm(), 12.0, "tempo");
        }
    });
    CHECK(valid > 1000, "valid in %d of 4000 frames", valid);
}

void test_tracker_reset_forgets_the_beat() {
    BeatTracker tracker;
    feed(tracker, Train(10).beat(120.0));
    TEST_ASSERT_TRUE(tracker.valid());
    tracker.reset();
    TEST_ASSERT_FALSE(tracker.valid());
    TEST_ASSERT_TRUE(tracker.bpm() == 0.0f);
    TEST_ASSERT_TRUE(tracker.phase() == 0.0f);
    feed(tracker, Train(3).beat(120.0));
    TEST_ASSERT_FALSE_MESSAGE(tracker.valid(), "three seconds are not enough again");
}

// ================================================================================================
// AudioAnalyzer (AudioAnalyzerTest.kt)
// ================================================================================================

// Feeds `pcm` in blocks of `block` samples, the way a microphone would,
// calling `after` with the time so far.
template <typename After>
void feed(AudioAnalyzer& analyzer, const std::vector<int16_t>& pcm, size_t block, After after) {
    size_t offset = 0;
    while (offset < pcm.size()) {
        const size_t count = std::min(block, pcm.size() - offset);
        analyzer.process(pcm.data() + offset, count);
        offset += count;
        after(static_cast<double>(offset) / kRate);
    }
}
void feed(AudioAnalyzer& analyzer, const std::vector<int16_t>& pcm, size_t block = 320) {
    feed(analyzer, pcm, block, [](double) {});
}

std::vector<int16_t> part(const std::vector<int16_t>& pcm, size_t from, size_t to) {
    return std::vector<int16_t>(pcm.begin() + static_cast<std::ptrdiff_t>(from), pcm.begin() + static_cast<std::ptrdiff_t>(to));
}

// ---- levels ----------------------------------------------------------------------------------

void test_silence_is_level_zero_with_no_onset_and_no_beat() {
    AudioAnalyzer analyzer;
    feed(analyzer, Mix(10.0).pcm(), 320, [&](double) {
        TEST_ASSERT_EQUAL(0, analyzer.level());
        TEST_ASSERT_EQUAL(0, analyzer.bass());
        TEST_ASSERT_EQUAL(0, analyzer.mid());
        TEST_ASSERT_EQUAL(0, analyzer.treble());
        TEST_ASSERT_FALSE(analyzer.beatValid());
    });
    TEST_ASSERT_TRUE(analyzer.bpm() == 0.0f);
    TEST_ASSERT_EQUAL_UINT32(0, analyzer.onsets());
    TEST_ASSERT_EQUAL_UINT32(1000, analyzer.frames());
}

void test_the_hiss_of_a_quiet_room_stays_dark() {
    // About -75 dB below full scale: what a microphone gives in a silent room.
    AudioAnalyzer analyzer;
    KotlinRandom random(1);
    int highest = 0;
    feed(analyzer, Mix(20.0).noise(0.0003, random).pcm(), 320, [&](double) {
        highest = std::max({highest, static_cast<int>(analyzer.level()), static_cast<int>(analyzer.bass()),
                            static_cast<int>(analyzer.mid()), static_cast<int>(analyzer.treble())});
        TEST_ASSERT_FALSE(analyzer.beatValid());
    });
    CHECK(highest <= 8, "hiss read as %d", highest);
}

void test_a_steady_tone_lights_its_own_band_and_no_other() {
    struct Case {
        double hz;
        int band;
    };
    for (const Case& c : {Case{60.0, 0}, Case{100.0, 0}, Case{440.0, 1}, Case{1000.0, 1}, Case{1700.0, 1}, Case{3000.0, 2},
                          Case{6000.0, 2}}) {
        AudioAnalyzer analyzer;
        feed(analyzer, Mix(3.0).tone(c.hz, 0.1).pcm());
        CHECK(analyzer.level() >= 240, "level %d for %.0f Hz", analyzer.level(), c.hz);
        const int values[3] = {analyzer.bass(), analyzer.mid(), analyzer.treble()};
        for (int band = 0; band < 3; ++band) {
            CHECK(band == c.band ? values[band] >= 240 : values[band] <= 5, "%.0f Hz gave bass %d, mid %d, treble %d", c.hz,
                  values[0], values[1], values[2]);
        }
    }
}

void test_a_band_follows_its_own_sound_when_two_play_at_once() {
    // Bass steady, treble switched on half-way and 20 dB weaker: bass must
    // not move, treble must appear.
    AudioAnalyzer analyzer;
    const std::vector<int16_t> pcm = Mix(8.0).tone(90.0, 0.2).tone(5000.0, 0.02, 4.0).pcm();
    int treble_before = 0;
    feed(analyzer, pcm, 320, [&](double t) {
        if (t >= 3.0 && t <= 4.0) treble_before = std::max(treble_before, static_cast<int>(analyzer.treble()));
    });
    TEST_ASSERT_EQUAL(0, treble_before);
    CHECK(analyzer.bass() >= 240, "bass %d", analyzer.bass());
    CHECK(analyzer.treble() >= 200, "treble %d gets its own range although it is 20 dB weaker", analyzer.treble());
    CHECK(analyzer.mid() <= 5, "mid %d", analyzer.mid());
}

void test_a_hit_shows_at_once_and_is_over_when_the_sound_is() {
    // For the jacket to smooth as it likes.
    AudioAnalyzer analyzer;
    KotlinRandom random(2);
    // A click of a few milliseconds, then a kick (which itself rings for a
    // good 100 ms).
    Mix mix(6.0);
    mix.kick(4.0, 0.5, random);
    for (int i = 0; i < 48; ++i) mix.data[2 * kRate + i] += 0.5f * (random.nextFloat() * 2 - 1);
    std::vector<int> levels;
    feed(analyzer, mix.pcm(), kHop, [&](double) { levels.push_back(analyzer.level()); });
    auto at = [&](double seconds) { return levels[static_cast<size_t>(seconds * 100)]; };
    TEST_ASSERT_EQUAL(0, at(1.99));
    CHECK(at(2.02) >= 200, "20 ms after the click: %d", at(2.02));
    // The click is in the 32 ms analysis window for three frames; after that
    // only 30 ms of release are left.
    CHECK(at(2.10) <= 60, "100 ms after the click: %d", at(2.10));
    CHECK(at(2.20) <= 3, "200 ms after the click: %d", at(2.20));

    TEST_ASSERT_EQUAL(0, at(3.95));
    CHECK(at(4.03) >= 200, "30 ms after the kick: %d", at(4.03));
    CHECK(at(4.06) >= 60, "a kick is still sounding 60 ms on: %d", at(4.06));
    CHECK(at(4.4) <= 3, "and has rung out 400 ms on: %d", at(4.4));
}

void test_a_quiet_room_and_a_loud_club_both_use_the_whole_range() {
    for (double amplitude : {0.012, 0.9}) {
        KotlinRandom random(3);
        AudioAnalyzer analyzer;
        int highest = 0;
        int lowest = 255;
        int bass_highest = 0;
        Mix::Beat beat;
        beat.amplitude = amplitude;
        Mix mix(15.0);
        mix.beat(100.0, random, beat).noise(amplitude / 200, random);
        feed(analyzer, mix.pcm(), 320, [&](double t) {
            if (t >= 8.0) {
                highest = std::max(highest, static_cast<int>(analyzer.level()));
                lowest = std::min(lowest, static_cast<int>(analyzer.level()));
                bass_highest = std::max(bass_highest, static_cast<int>(analyzer.bass()));
            }
        });
        CHECK(highest >= 220, "peaks reach %d at amplitude %.3f", highest, amplitude);
        CHECK(lowest <= 60, "and fall to %d between hits", lowest);
        CHECK(bass_highest >= 220, "bass peaks reach %d", bass_highest);
    }
}

void test_a_hit_half_as_loud_reads_half_as_high() {
    // The jacket uses the values linearly, so they must be linear in what is
    // heard: kicks at full and at half amplitude in turn, once the gain has
    // settled on the loud ones.
    KotlinRandom random(18);
    Mix mix(16.0);
    mix.noise(0.0005, random);
    bool loud = true;
    for (double at = 0.5; at < 15.9; at += 0.5) {
        mix.kick(at, loud ? 0.5 : 0.25, random);
        loud = !loud;
    }
    std::vector<int> levels;
    AudioAnalyzer analyzer;
    feed(analyzer, mix.pcm(), kHop, [&](double) { levels.push_back(analyzer.level()); });
    auto peak = [&](double kick_at) {
        const size_t from = static_cast<size_t>(kick_at * 100);
        return *std::max_element(levels.begin() + static_cast<std::ptrdiff_t>(from),
                                 levels.begin() + static_cast<std::ptrdiff_t>(from + 15));
    };
    for (int second = 6; second <= 14; ++second) {
        const int full = peak(second + 0.5);
        const int half = peak(second + 1.0);
        CHECK(full >= 235, "a loud kick reads %d (second %d)", full, second);
        CHECK(half >= 105 && half <= 150, "one half as loud reads %d next to %d (second %d)", half, full, second);
    }
}

void test_the_gain_follows_a_20_db_change_within_a_few_seconds() {
    // Steady noise, ten times louder from 15 s to 25 s.
    KotlinRandom random(4);
    const std::vector<int16_t> pcm = Mix(40.0).noise(0.01, random).scale(10.0, 15.0, 25.0).pcm();
    AudioAnalyzer analyzer;
    std::vector<int> levels;
    feed(analyzer, pcm, kHop, [&](double) { levels.push_back(analyzer.level()); });
    auto lowest = [&](double from, double to) {
        return *std::min_element(levels.begin() + static_cast<int>(from * 100), levels.begin() + static_cast<int>(to * 100));
    };
    auto highest = [&](double from, double to) {
        return *std::max_element(levels.begin() + static_cast<int>(from * 100), levels.begin() + static_cast<int>(to * 100));
    };
    CHECK(lowest(5.0, 15.0) >= 225, "settled before the step: %d", lowest(5.0, 15.0));
    CHECK(lowest(15.0, 25.0) >= 225, "louder is still full, never more: %d", lowest(15.0, 25.0));
    CHECK(lowest(25.0, 26.5) <= 40, "20 dB down is nearly dark at first: %d", lowest(25.0, 26.5));
    CHECK(highest(25.3, 26.0) <= 120, "and is not back within a second: %d", highest(25.3, 26.0));
    CHECK(lowest(30.0, 40.0) >= 225, "but is within five: %d", lowest(30.0, 40.0));
}

void test_sensitivity_decides_how_quiet_a_sound_may_be() {
    // A soft tone at -58 dB: below the floor at neutral sensitivity, above it
    // at full.
    const std::vector<int16_t> pcm = Mix(5.0).tone(440.0, 0.0018).pcm();
    auto level = [&](int sensitivity) {
        AudioAnalyzer analyzer;
        analyzer.setSensitivity(sensitivity);
        feed(analyzer, pcm);
        return static_cast<int>(analyzer.level());
    };
    const int low = level(0);
    const int neutral = level(50);
    const int high = level(100);
    CHECK(low < neutral && neutral < high, "%d < %d < %d", low, neutral, high);
    CHECK(low <= 8, "least sensitive: %d", low);
    CHECK(neutral >= 40 && neutral <= 200, "neutral: %d", neutral);
    TEST_ASSERT_EQUAL(255, high);

    // Not in the app, where the value is clamped when used: here when set.
    AudioAnalyzer analyzer;
    TEST_ASSERT_EQUAL(50, analyzer.sensitivity());
    analyzer.setSensitivity(5000);
    TEST_ASSERT_EQUAL(100, analyzer.sensitivity());
    analyzer.setSensitivity(-1);
    TEST_ASSERT_EQUAL(0, analyzer.sensitivity());
    TEST_ASSERT_EQUAL(low, level(-1));
    TEST_ASSERT_EQUAL(high, level(5000));
}

// ---- onsets ----------------------------------------------------------------------------------

void test_every_kick_is_an_onset_and_nothing_else_is() {
    KotlinRandom random(5);
    AudioAnalyzer analyzer;
    std::vector<double> times;
    Mix::Beat beat;
    beat.first = 1.0;
    Mix mix(12.0);
    mix.beat(120.0, random, beat).noise(0.003, random);
    feed(analyzer, mix.pcm(), kHop, [&](double t) {
        if (analyzer.onset()) times.push_back(t);
    });
    CHECK(times.size() == 22, "one per kick: %d onsets", static_cast<int>(times.size()));
    for (size_t i = 0; i < times.size(); ++i) {
        const double kick = 1.0 + static_cast<double>(i) * 0.5;
        CHECK(times[i] - kick >= 0.0 && times[i] - kick <= 0.04, "onset %d at %.3f for the kick at %.3f", static_cast<int>(i),
              times[i], kick);
    }
    TEST_ASSERT_EQUAL_UINT32(22, analyzer.onsets());
}

// ---- tempo and phase -------------------------------------------------------------------------

// The analyser's beat phase at `seconds` against the truth, in milliseconds;
// positive means it runs ahead.
double phaseError(const AudioAnalyzer& analyzer, double seconds, double bpm, double first) {
    const double period = 60 / bpm;
    const double truth = (seconds - first) / period;
    double error = analyzer.beatPhase() - (truth - std::floor(truth));
    error -= std::floor(error + 0.5);
    return error * period * 1000;
}

struct Tracking {
    double locked_at = -1.0;
    double worst_tempo = 0.0;
    double worst_phase = 0.0;
};

// Feeds `pcm` and checks tempo and phase against `bpm` in every block from
// `from` seconds on.
Tracking track(AudioAnalyzer& analyzer, const std::vector<int16_t>& pcm, double bpm, double first, double from = kConvergence,
               double to = 1e300) {
    Tracking result;
    feed(analyzer, pcm, 320, [&](double t) {
        if (result.locked_at < 0 && analyzer.beatValid()) result.locked_at = t;
        if (t >= from && t <= to) {
            CHECK(analyzer.beatValid(), "no beat at %.2f s for %.1f BPM", t, bpm);
            result.worst_tempo = std::max(result.worst_tempo, std::fabs(analyzer.bpm() - bpm));
            result.worst_phase = std::max(result.worst_phase, std::fabs(phaseError(analyzer, t, bpm, first)));
            CHECK_NEAR(60.0 / analyzer.bpm(), analyzer.beatPeriod(), 1e-4, "seconds per beat");
        }
    });
    return result;
}
Tracking track(const std::vector<int16_t>& pcm, double bpm, double first, double from = kConvergence, double to = 1e300) {
    AudioAnalyzer analyzer;
    return track(analyzer, pcm, bpm, first, from, to);
}

void test_kick_tracks_are_followed_in_tempo_and_phase_across_the_range() {
    for (double bpm : {70.0, 90.0, 100.0, 120.0, 128.0, 140.0, 174.0, 180.0}) {
        for (int seed = 1; seed <= 2; ++seed) {
            KotlinRandom random(seed);
            Mix::Beat beat;
            beat.first = 0.1 + random.nextDouble() * 0.5;
            // Noise 30 dB under the kicks.
            Mix mix(20.0);
            mix.beat(bpm, random, beat).noise(0.016, random);
            const Tracking result = track(mix.pcm(), bpm, beat.first);
            CHECK(result.locked_at >= 3.0 && result.locked_at <= kConvergence, "%.0f BPM locked at %.2f s", bpm, result.locked_at);
            CHECK(result.worst_tempo <= kTempoTolerance, "%.0f BPM: tempo off by %.3f", bpm, result.worst_tempo);
            CHECK(result.worst_phase <= kPhaseToleranceMs, "%.0f BPM: phase off by %.2f ms", bpm, result.worst_phase);
        }
    }
}

void test_hats_between_the_kicks_do_not_double_the_tempo() {
    for (double bpm : {90.0, 128.0}) {
        KotlinRandom random(6);
        Mix::Beat beat;
        beat.first = 0.3;
        beat.hats = 0.12;
        Mix mix(20.0);
        mix.beat(bpm, random, beat).noise(0.01, random);
        const Tracking result = track(mix.pcm(), bpm, 0.3);
        CHECK(result.worst_tempo <= kTempoTolerance, "%.0f BPM with hats: tempo off by %.3f", bpm, result.worst_tempo);
        CHECK(result.worst_phase <= kPhaseToleranceMs, "%.0f BPM with hats: phase off by %.2f ms", bpm, result.worst_phase);
    }
}

void test_a_pulse_as_strong_between_the_beats_as_on_them_is_the_faster_tempo() {
    // Kicks every 345 ms are 174 BPM, not 87 with something in between:
    // nothing tells the two halves apart.
    KotlinRandom random(7);
    Mix mix(20.0);
    mix.beat(174.0, random).noise(0.01, random);
    const Tracking result = track(mix.pcm(), 174.0, 0.2);
    CHECK(result.worst_tempo <= kTempoTolerance, "tempo off by %.3f", result.worst_tempo);
}

void test_a_played_beat_with_loose_timing_snare_hats_and_bass_line_is_followed() {
    for (double bpm : {87.0, 100.0, 126.0}) {
        KotlinRandom random(8);
        Mix mix(30.0);
        mix.noise(0.02, random);
        const double period = 60 / bpm;
        auto loose = [&] { return 0.008 * (random.nextDouble() * 2 - 1); };  // a drummer within 8 ms
        int beat = 0;
        for (double t = 0.31; t < 30; t += period, ++beat) {
            // One statement per random number: they must be drawn in the app's order.
            if (beat % 2 == 0) {
                const double at = t + loose();
                const double amplitude = 0.5 * (0.8 + 0.4 * random.nextDouble());
                mix.kick(at, amplitude, random);
            } else {
                const double hat_at = t + loose();
                mix.hat(hat_at, 0.5, random);
                const double kick_at = t + loose();
                mix.kick(kick_at, 0.15, random);  // a snare, more or less
            }
            const double off_at = t + period / 2 + loose();
            const double off_amplitude = 0.12 * (0.5 + random.nextDouble());
            mix.hat(off_at, off_amplitude, random);
            if (beat % 4 == 3) {
                const double late_at = t + period * 0.75 + loose();
                mix.hat(late_at, 0.1, random);
            }
            mix.tone(55.0 * (1 + (beat / 4 % 3) * 0.25), 0.15, t, std::min(30.0, t + period * 0.9));
        }
        const Tracking result = track(mix.pcm(), bpm, 0.31, kConvergence, 29.5);
        CHECK(result.worst_tempo <= kTempoTolerance, "%.0f BPM: tempo off by %.3f", bpm, result.worst_tempo);
        CHECK(result.worst_phase <= kLoosePhaseToleranceMs, "%.0f BPM: phase off by %.2f ms", bpm, result.worst_phase);
    }
}

void test_a_tempo_change_is_followed() {
    KotlinRandom random(9);
    Mix mix(40.0);
    mix.noise(0.01, random);
    Mix::Beat before;
    before.to = 20.0;
    mix.beat(120.0, random, before);
    Mix::Beat after;
    after.first = 20.1;
    after.from = 20.0;
    mix.beat(140.0, random, after);
    const std::vector<int16_t> pcm = mix.pcm();
    AudioAnalyzer analyzer;
    track(analyzer, part(pcm, 0, 20 * kRate), 120.0, 0.2);

    double worst_tempo = 0.0;
    double worst_phase = 0.0;
    double wrong_tempo_shown = 0.0;
    feed(analyzer, part(pcm, 20 * kRate, pcm.size()), 320, [&](double t) {
        // While it makes up its mind it may keep the old tempo or show none,
        // but never a third one.
        if (analyzer.beatValid() && std::fabs(analyzer.bpm() - 120) > 3 && std::fabs(analyzer.bpm() - 140) > 3) {
            wrong_tempo_shown = analyzer.bpm();
        }
        if (t >= kConvergence) {
            CHECK(analyzer.beatValid(), "no beat %.2f s after the change", t);
            worst_tempo = std::max(worst_tempo, std::fabs(analyzer.bpm() - 140.0));
            worst_phase = std::max(worst_phase, std::fabs(phaseError(analyzer, t, 140.0, 0.1)));
        }
    });
    CHECK(wrong_tempo_shown == 0.0, "a tempo that was never played: %.2f", wrong_tempo_shown);
    CHECK(worst_tempo <= kTempoTolerance, "tempo off by %.3f after the change", worst_tempo);
    CHECK(worst_phase <= kPhaseToleranceMs, "phase off by %.2f ms after the change", worst_phase);
}

void test_when_the_music_stops_the_beat_flag_drops_whatever_the_room_sounds_like() {
    for (double room : {0.0, 0.001, 0.01, 0.05}) {
        KotlinRandom random(10);
        Mix mix(45.0);
        mix.noise(room, random);
        Mix::Beat beat;
        beat.to = 20.0;
        mix.beat(120.0, random, beat);
        AudioAnalyzer analyzer;
        double last_valid = 0.0;
        feed(analyzer, mix.pcm(), 320, [&](double t) {
            if (analyzer.beatValid()) last_valid = t;
            if (!analyzer.beatValid()) TEST_ASSERT_TRUE(analyzer.bpm() == 0.0f);
        });
        CHECK(last_valid >= 20.0 && last_valid <= 20.0 + kConvergence, "with room noise %.3f the beat flag stayed up until %.2f s",
              room, last_valid);
    }
}

void assertSoundHasNoBeat(const char* name, int seed, const std::vector<int16_t>& pcm) {
    AudioAnalyzer analyzer;
    float confidence = 0;
    feed(analyzer, pcm, 320, [&](double t) {
        CHECK(!analyzer.beatValid(), "%s %d got a beat of %.2f BPM at %.2f s", name, seed, static_cast<double>(analyzer.bpm()), t);
        confidence = std::max(confidence, analyzer.beatConfidence());
    });
    CHECK(confidence < BeatTracker::kMinConfidence, "%s %d: confidence reached %.3f", name, seed, static_cast<double>(confidence));
}

void test_sound_without_a_pulse_never_gets_a_tempo() {
    {
        KotlinRandom random(11);
        assertSoundHasNoBeat("steady noise", 0, Mix(40.0).noise(0.2, random).pcm());
    }
    assertSoundHasNoBeat("a steady tone", 0, Mix(40.0).tone(220.0, 0.3).pcm());
    {
        KotlinRandom random(12);
        Mix mix(40.0);
        mix.noise(0.2, random);
        for (size_t i = 0; i < mix.data.size(); ++i) {
            mix.data[i] *= static_cast<float>(0.55 + 0.45 * std::sin(static_cast<double>(i) / 16000.0 * 0.9));
        }
        assertSoundHasNoBeat("noise that swells and fades", 0, mix.pcm());
    }
    for (int seed = 1; seed <= 5; ++seed) {
        // Hits at random moments, 0.15 to 1.05 s apart.
        KotlinRandom random(seed);
        Mix mix(40.0);
        mix.noise(0.005, random);
        for (double t = 0.3; t < 40;) {
            mix.kick(t, 0.5, random);
            t += 0.15 + random.nextDouble() * 0.9;
        }
        assertSoundHasNoBeat("random hits", seed, mix.pcm());
    }
    for (int seed = 1; seed <= 4; ++seed) {
        // Something like speech: syllables of uneven length with uneven gaps.
        KotlinRandom random(seed);
        Mix mix(40.0);
        mix.noise(0.003, random);
        for (double t = 0.3; t < 39;) {
            const double length = 0.08 + random.nextDouble() * 0.15;
            const double voice = 180.0 + random.nextDouble() * 60;
            mix.tone(voice, 0.2, t, t + length);
            const double formant = 700.0 + random.nextDouble() * 900;
            mix.tone(formant, 0.1, t, t + length);
            mix.noise(0.03, random, t, t + length);
            t += length + 0.05 + random.nextDouble() * 0.25;
            if (random.nextInt(6) == 0) t += 0.5;
        }
        assertSoundHasNoBeat("speech", seed, mix.pcm());
    }
}

// ---- mechanics -------------------------------------------------------------------------------

// Everything the analyser says, to compare two of them exactly.
struct Outputs {
    uint8_t level, bass, mid, treble;
    bool onset, beat_valid;
    uint32_t onsets, frames;
    float bpm, phase, confidence;

    explicit Outputs(const AudioAnalyzer& a)
        : level(a.level()), bass(a.bass()), mid(a.mid()), treble(a.treble()), onset(a.onset()), beat_valid(a.beatValid()),
          onsets(a.onsets()), frames(a.frames()), bpm(a.bpm()), phase(a.beatPhase()), confidence(a.beatConfidence()) {}
    bool operator==(const Outputs& o) const {
        return level == o.level && bass == o.bass && mid == o.mid && treble == o.treble && onset == o.onset &&
               beat_valid == o.beat_valid && onsets == o.onsets && frames == o.frames && bpm == o.bpm && phase == o.phase &&
               confidence == o.confidence;
    }
};

// The app's test with its block sizes (320 against 1, 7, 160, 333, 4096 and
// everything at once), and 1000 as well. Compared exactly, floats included,
// and not only at the end: at every multiple of 28 000 samples, where blocks
// of 1, 7, 160 and 1000 all end.
void test_the_size_of_the_blocks_makes_no_difference() {
    KotlinRandom random(13);
    Mix::Beat beat;
    beat.hats = 0.1;
    Mix mix(12.0);
    mix.beat(128.0, random, beat).noise(0.01, random);
    const std::vector<int16_t> pcm = mix.pcm();
    auto run = [&](size_t block, std::vector<Outputs>* on_the_way) {
        AudioAnalyzer analyzer;
        size_t fed = 0;
        feed(analyzer, pcm, block, [&](double) {
            fed = std::min(fed + block, pcm.size());
            if (on_the_way != nullptr && fed % 28000 == 0) on_the_way->push_back(Outputs(analyzer));
        });
        return Outputs(analyzer);
    };
    const Outputs reference = run(320, nullptr);
    TEST_ASSERT_TRUE(reference.beat_valid);
    TEST_ASSERT_EQUAL_UINT32(1200, reference.frames);
    for (size_t block : {size_t{1}, size_t{7}, size_t{160}, size_t{333}, size_t{1000}, size_t{4096}, pcm.size()}) {
        CHECK(run(block, nullptr) == reference, "in blocks of %d", static_cast<int>(block));
    }
    std::vector<Outputs> by_one;
    run(1, &by_one);
    TEST_ASSERT_EQUAL(6, by_one.size());
    for (size_t block : {size_t{7}, size_t{160}, size_t{1000}}) {
        std::vector<Outputs> other;
        run(block, &other);
        TEST_ASSERT_EQUAL(by_one.size(), other.size());
        for (size_t i = 0; i < by_one.size(); ++i) {
            CHECK(other[i] == by_one[i], "in blocks of %d, after %d samples", static_cast<int>(block), static_cast<int>(i + 1) * 28000);
        }
    }
}

void test_the_phase_is_known_for_the_last_sample_not_the_last_frame() {
    // Feed a locked analyser one sample at a time: the phase must advance
    // with every sample, evenly.
    KotlinRandom random(14);
    Mix mix(8.0);
    mix.beat(120.0, random);
    const std::vector<int16_t> pcm = mix.pcm();
    AudioAnalyzer analyzer;
    feed(analyzer, part(pcm, 0, pcm.size() - 400));
    TEST_ASSERT_TRUE(analyzer.beatValid());
    float last = analyzer.beatPhase();
    for (size_t i = pcm.size() - 400; i < pcm.size(); ++i) {
        analyzer.process(&pcm[i], 1);
        float step = analyzer.beatPhase() - last;
        step -= std::floor(step + 0.5f);
        // One sample of a 0.5 s beat is 1/8000 of it; a frame boundary may
        // correct by a little more.
        CHECK(step > 0.0f && step < 0.004f, "step %.6f at sample %d", static_cast<double>(step), static_cast<int>(i));
        last = analyzer.beatPhase();
    }
}

void test_reset_forgets_everything() {
    KotlinRandom random(15);
    Mix mix(10.0);
    mix.beat(100.0, random);
    const std::vector<int16_t> pcm = mix.pcm();
    AudioAnalyzer analyzer;
    feed(analyzer, pcm);
    TEST_ASSERT_TRUE(analyzer.beatValid());
    const Outputs first(analyzer);
    analyzer.reset();
    TEST_ASSERT_FALSE(analyzer.beatValid());
    TEST_ASSERT_EQUAL(0, analyzer.level());
    TEST_ASSERT_EQUAL_UINT32(0, analyzer.frames());
    feed(analyzer, Mix(1.0).pcm());
    TEST_ASSERT_EQUAL(0, analyzer.level());
    TEST_ASSERT_FALSE(analyzer.beatValid());

    // Not in the app: after a reset the same sound gives exactly the same again.
    analyzer.reset();
    feed(analyzer, pcm);
    TEST_ASSERT_TRUE(Outputs(analyzer) == first);
}

void test_analysing_allocates_nothing() {
    // The app asks its JVM; here every use of new by the program is counted.
    KotlinRandom random(16);
    Mix::Beat beat;
    beat.hats = 0.1;
    Mix mix(30.0);
    mix.beat(128.0, random, beat).noise(0.02, random);
    const std::vector<int16_t> pcm = mix.pcm();
    static MicLive mic;  // as on the device: not on the stack
    const size_t counted = g_allocations;
    const std::vector<int16_t> proof(1);  // the count does see an allocation
    TEST_ASSERT_EQUAL(counted + 1, g_allocations);

    const size_t before = g_allocations;
    LiveInput frame;
    size_t frames = 0;
    for (size_t offset = 0; offset < pcm.size(); offset += 320) {
        mic.process(pcm.data() + offset, std::min<size_t>(320, pcm.size() - offset));
        if (mic.snapshot(static_cast<uint32_t>(offset / 16), frame)) ++frames;
    }
    TEST_ASSERT_EQUAL(before, g_allocations);
    TEST_ASSERT_EQUAL(1500, frames);
    TEST_ASSERT_EQUAL_UINT32(3000, mic.analyzer().frames());
    TEST_ASSERT_TRUE(mic.analyzer().beatValid());
}

// The app's bound, 25 ms per second of audio, on a host build without
// optimisation; it catches an accidental O(n^2), nothing finer. The printed
// numbers are for sizing the capture task: how much longer the call with a
// tempo estimate in it takes than an ordinary one.
void test_a_second_of_audio_costs_a_small_fraction_of_a_second() {
    using Clock = std::chrono::steady_clock;
    KotlinRandom random(17);
    Mix::Beat beat;
    beat.hats = 0.1;
    Mix mix(60.0);
    mix.beat(128.0, random, beat).noise(0.02, random);
    const std::vector<int16_t> pcm = mix.pcm();

    AudioAnalyzer analyzer;
    const Clock::time_point start = Clock::now();
    feed(analyzer, pcm);
    const double ms_per_second = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / 60;

    // Frame by frame: every 25th call holds a tempo estimate.
    analyzer.reset();
    std::vector<double> plain;
    std::vector<double> with_estimate;
    for (size_t frame = 0; (frame + 1) * kHop <= pcm.size(); ++frame) {
        const Clock::time_point t0 = Clock::now();
        analyzer.process(pcm.data() + frame * kHop, kHop);
        const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        // From 6 s on the history is full and an estimate costs the most.
        (frame % BeatTracker::kEstimateEvery == BeatTracker::kEstimateEvery - 1 && frame >= 600 ? with_estimate : plain).push_back(us);
    }
    std::sort(plain.begin(), plain.end());
    std::sort(with_estimate.begin(), with_estimate.end());
    const double plain_us = plain[plain.size() / 2];
    const double estimate_us = with_estimate[with_estimate.size() / 2];
    printf("TIME   %.3f ms per second of audio on this host; one frame %.1f us, one with a tempo estimate %.1f us (%.0f times)\n",
           ms_per_second, plain_us, estimate_us, estimate_us / plain_us);
    CHECK(ms_per_second < 25, "%.3f ms per second of audio", ms_per_second);
}

// ================================================================================================
// MicLive: the frame the jacket's own microphone makes (LiveFrameBuilderTest.kt, as far as it
// concerns audio and beat; sources that switch off, staleness and the latency offset are the
// phone's business).
// ================================================================================================

// 14 bytes that no snapshot would produce, to see that a refused one writes nothing.
LiveInput untouched() {
    LiveInput in;
    in.flags = 0xA5;
    in.seq = 0x5A;
    in.level = 1;
    in.bass = 2;
    in.mid = 3;
    in.treble = 4;
    in.bpm_x10 = 0x1234;
    in.beat_phase = 5;
    in.motion = 6;
    in.tilt_x = -7;
    in.tilt_y = 8;
    in.t_ms = 0x4321;
    return in;
}

bool same(const LiveInput& a, const LiveInput& b) {
    return a.flags == b.flags && a.seq == b.seq && a.level == b.level && a.bass == b.bass && a.mid == b.mid && a.treble == b.treble &&
           a.bpm_x10 == b.bpm_x10 && a.beat_phase == b.beat_phase && a.motion == b.motion && a.tilt_x == b.tilt_x &&
           a.tilt_y == b.tilt_y && a.t_ms == b.t_ms;
}

void test_mic_with_nothing_heard_yet_there_is_no_frame() {
    static MicLive mic;
    mic.reset();
    LiveInput frame = untouched();
    TEST_ASSERT_FALSE(mic.snapshot(5000, frame));
    TEST_ASSERT_TRUE(same(untouched(), frame));
    mic.process(nullptr, 0);  // an empty read is not audio
    TEST_ASSERT_FALSE(mic.snapshot(5001, frame));
    TEST_ASSERT_TRUE(same(untouched(), frame));
}

void test_mic_without_a_beat_the_beat_fields_are_zero_and_the_levels_still_go_out() {
    static MicLive mic;
    mic.reset();
    const std::vector<int16_t> pcm = Mix(3.0).tone(440.0, 0.1).pcm();
    // The first frame holds the peaks since the start, and a tone that starts
    // from nothing is a click in every band. The second holds the tone alone.
    LiveInput frame = untouched();
    mic.process(pcm.data(), 2 * kRate);
    TEST_ASSERT_TRUE(mic.snapshot(69000, frame));
    TEST_ASSERT_EQUAL(0, frame.seq);
    mic.process(pcm.data() + 2 * kRate, kRate);
    frame = untouched();
    TEST_ASSERT_TRUE(mic.snapshot(70000, frame));
    TEST_ASSERT_EQUAL_HEX8(kLiveAudioValid, frame.flags);
    TEST_ASSERT_EQUAL(1, frame.seq);
    CHECK(frame.level >= 240 && frame.mid >= 240, "level %d, mid %d", frame.level, frame.mid);
    CHECK(frame.bass <= 5 && frame.treble <= 5, "bass %d, treble %d", frame.bass, frame.treble);
    TEST_ASSERT_EQUAL(0, frame.bpm_x10);
    TEST_ASSERT_EQUAL(0, frame.beat_phase);
    TEST_ASSERT_EQUAL(0, frame.motion);
    TEST_ASSERT_EQUAL(0, frame.tilt_x);
    TEST_ASSERT_EQUAL(0, frame.tilt_y);
    TEST_ASSERT_EQUAL_UINT16(70000 & 0xFFFF, frame.t_ms);  // the low 16 bits
    uint8_t bytes[kLiveFrameSize];
    TEST_ASSERT_EQUAL(kLiveFrameSize, encodeLive(frame, bytes, sizeof bytes));

    // Nothing fed since: no frame, and `out` is left alone.
    LiveInput again = untouched();
    TEST_ASSERT_FALSE(mic.snapshot(70033, again));
    TEST_ASSERT_TRUE(same(untouched(), again));
}

// The app's "a frame carries the peak since the frame before, so a hit between
// two frames is not lost", with the analyser's own frames as the reports.
void test_mic_a_frame_carries_the_peak_since_the_frame_before() {
    KotlinRandom random(2);
    Mix mix(3.0);
    for (int i = 0; i < 48; ++i) mix.data[2 * kRate + i] += 0.5f * (random.nextFloat() * 2 - 1);  // a click at 2 s
    mix.kick(2.03, 0.4, random);                                                                // and a kick right after
    const std::vector<int16_t> pcm = mix.pcm();

    static MicLive mic;
    mic.reset();
    AudioAnalyzer twin;  // hears the same, frame by frame
    LiveInput frame;
    size_t fed = 0;
    // Feeds up to `until` samples in odd blocks and returns the highest value
    // each output had in a frame on the way.
    struct Peaks {
        int level = 0, bass = 0, mid = 0, treble = 0, frames = 0;
    };
    auto feedTo = [&](size_t until) {
        Peaks peaks;
        while (fed < until) {
            const size_t count = std::min<size_t>(77, until - fed);
            mic.process(pcm.data() + fed, count);
            for (size_t i = 0; i < count; ++i) {
                const uint32_t frames = twin.frames();
                twin.process(pcm.data() + fed + i, 1);
                if (twin.frames() == frames) continue;
                peaks.level = std::max(peaks.level, static_cast<int>(twin.level()));
                peaks.bass = std::max(peaks.bass, static_cast<int>(twin.bass()));
                peaks.mid = std::max(peaks.mid, static_cast<int>(twin.mid()));
                peaks.treble = std::max(peaks.treble, static_cast<int>(twin.treble()));
                ++peaks.frames;
            }
            fed += count;
        }
        return peaks;
    };

    feedTo(static_cast<size_t>(1.97 * kRate));
    TEST_ASSERT_TRUE(mic.snapshot(1970, frame));
    TEST_ASSERT_EQUAL(0, frame.level);

    // 150 ms with the click and the start of the kick in it: 15 analysis
    // frames, each output at its own highest.
    const Peaks hit = feedTo(static_cast<size_t>(2.12 * kRate));
    TEST_ASSERT_EQUAL(15, hit.frames);
    TEST_ASSERT_TRUE(mic.snapshot(2120, frame));
    TEST_ASSERT_EQUAL(1, frame.seq);
    TEST_ASSERT_EQUAL(hit.level, frame.level);
    TEST_ASSERT_EQUAL(hit.bass, frame.bass);
    TEST_ASSERT_EQUAL(hit.mid, frame.mid);
    TEST_ASSERT_EQUAL(hit.treble, frame.treble);
    CHECK(frame.level >= 200 && frame.bass >= 200 && frame.treble >= 200, "the hit: level %d, bass %d, treble %d", frame.level,
          frame.bass, frame.treble);
    // They did not peak together: by now the click's treble is gone, the kick's bass is not.
    CHECK(mic.analyzer().treble() < frame.treble / 2, "treble is %d now, was %d at its peak", mic.analyzer().treble(), frame.treble);

    // The next frame says where things went since, not the old peak again.
    const Peaks after = feedTo(static_cast<size_t>(2.45 * kRate));
    TEST_ASSERT_TRUE(mic.snapshot(2450, frame));
    TEST_ASSERT_EQUAL(after.level, frame.level);
    TEST_ASSERT_EQUAL(after.bass, frame.bass);
    TEST_ASSERT_EQUAL(after.treble, frame.treble);
    CHECK(frame.level < hit.level && frame.treble < hit.treble, "after the hit: level %d, treble %d", frame.level, frame.treble);

    // "A fall is sent as soon as it is all there was."
    feedTo(static_cast<size_t>(2.95 * kRate));
    TEST_ASSERT_TRUE(mic.snapshot(2950, frame));
    CHECK(frame.level <= 3, "rung out: %d", frame.level);

    // Audio, but less than an analysis frame of it: where things stand.
    const uint32_t frames = mic.analyzer().frames();
    mic.process(pcm.data() + fed, 50);
    TEST_ASSERT_EQUAL_UINT32(frames, mic.analyzer().frames());
    TEST_ASSERT_TRUE(mic.snapshot(2953, frame));
    TEST_ASSERT_EQUAL(mic.analyzer().level(), frame.level);
    TEST_ASSERT_EQUAL_HEX8(kLiveAudioValid, frame.flags);
}

// The app's "tempo is tenths of a BPM, rounded" and "the byte is 0 on the
// beat, 128 half-way and 255 just before the next".
void test_mic_tempo_and_phase_are_encoded_as_the_app_does() {
    TEST_ASSERT_EQUAL(1280, MicLive::bpmX10(127.96f));
    TEST_ASSERT_EQUAL(1740, MicLive::bpmX10(174.04f));
    TEST_ASSERT_EQUAL(703, MicLive::bpmX10(70.25f));  // 702.5 rounds up
    TEST_ASSERT_EQUAL(1200, MicLive::bpmX10(120.0f));
    TEST_ASSERT_EQUAL(0, MicLive::bpmX10(0.0f));
    TEST_ASSERT_EQUAL(0xFFFF, MicLive::bpmX10(9999.0f));  // fits the wire format whatever comes

    TEST_ASSERT_EQUAL(0, MicLive::phaseByte(0.0f));
    TEST_ASSERT_EQUAL(0, MicLive::phaseByte(0.0039f));
    TEST_ASSERT_EQUAL(1, MicLive::phaseByte(0.004f));
    TEST_ASSERT_EQUAL(64, MicLive::phaseByte(0.25f));
    TEST_ASSERT_EQUAL(128, MicLive::phaseByte(0.5f));
    TEST_ASSERT_EQUAL(255, MicLive::phaseByte(0.9999f));
    TEST_ASSERT_EQUAL(255, MicLive::phaseByte(0.99999994f));
    TEST_ASSERT_EQUAL_MESSAGE(255, MicLive::phaseByte(1.0f), "never 256");
}

void test_mic_seq_counts_frames_and_wraps_and_t_ms_is_the_low_16_bits() {
    static MicLive mic;
    mic.reset();
    const int16_t sample = 0;
    LiveInput frame;
    int wraps = 0;
    uint16_t last_t = 0;
    for (int i = 0; i < 600; ++i) {
        const uint32_t now = 65000u + static_cast<uint32_t>(i) * 33u;
        mic.process(&sample, 1);
        TEST_ASSERT_TRUE(mic.snapshot(now, frame));
        TEST_ASSERT_EQUAL(i & 0xFF, frame.seq);
        TEST_ASSERT_EQUAL_UINT16(now & 0xFFFF, frame.t_ms);
        if (frame.t_ms < last_t) ++wraps;
        last_t = frame.t_ms;
        // A snapshot that is refused uses no number.
        TEST_ASSERT_FALSE(mic.snapshot(now + 1, frame));
    }
    TEST_ASSERT_EQUAL_MESSAGE(1, wraps, "t_ms did wrap");
}

// Feeds a kick track the way a capture task would and hands every frame to
// the jacket's own LiveSignals. Not in the app: this is the whole path from
// microphone samples to the beat position the effects draw from.
//
// Asserted: from kConvergence seconds on, the beat bit is set, the tempo sent
// is within 1 BPM and the phase sent is within kPhaseToleranceMs of the truth
// (the app's bound for the analyser, which one step of the phase byte, 2 ms,
// must fit into as well); and from half a second later the jacket's beat
// clock is within 15 ms of it.
void test_mic_frames_carry_the_beat_to_the_jackets_beat_clock() {
    const double bpm = 120.0;
    const double first = 0.37;
    KotlinRandom random(31);
    Mix::Beat beat;
    beat.first = first;
    Mix mix(20.0);
    mix.beat(bpm, random, beat).noise(0.016, random);
    const std::vector<int16_t> pcm = mix.pcm();

    static MicLive mic;
    mic.reset();
    LiveSignals signals;
    const uint32_t boot = 55000;  // the local clock at the first sample: t_ms wraps on the way
    const size_t block = 256;     // 16 ms per read
    double worst_sent = 0;
    double worst_clock = 0;
    int frames = 0;
    for (size_t fed = 0; fed + block <= pcm.size();) {
        mic.process(pcm.data() + fed, block);
        fed += block;
        if (fed % (2 * block) != 0) continue;  // a frame every 32 ms
        const double seconds = static_cast<double>(fed) / kRate;
        // The time of the newest sample, as both the frame's t_ms and its arrival.
        const uint32_t now = boot + static_cast<uint32_t>(fed / 16);
        LiveInput frame;
        TEST_ASSERT_TRUE(mic.snapshot(now, frame));
        TEST_ASSERT_EQUAL(frames & 0xFF, frame.seq);
        TEST_ASSERT_EQUAL_UINT16(now & 0xFFFF, frame.t_ms);
        TEST_ASSERT_EQUAL(0, frame.flags & kLiveMotionValid);
        ++frames;
        signals.onFrame(frame, now);
        const Reactive& reactive = signals.advance(now);

        if ((frame.flags & kLiveBeatValid) == 0) {
            CHECK(seconds < kConvergence, "no beat bit at %.2f s", seconds);
            TEST_ASSERT_EQUAL(0, frame.bpm_x10);
            TEST_ASSERT_EQUAL(0, frame.beat_phase);
            continue;
        }
        TEST_ASSERT_EQUAL_HEX8(kLiveAudioValid | kLiveBeatValid, frame.flags);
        TEST_ASSERT_EQUAL(MicLive::bpmX10(mic.analyzer().bpm()), frame.bpm_x10);
        TEST_ASSERT_EQUAL(MicLive::phaseByte(mic.analyzer().beatPhase()), frame.beat_phase);
        if (seconds < kConvergence) continue;

        const double period = 60 / bpm;
        const double truth = (seconds - first) / period;
        auto error_ms = [&](double phase) {
            double error = phase - (truth - std::floor(truth));
            error -= std::floor(error + 0.5);
            return error * period * 1000;
        };
        CHECK(frame.bpm_x10 >= 1190 && frame.bpm_x10 <= 1210, "tempo sent at %.2f s: %d", seconds, frame.bpm_x10);
        worst_sent = std::max(worst_sent, std::fabs(error_ms((frame.beat_phase + 0.5) / 256.0)));
        if (seconds < kConvergence + 0.5) continue;  // the beat group takes 300 ms to fade in
        TEST_ASSERT_EQUAL(255, reactive.beat_mix);
        worst_clock = std::max(worst_clock, std::fabs(error_ms((reactive.beat & 0xFFFF) / 65536.0)));
    }
    printf("BOUND  microphone to beat clock at 120 BPM: phase sent within %.2f ms, beat clock within %.2f ms\n", worst_sent,
           worst_clock);
    TEST_ASSERT_EQUAL(625, frames);
    CHECK(worst_sent <= kPhaseToleranceMs, "phase sent off by %.2f ms", worst_sent);
    CHECK(worst_clock <= 15.0, "beat clock off by %.2f ms", worst_clock);
    TEST_ASSERT_INT_WITHIN(10, 1200, signals.beatClock().bpmX10());
}

// Not in the app. The same sound in blocks of 1, 7, 160 and 1000 samples,
// with a snapshot every 533 samples (blocks are cut there): the same frames,
// byte for byte.
void test_mic_frames_do_not_depend_on_the_block_size() {
    KotlinRandom random(13);
    Mix::Beat beat;
    beat.hats = 0.1;
    Mix mix(12.0);
    mix.beat(128.0, random, beat).noise(0.01, random);
    const std::vector<int16_t> pcm = mix.pcm();
    static MicLive mic;
    auto run = [&](size_t block) {
        mic.reset();
        std::vector<LiveInput> frames;
        size_t fed = 0;
        while (fed < pcm.size()) {
            const size_t next_snapshot = (fed / 533 + 1) * 533;
            const size_t count = std::min({block, next_snapshot - fed, pcm.size() - fed});
            mic.process(pcm.data() + fed, count);
            fed += count;
            if (fed % 533 == 0) {
                LiveInput frame;
                TEST_ASSERT_TRUE(mic.snapshot(static_cast<uint32_t>(fed / 16), frame));
                frames.push_back(frame);
            }
        }
        return frames;
    };
    const std::vector<LiveInput> reference = run(160);
    TEST_ASSERT_EQUAL(360, reference.size());
    TEST_ASSERT_EQUAL_HEX8(kLiveAudioValid | kLiveBeatValid, reference.back().flags);
    for (size_t block : {size_t{1}, size_t{7}, size_t{1000}}) {
        const std::vector<LiveInput> frames = run(block);
        TEST_ASSERT_EQUAL(reference.size(), frames.size());
        for (size_t i = 0; i < frames.size(); ++i) {
            CHECK(same(reference[i], frames[i]), "in blocks of %d, frame %d differs", static_cast<int>(block), static_cast<int>(i));
        }
    }
}

void test_mic_reset_and_sensitivity() {
    KotlinRandom random(15);
    Mix mix(10.0);
    mix.beat(100.0, random);
    const std::vector<int16_t> pcm = mix.pcm();
    static MicLive mic;
    mic.reset();
    mic.setSensitivity(80);
    mic.process(pcm.data(), pcm.size());
    LiveInput frame;
    TEST_ASSERT_TRUE(mic.snapshot(1, frame));
    TEST_ASSERT_EQUAL_HEX8(kLiveAudioValid | kLiveBeatValid, frame.flags);
    mic.process(pcm.data(), 10);

    mic.reset();
    TEST_ASSERT_EQUAL(80, mic.analyzer().sensitivity());  // a setting, not something heard
    TEST_ASSERT_EQUAL_UINT32(0, mic.analyzer().frames());
    LiveInput none = untouched();
    TEST_ASSERT_FALSE_MESSAGE(mic.snapshot(2, none), "what was fed before the reset is gone");
    TEST_ASSERT_TRUE(same(untouched(), none));
    const std::vector<int16_t> quiet = Mix(1.0).pcm();
    mic.process(quiet.data(), quiet.size());
    TEST_ASSERT_TRUE(mic.snapshot(3, frame));
    TEST_ASSERT_EQUAL_HEX8(kLiveAudioValid, frame.flags);
    TEST_ASSERT_EQUAL(0, frame.seq);
    TEST_ASSERT_EQUAL(0, frame.level);
    TEST_ASSERT_EQUAL(0, frame.bpm_x10);

    // The sensitivity reaches the analyser: the soft tone of the analyser's test.
    const std::vector<int16_t> soft = Mix(5.0).tone(440.0, 0.0018).pcm();
    int levels[3];
    int i = 0;
    for (int sensitivity : {0, 50, 100}) {
        mic.reset();
        mic.setSensitivity(sensitivity);
        mic.process(soft.data(), soft.size() - 4000);
        TEST_ASSERT_TRUE(mic.snapshot(0, frame));
        mic.process(soft.data() + soft.size() - 4000, 4000);
        TEST_ASSERT_TRUE(mic.snapshot(250, frame));
        levels[i++] = frame.level;
    }
    CHECK(levels[0] <= 8 && levels[1] >= 40 && levels[1] <= 200 && levels[2] == 255, "levels %d, %d, %d", levels[0], levels[1],
          levels[2]);
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_random_numbers_are_kotlins);
    RUN_TEST(test_compile_time_maths);
    RUN_TEST(test_tempo_prior_is_the_apps);
    RUN_TEST(test_fft_is_the_transform_of_its_input);
    RUN_TEST(test_memory_is_what_the_headers_say);

    RUN_TEST(test_tracker_follows_a_metronome_at_every_tempo);
    RUN_TEST(test_tracker_phase_zero_is_the_beat_itself);
    RUN_TEST(test_tracker_claims_nothing_before_there_is_enough_to_go_on);
    RUN_TEST(test_tracker_equal_pulses_are_the_beat_weaker_ones_in_between_are_not);
    RUN_TEST(test_tracker_does_not_double_a_slow_beat_with_weak_off_beats);
    RUN_TEST(test_tracker_gives_tempos_outside_the_range_as_half_or_double);
    RUN_TEST(test_tracker_keeps_the_beat_in_a_three_against_two_pattern);
    RUN_TEST(test_tracker_holds_a_lock_through_small_noise_and_loose_timing);
    RUN_TEST(test_tracker_follows_a_tempo_that_drifts_all_the_way);
    RUN_TEST(test_tracker_finds_a_beat_on_top_of_a_level_that_swells_and_sinks);
    RUN_TEST(test_tracker_drops_the_beat_within_three_seconds_when_noise_takes_over);
    RUN_TEST(test_tracker_replaces_the_old_tempo_and_shows_no_third_on_the_way);
    RUN_TEST(test_tracker_follows_a_jump_in_phase_after_a_moment_not_at_once);
    RUN_TEST(test_tracker_loses_the_beat_when_the_pulses_stop_and_finds_it_again);
    RUN_TEST(test_tracker_forgets_a_beat_that_stays_away);
    RUN_TEST(test_tracker_finds_no_beat_in_silence_a_constant_noise_or_a_ramp);
    RUN_TEST(test_tracker_almost_never_takes_random_hits_for_a_beat);
    RUN_TEST(test_tracker_takes_roughly_regular_hits_for_a_beat);
    RUN_TEST(test_tracker_reset_forgets_the_beat);

    RUN_TEST(test_silence_is_level_zero_with_no_onset_and_no_beat);
    RUN_TEST(test_the_hiss_of_a_quiet_room_stays_dark);
    RUN_TEST(test_a_steady_tone_lights_its_own_band_and_no_other);
    RUN_TEST(test_a_band_follows_its_own_sound_when_two_play_at_once);
    RUN_TEST(test_a_hit_shows_at_once_and_is_over_when_the_sound_is);
    RUN_TEST(test_a_quiet_room_and_a_loud_club_both_use_the_whole_range);
    RUN_TEST(test_a_hit_half_as_loud_reads_half_as_high);
    RUN_TEST(test_the_gain_follows_a_20_db_change_within_a_few_seconds);
    RUN_TEST(test_sensitivity_decides_how_quiet_a_sound_may_be);
    RUN_TEST(test_every_kick_is_an_onset_and_nothing_else_is);
    RUN_TEST(test_kick_tracks_are_followed_in_tempo_and_phase_across_the_range);
    RUN_TEST(test_hats_between_the_kicks_do_not_double_the_tempo);
    RUN_TEST(test_a_pulse_as_strong_between_the_beats_as_on_them_is_the_faster_tempo);
    RUN_TEST(test_a_played_beat_with_loose_timing_snare_hats_and_bass_line_is_followed);
    RUN_TEST(test_a_tempo_change_is_followed);
    RUN_TEST(test_when_the_music_stops_the_beat_flag_drops_whatever_the_room_sounds_like);
    RUN_TEST(test_sound_without_a_pulse_never_gets_a_tempo);
    RUN_TEST(test_the_size_of_the_blocks_makes_no_difference);
    RUN_TEST(test_the_phase_is_known_for_the_last_sample_not_the_last_frame);
    RUN_TEST(test_reset_forgets_everything);
    RUN_TEST(test_analysing_allocates_nothing);
    RUN_TEST(test_a_second_of_audio_costs_a_small_fraction_of_a_second);

    RUN_TEST(test_mic_with_nothing_heard_yet_there_is_no_frame);
    RUN_TEST(test_mic_without_a_beat_the_beat_fields_are_zero_and_the_levels_still_go_out);
    RUN_TEST(test_mic_a_frame_carries_the_peak_since_the_frame_before);
    RUN_TEST(test_mic_tempo_and_phase_are_encoded_as_the_app_does);
    RUN_TEST(test_mic_seq_counts_frames_and_wraps_and_t_ms_is_the_low_16_bits);
    RUN_TEST(test_mic_frames_carry_the_beat_to_the_jackets_beat_clock);
    RUN_TEST(test_mic_frames_do_not_depend_on_the_block_size);
    RUN_TEST(test_mic_reset_and_sensitivity);
    return UNITY_END();
}
