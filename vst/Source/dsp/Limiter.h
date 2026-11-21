#pragma once
// ============================================================================
//  The output stage (phase 3). Header-only, twin in dsp-worklet.js.
//
//  AutoOutput: matches the wet signal's level to the conditioned input's.
//    Both RMS trackers (1 s) see the signal before ducking and before the
//    dry / wet mix; the gain (input RMS / wet RMS, +-12 dB) is smoothed over 1 s
//    and held while either side is near silent.
//
//  TruePeakLimiter: a stereo-linked lookahead limiter.
//    True peaks come from the signal 4x oversampled with the HIIR half-bands
//    (dsp/Oversampler.h): the largest of the four sub-samples. The gain each
//    sample needs is ceiling / true peak; the limiter's gain follows the minimum
//    of those over the lookahead window (1 ms) with a 0.3 ms attack and a
//    50 ms release, while the audio is delayed by the lookahead. The ceiling is
//    -0.3 dBFS. The safety clipper after it stays as the backstop for the
//    overshoot a one-pole attack leaves.
// ============================================================================
#include "Common.h"
#include "Filters.h"
#include "Oversampler.h"

namespace wub
{
struct AutoOutput
{
    static constexpr double kMaxDb = 12.0;
    RmsTracker inRms, wetRms;
    double gain = 1.0;
    double a = 0.001;

    void prepare (double sr)
    {
        inRms.prepare (1.0, sr);
        wetRms.prepare (1.0, sr);
        gain = 1.0;
        a = tauCoef (1.0, kChunk / sr);
    }
    // once per chunk: the gain to apply to the wet signal (enabled or not, so switching it on is smooth)
    double update (bool enabled)
    {
        const double in = inRms.rms(), wet = wetRms.rms();
        double target = 1.0;
        if (enabled)
        {
            const double lim = dbToGain (kMaxDb);
            target = (in > 1e-4 && wet > 1e-4) ? clampd (in / wet, 1.0 / lim, lim) : gain;
        }
        gain += a * (target - gain);
        return gain;
    }
    void flush()
    {
        if (inRms.ms < 1e-30) inRms.ms = 0;
        if (wetRms.ms < 1e-30) wetRms.ms = 0;
    }
};

struct TruePeakLimiter
{
    static constexpr double kCeiling = 0.9660;       // -0.3 dBFS
    static constexpr double kLookaheadSec = 0.001;
    static constexpr int kMaxLook = 256;             // 1 ms up to 256 kHz

    HalfbandState upA[2], upB[2];
    double delay[2][kMaxLook] {};
    double need[kMaxLook] {};     // the gain each of the last `look` samples needs (a ring)
    int pos = 0, look = 48;
    double env = 1.0, aAttack = 0.1, aRelease = 0.001;
    double reduction = 1.0;       // the smallest gain applied since the last stats read

    static int lookaheadSamples (double sr) { const int n = (int) (kLookaheadSec * sr + 0.5); return n > kMaxLook ? kMaxLook : (n < 1 ? 1 : n); }

    void prepare (double sr)
    {
        look = lookaheadSamples (sr);
        for (int c = 0; c < 2; ++c) { upA[c].reset(); upB[c].reset(); for (auto& d : delay[c]) d = 0; }
        for (auto& n : need) n = 1.0;
        pos = 0;
        env = 1.0;
        aAttack = tauCoef (0.0003, 1.0 / sr);
        aRelease = tauCoef (0.05, 1.0 / sr);
        reduction = 1.0;
    }

    // the true peak of one channel's sample (4x oversampled)
    double truePeak (int c, double x)
    {
        double a0, a1, b0, b1, b2, b3;
        upA[c].up (halfbandA(), x, a0, a1);
        upB[c].up (halfbandB(), a0, b0, b1);
        upB[c].up (halfbandB(), a1, b2, b3);
        double p = std::abs (b0);
        if (std::abs (b1) > p) p = std::abs (b1);
        if (std::abs (b2) > p) p = std::abs (b2);
        if (std::abs (b3) > p) p = std::abs (b3);
        return p;
    }

    // one stereo sample in, the delayed and limited sample out (in place)
    void process (double& L, double& R)
    {
        const double pk = std::max (truePeak (0, L), truePeak (1, R));
        need[pos] = pk > kCeiling ? kCeiling / pk : 1.0;
        double target = 1.0;
        for (int i = 0; i < look; ++i) if (need[i] < target) target = need[i];
        env += (target < env ? aAttack : aRelease) * (target - env);
        if (env < reduction) reduction = env;
        const double outL = delay[0][pos], outR = delay[1][pos];
        delay[0][pos] = L; delay[1][pos] = R;
        pos = (pos + 1) % look;
        L = outL * env;
        R = outR * env;
    }

    double takeReductionDb() { const double r = reduction; reduction = 1.0; return gainToDb (r); }
    void flush()
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 12; ++i)
            {
                if (std::abs (upA[c].x[i]) < 1e-30) upA[c].x[i] = 0;
                if (std::abs (upA[c].y[i]) < 1e-30) upA[c].y[i] = 0;
                if (std::abs (upB[c].x[i]) < 1e-30) upB[c].x[i] = 0;
                if (std::abs (upB[c].y[i]) < 1e-30) upB[c].y[i] = 0;
            }
    }
};
} // namespace wub
