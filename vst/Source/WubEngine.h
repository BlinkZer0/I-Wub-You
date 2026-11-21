#pragma once
// ============================================================================
//  The I Wub You engine. Header-only, no JUCE, no allocation after prepare(), no locks.
//  Double precision. Line-for-line twin: vst/webhost/dsp-worklet.js (checked by tests/run-parity.ps1).
//
//  Signal path (phase 2):
//    stereo mode (Stereo: L and R · Mono Sum: (L + R) / 2 once · Mid-Side: M and S as the two channels)
//    -> DC blocker -> input gain x auto gain (the RMS before any gain, tracked to -18 dBFS, +-24 dB, 0.4 s)
//    -> the mono sum of the conditioned channels feeds the analysis: the YIN pitch tracker (dsp/Pitch.h),
//       the features (RMS frequency, crest, low ratio) for the Auto source type and the thin-source detector,
//       the harmonic oscillator's phase lock
//    -> harmonics (Harmonics on): + blend x (Shaper residue of this channel | tracked saw | tracked square) x peak env,
//       + sub blend x sub-octave x peak env; blend = Amount, or Amount x thinness in Auto mode (thinness from the
//       RMS frequency over the pitch: 1 for a sine, 0 for a saw), gated by the pitch confidence
//    -> Single band, or Linkwitz-Riley 4th-order split at Crossover Low / High (the low band
//       goes through the upper crossover's all-pass so the bands sum flat)
//    -> per band: drive (tanh, ADAA at 2x / 4x, plain at 1x; Drive 0..36 dB in, 1 / sqrt(pre) after)
//       -> tone (one-pole 800 Hz .. 20 kHz) -> wobble filter (ZDF SVF: LP / BP / HP / notch, or three formant
//       band-passes morphing A E I O U), cutoff swept between Cutoff Low and High (both shifted by
//       (pitch / 110 Hz) ^ Key Track; the held MIDI note wins over the tracked pitch) by the LFO x the band's Depth;
//       drive and filter in either order (Drive Position); resonance Q = 0.5 .. 20, divided down once the band's
//       peak x Q would pass -6 dBFS (Resonance Limit) -> band level
//    -> sum -> transient preserve: + Transients x attack x (dry - wet), attack = how far a peak follower
//       (instant attack, 20 ms release) leads its own 60 ms lagged copy
//    -> auto output gain (Auto Output on: the wet RMS matched to the input's, +-12 dB, 1 s; measured before ducking)
//    -> ducking (1 - Duck x (1 - LFO)) -> dry / wet (the dry path delayed by the oversampling latency)
//    -> back to L / R -> mono below (side high-passed, LR4) -> output gain
//    -> true-peak limiter (Limiter on: 4x oversampled peaks, 1 ms lookahead, -0.3 dBFS ceiling; the lookahead is
//       reported as latency) -> safety clip (linear below -1.9 dBFS) -> L, R.
//  Everything modulated is evaluated once per 32-sample chunk (kChunk) and ramped per sample.
//  Each channel has its own LFO (the right one offset by LFO Stereo Offset).
// ============================================================================
#include "dsp/Common.h"
#include "dsp/Filters.h"
#include "dsp/Oversampler.h"
#include "dsp/Lfo.h"
#include "dsp/Pitch.h"
#include "dsp/Harmonics.h"
#include "dsp/Limiter.h"

