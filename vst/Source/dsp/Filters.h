#pragma once
// ============================================================================
//  Filters and followers of the wobble chain. Header-only, no JUCE.
//  Twins in vst/webhost/dsp-worklet.js.
//
//  Svf        Zavalishin's zero-delay-feedback state-variable filter (TPT), the
//             wobble filter: low-pass, band-pass, high-pass and notch from one
//             structure; stable for any cutoff / resonance ramp.
//  Formant    three band-pass SVFs at the first three formants of a vowel,
//             morphed A -> E -> I -> O -> U by the modulation position.
//  Crossover  Linkwitz-Riley 4th order (two cascaded Butterworth biquads), with
//             the low band passed through the upper crossover's all-pass so the
//             three bands sum flat.
//  DcBlocker  one-pole high-pass at 5 Hz.
//  EnvFollower peak follower (attack / release in seconds).
//  RmsTracker one-pole mean square, windowed by a time constant.
//  DelayLine  a few samples, for the dry path (latency compensation).
// ============================================================================
#include "Common.h"

namespace wub
{
// ---------------------------------------------------------------------------
struct Svf
{
    double ic1 = 0, ic2 = 0;
    void reset() { ic1 = ic2 = 0; }
    // g = tan (pi fc / sr), k = 1 / Q. Returns the four responses of this sample.
    struct Out { double lp, bp, hp, notch; };
    Out process (double x, double g, double k)
    {
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        Out o;
        o.lp = v2;
        o.bp = v1 * k;            // unity peak gain
        o.hp = x - k * v1 - v2;
        o.notch = x - k * v1;
        return o;
    }
};

inline double svfG (double fc, double sr) { return std::tan (kPi * clampd (fc, 10.0, 0.45 * sr) / sr); }

// Resonance 0..1 -> Q 0.5 .. 20 (log)
inline double resoToQ (double reso) { return 0.5 * std::pow (10.0, clampd (reso, 0.0, 1.0) * 1.60206); }

// ---------------------------------------------------------------------------
// Vowel formants (Hz) and relative amplitudes of the first three formants: A E I O U.
constexpr int kNumVowels = 5;
constexpr double kFormantHz[kNumVowels][3] {
    { 800, 1150, 2900 }, { 400, 1600, 2700 }, { 350, 1700, 2700 }, { 450, 800, 2830 }, { 325, 700, 2700 } };
constexpr double kFormantAmp[kNumVowels][3] {
    { 1.0, 0.63, 0.10 }, { 1.0, 0.25, 0.03 }, { 1.0, 0.20, 0.03 }, { 1.0, 0.50, 0.05 }, { 1.0, 0.30, 0.03 } };

// morph 0..1 across the five vowels: frequency (log-interpolated) and amplitude of formant i
inline void formantAt (double morph, int i, double& hz, double& amp)
{
    const double m = clampd (morph, 0.0, 1.0) * (kNumVowels - 1);
    int v0 = (int) m;
    if (v0 > kNumVowels - 2) v0 = kNumVowels - 2;
    const double t = m - v0;
    hz  = kFormantHz[v0][i] * std::pow (kFormantHz[v0 + 1][i] / kFormantHz[v0][i], t);
    amp = kFormantAmp[v0][i] + (kFormantAmp[v0 + 1][i] - kFormantAmp[v0][i]) * t;
}

// ---------------------------------------------------------------------------
// Linkwitz-Riley 4th order crossover pair at one frequency: LP4 and HP4 (two Butterworth
// biquads each), and the all-pass LP4 + HP4 for the band that does not pass this crossover.
struct Lr4
{
    Biquad lp1, lp2, hp1, hp2;
    void set (double fc, double sr)
    {
        const double q = 0.7071067811865476;
        lp1.setLowpass (fc, q, sr); lp2.copyCoefs (lp1);
        hp1.setHighpass (fc, q, sr); hp2.copyCoefs (hp1);
    }
    void reset() { lp1.reset(); lp2.reset(); hp1.reset(); hp2.reset(); }
    void split (double x, double& lo, double& hi)
    {
        lo = lp2.process (lp1.process (x));
        hi = hp2.process (hp1.process (x));
    }
    double allpass (double x) { double lo, hi; split (x, lo, hi); return lo + hi; }
};

// ---------------------------------------------------------------------------
struct DcBlocker
{
    double x1 = 0, y1 = 0, r = 0.9993;
    void prepare (double sr) { r = 1.0 - kTwoPi * 5.0 / sr; x1 = y1 = 0; }
    double process (double x) { const double y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
};

struct EnvFollower
{
    double env = 0, aUp = 1, aDown = 0.01;
    void prepare (double attackSec, double releaseSec, double sr)
    {
        aUp = tauCoef (attackSec, 1.0 / sr);
        aDown = tauCoef (releaseSec, 1.0 / sr);
        env = 0;
    }
    double process (double x)
    {
        const double a = std::abs (x);
        env += (a > env ? aUp : aDown) * (a - env);
        return env;
    }
};

struct RmsTracker
{
    double ms = 0, a = 0.001;
    void prepare (double windowSec, double sr) { a = tauCoef (windowSec, 1.0 / sr); ms = 0; }
    void push (double x) { ms += a * (x * x - ms); }
    double rms() const { return std::sqrt (ms); }
};

// Up to 15 samples of delay (the oversampling latency is 0 / 3 / 4).
struct DelayLine
{
    double buf[16] {};
    int pos = 0;
    void reset() { for (auto& b : buf) b = 0; pos = 0; }
    double process (double x, int delay)
    {
        buf[pos] = x;
        const double y = buf[(pos - delay + 16) & 15];
        pos = (pos + 1) & 15;
        return y;
    }
};
} // namespace wub
