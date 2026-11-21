#pragma once
// ============================================================================
//  The wobble LFO. Evaluated once per 32-sample chunk (the control rate), 0..1.
//  Shapes: sine, triangle, saw down, saw up, square, sample & hold, smooth random.
//  Rate in Hz, or a note division of the host tempo; synced and playing, the
//  phase is locked to the transport's beat position (so every bar wobbles the
//  same way); synced but stopped it free-runs at the division's rate.
//  A one-pole smoother (LFO Smooth) rounds square / S&H edges and the random
//  steps. Twin: vst/webhost/dsp-worklet.js.
// ============================================================================
#include "Common.h"

namespace wub
{
// beats per LFO cycle for each division choice: 1/1 1/2 1/2T 1/4 1/4D 1/4T 1/8 1/8D 1/8T 1/16 1/16D 1/16T 1/32
constexpr int kNumDivs = 13;
constexpr double kDivBeats[kNumDivs] { 4.0, 2.0, 4.0 / 3.0, 1.0, 1.5, 2.0 / 3.0, 0.5, 0.75, 1.0 / 3.0, 0.25, 0.375, 1.0 / 6.0, 0.125 };

inline double fracPart (double x) { return x - std::floor (x); }

struct Lfo
{
    double phase = 0;        // 0..1, the free-running or transport-derived position
    double lastPos = 0;      // position of the previous evaluation (for cycle wrap detection)
    double shValue = 0;      // sample & hold: current value
    double rndFrom = 0, rndTo = 0.5;   // smooth random: the two values being interpolated
    double smoothed = 0;
    bool started = false;
    Rng rng;

    void reset (uint32_t seed)
    {
        phase = 0; lastPos = 0; smoothed = 0; started = false;
        rng.seed (seed);
        shValue = rng.uni();
        rndFrom = rng.uni();
        rndTo = rng.uni();
    }
    void retrigger() { phase = 0; lastPos = 0; }

    // Hz of the LFO for the current settings (sync: from the tempo)
    static double rateHz (bool sync, double rate, int div, double bpm)
    {
        if (! sync) return rate;
        const int d = div < 0 ? 0 : (div >= kNumDivs ? kNumDivs - 1 : div);
        return (bpm / 60.0) / kDivBeats[d];
    }

    // One control-rate step. dt: seconds per step. Returns the smoothed 0..1 value.
    double next (int shape, bool sync, double rate, int div, double phaseDeg, double smooth, double dt,
                 double bpm, double ppq, bool playing)
    {
        const double hz = rateHz (sync, rate, div, bpm);
        if (sync && playing)
        {
            const int d = div < 0 ? 0 : (div >= kNumDivs ? kNumDivs - 1 : div);
            phase = fracPart (ppq / kDivBeats[d]);
        }
        else
        {
            phase = fracPart (phase + hz * dt);
        }
        const double pos = fracPart (phase + phaseDeg / 360.0);
        const bool wrapped = started && pos < lastPos;
        lastPos = pos;
        if (wrapped || ! started)
        {
            if (wrapped) { shValue = rng.uni(); rndFrom = rndTo; rndTo = rng.uni(); }
        }

        double raw;
        switch (shape)
        {
            case 1:  raw = pos < 0.5 ? 2.0 * pos : 2.0 - 2.0 * pos; break;      // triangle
            case 2:  raw = 1.0 - pos; break;                                       // saw down
            case 3:  raw = pos; break;                                             // saw up
            case 4:  raw = pos < 0.5 ? 1.0 : 0.0; break;                           // square
            case 5:  raw = shValue; break;                                         // sample & hold
            case 6:  raw = rndFrom + (rndTo - rndFrom) * pos; break;               // smooth random
            default: raw = 0.5 - 0.5 * std::cos (kTwoPi * pos); break;            // sine (starts low)
        }
        if (! started) { smoothed = raw; started = true; }
        // smooth 0..1 -> time constant 0 .. 250 ms (squared: fine control near 0)
        const double tau = smooth * smooth * 0.25;
        smoothed += tauCoef (tau, dt) * (raw - smoothed);
        return smoothed;
    }
};
} // namespace wub