namespace wub
{
// Snapshot of everything the engine needs from the parameters (EngineParamsFromValues.h).
struct EngineParams
{
    int source = 0;
    bool autoIn = true;
    double inGain = 1.0;                       // linear
    // harmonics
    bool harmOn = false;
    int harmMode = 0, harmShape = 0;
    double harmAmount = 0.5;
    bool subOn = false;
    double subAmount = 0.5;
    // bands
    int bands = 0;                             // 0 single, 1 three-band
    double xLow = 200, xHigh = 2500;
    int os = 1;                                // 0 1x, 1 2x, 2 4x
    int drivePos = 0;                          // 0 pre filter, 1 post filter
    bool wob[3] { true, true, false };
    double depth[3] { 1, 1, 1 }, drive[3] { 0.3, 0.3, 0.3 }, tone[3] { 0.7, 0.7, 0.7 }, level[3] { 1, 1, 1 }; // level linear
    // wobble filter
    int filterType = 0;
    double cutoffLo = 120, cutoffHi = 4000, reso = 0.5, resoLimit = 0.5, keyTrack = 0;
    // LFO
    int lfoShape = 0;
    bool lfoSync = true;
    double lfoRate = 2;
    int lfoDiv = 6;
    double lfoPhase = 0, lfoStereo = 0, lfoSmooth = 0.1;
    int lfoRetrig = 0;
    // transient
    double transPreserve = 0;
    // stereo
    int stereoMode = 0;
    double monoBelow = 0;
    // output
    double duck = 0, mix = 1, outGain = 1;     // outGain linear
    bool autoOut = false, limiter = true;
};

class WubEngine
{
public:
    static constexpr double kRmsTarget = 0.12589254117941673;   // -18 dBFS
    static constexpr double kAutoGainMax = 15.848931924611133;  // +24 dB
    static constexpr double kAutoGainTau = 0.4;                 // seconds
    static constexpr double kResoLimitPeak = 0.5;               // the resonant peak (band peak x Q) the limit allows: -6 dBFS
    static constexpr double kKeyRef = 110.0;                    // key tracking: the pitch at which the sweep range is as set
    static constexpr int kDetectHold = 70;                      // analyses (x 1024 samples: 1.5 s at 48 kHz) a type must persist to replace another
    static constexpr int kDetectFirst = 48;                     // analyses (1 s) before the first decision

    // the oversampling filters' delay (the dry path is delayed to match it inside the engine)
    static int latencySamples (int osChoice) { return OversampledShaper::latency (osChoice); }
    // what the host must be told: the oversampling delay plus the limiter's lookahead when it is on
    static int totalLatency (int osChoice, bool limiterOn, double sr) { return latencySamples (osChoice) + (limiterOn ? TruePeakLimiter::lookaheadSamples (sr) : 0); }
    int totalLatency() const { return totalLatency (params.os, params.limiter, sr); }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        autoOut.prepare (sr);
        limiter.prepare (sr);
        autoOutRamp.jump (1.0);
        inRmsShared = 0;
        for (int c = 0; c < 2; ++c)
        {
            auto& C = ch[c];
            C.dc.prepare (sr);
            C.xa.reset(); C.xb.reset(); C.apb.reset();
            C.lfo.reset (0x57554231u + (uint32_t) c);   // "WUB1", "WUB2"
            C.dry.reset();
            C.duckGain.jump (1.0);
            C.lfoVal = 0;
            C.meanAbs = 0;
            C.fastEnv.prepare (0.0, 0.02, sr);
            C.slowEnv = 0; C.aSlow = tauCoef (0.06, 1.0 / sr);
            C.transGain.jump (0.0);
            for (int b = 0; b < 3; ++b)
            {
                auto& B = C.band[b];
                B.shaper.reset();
                B.toneState = 0;
                B.toneCoef = 1.0;
                for (auto& f : B.svf) f.reset();
                B.env.prepare (0.0, 0.12, sr);   // a true peak follower: instant attack, 120 ms release
                B.g.jump (svfG (1000.0, sr)); B.k.jump (1.0);
                B.pre.jump (1.0); B.makeup.jump (1.0); B.level.jump (1.0);
                for (int i = 0; i < 3; ++i) { B.fg[i].jump (svfG (1000.0, sr)); B.famp[i].jump (0.0); }
                B.fk = 0.1;
            }
        }
        monoSplit.reset();
        rms.prepare (0.3, sr);
        agGain = 1.0;
        inGainRamp.jump (params.inGain);
        outGainRamp.jump (params.outGain);
        mixRamp.jump (params.mix);
        lastXLow = lastXHigh = lastMono = -1.0;
        chunkLeft = 0;
        bpm = 120; ppq = 0; playing = false;
        peakIn = peakOut = 0;
        for (auto& c : cutoffStat) c = 0;
        lastNote = -1;
        // analysis
        dcMono.prepare (sr);
        tracker.prepare (sr);
        features.prepare (sr);
        osc.reset();
        harmEnv.prepare (0.0, 0.05, sr);
        pitchSm = kKeyRef; confSm = 0; confAvg = 0; confStarted = false; thinSm = 0; gateSm = 0; ktLog = 0;
        aMeanAbs = tauCoef (0.05, 1.0 / sr);
        harmRamp.jump (0.0); subRamp.jump (0.0);
        detected = -1; candidate = -1; candidateCount = 0;
        updateCrossovers();
    }

    void setParams (const EngineParams& p) { params = p; }

