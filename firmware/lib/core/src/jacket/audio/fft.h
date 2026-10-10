// The spectrum of 512 real samples, for the audio analyser.
//
// The Android app runs a plain 512-point complex FFT with the imaginary half
// set to zero (Fft.kt). The result here is the same spectrum, bins 0 to 255,
// at well under half the work: the samples are packed in pairs into 256
// complex numbers, transformed with a 256-point radix-2 FFT, and the two
// interleaved halves are separated again afterwards. The twiddle factors and
// the input order are constant tables in flash.
//
// Cost of one transform(): 1024 butterflies, of which 768 multiply (4
// multiplications each), and 127 bin pairs to separate (8 multiplications
// each): about 4 100 multiplications and 6 900 additions in single precision.
// The app's FFT takes 9 200 and 13 800.
//
// Single-precision float, like the rest of jacket/audio: see audio_analyzer.h.
// No heap, no state: the work arrays belong to the caller.
#pragma once

#include <stdint.h>

namespace jacket {

struct RealFft {
    static constexpr int kSize = 512;        // real samples in
    static constexpr int kBins = kSize / 2;  // complex bins out: 0 Hz up to just under half the sample rate

    // kBins entries: where sample pair n (samples 2n and 2n + 1) goes in the
    // work arrays. The transform wants its input in bit-reversed order;
    // writing it there in the first place saves a pass of swaps.
    static const uint8_t* slots();

    // In:  re[slots()[n]] = x[2n], im[slots()[n]] = x[2n + 1] for n = 0..kBins-1.
    // Out: re[k] + i im[k] = sum over n of x[n] e^(-2 pi i k n / kSize), for
    //      k = 0..kBins-1, not scaled.
    // Both arrays hold kBins values.
    static void transform(float* re, float* im);
};

}  // namespace jacket
