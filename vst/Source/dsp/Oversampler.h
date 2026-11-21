#pragma once
// ============================================================================
//  Oversampling and the drive shaper.
//
//  Half-band filters: polyphase IIR (two all-pass chains), designed at start-up
//  from a closed formula, so the JS engine computes the same coefficients:
//  the Valenzuela-Constantinides design as published in Laurent de Soras' HIIR
//  library (WTFPL), as in The Ham. Stage A (base <-> 2x): 8 coefficients,
//  transition 0.04: flat to 20 kHz at 48 kHz, images / aliases from 28 kHz down
//  by 99 dB. Stage B (2x <-> 4x): 4 coefficients, transition 0.14: -82 dB from
//  72 kHz. Not linear phase: the delay of a 200 Hz tone is 3.3 samples at 2x and
//  4.2 at 4x (The Ham's measurement, same filters); kLatency2x / kLatency4x
//  round them, the engine delays the dry path by them and the plugin reports
//  them to the host.
//
//  The shaper is f(x) = 4 tanh(x / 4): linear to within 0.02 dB at the engine's
//  working level (-18 dBFS) with Drive at 0, a hard clip at Drive 100% (36 dB
//  into it). At 2x and 4x it runs with first-order ADAA (antiderivative
//  anti-aliasing: the average of f over the step between two samples,
//  F(x) = 16 ln cosh(x / 4)), which also tames what the half-bands let through.
//  At 1x it is the plain function: ADAA's average of two samples would be a
//  3 dB loss at 12 kHz at the base rate, so 1x trades aliasing for a flat
//  response (pick 2x or 4x for bright sources with heavy drive).
//  The output is scaled by 1 / sqrt(pre-gain) (a compromise between "louder as
//  it clips" and level matching; Auto Output Gain, phase 3, does the rest).
// ============================================================================
#include "Common.h"