    // Host transport, once per block (the engine advances ppq itself within the block).
    void setTransport (double bpmIn, double ppqIn, bool playingIn)
    {
        bpm = bpmIn > 1.0 ? bpmIn : 120.0;
        ppq = ppqIn;
        playing = playingIn;
    }
    // The tempo alone (the web build has no transport: its ppq clock keeps running inside the engine).
    void setTempo (double bpmIn) { bpm = bpmIn > 1.0 ? bpmIn : 120.0; }

    void noteOn (int note, int /*vel*/)
    {
        lastNote = note;
        if (params.lfoRetrig == 1) retrigger();
    }
    void noteOff (int note) { if (note == lastNote) lastNote = -1; }
    void retrigger() { ch[0].lfo.retrigger(); ch[1].lfo.retrigger(); }

    // ---- stats (for the UI) ----
    double takePeakIn()  { const double p = peakIn;  peakIn = 0;  return p; }
    double takePeakOut() { const double p = peakOut; peakOut = 0; return p; }
    double lfoValue (int c) const { return ch[c & 1].lfoVal; }
    double cutoffHz (int band) const { return cutoffStat[band < 0 ? 0 : (band > 2 ? 2 : band)]; }
    double autoGainDb() const { return gainToDb (agGain); }
    int detectedSource() const { return detected; }           // 0 Bass .. 5 Full Mix (the source choice - 1), -1 undecided
    bool bandActive (int b) const { return params.bands == 1 || b == 1; }
    int heldNote() const { return lastNote; }
    double pitchHz() const { return tracker.pitch; }
    double pitchConfidence() const { return confSm; }
    double harmonicsBlend() const { return harmRamp.target; }   // how much harmonic content is being added (0..1)
    double rmsFrequency() const { return features.rmsFreq(); }
    double crestFactor() const { return features.crest(); }
    double envelopeVariation() const { return features.variation(); }
    double averageConfidence() const { return confAvg; }
    double autoOutputDb() const { return gainToDb (autoOut.gain); }        // the auto output gain being applied
    double takeLimiterReductionDb() { return limiter.takeReductionDb(); }  // the deepest gain reduction since the last read (<= 0)

