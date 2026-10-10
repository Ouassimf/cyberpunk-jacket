#include "jacket/audio/fft.h"

#include "jacket/audio/const_math.h"

namespace jacket {

namespace {

constexpr int kBins = RealFft::kBins;

// cos and sin of 2 pi i / 512 for the first half turn: every factor the
// 256-point transform and the separation afterwards need.
struct Twiddles {
    float cos[kBins];
    float sin[kBins];
    constexpr Twiddles() : cos(), sin() {
        for (int i = 0; i < kBins; ++i) {
            cos[i] = static_cast<float>(constmath::cosTurn(i, RealFft::kSize));
            sin[i] = static_cast<float>(constmath::sinTurn(i, RealFft::kSize));
        }
    }
};
constexpr Twiddles kTwiddles;

struct Slots {
    uint8_t slot[kBins];
    constexpr Slots() : slot() {
        for (int i = 0; i < kBins; ++i) {
            int reversed = 0;
            for (int bit = 1, mirror = kBins / 2; bit < kBins; bit <<= 1, mirror >>= 1) {
                if (i & bit) reversed |= mirror;
            }
            slot[i] = static_cast<uint8_t>(reversed);
        }
    }
};
constexpr Slots kSlots;

}  // namespace

const uint8_t* RealFft::slots() { return kSlots.slot; }

void RealFft::transform(float* re, float* im) {
    // The 256-point transform of z[n] = x[2n] + i x[2n + 1], decimation in
    // time. Its first two stages only add, subtract and turn by a quarter
    // (their factors are 1 and -i), so they are done together, four at a time.
    for (int i = 0; i < kBins; i += 4) {
        const float ar = re[i] + re[i + 1], ai = im[i] + im[i + 1];
        const float br = re[i] - re[i + 1], bi = im[i] - im[i + 1];
        const float cr = re[i + 2] + re[i + 3], ci = im[i + 2] + im[i + 3];
        const float dr = re[i + 2] - re[i + 3], di = im[i + 2] - im[i + 3];
        re[i] = ar + cr;
        im[i] = ai + ci;
        re[i + 2] = ar - cr;
        im[i + 2] = ai - ci;
        re[i + 1] = br + di;  // d times -i is (di, -dr)
        im[i + 1] = bi - dr;
        re[i + 3] = br - di;
        im[i + 3] = bi + dr;
    }
    for (int half = 4; half < kBins; half *= 2) {
        const int stride = kBins / half;  // e^(-2 pi i k / (2 half)) is entry k * stride of the table
        for (int k = 0; k < half; ++k) {
            const float wr = kTwiddles.cos[k * stride];
            const float wi = -kTwiddles.sin[k * stride];
            for (int i = k; i < kBins; i += 2 * half) {
                const int j = i + half;
                const float tr = re[j] * wr - im[j] * wi;
                const float ti = re[j] * wi + im[j] * wr;
                re[j] = re[i] - tr;
                im[j] = im[i] - ti;
                re[i] += tr;
                im[i] += ti;
            }
        }
    }

    // Z = A + i B, where A and B are the transforms of the even and the odd
    // samples. Both are transforms of real numbers, so A[256 - k] is the
    // conjugate of A[k] (and the same for B), which gives
    //     2 A[k] = Z[k] + conj(Z[256 - k]),   2 i B[k] = Z[k] - conj(Z[256 - k]).
    // The spectrum of all 512 samples is X[k] = A[k] + w B[k] with
    // w = e^(-2 pi i k / 512), and X[256 - k] = conj(A[k] - w B[k]).
    re[0] += im[0];
    im[0] = 0.0f;
    im[kBins / 2] = -im[kBins / 2];
    for (int k = 1; k < kBins / 2; ++k) {
        const int j = kBins - k;
        const float ar = re[k] + re[j], ai = im[k] - im[j];  // 2 A
        const float br = im[k] + im[j], bi = re[j] - re[k];  // 2 B
        const float c = kTwiddles.cos[k];
        const float s = kTwiddles.sin[k];
        const float tr = c * br + s * bi;  // 2 w B
        const float ti = c * bi - s * br;
        re[k] = 0.5f * (ar + tr);
        im[k] = 0.5f * (ai + ti);
        re[j] = 0.5f * (ar - tr);
        im[j] = 0.5f * (ti - ai);
    }
}

}  // namespace jacket
