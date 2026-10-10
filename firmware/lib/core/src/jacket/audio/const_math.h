// Sine, cosine, logarithm and exponential for the compiler. They fill the
// constant tables of the audio analysis (window, FFT twiddles, tempo prior),
// which then sit in flash instead of being computed at boot and held in RAM.
//
// For constexpr tables only: the double arithmetic here is done by the
// compiler and never reaches the ESP32, whose FPU is single precision. Nothing
// in this file may be called at run time.
#pragma once

namespace jacket {
namespace constmath {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kLn2 = 0.693147180559945309417232121458;

// Taylor series, for |x| up to pi/4: 13 terms leave nothing a double can hold.
constexpr double sinSeries(double x) {
    double term = x;
    double sum = x;
    for (int n = 1; n <= 13; ++n) {
        term *= -x * x / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

constexpr double cosSeries(double x) {
    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n <= 13; ++n) {
        term *= -x * x / ((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}

// cos and sin of `index` / `turn` of a full circle. `turn` must be a multiple
// of 8: the angle is folded into the first eighth in whole numbers, so the
// tables are exactly symmetric and a quarter turn is exactly 0.
constexpr double cosTurn(int index, int turn) {
    int a = index % turn;
    if (a < 0) a += turn;
    if (a > turn / 2) a = turn - a;
    double sign = 1.0;
    if (a > turn / 4) {
        a = turn / 2 - a;
        sign = -1.0;
    }
    return sign * (a > turn / 8 ? sinSeries(kTwoPi * (turn / 4 - a) / turn) : cosSeries(kTwoPi * a / turn));
}

constexpr double sinTurn(int index, int turn) { return cosTurn(index - turn / 4, turn); }

// Natural logarithm of x > 0: x is brought into [0.71, 1.41] by powers of two,
// and ln there is 2 atanh((x - 1) / (x + 1)).
constexpr double ln(double x) {
    int exponent = 0;
    while (x > 1.4142135623730951) {
        x *= 0.5;
        ++exponent;
    }
    while (x < 0.7071067811865476) {
        x *= 2.0;
        --exponent;
    }
    const double z = (x - 1.0) / (x + 1.0);
    double term = z;
    double sum = 0.0;
    for (int k = 0; k < 24; ++k) {
        sum += term / (2 * k + 1);
        term *= z * z;
    }
    return 2.0 * sum + exponent * kLn2;
}

// e^x for moderate x (the reduction below is by repeated subtraction).
constexpr double exp(double x) {
    int doublings = 0;
    while (x > 0.5 * kLn2) {
        x -= kLn2;
        ++doublings;
    }
    while (x < -0.5 * kLn2) {
        x += kLn2;
        --doublings;
    }
    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n <= 22; ++n) {
        term *= x / n;
        sum += term;
    }
    for (; doublings > 0; --doublings) sum *= 2.0;
    for (; doublings < 0; ++doublings) sum *= 0.5;
    return sum;
}

}  // namespace constmath
}  // namespace jacket
