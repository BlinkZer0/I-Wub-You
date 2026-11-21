#pragma once
// ============================================================================
//  Harmonic generation for thin sources (phase 2). Header-only, twin in dsp-worklet.js.
//
//  HarmOsc: a PolyBLEP saw / square at the tracked pitch, phase-locked to the
//    input: at a rising zero crossing of the (low-passed) input that comes at
//    least 80% of a period after the last lock, the phase resets to 0, so the
//    oscillator never beats against the source. The sub-octave is a flip-flop
//    toggled at those locks (a square at half the pitch) through a one-pole at
//    the pitch, which makes it close to a sine.
//  shaperResidue: the Shaper mode: the harmonics a soft clip and a rectifier
//    add to the signal, without the signal itself (so Amount adds harmonics
//    instead of replacing the fundamental):
//      odd  = 3 (0.2 tanh(5x) - x)          (3rd, 5th ...)
//      even = 1.2 (|x| - mean|x|)           (2nd, 4th ..., no DC)
// ============================================================================
#include "Common.h"

namespace wub
{
inline double polyBlep (double t, double dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

struct HarmOsc
{
    double phase = 0, inc = 0;
    double prevX = 0;
    int sinceLock = 0;
    double subFlip = 1.0, subLp = 0, subCoef = 0.01;
    double saw = 0, square = 0, sub = 0;   // this sample's outputs

    void reset() { phase = 0; inc = 0; prevX = 0; sinceLock = 0; subFlip = 1.0; subLp = 0; saw = square = sub = 0; }

    // once per chunk: the tracked pitch (Hz; 0 = none)
    void setPitch (double hz, double sr)
    {
        inc = hz > 0 ? hz / sr : 0.0;
        subCoef = onePoleCoef (hz > 0 ? clampd (hz, 20.0, 2000.0) : 100.0, sr);
    }

    // once per sample: x = the (low-passed) mono input the oscillator locks to
    void process (double x)
    {
        ++sinceLock;
        if (inc > 0 && prevX <= 0 && x > 0 && sinceLock >= (int) (0.8 / inc))
        {
            phase = 0;
            sinceLock = 0;
            subFlip = -subFlip;
        }
        prevX = x;
        if (inc > 0)
        {
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            saw = 2.0 * phase - 1.0 - polyBlep (phase, inc);
            double sq = (phase < 0.5 ? 1.0 : -1.0) + polyBlep (phase, inc);
            double t2 = phase + 0.5;
            if (t2 >= 1.0) t2 -= 1.0;
            sq -= polyBlep (t2, inc);
            square = sq;
        }
        else { saw = 0; square = 0; }
        subLp += subCoef * (subFlip - subLp);
        sub = subLp;
    }
};

// Shaper-mode harmonics of one channel: the clip / rectifier residue. meanAbs is tracked by the caller.
inline double shaperResidue (double x, double meanAbs)
{
    const double odd = 3.0 * (0.2 * std::tanh (5.0 * x) - x);
    const double even = 1.2 * (std::abs (x) - meanAbs);
    return odd + even;
}
} // namespace wub
