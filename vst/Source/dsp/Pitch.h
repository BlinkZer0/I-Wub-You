#pragma once
// ============================================================================
//  Pitch tracker and source analysis (phase 2). Header-only, twin in dsp-worklet.js.
//
//  PitchTracker: YIN on a decimated copy of the (mono, conditioned) input.
//    The input goes through a 2nd-order low-pass at 2.5 kHz, every Nth sample
//    (N = the sample rate / 12 kHz, rounded: 4 at 44.1 / 48 kHz, 8 at 96 kHz)
//    lands in a 1024-sample ring at about 12 kHz. Every 256 decimated
//    samples (1024 input samples, 21 ms) the difference function
//    d(tau) = sum (x[i] - x[i + tau])^2 over 512 samples is computed for
//    tau = 1 .. 400 (30 Hz .. 1.5 kHz), normalised by its cumulative mean, and
//    the first dip under 0.15 (walked down to its minimum, parabolic refinement)
//    gives the period; confidence = 1 - d'(tau). No dip under the threshold:
//    the global minimum, with its (low) confidence. Silence: confidence 0.
//    Cost: about 200 multiply-adds per input sample.
//
//  Features (for the Auto source type and the "thin source" detector):
//    rmsFreq   the RMS frequency of the signal from the mean square of its first
//              difference (a cheap spectral centroid): f = asin(sqrt(msD / msX) / 2) * sr / pi
//    crest     peak (300 ms release) over RMS (300 ms)
//    lowRatio  energy below 200 Hz over the total
//    variation the coefficient of variation of the 20 ms RMS envelope over 1 s
//              (0 for a steady tone, about 1 for a drum loop)
// ============================================================================
#include "Common.h"

namespace wub
{
struct PitchTracker
{
    static constexpr int kRing = 1024, kWin = 512, kMaxLag = 400, kMinLag = 8, kHop = 256;
    static constexpr double kThreshold = 0.15, kTargetRate = 12000.0;

    Biquad lp;
    double ring[kRing] {};
    double d[kMaxLag + 2] {};
    int decim = 4;                // sample rate / ~12 kHz
    int pos = 0, decimCount = 0, sinceHop = 0;
    int filled = 0;               // decimated samples in the ring so far (no analysis before the window is full)
    double srDec = 12000.0;
    double pitch = 0, conf = 0;   // the last analysis
    double lastLp = 0;            // this sample's low-passed input (the oscillator locks to it)
    bool analysed = false;        // true right after an analysis (cleared by the engine)

    void prepare (double sr)
    {
        lp.setLowpass (2500.0, 0.7071067811865476, sr);
        lp.reset();
        for (auto& r : ring) r = 0;
        pos = decimCount = sinceHop = 0;
        filled = 0;
        decim = (int) (sr / kTargetRate + 0.5);
        if (decim < 1) decim = 1;
        srDec = sr / decim;
        pitch = 0; conf = 0; lastLp = 0; analysed = false;
    }

    void push (double x)
    {
        const double y = lp.process (x);
        lastLp = y;
        if (++decimCount < decim) return;
        decimCount = 0;
        ring[pos] = y;
        pos = (pos + 1) & (kRing - 1);
        if (filled < kWin + kMaxLag) ++filled;
        if (++sinceHop >= kHop) { sinceHop = 0; analyse(); }
    }