    // inR may be null (mono input: the left channel feeds both sides). outR may be null.
    void render (const float* inL, const float* inR, float* outL, float* outR, int n)
    {
        const int lat = latencySamples (params.os);
        for (int i = 0; i < n; ++i)
        {
            if (chunkLeft == 0) { control(); chunkLeft = kChunk; }
            --chunkLeft;

            const double iL = inL[i], iR = inR != nullptr ? inR[i] : iL;
            double xin[2];
            if (params.stereoMode == 1)      { xin[0] = xin[1] = 0.5 * (iL + iR); }
            else if (params.stereoMode == 2) { xin[0] = 0.5 * (iL + iR); xin[1] = 0.5 * (iL - iR); }
            else                             { xin[0] = iL; xin[1] = iR; }

            const double gIn = inGainRamp.next();
            const double mixv = mixRamp.next();
            const double gOut = outGainRamp.next();
            const double harmB = harmRamp.next();
            const double subB = subRamp.next();
            const double gAuto = autoOutRamp.next();

            // the analysis signal: the conditioned mono input
            const double xm = dcMono.process (0.5 * (iL + iR)) * gIn;
            tracker.push (xm);
            features.push (xm);
            const double env = harmEnv.process (xm);
            osc.process (tracker.lastLp);   // the tracker's low-passed input locks the oscillator
            const double gen = params.harmShape == 1 ? osc.saw : (params.harmShape == 2 ? osc.square : 0.0);

            const int channels = params.stereoMode == 1 ? 1 : 2;
            double o[2] { 0, 0 };
            for (int c = 0; c < channels; ++c)
            {
                auto& C = ch[c];
                const double raw = C.dc.process (xin[c]);
                rms.push (raw);                      // the auto gain tracks the level before any gain
                double x = raw * gIn;
                // harmonics
                C.meanAbs += aMeanAbs * (std::abs (x) - C.meanAbs);
                if (harmB > 0)
                    x += harmB * (params.harmShape == 0 ? shaperResidue (x, C.meanAbs) : gen * env);
                if (subB > 0) x += subB * osc.sub * env;
                const double ax = std::abs (x);
                if (ax > peakIn) peakIn = ax;
                C.slowEnv += C.aSlow * (C.fastEnv.process (x) - C.slowEnv);

                double bandIn[3] { 0, 0, 0 };
                if (params.bands == 1)
                {
                    double lo, rest, mid, hi;
                    C.xa.split (x, lo, rest);
                    C.xb.split (rest, mid, hi);
                    bandIn[0] = C.apb.allpass (lo);
                    bandIn[1] = mid;
                    bandIn[2] = hi;
                }
                else bandIn[1] = x;

                double sum = 0;
                for (int b = 0; b < 3; ++b)
                {
                    auto& B = C.band[b];
                    const double g = B.g.next(), k = B.k.next(), pre = B.pre.next(), mk = B.makeup.next(), lv = B.level.next();
                    double fg[3], fa[3];
                    for (int f = 0; f < 3; ++f) { fg[f] = B.fg[f].next(); fa[f] = B.famp[f].next(); }
                    if (! bandActive (b)) continue;
                    double y = bandIn[b];
                    B.env.process (y);
                    if (params.drivePos == 0) y = drive (B, y, pre, mk);
                    if (params.wob[b]) y = filter (B, y, g, k, fg, fa);
                    if (params.drivePos == 1) y = drive (B, y, pre, mk);
                    sum += y * lv;
                }
                const double dry = C.dry.process (x, lat);
                const double tg = C.transGain.next();
                const double wetPre = sum + tg * (dry - sum);
                autoOut.inRms.push (x);
                autoOut.wetRms.push (wetPre);
                const double wet = wetPre * gAuto * C.duckGain.next();
                o[c] = dry + (wet - dry) * mixv;
            }
            if (channels == 1) o[1] = o[0];

            double L, R;
            if (params.stereoMode == 2) { L = o[0] + o[1]; R = o[0] - o[1]; }
            else { L = o[0]; R = o[1]; }
            if (params.monoBelow >= 1.0)
            {
                const double m = 0.5 * (L + R);
                double lo, hi;
                monoSplit.split (0.5 * (L - R), lo, hi);
                L = m + hi; R = m - hi;
            }
            L *= gOut;
            R *= gOut;
            if (params.limiter) limiter.process (L, R);
            L = safetyClip (L);
            R = safetyClip (R);
            const double aL = std::abs (L), aR = std::abs (R);
            if (aL > peakOut) peakOut = aL;
            if (aR > peakOut) peakOut = aR;
            outL[i] = (float) L;
            if (outR != nullptr) outR[i] = (float) R;
        }
    }

private:
    struct Band
    {
        OversampledShaper shaper;
        double toneState = 0, toneCoef = 1;
        Svf svf[3];
        EnvFollower env;
        ChunkRamp g, k, pre, makeup, level;
        ChunkRamp fg[3], famp[3];   // formant band-passes
        double fk = 0.1;
    };
    struct Channel
    {
        DcBlocker dc;
        Lr4 xa, xb, apb;
        Band band[3];
        Lfo lfo;
        ChunkRamp duckGain;
        DelayLine dry;
        double lfoVal = 0;
        double meanAbs = 0;              // for the shaper's even harmonics
        EnvFollower fastEnv;             // transient detector: a peak follower (instant attack, 20 ms release)...
        double slowEnv = 0, aSlow = 0.001;   // ...and a 60 ms lagged copy of it: the attack = how far the fast one leads
        ChunkRamp transGain;
    };

    double drive (Band& B, double y, double pre, double mk)
    {
        y = B.shaper.process (y * pre, params.os) * mk;
        B.toneState += B.toneCoef * (y - B.toneState);
        return B.toneState;
    }

    double filter (Band& B, double y, double g, double k, const double* fg, const double* fa)
    {
        if (params.filterType == 4)
        {
            double acc = 0;
            for (int f = 0; f < 3; ++f)
            {
                const auto o = B.svf[f].process (y, fg[f], B.fk);
                acc += o.bp * fa[f];
            }
            return acc * 1.5;
        }
        const auto o = B.svf[0].process (y, g, k);
        switch (params.filterType)
        {
            case 1:  return o.bp;
            case 2:  return o.hp;
            case 3:  return o.notch;
            default: return o.lp;
        }
    }

