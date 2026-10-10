#include "jacket/audio/mic_live.h"

namespace jacket {

void MicLive::reset() {
    analyzer_.reset();
    to_frame_ = AudioAnalyzer::kHop;
    fed_ = false;
    analysed_ = false;
    for (uint8_t& peak : peak_) peak = 0;
    seq_ = 0;
}

void MicLive::process(const int16_t* pcm, size_t count) {
    if (count != 0) fed_ = true;
    // In pieces that end where the analyser finishes a frame, so that every
    // frame's levels are seen, not only those of the last frame of a block.
    while (count != 0) {
        const size_t piece = count < to_frame_ ? count : to_frame_;
        analyzer_.process(pcm, piece);
        pcm += piece;
        count -= piece;
        to_frame_ = static_cast<uint16_t>(to_frame_ - piece);
        if (to_frame_ != 0) break;  // the block ended inside a frame
        to_frame_ = AudioAnalyzer::kHop;

        const uint8_t now[4] = {analyzer_.level(), analyzer_.bass(), analyzer_.mid(), analyzer_.treble()};
        for (int i = 0; i < 4; ++i) {
            if (!analysed_ || now[i] > peak_[i]) peak_[i] = now[i];
        }
        analysed_ = true;
    }
}

bool MicLive::snapshot(uint32_t t_ms, LiveInput& out) {
    if (!fed_) return false;

    LiveInput frame;
    frame.flags = kLiveAudioValid;
    frame.seq = seq_++;
    if (analysed_) {
        frame.level = peak_[0];
        frame.bass = peak_[1];
        frame.mid = peak_[2];
        frame.treble = peak_[3];
    } else {
        // Nothing new was analysed: this is where things stand.
        frame.level = analyzer_.level();
        frame.bass = analyzer_.bass();
        frame.mid = analyzer_.mid();
        frame.treble = analyzer_.treble();
    }
    if (analyzer_.beatValid() && analyzer_.beatPeriod() > 0.0f) {
        frame.flags |= kLiveBeatValid;
        frame.bpm_x10 = bpmX10(analyzer_.bpm());
        // The phase of the newest sample, which is the instant t_ms names.
        frame.beat_phase = phaseByte(analyzer_.beatPhase());
    }
    frame.t_ms = static_cast<uint16_t>(t_ms);
    out = frame;

    fed_ = false;
    analysed_ = false;
    return true;
}

uint16_t MicLive::bpmX10(float bpm) {
    const float tenths = bpm * 10.0f;
    if (!(tenths > 0.0f)) return 0;
    if (tenths >= 65535.0f) return 65535;
    uint16_t whole = static_cast<uint16_t>(tenths);
    if (tenths - static_cast<float>(whole) >= 0.5f) ++whole;
    return whole;
}

uint8_t MicLive::phaseByte(float phase) {
    if (!(phase > 0.0f)) return 0;
    if (phase >= 1.0f) return 255;  // never 256
    return static_cast<uint8_t>(phase * 256.0f);
}

}  // namespace jacket
