// A simulated phone and Bluetooth link for the host tests: music with a beat,
// a phone clock that is not the jacket's, live frames every 33.3 ms, each
// delayed by a random amount, some lost, delivered in order.
#pragma once

#include <jacket.h>

#include <cmath>
#include <stdint.h>
#include <vector>

namespace sim {

// Deterministic random numbers.
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    double unit() { return (next() & 0xFFFFFF) / 16777216.0; }  // [0, 1)
};

// The phone and the music, in the jacket's (real) time `t` in milliseconds.
struct Phone {
    double bpm = 120.0;
    double step_at_ms = -1;         // the tempo changes to step_bpm here (if >= 0)
    double step_bpm = 0;
    double jump_at_ms = -1;         // the beat tracker re-phases here by jump_beats
    double jump_beats = 0;
    double beats0 = 0.37;           // beat position at t = 0
    double clock0 = 61000.0;        // what the phone's ms clock reads at t = 0
    double clock_rate = 1.0;        // phone ms per real ms (crystal error)
    double clock_reset_at_ms = -1;  // the app restarts: its clock starts again from 5

    // What the other fields of every frame carry.
    uint8_t flags = jacket::kLiveAudioValid | jacket::kLiveMotionValid | jacket::kLiveBeatValid;
    uint8_t level = 0, bass = 0, mid = 0, treble = 0, motion = 0;
    int8_t tilt_x = 0, tilt_y = 0;

    double tempoAt(double t) const { return step_at_ms >= 0 && t >= step_at_ms ? step_bpm : bpm; }
    double beatsAt(double t) const {
        double b = beats0;
        if (step_at_ms >= 0 && t >= step_at_ms) {
            b += step_at_ms * bpm / 60000.0 + (t - step_at_ms) * step_bpm / 60000.0;
        } else {
            b += t * bpm / 60000.0;
        }
        if (jump_at_ms >= 0 && t >= jump_at_ms) b += jump_beats;
        return b;
    }
    double clockAt(double t) const {
        if (clock_reset_at_ms >= 0 && t >= clock_reset_at_ms) return 5.0 + (t - clock_reset_at_ms) * clock_rate;
        return clock0 + t * clock_rate;
    }
};

struct Link {
    double min_delay_ms = 20;
    double max_delay_ms = 120;
    double loss = 0.10;
    double frame_ms = 1000.0 / 30.0;
};

struct Frame {
    uint32_t arrival_ms;  // on the jacket's clock
    jacket::LiveInput in;

    bool beatValid() const { return (in.flags & jacket::kLiveBeatValid) != 0; }
    // The 14 bytes as they would be written to the `live` characteristic.
    std::vector<uint8_t> bytes() const {
        uint8_t raw[jacket::kLiveFrameSize];
        jacket::encodeLive(in, raw, sizeof raw);
        return std::vector<uint8_t>(raw, raw + sizeof raw);
    }
};

// The frames the jacket receives of those sent between from_ms and to_ms, in
// arrival order. Frames sent at or after beat_valid_until_ms have the
// beat-valid flag clear.
inline std::vector<Frame> stream(const Phone& phone, const Link& link, double from_ms, double to_ms, uint32_t seed,
                                 double beat_valid_until_ms = 1e18) {
    Lcg rng(seed);
    std::vector<Frame> frames;
    double last_arrival = 0;
    uint8_t seq = 0;
    for (double sent = from_ms; sent < to_ms; sent += link.frame_ms) {
        const double delay = link.min_delay_ms + rng.unit() * (link.max_delay_ms - link.min_delay_ms);
        const bool lost = rng.unit() < link.loss;
        ++seq;
        if (lost) continue;
        double arrival = sent + delay;
        if (arrival < last_arrival) arrival = last_arrival;  // a link delivers in order
        last_arrival = arrival;
        const double beats = phone.beatsAt(sent);
        Frame f;
        f.arrival_ms = static_cast<uint32_t>(arrival);
        f.in.flags = phone.flags;
        if (sent >= beat_valid_until_ms) f.in.flags = static_cast<uint8_t>(f.in.flags & ~jacket::kLiveBeatValid);
        f.in.seq = seq;
        f.in.level = phone.level;
        f.in.bass = phone.bass;
        f.in.mid = phone.mid;
        f.in.treble = phone.treble;
        f.in.motion = phone.motion;
        f.in.tilt_x = phone.tilt_x;
        f.in.tilt_y = phone.tilt_y;
        // The phone measures tempo with its own clock.
        f.in.bpm_x10 = static_cast<uint16_t>(std::lround(phone.tempoAt(sent) / phone.clock_rate * 10.0));
        f.in.beat_phase = static_cast<uint8_t>(static_cast<uint32_t>(std::floor((beats - std::floor(beats)) * 256.0)) & 0xFF);
        f.in.t_ms = static_cast<uint16_t>(static_cast<uint64_t>(phone.clockAt(sent)) & 0xFFFF);
        frames.push_back(f);
    }
    return frames;
}

// The part of a stream that arrives in [from_ms, to_ms).
inline std::vector<Frame> between(const std::vector<Frame>& frames, uint32_t from_ms, uint32_t to_ms) {
    std::vector<Frame> out;
    for (const Frame& f : frames) {
        if (f.arrival_ms >= from_ms && f.arrival_ms < to_ms) out.push_back(f);
    }
    return out;
}

}  // namespace sim
