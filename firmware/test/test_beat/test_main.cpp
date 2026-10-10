// Beat clock and live-signal smoothing, driven by synthetic streams of live
// frames: a simulated phone with its own clock sends a frame every 33.3 ms,
// each frame is delayed by a random amount, some are lost, and the jacket's
// clock is sampled 60 times a second, as the render loop would.
//
// The reference is the phone's true beat position delayed by the link's
// minimum latency: the jacket cannot know that latency, so it is a constant
// lag by design (beat_clock.h). The tests bound what is left on top of it,
// in milliseconds, and say the bound they assert.
//
// The simulated link delivers in order, like a real one: a frame that is
// held up holds up the ones behind it. With delays of 20..120 ms on frames
// 33 ms apart that makes a frame with the minimum delay rare (about one in a
// hundred), which is what limits the accuracy: until one comes through, the
// clock takes a slightly later frame for "not delayed".
//
// Measured with this file (pio test -e native -f test_beat -v prints the
// BOUND lines): over 40 one-minute streams at 60..174 BPM, delay 20..120 ms,
// 10 % loss, from 2 s after the start: worst error 12.6 ms, 99 % of all
// samples within 6.5 ms, mean -0.5 ms. kBoundMs is what the tests assert.

#include <algorithm>
#include <cmath>
#include <vector>

#include "../support/live_sim.h"
#include "../support/test_support.h"

using namespace jacket;

void setUp() {}
void tearDown() {}