    void updateCrossovers()
    {
        if (params.xLow != lastXLow || params.xHigh != lastXHigh)
        {
            lastXLow = params.xLow; lastXHigh = params.xHigh;
            for (auto& C : ch) { C.xa.set (params.xLow, sr); C.xb.set (params.xHigh, sr); C.apb.set (params.xHigh, sr); }
        }
        if (params.monoBelow != lastMono)
        {
            lastMono = params.monoBelow;
            monoSplit.set (params.monoBelow < 1.0 ? 1.0 : params.monoBelow, sr);
        }
    }

    static void flushSvf (Svf& s)
    {
        if (std::abs (s.ic1) < 1e-30) s.ic1 = 0;
        if (std::abs (s.ic2) < 1e-30) s.ic2 = 0;
    }
    static void flushBiquad (Biquad& b)
    {
        if (std::abs (b.s1) < 1e-30) b.s1 = 0;
        if (std::abs (b.s2) < 1e-30) b.s2 = 0;
    }
    static void flushLr4 (Lr4& x) { flushBiquad (x.lp1); flushBiquad (x.lp2); flushBiquad (x.hp1); flushBiquad (x.hp2); }

    // The analysis results, once per chunk: the smoothed pitch, the thin-source measure, the Auto source type.
    void analysis (double dt)
    {
        const auto& p = params;
        // pitch: follow the tracker while it is confident (30 ms), hold otherwise
        const double a = tauCoef (0.03, dt);
        confSm += a * (tracker.conf - confSm);
        if (tracker.analysed && ! confStarted) { confAvg = tracker.conf; confStarted = true; }   // no ramp from 0
        confAvg += tauCoef (1.0, dt) * (tracker.conf - confAvg);
        if (tracker.conf > 0.5 && tracker.pitch > 0)
            pitchSm += a * (tracker.pitch - pitchSm);
        // the pitch the filter tracks: a held MIDI note wins over the tracked pitch
        double keyPitch = pitchSm;
        if (lastNote >= 0) keyPitch = 440.0 * std::pow (2.0, (lastNote - 69) / 12.0);
        const double ktTarget = p.keyTrack * std::log (clampd (keyPitch, 20.0, 5000.0) / kKeyRef);
        ktLog += tauCoef (0.02, dt) * (ktTarget - ktLog);
        // thinness: a sine has an RMS frequency at its pitch, a saw well above it
        const double rf = features.rmsFreq();
        const double thin = (confSm > 0.5 && pitchSm > 0) ? clampd ((1.8 - rf / pitchSm) / 0.6, 0.0, 1.0) : 0.0;
        thinSm += tauCoef (0.1, dt) * (thin - thinSm);
        const double gate = confSm > 0.5 ? 1.0 : 0.0;
        gateSm += tauCoef (0.05, dt) * (gate - gateSm);
        const double blend = p.harmOn ? p.harmAmount * (p.harmMode == 0 ? thinSm : 1.0) * (p.harmShape == 0 ? 1.0 : gateSm) : 0.0;
        harmRamp.set (blend);
        subRamp.set (p.subOn ? p.subAmount * gateSm : 0.0);
        osc.setPitch (pitchSm, sr);

        // the Auto source type: classified after each analysis, applied once it has persisted
        if (tracker.analysed)
        {
            tracker.analysed = false;
            if (features.rms() > 1e-4)   // not on near-silence
            {
                const int type = classifySource (confAvg, pitchSm, rf, features.variation(), features.crest(), features.lowRatio());
                if (type == candidate) ++candidateCount; else { candidate = type; candidateCount = 1; }
                if (candidateCount >= kDetectHold || (detected < 0 && candidateCount >= kDetectFirst)) detected = candidate;
            }
        }
        features.flush();
        if (std::abs (osc.subLp) < 1e-30) osc.subLp = 0;
    }