namespace wub
{
struct HalfbandDesign
{
    int n = 0;
    double c[12] {};
    HalfbandDesign (int numCoefs, double transition) : n (numCoefs)
    {
        const int order = n * 2 + 1;
        double k = std::tan ((1.0 - 2.0 * transition) * kPi / 4.0);
        k *= k;
        const double kk = std::sqrt (std::sqrt (1.0 - k * k));
        const double e = 0.5 * (1.0 - kk) / (1.0 + kk);
        const double e4 = e * e * e * e;
        const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
        for (int idx = 0; idx < n; ++idx)
        {
            const int cc = idx + 1;
            double num = 0, den = 0, t;
            int i = 0, j = 1;
            do { t = std::pow (q, i * (i + 1)) * std::sin ((i * 2 + 1) * cc * kPi / order) * j; num += t; j = -j; ++i; } while (std::abs (t) > 1e-100);
            num *= std::pow (q, 0.25);
            i = 1; j = -1;
            do { t = std::pow (q, i * i) * std::cos (i * 2 * cc * kPi / order) * j; den += t; j = -j; ++i; } while (std::abs (t) > 1e-100);
            den += 0.5;
            const double ww = num / den, w2 = ww * ww;
            const double x = std::sqrt ((1.0 - w2 * k) * (1.0 - w2 / k)) / (1.0 + w2);
            c[idx] = (1.0 - x) / (1.0 + x);
        }
    }
};

inline const HalfbandDesign& halfbandA() { static const HalfbandDesign d (8, 0.04); return d; }
inline const HalfbandDesign& halfbandB() { static const HalfbandDesign d (4, 0.14); return d; }

// Two all-pass chains (even / odd coefficients) run side by side, as in HIIR's StageProc.
struct HalfbandState
{
    double x[12] {}, y[12] {};
    void reset() { for (int i = 0; i < 12; ++i) x[i] = y[i] = 0; }
    void run (const HalfbandDesign& d, double& s0, double& s1)
    {
        int i = 0;
        for (; i + 1 < d.n; i += 2)
        {
            const double t0 = (s0 - y[i]) * d.c[i] + x[i];
            const double t1 = (s1 - y[i + 1]) * d.c[i + 1] + x[i + 1];
            x[i] = s0; x[i + 1] = s1;
            y[i] = t0; y[i + 1] = t1;
            s0 = t0; s1 = t1;
        }
        if (i < d.n)
        {
            const double t0 = (s0 - y[i]) * d.c[i] + x[i];
            x[i] = s0; y[i] = t0; s0 = t0;
        }
    }
    // one input -> two outputs at twice the rate
    void up (const HalfbandDesign& d, double in, double& out0, double& out1) { out0 = in; out1 = in; run (d, out0, out1); }
    // two inputs -> one output at half the rate
    double down (const HalfbandDesign& d, double in0, double in1)
    {
        double s0 = in1, s1 = in0;
        run (d, s0, s1);
        return 0.5 * (s0 + s1);
    }
};

// The shaper f(x) = 4 tanh(x / 4), with first-order ADAA when `process` is used (2x / 4x) and
// plain when `direct` is used (1x).
struct TanhShaper
{
    double x1 = 0.0;
    void reset() { x1 = 0.0; }
    static double f (double x) { return 4.0 * std::tanh (0.25 * x); }
    // F(x) = 16 ln cosh(x / 4) = 16 (|x| / 4 + ln(1 + e^(-|x| / 2)) - ln 2)
    static double F (double x) { const double a = std::abs (x); return 16.0 * (0.25 * a + std::log (1.0 + std::exp (-0.5 * a)) - 0.6931471805599453); }
    double process (double x)
    {
        const double d = x - x1;
        double y;
        if (std::abs (d) < 1e-6) y = f (0.5 * (x + x1));
        else y = (F (x) - F (x1)) / d;
        x1 = x;
        return y;
    }
    double direct (double x) { x1 = x; return f (x); }
};

// The shaper at 1x, 2x or 4x.
struct OversampledShaper
{
    // Delay (base-rate samples, rounded) of the up / down filters (The Ham's measurement of these filters).
    static constexpr int kLatency2x = 3, kLatency4x = 4;

    HalfbandState upA, upB, downA, downB;
    TanhShaper s1, s2, s4;
    void reset() { upA.reset(); upB.reset(); downA.reset(); downB.reset(); s1.reset(); s2.reset(); s4.reset(); }
    static int latency (int factor) { return factor >= 2 ? kLatency4x : factor == 1 ? kLatency2x : 0; } // factor as the os choice: 0 = 1x, 1 = 2x, 2 = 4x

    // osChoice: 0 = 1x, 1 = 2x, 2 = 4x
    double process (double x, int osChoice)
    {
        if (osChoice <= 0) return s1.direct (x);
        const auto& A = halfbandA();
        double a0, a1;
        upA.up (A, x, a0, a1);
        if (osChoice == 1)
        {
            const double y0 = s2.process (a0);
            const double y1 = s2.process (a1);
            return downA.down (A, y0, y1);
        }
        const auto& B = halfbandB();
        double b0, b1, b2, b3;
        upB.up (B, a0, b0, b1);
        upB.up (B, a1, b2, b3);
        // one statement per shaper call: function arguments have no evaluation order in C++,
        // and the ADAA needs its samples in sequence
        const double y0 = s4.process (b0);
        const double y1 = s4.process (b1);
        const double y2 = s4.process (b2);
        const double y3 = s4.process (b3);
        const double c0 = downB.down (B, y0, y1);
        const double c1 = downB.down (B, y2, y3);
        return downA.down (A, c0, c1);
    }
};

// Drive 0..1 -> gain into the tanh (0..36 dB) and the make-up after it.
inline double drivePreGain (double drive) { return dbToGain (36.0 * clampd (drive, 0.0, 1.0)); }
inline double driveMakeup (double pre) { return 1.0 / std::sqrt (pre); }
} // namespace wub