    void analyse()
    {
        if (filled < kWin + kMaxLag) return;   // the first 76 ms: nothing to analyse yet
        analysed = true;
        const int start = (pos - (kWin + kMaxLag) + kRing) & (kRing - 1);
        double energy = 0;
        for (int i = 0; i < kWin; ++i) { const double v = ring[(start + i) & (kRing - 1)]; energy += v * v; }
        if (energy < 1e-7 * kWin) { conf = 0; return; }   // -70 dBFS: nothing to track (the pitch is held)

        double running = 0;
        int best = -1, firstDip = -1;
        double bestVal = 1e9;
        for (int tau = 1; tau <= kMaxLag; ++tau)
        {
            double acc = 0;
            for (int i = 0; i < kWin; ++i)
            {
                const double diff = ring[(start + i) & (kRing - 1)] - ring[(start + i + tau) & (kRing - 1)];
                acc += diff * diff;
            }
            running += acc;
            d[tau] = running > 0 ? acc * tau / running : 1.0;
            if (tau >= kMinLag)
            {
                if (firstDip < 0 && d[tau] < kThreshold) firstDip = tau;
                if (d[tau] < bestVal) { bestVal = d[tau]; best = tau; }
            }
        }
        int tau = firstDip;
        if (tau >= 0)
        {
            // walk down to the local minimum of this dip (the lag before the threshold crossing can be lower too)
            while (tau + 1 <= kMaxLag && d[tau + 1] < d[tau]) ++tau;
            while (tau - 1 >= kMinLag && d[tau - 1] < d[tau]) --tau;
        }
        else tau = best;
        if (tau < kMinLag) { conf = 0; return; }
        double period = tau;
        if (tau > kMinLag && tau < kMaxLag)
        {
            const double a = d[tau - 1], b = d[tau], c = d[tau + 1];
            const double den = a - 2.0 * b + c;
            if (den > 1e-12) period = tau + 0.5 * (a - c) / den;
        }
        pitch = srDec / period;
        conf = clampd (1.0 - d[tau], 0.0, 1.0);
    }
};

// Running features of the conditioned mono input.
struct SourceFeatures
{
    double msX = 0, msD = 0, prev = 0, aFast = 0.001;    // 100 ms mean squares of x and of its first difference
    double peak = 0, ms300 = 0, aPeakDown = 0.001, a300 = 0.001;
    double lowState = 0, aLow = 0.03, msLow = 0;         // one-pole at 200 Hz, its mean square (300 ms)
    double ms20 = 0, a20 = 0.001, envMean = 0, envMs = 0, a1s = 0.0001;   // the 20 ms envelope and its 1 s statistics
    bool started = false;   // the statistics start from the envelope after 100 ms (its own ramp from 0 would look like variation)
    int startCount = 0, startAfter = 4800;
    double sr = 48000;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        aFast = tauCoef (0.1, 1.0 / sr);
        aPeakDown = tauCoef (0.3, 1.0 / sr);
        a300 = tauCoef (0.3, 1.0 / sr);
        aLow = onePoleCoef (200.0, sr);
        a20 = tauCoef (0.02, 1.0 / sr);
        a1s = tauCoef (1.0, 1.0 / sr);
        msX = msD = prev = peak = ms300 = lowState = msLow = ms20 = envMean = envMs = 0;
        started = false; startCount = 0; startAfter = (int) (0.1 * sr);
    }
    void push (double x)
    {
        const double dx = x - prev;
        prev = x;
        msX += aFast * (x * x - msX);
        msD += aFast * (dx * dx - msD);
        const double a = std::abs (x);
        if (a > peak) peak = a; else peak += aPeakDown * (a - peak);
        ms300 += a300 * (x * x - ms300);
        lowState += aLow * (x - lowState);
        msLow += a300 * (lowState * lowState - msLow);
        ms20 += a20 * (x * x - ms20);
        const double e = std::sqrt (ms20);
        if (! started) { if (++startCount >= startAfter) { envMean = e; envMs = e * e; started = true; } }
        else
        {
            envMean += a1s * (e - envMean);
            envMs += a1s * (e * e - envMs);
        }
    }
    double variation() const
    {
        if (envMean < 1e-6) return 0.0;
        return std::sqrt (std::max (envMs - envMean * envMean, 0.0)) / envMean;
    }
    double rmsFreq() const
    {
        if (msX < 1e-12) return 0.0;
        const double r = clampd (std::sqrt (msD / msX) * 0.5, 0.0, 1.0);
        return 2.0 * std::asin (r) * sr / kTwoPi;
    }
    double crest() const { return ms300 > 1e-12 ? peak / std::sqrt (ms300) : 0.0; }
    double lowRatio() const { return ms300 > 1e-12 ? clampd (msLow / ms300, 0.0, 1.0) : 0.0; }
    double rms() const { return std::sqrt (ms300); }
    void flush()
    {
        if (msX < 1e-30) msX = 0;
        if (msD < 1e-30) msD = 0;
        if (ms300 < 1e-30) ms300 = 0;
        if (msLow < 1e-30) msLow = 0;
        if (std::abs (lowState) < 1e-30) lowState = 0;
        if (peak < 1e-30) peak = 0;
        if (ms20 < 1e-30) ms20 = 0;
        if (envMean < 1e-30) envMean = 0;
        if (envMs < 1e-30) envMs = 0;
    }
};

// The Auto source type: 0 Bass, 1 Vocal, 2 Guitar, 3 Drums, 4 Pad, 5 Full Mix (= the source choice - 1).
//   confAvg   the pitch confidence averaged over a second
//   pitch     the tracked pitch (Hz)
//   variation the envelope's coefficient of variation (SourceFeatures::variation)
//   crest     peak over RMS (300 ms); lowRatio: energy below 200 Hz over the total
inline int classifySource (double confAvg, double pitch, double rmsFreq, double variation, double crest, double lowRatio)
{
    if (variation > 1.2) return 3;                            // hits with gaps: drums (a kick's tail may read as a pitch)
    if (variation > 0.6 && confAvg < 0.6) return 3;          // moving, no steady pitch: drums
    if (confAvg > 0.6)
    {
        if (pitch < 130.0) return 0;                          // a pitched low source: bass (sine or saw alike)
        if (pitch < 1000.0) return crest > 5.0 ? 2 : 1;       // plucked (high crest): guitar, else vocal
        return 1;
    }
    if (rmsFreq < 300.0 && lowRatio > 0.6) return 0;          // unpitched but all low: bass
    return variation < 0.25 ? 4 : 5;                          // dense and steady: pad; dense and moving: a mix
}
} // namespace wub