namespace {

constexpr double kBoundMs = 15.0;       // the stated bound: 20..120 ms delay, 10 % loss, after 2 s
constexpr double kCleanBoundMs = 2.0;   // a link with 5..8 ms delay and no loss
constexpr double kRoughBoundMs = 20.0;  // 40..250 ms delay, 30 % loss; or a phone clock 0.1 % off

using sim::Frame;
using sim::Lcg;
using sim::Link;
using sim::Phone;
using sim::between;
using sim::stream;

void feed(BeatClock& clock, const Frame& f) {
    clock.onFrame(f.beatValid(), f.in.bpm_x10, f.in.beat_phase, f.in.t_ms, f.arrival_ms);
}

struct Result {
    double worst_ms = 0;        // largest |jacket - reference| after the settling time
    double p99_ms = 0;          // 99 % of the samples are within this
    std::vector<double> errors;
    double mean_ms = 0;         // mean signed error: positive = the jacket is ahead
    bool went_backwards = false;
    double slowest = 1e9;       // smallest and largest advance per sample, relative to the tempo
    double fastest = 0;
    uint32_t beats_counted = 0;
    int samples = 0;
};

// Feeds `frames` to `clock` and samples it at 60 fps from from_ms to to_ms.
// Errors are measured from judge_from_ms on, against the phone's true beat
// position `lag_ms` earlier.
Result run(BeatClock& clock, const Phone& phone, const std::vector<Frame>& frames, double from_ms, double to_ms,
           double judge_from_ms, double lag_ms) {
    Result r;
    size_t next = 0;
    uint32_t last_position = 0;
    bool have_last = false;
    double sum = 0;
    const uint32_t first_beats = clock.beats();
    for (int k = 0;; ++k) {
        const uint32_t t = static_cast<uint32_t>(from_ms + k * 1000.0 / 60.0);
        if (t >= to_ms) break;
        while (next < frames.size() && frames[next].arrival_ms <= t) {
            feed(clock, frames[next++]);
        }
        clock.advance(t);
        const uint32_t position = clock.position();
        if (have_last) {
            const int32_t moved = static_cast<int32_t>(position - last_position);
            if (moved < 0) r.went_backwards = true;
            if (t >= judge_from_ms) {
                const double nominal = phone.tempoAt(t) / 60000.0 * (1000.0 / 60.0) * 65536.0;
                const double ratio = moved / nominal;
                if (ratio < r.slowest) r.slowest = ratio;
                if (ratio > r.fastest) r.fastest = ratio;
            }
        }
        last_position = position;
        have_last = true;
        if (t >= judge_from_ms && clock.active()) {
            const double reference = phone.beatsAt(t - lag_ms);
            double diff = position / 65536.0 - reference;
            diff -= std::round(diff);  // phase error in beats, -0.5 .. 0.5
            const double ms = diff * 60000.0 / phone.tempoAt(t);
            if (std::fabs(ms) > r.worst_ms) r.worst_ms = std::fabs(ms);
            r.errors.push_back(std::fabs(ms));
            sum += ms;
            ++r.samples;
        }
    }
    // Whatever arrived after the last sample still arrived.
    for (; next < frames.size() && frames[next].arrival_ms < to_ms; ++next) {
        feed(clock, frames[next]);
    }
    r.mean_ms = r.samples ? sum / r.samples : 0;
    r.beats_counted = clock.beats() - first_beats;
    if (!r.errors.empty()) {
        std::vector<double> sorted = r.errors;
        std::sort(sorted.begin(), sorted.end());
        r.p99_ms = sorted[sorted.size() * 99 / 100];
    }
    return r;
}

// Unity's own comparisons are for integers; these compare doubles and say the numbers.
#define ASSERT_BELOW(limit, value, what)                                                                   \
    do {                                                                                                   \
        char m_[160];                                                                                      \
        snprintf(m_, sizeof m_, "%s: %.3f is not below %.3f", what, static_cast<double>(value), static_cast<double>(limit)); \
        TEST_ASSERT_TRUE_MESSAGE((value) < (limit), m_);                                                   \
    } while (0)
#define ASSERT_ABOVE(limit, value, what)                                                                   \
    do {                                                                                                   \
        char m_[160];                                                                                      \
        snprintf(m_, sizeof m_, "%s: %.3f is not above %.3f", what, static_cast<double>(value), static_cast<double>(limit)); \
        TEST_ASSERT_TRUE_MESSAGE((value) > (limit), m_);                                                   \
    } while (0)

void report(const char* what, const Result& r) {
    printf("BOUND  %-44s worst %.2f ms, 99%% within %.2f ms, mean %+.2f ms, rate %.2f..%.2f, %d samples\n", what,
           r.worst_ms, r.p99_ms, r.mean_ms, r.slowest, r.fastest, r.samples);
}

// ---- Beat clock ------------------------------------------------------------------------

// The headline case: constant tempo, every frame 20..120 ms late, one in ten lost.
// Asserted bound: within kBoundMs (15 ms, 3 % of a beat at 120 BPM and less
// than one 60 fps frame) of the phone's beat as delayed by the link's 20 ms
// minimum, from 2 s after the first frame; and no bias beyond 3 ms.
void test_constant_tempo_with_jitter_and_loss() {
    Result overall;
    uint32_t seed = 100;
    for (double bpm : {60.0, 96.0, 120.0, 128.0, 174.0}) {
      for (int repeat = 0; repeat < 8; ++repeat) {
        Phone phone;
        phone.bpm = bpm;
        phone.beats0 = 0.13 * repeat;
        phone.clock0 = 7919.0 * repeat;
        const Link link;
        BeatClock clock;
        const Result r = run(clock, phone, stream(phone, link, 0, 60000, ++seed), 0, 60000, 2000, link.min_delay_ms);
        char what[64];
        snprintf(what, sizeof what, "%.0f BPM, delay 20..120 ms, 10%% loss", bpm);
        if (repeat == 0) report(what, r);
        if (r.worst_ms > overall.worst_ms) overall.worst_ms = r.worst_ms;
        overall.errors.insert(overall.errors.end(), r.errors.begin(), r.errors.end());
        if (r.slowest < overall.slowest) overall.slowest = r.slowest;
        if (r.fastest > overall.fastest) overall.fastest = r.fastest;
        overall.mean_ms += r.mean_ms / 40.0;
        overall.samples += r.samples;
        TEST_ASSERT_TRUE(clock.active());
        TEST_ASSERT_TRUE(clock.locked());
        ASSERT_BELOW(kBoundMs, r.worst_ms, what);
        TEST_ASSERT_FALSE_MESSAGE(r.went_backwards, "the beat position must never go back");
        // Smooth: between two frames of the picture the beat never stalls or leaps.
        ASSERT_ABOVE(0.70, r.slowest, what);
        ASSERT_BELOW(1.30, r.fastest, what);
        // One count per beat: 60 s of music is bpm beats, give or take the edges.
        TEST_ASSERT_INT_WITHIN(1, static_cast<int>(bpm), static_cast<int>(r.beats_counted));
        TEST_ASSERT_EQUAL(static_cast<int>(std::lround(bpm * 10)), clock.bpmX10());
      }
    }
    std::sort(overall.errors.begin(), overall.errors.end());
    overall.p99_ms = overall.errors[overall.errors.size() * 99 / 100];
    report("all 40 runs (5 tempos x 8 streams)", overall);
    ASSERT_BELOW(3.0, std::fabs(overall.mean_ms), "mean error over all runs");
    ASSERT_BELOW(10.0, overall.p99_ms, "99 % of all samples");
}

// A cleaner or a much worse link. Asserted: 2 ms on a link with 5..8 ms of
// delay, kBoundMs on the standard one, 20 ms with 40..250 ms and 30 % loss.
void test_error_does_not_depend_on_the_jitter() {
    struct Case {
        double min_delay, max_delay, loss;
    };
    for (const Case& c : {Case{5, 8, 0.0}, Case{20, 120, 0.10}, Case{40, 250, 0.30}}) {
        Phone phone;
        Link link;
        link.min_delay_ms = c.min_delay;
        link.max_delay_ms = c.max_delay;
        link.loss = c.loss;
        BeatClock clock;
        const Result r = run(clock, phone, stream(phone, link, 0, 60000, 11), 0, 60000, 3000, c.min_delay);
        char what[64];
        snprintf(what, sizeof what, "delay %.0f..%.0f ms, %.0f%% loss", c.min_delay, c.max_delay, c.loss * 100);
        report(what, r);
        ASSERT_BELOW(c.max_delay < 10 ? kCleanBoundMs : c.max_delay > 200 ? kRoughBoundMs : kBoundMs, r.worst_ms, what);
        TEST_ASSERT_FALSE(r.went_backwards);
    }
}

// The tempo steps from 120 to 140 BPM: the clock follows without stepping
// and is back inside the bound one second later.
void test_tempo_step() {
    Phone phone;
    phone.step_at_ms = 20000;
    phone.step_bpm = 140;
    const Link link;
    const std::vector<Frame> frames = stream(phone, link, 0, 40000, 3);
    BeatClock clock;
    Result r = run(clock, phone, between(frames, 0, 20000), 0, 20000, 2000, link.min_delay_ms);
    ASSERT_BELOW(kBoundMs, r.worst_ms, "phase error in ms");
    TEST_ASSERT_EQUAL(1200, clock.bpmX10());

    // The second around the step, then the rest.
    r = run(clock, phone, between(frames, 20000, 21000), 20000, 21000, 20000, link.min_delay_ms);
    report("the second after a 120 -> 140 BPM step", r);
    TEST_ASSERT_FALSE_MESSAGE(r.went_backwards, "no step back at a tempo change");
    ASSERT_BELOW(1.45, r.fastest, "no leap forward either");
    ASSERT_BELOW(30.0, r.worst_ms, "the new tempo is only known once a frame says so");

    r = run(clock, phone, between(frames, 21000, 99999), 21000, 40000, 21000, link.min_delay_ms);
    report("after the step", r);
    ASSERT_BELOW(kBoundMs, r.worst_ms, "phase error in ms");
    TEST_ASSERT_EQUAL(1400, clock.bpmX10());
}

// t_ms is 16 bits: it wraps every 65.5 s. Three minutes cross it twice, and
// the phone's clock starts just below a wrap.
void test_t_ms_wrap() {
    Phone phone;
    phone.clock0 = 65000;
    const Link link;
    BeatClock clock;
    const std::vector<Frame> frames = stream(phone, link, 0, 180000, 5);
    int wraps = 0;
    for (size_t i = 1; i < frames.size(); ++i) wraps += frames[i].in.t_ms < frames[i - 1].in.t_ms;
    TEST_ASSERT_GREATER_OR_EQUAL(3, wraps);
    const Result r = run(clock, phone, frames, 0, 180000, 2000, link.min_delay_ms);
    report("three minutes across t_ms wraps", r);
    ASSERT_BELOW(kBoundMs, r.worst_ms, "phase error in ms");
    TEST_ASSERT_FALSE(r.went_backwards);
    TEST_ASSERT_LESS_THAN_MESSAGE(110, clock.lastExcessDelayMs(), "a wrap must not look like a late frame");
}

// The phone's crystal is not ours: its milliseconds are longer or shorter, and
// so is the tempo it reports. At 100 ppm (a poor pair of crystals) the bound
// is the usual one; at 0.1 %, ten times worse than that, it is 20 ms.
void test_phone_clock_drift() {
    for (double rate : {1.0001, 0.9999, 1.001, 0.999}) {
        Phone phone;
        phone.clock_rate = rate;
        const Link link;
        BeatClock clock;
        const Result r = run(clock, phone, stream(phone, link, 0, 120000, 9), 0, 120000, 2000, link.min_delay_ms);
        char what[64];
        snprintf(what, sizeof what, "phone clock %+.0f ppm, two minutes", (rate - 1.0) * 1e6);
        report(what, r);
        const bool realistic = std::fabs(rate - 1.0) < 0.0005;
        ASSERT_BELOW(realistic ? kBoundMs : kRoughBoundMs, r.worst_ms, what);
        TEST_ASSERT_FALSE(r.went_backwards);
    }
}

// Frames keep coming but their beat-valid flag drops: the clock coasts at the
// last tempo for kCoastMs, then says so.
void test_beat_valid_drops() {
    Phone phone;
    phone.bpm = 126;
    const Link link;
    BeatClock clock;
    const std::vector<Frame> frames = stream(phone, link, 0, 20000, 21, 10000);  // valid for the first 10 s
    uint32_t last_valid = 0;
    for (const Frame& f : frames) {
        if (f.beatValid()) last_valid = f.arrival_ms;
    }
    size_t next = 0;
    bool was_active = false;
    uint32_t inactive_at = 0;
    for (uint32_t t = 0; t < 20000; t += 5) {
        while (next < frames.size() && frames[next].arrival_ms <= t) {
            feed(clock, frames[next++]);
        }
        clock.advance(t);
        if (was_active && !clock.active() && inactive_at == 0) inactive_at = t;
        was_active = was_active || clock.active();
        if (t > last_valid && t < last_valid + BeatClock::kCoastMs - 10) {
            TEST_ASSERT_TRUE_MESSAGE(clock.active(), "still coasting");
            // Coasting is still on the beat: nothing has drifted in 3 s.
            double diff = clock.position() / 65536.0 - phone.beatsAt(t - link.min_delay_ms);
            diff -= std::round(diff);
            ASSERT_BELOW(kBoundMs, std::fabs(diff * 60000.0 / phone.bpm), "coasting error in ms");
        }
        if (t > last_valid + BeatClock::kFreshMs + 5 && t < 20000) TEST_ASSERT_FALSE(clock.locked());
    }
    TEST_ASSERT_UINT32_WITHIN(5, last_valid + BeatClock::kCoastMs, inactive_at);
    TEST_ASSERT_EQUAL_MESSAGE(1260, clock.bpmX10(), "the last tempo is kept");
}

// The stream stops dead (the phone is gone): same coast, then "no beat", and
// the position goes on moving evenly so whatever fades out has no step in it.
void test_frames_stop() {
    Phone phone;
    const Link link;
    BeatClock clock;
    const std::vector<Frame> frames = stream(phone, link, 0, 7800, 33);  // the last one arrives before 8000
    Result r = run(clock, phone, frames, 0, 8000, 2000, link.min_delay_ms);
    ASSERT_BELOW(kBoundMs, r.worst_ms, "phase error in ms");
    const uint32_t last = frames.back().arrival_ms;

    r = run(clock, phone, {}, 8000, last + BeatClock::kCoastMs - 20, 8000, link.min_delay_ms);
    report("coasting with no frames at all", r);
    TEST_ASSERT_TRUE(clock.active());
    ASSERT_BELOW(kBoundMs, r.worst_ms, "phase error in ms");

    clock.advance(last + BeatClock::kCoastMs - 30);
    uint32_t previous = clock.position();
    for (uint32_t t = last + BeatClock::kCoastMs - 20; t < last + BeatClock::kCoastMs + 2000; t += 10) {
        clock.advance(t);
        if (t >= last + BeatClock::kCoastMs) TEST_ASSERT_FALSE_MESSAGE(clock.active(), "no beat any more");
        const uint32_t moved = clock.position() - previous;
        TEST_ASSERT_UINT32_WITHIN_MESSAGE(2, 1311, moved, "10 ms at 120 BPM is 0.02 beats, before and after");
        previous = clock.position();
    }
    TEST_ASSERT_FALSE(clock.locked());
}

// The phone's beat tracker changes its mind by 0.4 beats: the clock follows
// within a few frames (a snap, not a slow slew). One wild frame is ignored.
void test_phase_jump_and_outlier() {
    Phone phone;
    phone.jump_at_ms = 10000;
    phone.jump_beats = 0.4;
    Link link;
    link.loss = 0;
    BeatClock clock;
    const std::vector<Frame> frames = stream(phone, link, 0, 20000, 17);
    run(clock, phone, between(frames, 0, 10300), 0, 10300, 2000, link.min_delay_ms);
    const Result r = run(clock, phone, between(frames, 10300, 99999), 10300, 20000, 10300, link.min_delay_ms);
    report("0.3 s after a 0.4-beat jump", r);
    ASSERT_BELOW(kBoundMs, r.worst_ms, "relocked within 300 ms of the jump");

    // One frame with a phase 0.4 beats off, in an otherwise steady stream:
    // the clock stays within a millisecond of a twin that never saw it.
    Phone steady;
    const std::vector<Frame> clean = stream(steady, link, 0, 12000, 19);
    std::vector<Frame> spoiled = clean;
    const size_t victim = spoiled.size() / 2;
    spoiled[victim].in.beat_phase = static_cast<uint8_t>(spoiled[victim].in.beat_phase + 102);
    BeatClock twin, other;
    double apart_ms = 0;
    size_t next = 0;
    for (uint32_t t = 0; t < 12000; t += 4) {
        for (; next < clean.size() && clean[next].arrival_ms <= t; ++next) {
            feed(twin, clean[next]);
            feed(other, spoiled[next]);
        }
        twin.advance(t);
        other.advance(t);
        const double ms = std::fabs(static_cast<int32_t>(other.position() - twin.position()) / 65536.0 * 500.0);
        if (ms > apart_ms) apart_ms = ms;
    }
    printf("BOUND  %-44s moved the beat by at most %.2f ms\n", "one wild frame", apart_ms);
    ASSERT_BELOW(1.0, apart_ms, "a single outlier must not move the beat");
}

// Tempos outside 30..300 BPM are not a beat.
void test_implausible_tempo_is_no_beat() {
    BeatClock clock;
    for (uint32_t t = 0; t < 3000; t += 33) {
        clock.onFrame(true, t % 2 ? 0 : 299, static_cast<uint8_t>(t), static_cast<uint16_t>(t), t + 30);
        clock.onFrame(true, 3001, static_cast<uint8_t>(t), static_cast<uint16_t>(t), t + 30);
    }
    TEST_ASSERT_FALSE(clock.active());
    TEST_ASSERT_EQUAL(0, clock.bpmX10());
    clock.onFrame(true, 300, 0, 3000, 3030);
    TEST_ASSERT_TRUE(clock.active());
    clock.onFrame(true, 3000, 0, 3033, 3063);
    TEST_ASSERT_EQUAL(3000, clock.bpmX10());
}

// The stream pauses for 40 s (longer than t_ms can be unwrapped across) and
// comes back; and the app restarts, so its clock starts again near zero.
// Either way the clock is back inside the bound a second later.
void test_resume_after_a_gap_and_phone_clock_reset() {
    Phone phone;
    const Link link;
    BeatClock clock;
    run(clock, phone, stream(phone, link, 0, 10000, 41), 0, 10000, 2000, link.min_delay_ms);
    run(clock, phone, {}, 10000, 50000, 60000, 0);
    TEST_ASSERT_FALSE(clock.active());
    const std::vector<Frame> second = stream(phone, link, 50000, 60000, 43);
    Result r = run(clock, phone, second, 50000, 60000, 51000, link.min_delay_ms);
    report("after a 40 s pause", r);
    TEST_ASSERT_TRUE(clock.active());
    ASSERT_BELOW(kBoundMs, r.worst_ms, "phase error in ms");

    Phone restarted;
    restarted.clock_reset_at_ms = 10000;
    BeatClock other;
    const std::vector<Frame> frames = stream(restarted, link, 0, 20000, 47);
    run(other, restarted, between(frames, 0, 11500), 0, 11500, 2000, link.min_delay_ms);
    r = run(other, restarted, between(frames, 11500, 99999), 11500, 20000, 11500, link.min_delay_ms);
    report("1.5 s after the phone's clock restarted", r);
    ASSERT_BELOW(kBoundMs, r.worst_ms, "phase error in ms");
}

// advance() may be called at any rhythm: the position at a given time is the same.
void test_advance_is_independent_of_call_rate() {
    Phone phone;
    phone.bpm = 133;
    const Link link;
    const std::vector<Frame> frames = stream(phone, link, 0, 15000, 51);
    BeatClock every_ms, at_60fps, lazy;
    size_t next = 0;
    for (uint32_t t = 0; t < 15000; ++t) {
        while (next < frames.size() && frames[next].arrival_ms <= t) {
            const Frame& f = frames[next++];
            for (BeatClock* c : {&every_ms, &at_60fps, &lazy}) feed(*c, f);
        }
        every_ms.advance(t);
        if (t % 17 == 0) {
            at_60fps.advance(t);
            TEST_ASSERT_EQUAL_UINT32(every_ms.position(), at_60fps.position());
        }
        if (t % 1000 == 0) {
            lazy.advance(t);
            TEST_ASSERT_EQUAL_UINT32(every_ms.position(), lazy.position());
        }
    }
}

// A frame is often handed over a few milliseconds after it arrived, when the
// clock has already been advanced past its arrival time (the render loop was
// busy). Stamped with its true arrival it must lock to the same beat: no
// bias, only the correction starting those few milliseconds later.
void test_frames_handled_late_count_from_their_arrival() {
    Phone phone;
    const Link link;
    const std::vector<Frame> frames = stream(phone, link, 0, 30000, 71);
    BeatClock prompt, tardy, unordered;
    Lcg rng(5);
    double worst_ms = 0, sum_ms = 0, unordered_worst_ms = 0;
    int samples = 0;
    for (const Frame& f : frames) {
        feed(prompt, f);
        const uint32_t handled_at = f.arrival_ms + rng.next() % 17;  // up to one 60 fps frame later
        tardy.advance(handled_at);
        feed(tardy, f);
        // Stamps that are not even in order (each frame stamped when it was
        // handled, and handled out of step): still no restart, still locked.
        unordered.onFrame(f.beatValid(), f.in.bpm_x10, f.in.beat_phase, f.in.t_ms, handled_at);
        prompt.advance(handled_at);
        if (f.arrival_ms < 3000) continue;
        const double ms = static_cast<int32_t>(tardy.position() - prompt.position()) / 65536.0 * 500.0;
        if (std::fabs(ms) > worst_ms) worst_ms = std::fabs(ms);
        sum_ms += ms;
        const double other = std::fabs(static_cast<int32_t>(unordered.position() - prompt.position()) / 65536.0 * 500.0);
        if (other > unordered_worst_ms) unordered_worst_ms = other;
        ++samples;
    }
    printf("BOUND  %-44s mean %+.2f ms, worst %.2f ms; with stamps out of order: worst %.2f ms\n",
           "frames handled up to 16 ms late", sum_ms / samples, worst_ms, unordered_worst_ms);
    ASSERT_BELOW(0.3, std::fabs(sum_ms / samples), "handling delay must not shift the beat");
    ASSERT_BELOW(4.0, worst_ms, "nor disturb it by more than a quarter of the delay");
    ASSERT_BELOW(kBoundMs, unordered_worst_ms, "stamps a few ms out of order");
}

// ---- Live signals ----------------------------------------------------------------------

LiveInput liveFrame(uint8_t flags, uint8_t level = 0, uint8_t motion = 0, int8_t tilt_x = 0, int8_t tilt_y = 0) {
    LiveInput in;
    in.flags = flags;
    in.level = level;
    in.bass = static_cast<uint8_t>(level / 2);
    in.mid = static_cast<uint8_t>(level / 3);
    in.treble = static_cast<uint8_t>(level / 4);
    in.motion = motion;
    in.tilt_x = tilt_x;
    in.tilt_y = tilt_y;
    in.bpm_x10 = 1200;
    return in;
}

// Each group fades in over kReactiveRampMs when its valid bit appears, and
// out again when the bit goes or the stream ends.
void test_groups_fade_in_and_out() {
    LiveSignals signals;
    TEST_ASSERT_EQUAL(0, signals.advance(1000).audio_mix);

    // Audio only, from t = 1000.
    for (uint32_t t = 1000; t <= 2000; t += 33) {
        LiveInput in = liveFrame(kLiveAudioValid, 200, 99, 50, -50);
        in.t_ms = static_cast<uint16_t>(t);
        signals.onFrame(in, t);
    }
    const Reactive& r = signals.advance(2000);
    TEST_ASSERT_EQUAL(255, r.audio_mix);
    TEST_ASSERT_EQUAL_MESSAGE(0, r.motion_mix, "the motion bit was clear");
    TEST_ASSERT_EQUAL_MESSAGE(0, r.motion, "fields without their valid bit are ignored");
    TEST_ASSERT_EQUAL(0, r.tilt_x);
    TEST_ASSERT_EQUAL_MESSAGE(0, r.beat_mix, "the beat bit was clear");
    TEST_ASSERT_EQUAL(200, r.level);
    TEST_ASSERT_EQUAL(100, r.bass);
    TEST_ASSERT_EQUAL(66, r.mid);
    TEST_ASSERT_EQUAL(50, r.treble);

    // The ramp is linear in time.
    LiveSignals ramp;
    ramp.advance(0);
    ramp.onFrame(liveFrame(kLiveAudioValid | kLiveMotionValid | kLiveBeatValid, 10, 10), 0);
    int previous = 0;
    for (uint32_t t = 1; t <= kReactiveRampMs + 5u; ++t) {
        if (t % 33 == 0) ramp.onFrame(liveFrame(kLiveAudioValid | kLiveMotionValid | kLiveBeatValid, 10, 10), t);
        const Reactive& now = ramp.advance(t);
        TEST_ASSERT_INT_WITHIN(1, static_cast<int>(t * 255 / kReactiveRampMs > 255 ? 255 : t * 255 / kReactiveRampMs), now.audio_mix);
        TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(1, now.audio_mix - previous, "never more than one step per millisecond");
        TEST_ASSERT_EQUAL(now.audio_mix, now.motion_mix);
        TEST_ASSERT_EQUAL(now.audio_mix, now.beat_mix);
        previous = now.audio_mix;
    }
    TEST_ASSERT_EQUAL(255, previous);

    // The stream ends: audio and motion fade at once, the beat only after the coast.
    const uint32_t end = kReactiveRampMs + 5u;
    ramp.onTimeout(end + 500);
    TEST_ASSERT_EQUAL(255, ramp.current().audio_mix);
    const Reactive& mid_fade = ramp.advance(end + 500 + kReactiveRampMs / 2);
    TEST_ASSERT_INT_WITHIN(2, 127, mid_fade.audio_mix);
    TEST_ASSERT_INT_WITHIN(2, 127, mid_fade.motion_mix);
    TEST_ASSERT_EQUAL(255, mid_fade.beat_mix);
    const Reactive& gone = ramp.advance(end + 500 + kReactiveRampMs + 2);
    TEST_ASSERT_EQUAL(0, gone.audio_mix);
    TEST_ASSERT_EQUAL(0, gone.motion_mix);
    const uint32_t last_beat = (end / 33) * 33;
    TEST_ASSERT_EQUAL(255, ramp.advance(last_beat + BeatClock::kCoastMs - 1).beat_mix);
    TEST_ASSERT_INT_WITHIN(2, 127, ramp.advance(last_beat + BeatClock::kCoastMs + kReactiveRampMs / 2).beat_mix);
    const Reactive& none = ramp.advance(last_beat + BeatClock::kCoastMs + kReactiveRampMs + 2);
    TEST_ASSERT_EQUAL(0, none.beat_mix);

    // A group that fades out holds its values (the fade is between steady
    // pictures); one that comes back from nothing starts at its new values.
    LiveSignals hold;
    hold.advance(0);
    for (uint32_t t = 0; t <= 990; t += 33) hold.onFrame(liveFrame(kLiveAudioValid | kLiveMotionValid, 200, 150, 40, -40), t);
    hold.onFrame(liveFrame(0, 7, 7, 7, 7), 1000);  // the stream goes on, without valid audio or motion
    const Reactive& fading = hold.advance(1000 + kReactiveRampMs / 2);
    TEST_ASSERT_INT_WITHIN(2, 127, fading.audio_mix);
    TEST_ASSERT_EQUAL(200, fading.level);
    TEST_ASSERT_EQUAL(150, fading.motion);
    TEST_ASSERT_EQUAL(40, fading.tilt_x);
    hold.advance(1000 + kReactiveRampMs + 5);
    TEST_ASSERT_EQUAL(0, hold.current().audio_mix);
    hold.onFrame(liveFrame(kLiveAudioValid | kLiveMotionValid, 30, 20, -10, 10), 2000);
    TEST_ASSERT_EQUAL_MESSAGE(30, hold.current().level, "no glide down from the stale 200");
    TEST_ASSERT_EQUAL(20, hold.current().motion);
    TEST_ASSERT_EQUAL(-10, hold.current().tilt_x);
}

// A value that steps at 30 Hz comes out as a ramp: fast up, slower down, and
// never a visible step from one millisecond (or one 60 fps frame) to the next.
void test_values_are_smoothed() {
    LiveSignals signals;
    signals.advance(0);
    uint32_t t = 0;
    auto feed = [&](uint8_t level, uint8_t motion, int8_t tilt, uint32_t until, int& biggest_step, int& biggest_frame_step) {
        int last = signals.current().level;
        int last_frame = last;
        for (; t < until; ++t) {
            if (t % 33 == 0) signals.onFrame(liveFrame(kLiveAudioValid | kLiveMotionValid, level, motion, tilt, static_cast<int8_t>(-tilt)), t);
            const int now = signals.advance(t).level;
            if (std::abs(now - last) > biggest_step) biggest_step = std::abs(now - last);
            last = now;
            if (t % 16 == 0) {
                if (std::abs(now - last_frame) > biggest_frame_step) biggest_frame_step = std::abs(now - last_frame);
                last_frame = now;
            }
        }
    };
    int step = 0, frame_step = 0;
    feed(0, 0, 0, 396, step, frame_step);  // a silent, still stream first: the groups are present
    TEST_ASSERT_EQUAL(255, signals.current().audio_mix);
    TEST_ASSERT_EQUAL(0, signals.current().level);

    // The frame at t = 396 says 255. One attack time constant later: 63 %.
    feed(255, 255, 100, 396 + kAudioAttackMs, step, frame_step);
    TEST_ASSERT_INT_WITHIN(8, 161, signals.current().level);
    feed(255, 255, 100, 800, step, frame_step);
    TEST_ASSERT_EQUAL(255, signals.current().level);
    TEST_ASSERT_GREATER_THAN_MESSAGE(250, signals.current().motion, "motion has arrived too");
    TEST_ASSERT_INT_WITHIN(4, 100, signals.current().tilt_x);
    TEST_ASSERT_INT_WITHIN(4, -100, signals.current().tilt_y);
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(11, step, "the fastest rise is 255 / 25 per millisecond");

    // Down is slower: one release time constant after the level drops to 0.
    // (The frame carrying the drop arrives at t = 825, the next multiple of 33.)
    step = 0;
    frame_step = 0;
    feed(0, 0, 0, 825 + kAudioReleaseMs, step, frame_step);
    TEST_ASSERT_INT_WITHIN(10, 94, signals.current().level);  // 37 % of 255
    TEST_ASSERT_LESS_OR_EQUAL(2, step);
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(24, frame_step, "under a tenth of full scale per 60 fps frame on the way down");
    feed(0, 0, 0, 4500, step, frame_step);
    TEST_ASSERT_EQUAL(0, signals.current().level);
    TEST_ASSERT_EQUAL(0, signals.current().motion);
    TEST_ASSERT_EQUAL(0, signals.current().tilt_x);

    // Tilt beyond +-kTiltFull is clamped.
    signals.onFrame(liveFrame(kLiveAudioValid | kLiveMotionValid, 0, 0, -128, 127), 4500);
    TEST_ASSERT_EQUAL(-127, signals.advance(5400).tilt_x);
    TEST_ASSERT_EQUAL(127, signals.current().tilt_y);
}

// The smoothed values at a given time do not depend on how often advance()
// is called between the frames.
void test_signals_are_independent_of_call_rate() {
    LiveSignals a, b, c;
    Lcg rng(77);
    uint32_t next_frame = 5;
    for (uint32_t t = 0; t < 6000; ++t) {
        if (t == next_frame && t < 4000) {
            LiveInput in = liveFrame(static_cast<uint8_t>(rng.next() & 7), static_cast<uint8_t>(rng.next()), static_cast<uint8_t>(rng.next()),
                                     static_cast<int8_t>(rng.next()), static_cast<int8_t>(rng.next()));
            in.beat_phase = static_cast<uint8_t>(t * 256 / 500);
            in.t_ms = static_cast<uint16_t>(t);
            a.onFrame(in, t);
            b.onFrame(in, t);
            c.onFrame(in, t);
            next_frame += 20 + rng.next() % 40;
        }
        // The stream ran out at 4500. Each user notices at its next call, as
        // the engine does (tick() before render()), and says when it happened.
        if (t == 4500) a.onTimeout(4500);
        const Reactive& ra = a.advance(t);
        if (t % 16 == 0) {
            if (t >= 4500 && t < 4516) b.onTimeout(4500);
            const Reactive& rb = b.advance(t);
            TEST_ASSERT_EQUAL_MEMORY(&ra, &rb, sizeof(Reactive));
        }
        if (t % 250 == 0) {
            if (t >= 4500 && t < 4750) c.onTimeout(4500);
            const Reactive& rc = c.advance(t);
            TEST_ASSERT_EQUAL_MEMORY(&ra, &rc, sizeof(Reactive));
        }
    }
}

// A frame speaks only for the groups its sender is responsible for: the rest
// is neither updated nor taken for absent.
void test_frames_speak_for_their_groups_only() {
    LiveSignals signals;
    signals.advance(0);
    // A microphone supplies sound; a phone supplies motion, with fields for
    // sound that it does not vouch for (and that must not be believed).
    for (uint32_t t = 0; t <= 2000; ++t) {
        if (t % 30 == 0) {
            LiveInput mic = liveFrame(kLiveAudioValid | kLiveBeatValid, 180);
            mic.motion = 77;  // not its business
            mic.t_ms = static_cast<uint16_t>(t);
            signals.onFrame(mic, t, kLiveSoundGroups);
        }
        if (t % 33 == 0) {
            LiveInput phone = liveFrame(kLiveMotionValid, 250, 120, 40, -40);
            phone.t_ms = static_cast<uint16_t>(t + 31000);
            signals.onFrame(phone, t, kLiveMotionValid);
        }
        signals.advance(t);
    }
    const Reactive& r = signals.current();
    TEST_ASSERT_EQUAL(255, r.audio_mix);
    TEST_ASSERT_EQUAL_MESSAGE(180, r.level, "the phone's audio fields were not its to give");
    TEST_ASSERT_EQUAL(255, r.motion_mix);
    TEST_ASSERT_EQUAL_MESSAGE(120, r.motion, "nor the microphone's motion field");
    TEST_ASSERT_EQUAL(40, r.tilt_x);
    TEST_ASSERT_EQUAL(255, r.beat_mix);
    TEST_ASSERT_EQUAL_MESSAGE(0, signals.beatClock().lastExcessDelayMs(), "the beat clock saw one clock only");

    // The phone goes silent: motion fades, sound does not.
    for (uint32_t t = 2001; t <= 3000; ++t) {
        if (t % 30 == 0) {
            LiveInput mic = liveFrame(kLiveAudioValid | kLiveBeatValid, 180);
            mic.t_ms = static_cast<uint16_t>(t);
            signals.onFrame(mic, t, kLiveSoundGroups);
        }
        if (t == 2500) signals.onTimeout(2500, kLiveMotionValid);
        signals.advance(t);
    }
    TEST_ASSERT_EQUAL(0, signals.current().motion_mix);
    TEST_ASSERT_EQUAL(255, signals.current().audio_mix);

    // The microphone goes silent: now the sound fades.
    signals.onTimeout(3500, kLiveSoundGroups);
    TEST_ASSERT_EQUAL(0, signals.advance(3500 + kReactiveRampMs).audio_mix);

    // By default a frame speaks for everything, as it always has.
    LiveSignals whole;
    whole.advance(0);
    whole.onFrame(liveFrame(kLiveAudioValid | kLiveMotionValid, 10, 10), 0);
    whole.advance(400);
    whole.onFrame(liveFrame(kLiveMotionValid, 10, 10), 400);
    TEST_ASSERT_EQUAL_MESSAGE(0, whole.advance(400 + kReactiveRampMs).audio_mix, "no audio bit: the audio fades");
    TEST_ASSERT_EQUAL(255, whole.current().motion_mix);
}

// Another sender, another clock behind t_ms: told so, the beat clock measures
// the offset afresh instead of taking the difference for delay.
void test_another_sender_has_another_clock() {
    const uint32_t period = 500;  // 120 BPM from both, in phase: only the clocks differ
    auto phaseAt = [&](uint32_t t) { return static_cast<uint8_t>((t % period) * 256u / period); };

    for (const bool told : {false, true}) {
        BeatClock clock;
        uint32_t t = 0;
        for (; t < 4000; t += 30) clock.onFrame(true, 1200, phaseAt(t), static_cast<uint16_t>(t), t);  // the jacket's own clock
        if (told) clock.sourceChanged();
        // A phone whose clock is 400 ms behind: untold, each frame seems 400 ms late.
        uint16_t worst_excess = 0;
        for (; t < 8000; t += 30) {
            clock.onFrame(true, 1200, phaseAt(t), static_cast<uint16_t>(t - 400), t);
            if (clock.lastExcessDelayMs() > worst_excess) worst_excess = clock.lastExcessDelayMs();
        }
        clock.advance(8000);
        const uint16_t truth = static_cast<uint16_t>((8000 % period) * 65536u / period);
        const int off = static_cast<int>(static_cast<uint16_t>(clock.phase() - truth));
        const int distance = off > 32768 ? 65536 - off : off;
        if (told) {
            TEST_ASSERT_EQUAL(0, worst_excess);
            TEST_ASSERT_LESS_THAN_MESSAGE(65536 / 32, distance, "the beat stays where it is");
        } else {
            // What sourceChanged() is for: without it the frames count as late
            // and the beat is moved by 400 ms, most of a 500 ms beat.
            TEST_ASSERT_GREATER_THAN(300, worst_excess);
            TEST_ASSERT_GREATER_THAN_MESSAGE(65536 / 8, distance, "(if this fails the clock learned to cope and the call may go)");
        }
    }
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_constant_tempo_with_jitter_and_loss);
    RUN_TEST(test_error_does_not_depend_on_the_jitter);
    RUN_TEST(test_tempo_step);
    RUN_TEST(test_t_ms_wrap);
    RUN_TEST(test_phone_clock_drift);
    RUN_TEST(test_beat_valid_drops);
    RUN_TEST(test_frames_stop);
    RUN_TEST(test_phase_jump_and_outlier);
    RUN_TEST(test_implausible_tempo_is_no_beat);
    RUN_TEST(test_resume_after_a_gap_and_phone_clock_reset);
    RUN_TEST(test_advance_is_independent_of_call_rate);
    RUN_TEST(test_frames_handled_late_count_from_their_arrival);
    RUN_TEST(test_groups_fade_in_and_out);
    RUN_TEST(test_values_are_smoothed);
    RUN_TEST(test_signals_are_independent_of_call_rate);
    RUN_TEST(test_frames_speak_for_their_groups_only);
    RUN_TEST(test_another_sender_has_another_clock);
    return UNITY_END();
}