    // Once per chunk: the LFOs, the cutoffs, the gains.
    void control()
    {
        const auto& p = params;
        const double dt = kChunk / sr;
        updateCrossovers();

        // auto input gain: RMS to -18 dBFS, +-24 dB, holding when the input is near silent
        double target = 1.0;
        if (p.autoIn)
        {
            const double r = rms.rms();
            target = r > 1e-4 ? clampd (kRmsTarget / r, 1.0 / kAutoGainMax, kAutoGainMax) : agGain;
        }
        agGain += tauCoef (kAutoGainTau, dt) * (target - agGain);
        inGainRamp.set (p.inGain * agGain);
        outGainRamp.set (p.outGain);
        mixRamp.set (p.mix);

        if (playing) ppq += dt * bpm / 60.0;

        analysis (dt);
        const double ktMul = std::exp (ktLog);
        autoOutRamp.set (autoOut.update (p.autoOut));
        autoOut.flush();
        limiter.flush();

        const double q0 = resoToQ (p.reso);
        const double fk = 1.0 / (4.0 + 16.0 * p.reso);
        for (int c = 0; c < 2; ++c)
        {
            auto& C = ch[c];
            C.lfoVal = C.lfo.next (p.lfoShape, p.lfoSync, p.lfoRate, p.lfoDiv, p.lfoPhase + (c == 1 ? p.lfoStereo : 0.0),
                                   p.lfoSmooth, dt, bpm, ppq, playing);
            C.duckGain.set (1.0 - p.duck * (1.0 - C.lfoVal));
            // transient preserve: the attack = how far the fast envelope leads the slow one
            const double attack = C.slowEnv > 1e-6 ? clampd ((C.fastEnv.env / C.slowEnv - 1.0) * 0.5, 0.0, 1.0) : 0.0;
            C.transGain.set (p.transPreserve * attack);
            flushLr4 (C.xa); flushLr4 (C.xb); flushLr4 (C.apb);
            if (C.fastEnv.env < 1e-30) C.fastEnv.env = 0;
            if (C.slowEnv < 1e-30) C.slowEnv = 0;
            if (C.meanAbs < 1e-30) C.meanAbs = 0;
            for (int b = 0; b < 3; ++b)
            {
                auto& B = C.band[b];
                const double pos = 0.5 + p.depth[b] * (C.lfoVal - 0.5);
                const double cutoff = p.cutoffLo * ktMul * std::pow (p.cutoffHi / p.cutoffLo, pos);
                if (c == 0) cutoffStat[b] = clampd (cutoff, 10.0, 0.45 * sr);
                // the resonant peak is about Q x the band's level: above kResoLimitPeak the limit divides Q down
                const double over = std::max (0.0, q0 * B.env.env / kResoLimitPeak - 1.0);
                const double q = q0 / (1.0 + p.resoLimit * over);
                B.g.set (svfG (cutoff, sr));
                B.k.set (1.0 / q);
                for (int f = 0; f < 3; ++f)
                {
                    double hz, amp;
                    formantAt (pos, f, hz, amp);
                    B.fg[f].set (svfG (hz * ktMul, sr));
                    B.famp[f].set (amp);
                }
                B.fk = fk;
                const double pre = drivePreGain (p.drive[b]);
                B.pre.set (pre);
                B.makeup.set (driveMakeup (pre));
                B.level.set (p.level[b]);
                // tone: a one-pole from 800 Hz (0) to 20 kHz; at 100% it is out of the way entirely
                const double toneHz = 800.0 * std::pow (25.0, p.tone[b]);
                B.toneCoef = p.tone[b] >= 0.999 ? 1.0 : onePoleCoef (std::min (toneHz, 0.45 * sr), sr);
                for (auto& s : B.svf) flushSvf (s);
                if (std::abs (B.toneState) < 1e-30) B.toneState = 0;
                if (B.env.env < 1e-30) B.env.env = 0;
            }
        }
        flushLr4 (monoSplit);
        if (harmEnv.env < 1e-30) harmEnv.env = 0;
    }

    EngineParams params;
    double sr = 48000.0;
    Channel ch[2];
    Lr4 monoSplit;
    RmsTracker rms;
    double agGain = 1.0;
    ChunkRamp inGainRamp, outGainRamp, mixRamp;
    double lastXLow = -1, lastXHigh = -1, lastMono = -1;
    int chunkLeft = 0;
    double bpm = 120, ppq = 0;
    bool playing = false;
    double peakIn = 0, peakOut = 0;
    double cutoffStat[3] { 0, 0, 0 };
    int lastNote = -1;
    // analysis (phase 2)
    DcBlocker dcMono;
    PitchTracker tracker;
    SourceFeatures features;
    HarmOsc osc;
    EnvFollower harmEnv;
    double pitchSm = 110, confSm = 0, confAvg = 0, thinSm = 0, gateSm = 0, ktLog = 0, aMeanAbs = 0.001;
    bool confStarted = false;
    ChunkRamp harmRamp, subRamp;
    int detected = -1, candidate = -1, candidateCount = 0;
    // output (phase 3)
    AutoOutput autoOut;
    ChunkRamp autoOutRamp;
    TruePeakLimiter limiter;
    double inRmsShared = 0;
};
} // namespace wub
