#pragma once
// ============================================================================
//  Shared DSP building blocks. Header-only, no JUCE, no allocation.
//  Everything here has a line-for-line twin in vst/webhost/dsp-worklet.js,
//  so keep it plain: doubles, std:: maths only, no SIMD or fast-math tricks.
//  Same helpers as The Ham's dsp/Common.h.
// ============================================================================
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace wub
{
constexpr double kPi     = 3.141592653589793;
constexpr double kTwoPi  = 6.283185307179586;
constexpr int    kChunk  = 32;  // control rate: modulation once per 32 samples, per-sample ramps in between

inline double clampd (double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double dbToGain (double db) { return std::pow (10.0, db / 20.0); }
inline double gainToDb (double g) { return 20.0 * std::log10 (std::max (g, 1e-12)); }

// Deterministic RNG (identical sequence in the JS engine: Math.imul LCG). Same as The Ham.
struct Rng
{
    uint32_t s = 0x2545F491u;
    void seed (uint32_t v) { s = v; }
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
    double uni() { return (double) (next() >> 8) * (1.0 / 16777216.0); } // [0, 1)
    double bi()  { return uni() * 2.0 - 1.0; }                            // [-1, 1)
};

// Output safety clipper (Brass Knuckles'): exactly linear up to 0.8 (-1.9 dBFS), then a tanh
// knee that never exceeds 1. Below the knee it adds no distortion and no aliasing.
inline double safetyClip (double x)
{
    const double a = std::abs (x);
    if (a <= 0.8) return x;
    const double y = 0.8 + 0.2 * std::tanh ((a - 0.8) / 0.2);
    return x < 0 ? -y : y;
}

// A value ramped per sample across one 32-sample chunk (set once per chunk).
struct ChunkRamp
{
    double cur = 0, target = 0, inc = 0;
    void set (double t) { cur = target; target = t; inc = (t - cur) / kChunk; }
    void jump (double t) { cur = target = t; inc = 0; }
    double next() { const double v = cur; cur += inc; return v; }
};

// Biquad, transposed direct form II; designs from the RBJ cookbook.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, s1 = 0, s2 = 0;
    double process (double x)
    {
        const double y = b0 * x + s1;
        s1 = b1 * x - a1 * y + s2;
        s2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { s1 = s2 = 0; }
    void copyCoefs (const Biquad& o) { b0 = o.b0; b1 = o.b1; b2 = o.b2; a1 = o.a1; a2 = o.a2; }
    void setLowpass (double f0, double q, double sr)
    {
        const double w = kTwoPi * f0 / sr, alpha = std::sin (w) / (2.0 * q), cw = std::cos (w), a0 = 1.0 + alpha;
        b0 = (1.0 - cw) / 2.0 / a0; b1 = (1.0 - cw) / a0; b2 = b0;
        a1 = -2.0 * cw / a0; a2 = (1.0 - alpha) / a0;
    }
    void setHighpass (double f0, double q, double sr)
    {
        const double w = kTwoPi * f0 / sr, alpha = std::sin (w) / (2.0 * q), cw = std::cos (w), a0 = 1.0 + alpha;
        b0 = (1.0 + cw) / 2.0 / a0; b1 = -(1.0 + cw) / a0; b2 = b0;
        a1 = -2.0 * cw / a0; a2 = (1.0 - alpha) / a0;
    }
};

// One-pole low-pass: y += a (x - y), a from the corner frequency.
inline double onePoleCoef (double hz, double sr) { return 1.0 - std::exp (-kTwoPi * hz / sr); }
// One-pole coefficient from a time constant (seconds) and an update interval (seconds).
inline double tauCoef (double tau, double dt) { return tau <= 0 ? 1.0 : 1.0 - std::exp (-dt / tau); }
} // namespace wub
