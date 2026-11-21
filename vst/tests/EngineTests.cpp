// Engine checks (--engine, and part of the default suite): what the wobble chain is supposed to do,
// measured on test signals. Each line prints the measurement so a change in behaviour is visible.
#include "TestRender.h"

using namespace wub;
using namespace wub::test;

namespace
{
int fails = 0;
void check (bool ok, const juce::String& what)
{
    if (! ok) ++fails;
    std::printf ("%-4s %s\n", ok ? "ok" : "FAIL", what.toRawUTF8());
}

// A neutral chain: single band, no wobble, no drive, auto gain off, 1x, no limiter.
Values neutral()
{
    auto v = defaultValues();
    set (v, "autoIn", 0); set (v, "midWob", 0); set (v, "midDrive", 0); set (v, "os", 0); set (v, "lfoSync", 0);
    set (v, "midTone", 1); set (v, "limiter", 0);
    return v;
}

std::vector<double> lfoTrace (Values v, double seconds, Transport tr = {})
{
    // the LFO value is not an output; read the modulation off a static sine through the LP:
    // instead sample the engine directly
    auto enginePtr = std::make_unique<WubEngine>();
    auto& e = *enginePtr;
    auto P = makeEngineParams ([&v] (int i) { return v[(size_t) i]; });
    e.setParams (P);
    e.prepare (kSr);
    std::vector<double> trace;
    std::vector<float> in ((size_t) kBlock, 0.0f), out ((size_t) kBlock);
    const int blocks = (int) (seconds * kSr / kBlock);
    for (int b = 0; b < blocks; ++b)
    {
        e.setTransport (tr.bpm, tr.ppq0 + b * kBlock / kSr * tr.bpm / 60.0, tr.playing);
        e.render (in.data(), in.data(), out.data(), out.data(), kBlock);
        trace.push_back (e.lfoValue (0));
    }
    return trace;
}
} // namespace

int runEngineTests()
{
    fails = 0;
    std::printf ("engine checks (48 kHz, 128-sample blocks)\n");

    // ---- silence and finiteness ---------------------------------------------------------
    {
        auto v = defaultValues();
        const auto a = render (v, { 0, 0, 0 }, 1.0);
        check (finite (a) && peak (a) == 0.0, "silence in -> exactly silence out");
    }

    // ---- the dry path: mix 0 is the (gain-conditioned) input, delayed by the oversampling latency ----
    {
        auto v = neutral();
        set (v, "mix", 0);
        for (int o = 0; o < 3; ++o)
        {
            set (v, "os", o);
            const auto a = render (v, { 2, 220.0, 0.5 }, 0.5);
            TestSource g; g.prepare (2, 220.0, 0.5, kSr);
            DcBlocker dc; dc.prepare (kSr);   // the dry path is the DC-blocked input
            std::vector<double> in ((size_t) a.size());
            for (auto& x : in) { double l, r; g.next (l, r); x = dc.process ((float) l); }
            const int lat = WubEngine::latencySamples (o);
            double err = 0;
            for (int i = sec (0.2); i < a.size(); ++i) err = std::max (err, std::abs ((double) a.L[(size_t) i] - in[(size_t) (i - lat)]));
            check (err < 1e-5, juce::String::formatted ("mix 0 at %dx: output = the DC-blocked input delayed by %d samples (max diff %.1e)", 1 << o, lat, err));
        }
    }

    // ---- auto input gain: a quiet sine comes up to -18 dBFS RMS -------------------------
    {
        auto v = neutral();
        set (v, "autoIn", 1); set (v, "mix", 0);
        const auto a = render (v, { 1, 110.0, 0.014142 }, 4.0);   // -40 dBFS RMS (needs +22 dB, inside the +-24 dB range)
        const double level = db (rms (a, sec (3.0), sec (4.0)));
        check (std::abs (level - (-18.0)) < 1.0, juce::String::formatted ("auto gain: -40 dBFS sine reaches %.1f dB RMS after 3 s (target -18)", level));
        const auto a2 = render (v, { 1, 110.0, 0.005 }, 4.0);   // -49 dBFS RMS: capped at +24 dB
        const double level2 = db (rms (a2, sec (3.0), sec (4.0)));
        check (std::abs (level2 - (-25.0)) < 1.0, juce::String::formatted ("auto gain: -49 dBFS sine gets the +24 dB cap: %.1f dB RMS", level2));
        const auto loud = render (v, { 1, 110.0, 0.9 }, 4.0);
        const double levelLoud = db (rms (loud, sec (3.0), sec (4.0)));
        check (std::abs (levelLoud - (-18.0)) < 1.0, juce::String::formatted ("auto gain: -1 dBFS sine comes down to %.1f dB RMS", levelLoud));
        const auto quiet = render (v, { 1, 110.0, 0.00003 }, 2.0);   // -90 dBFS: below the hold threshold
        check (peak (quiet) < 0.01, juce::String::formatted ("auto gain holds on near-silence (peak %.5f, no +24 dB pumping of the noise floor)", peak (quiet)));
    }

    // ---- LFO rates -----------------------------------------------------------------------
    {
        auto v = defaultValues();
        set (v, "lfoSync", 0); set (v, "lfoRate", 2.0); set (v, "lfoSmooth", 0);
        const auto t = lfoTrace (v, 8.0);
        const double f = traceFrequency (t, kSr / kBlock, 0.5, 10.0);
        check (std::abs (f - 2.0) < 0.02, juce::String::formatted ("free LFO at 2 Hz measures %.3f Hz", f));
        set (v, "lfoSync", 1); set (v, "lfoDiv", 3);   // 1/4 at 120 BPM = 2 Hz
        const auto t2 = lfoTrace (v, 8.0, { 120, true, 0 });
        const double f2 = traceFrequency (t2, kSr / kBlock, 0.5, 10.0);
        check (std::abs (f2 - 2.0) < 0.02, juce::String::formatted ("synced 1/4 at 120 BPM measures %.3f Hz", f2));
        set (v, "lfoDiv", 8);   // 1/8T at 150 BPM: 150/60/(1/3) = 7.5 Hz
        const auto t3 = lfoTrace (v, 8.0, { 150, false, 0 });
        const double f3 = traceFrequency (t3, kSr / kBlock, 0.5, 10.0);
        check (std::abs (f3 - 7.5) < 0.05, juce::String::formatted ("synced 1/8T at 150 BPM, transport stopped, free-runs at %.3f Hz (7.5)", f3));
        // phase lock: two renders starting at different ppq land at the same phase at the same beat
        // (384 blocks = 1.024 s = 2.048 beats from ppq 0; 192 blocks from ppq 1.024 end at the same ppq)
        set (v, "lfoDiv", 0); set (v, "lfoShape", 3);   // 1/1 (4 beats), saw up: value = phase
        const auto a = lfoTrace (v, 384 * kBlock / kSr, { 120, true, 0 });
        const auto b = lfoTrace (v, 192 * kBlock / kSr, { 120, true, 192 * kBlock / kSr * 2.0 });
        check (std::abs (a[a.size() - 1] - b[b.size() - 1]) < 1e-9 && std::abs (a[a.size() - 1] - 2.048 / 4.0) < 1e-6,
               juce::String::formatted ("transport lock: phase at ppq 2.048 is %.4f from ppq 0 and %.4f from ppq 1.024 (expected 0.512)", a[a.size() - 1], b[b.size() - 1]));
        // retrigger
        set (v, "lfoSync", 0); set (v, "lfoRate", 1.0); set (v, "lfoRetrig", 1); set (v, "lfoShape", 3);
        auto enginePtr = std::make_unique<WubEngine>();
        auto& e = *enginePtr;
        e.setParams (makeEngineParams ([&v] (int i) { return v[(size_t) i]; }));
        e.prepare (kSr);
        std::vector<float> in ((size_t) kBlock, 0.0f), out ((size_t) kBlock);
        for (int blk = 0; blk < 200; ++blk) e.render (in.data(), in.data(), out.data(), out.data(), kBlock);   // 0.53 s
        const double before = e.lfoValue (0);
        e.noteOn (60, 100);
        e.render (in.data(), in.data(), out.data(), out.data(), kBlock);
        const double after = e.lfoValue (0);
        check (before > 0.4 && after < 0.02, juce::String::formatted ("MIDI note retriggers the LFO: %.3f -> %.3f", before, after));
    }

    // ---- crossover: three bands sum flat -------------------------------------------------
    {
        auto v = neutral();
        set (v, "bands", 1); set (v, "loWob", 0); set (v, "hiWob", 0);
        set (v, "loDrive", 0); set (v, "hiDrive", 0); set (v, "loTone", 1); set (v, "hiTone", 1);
        set (v, "mix", 1);
        double worst = 0;
        for (double f : { 50.0, 200.0, 700.0, 2500.0, 6000.0, 12000.0 })
        {
            const auto a = render (v, { 1, f, 0.3 }, 1.0);
            const double amp = toneAmplitude (a, sec (0.5), sec (0.4), f);
            worst = std::max (worst, std::abs (db (amp / 0.3)));
        }
        check (worst < 0.15, juce::String::formatted ("3-band Linkwitz-Riley split sums flat: worst deviation %.3f dB at 50 Hz .. 12 kHz (incl. both crossover frequencies)", worst));
        const auto n = render (v, { 5, 0, 0.2 }, 2.0);
        TestSource g; g.prepare (5, 0, 0.2, kSr);
        Audio in; in.L.resize ((size_t) n.size()); in.R.resize ((size_t) n.size());
        for (int i = 0; i < n.size(); ++i) { double l, r; g.next (l, r); in.L[(size_t) i] = (float) l; in.R[(size_t) i] = (float) r; }
        const double gain = db (rms (n, sec (0.5), sec (2.0)) / rms (in, sec (0.5), sec (2.0)));
        check (std::abs (gain) < 0.2, juce::String::formatted ("white noise through the split: %.3f dB", gain));
    }

    // ---- the SVF ---------------------------------------------------------------------------
    {
        auto v = neutral();
        set (v, "midWob", 1); set (v, "midDepth", 0); set (v, "cutoffLo", 1000); set (v, "cutoffHi", 1000); set (v, "reso", 0);   // static LP at 1 kHz, Q 0.5
        const auto lo = render (v, { 1, 100.0, 0.3 }, 1.0);
        const auto hi = render (v, { 1, 10000.0, 0.3 }, 1.0);
        const double gLo = db (toneAmplitude (lo, sec (0.5), sec (0.4), 100.0) / 0.3);
        const double gHi = db (toneAmplitude (hi, sec (0.5), sec (0.4), 10000.0) / 0.3);
        check (gLo > -0.5 && gLo < 0.1 && gHi < -30, juce::String::formatted ("LP at 1 kHz: 100 Hz %.2f dB, 10 kHz %.1f dB", gLo, gHi));
        set (v, "reso", 1);
        const auto res = render (v, { 1, 1000.0, 0.05 }, 1.0);
        const double gRes = db (toneAmplitude (res, sec (0.5), sec (0.4), 1000.0) / 0.05);
        check (gRes > 20 && gRes < 30, juce::String::formatted ("resonance 100%% (Q 20) lifts the cutoff by %.1f dB", gRes));
        set (v, "filterType", 2);
        const auto hp = render (v, { 1, 100.0, 0.3 }, 1.0);
        check (db (toneAmplitude (hp, sec (0.5), sec (0.4), 100.0) / 0.3) < -30, juce::String::formatted ("HP at 1 kHz: 100 Hz %.1f dB", db (toneAmplitude (hp, sec (0.5), sec (0.4), 100.0) / 0.3)));
        set (v, "filterType", 1); set (v, "reso", 0.5);
        const auto bp = render (v, { 1, 1000.0, 0.3 }, 1.0);
        const double gBp = db (toneAmplitude (bp, sec (0.5), sec (0.4), 1000.0) / 0.3);
        check (std::abs (gBp) < 0.5, juce::String::formatted ("BP: unity at the centre (%.2f dB)", gBp));
        set (v, "filterType", 3);
        const auto notch = render (v, { 1, 1000.0, 0.3 }, 1.0);
        check (db (toneAmplitude (notch, sec (0.5), sec (0.4), 1000.0) / 0.3) < -30, juce::String::formatted ("notch at the centre: %.1f dB", db (toneAmplitude (notch, sec (0.5), sec (0.4), 1000.0) / 0.3)));
        set (v, "filterType", 4); set (v, "midDepth", 1); set (v, "lfoRate", 1);
        const auto formant = render (v, { 2, 110.0, 0.3 }, 2.0);
        check (finite (formant) && rms (formant, sec (0.5), sec (2.0)) > 0.02, juce::String::formatted ("formant morph on a saw: %.1f dB RMS, finite", db (rms (formant, sec (0.5), sec (2.0)))));
    }

    // ---- the wobble itself: the LFO modulates the level of a saw through the LP ----------------
    {
        auto v = neutral();
        set (v, "midWob", 1); set (v, "lfoRate", 1.5); set (v, "lfoSmooth", 0); set (v, "cutoffLo", 100); set (v, "cutoffHi", 4000);
        const auto a = render (v, { 2, 110.0, 0.3 }, 8.0);
        std::vector<double> env;
        for (int i = 0; i + sec (0.02) <= a.size(); i += sec (0.02)) env.push_back (rmsL (a, i, i + sec (0.02)));
        const double f = traceFrequency (env, 50.0, 0.5, 5.0);
        double mn = 1e9, mx = 0;
        for (size_t i = env.size() / 2; i < env.size(); ++i) { mn = std::min (mn, env[i]); mx = std::max (mx, env[i]); }
        check (std::abs (f - 1.5) < 0.05 && db (mx / mn) > 4, juce::String::formatted ("wobble on a 110 Hz saw: level pumps at %.2f Hz, %.1f dB deep (the fundamental sits below the sweep, so the level says less than the tone)", f, db (mx / mn)));
    }

    // ---- resonance limit --------------------------------------------------------------------
    {
        auto v = neutral();
        set (v, "midWob", 1); set (v, "midDepth", 0); set (v, "cutoffLo", 440); set (v, "cutoffHi", 440); set (v, "reso", 1); set (v, "resoLimit", 0);
        set (v, "outGain", -24);   // keep the safety clipper out of the measurement
        const auto wild = render (v, { 1, 440.0, 0.5 }, 1.0);   // a loud tone at the cutoff
        set (v, "resoLimit", 1);
        const auto tamed = render (v, { 1, 440.0, 0.5 }, 1.0);
        const double pw = peak (wild) * dbToGain (24), pt = peak (tamed) * dbToGain (24);
        check (pw > 5.0 && pt < 0.7, juce::String::formatted ("resonance limit on a -6 dBFS tone at the cutoff, Q 20: peak %.2f -> %.2f (the limit allows about 0.5)", pw, pt));
        set (v, "outGain", 0);
        set (v, "resoLimit", 1);
        const auto quiet = render (v, { 1, 440.0, 0.02 }, 1.0);
        set (v, "resoLimit", 0);
        const auto quiet0 = render (v, { 1, 440.0, 0.02 }, 1.0);
        check (std::abs (db (peak (quiet) / peak (quiet0))) < 0.5, juce::String::formatted ("..and does nothing to a -34 dBFS tone (%.2f dB)", db (peak (quiet) / peak (quiet0))));
    }

    // ---- drive and oversampling -----------------------------------------------------------
    {
        auto v = neutral();
        set (v, "midDrive", 1); set (v, "outGain", -12);   // the aliases are measured relative to the fundamental; keep the clipper out
        double alias[3];
        for (int o = 0; o < 3; ++o)
        {
            set (v, "os", o);
            const auto a = render (v, { 1, 5000.0, 0.5 }, 1.0);   // harmonics at 15, 25, 35 kHz fold to 33 (none), 23, 13 kHz
            const double fund = toneAmplitude (a, sec (0.5), sec (0.4), 5000.0);
            alias[o] = db (std::max (toneAmplitude (a, sec (0.5), sec (0.4), 13000.0), toneAmplitude (a, sec (0.5), sec (0.4), 23000.0)) / fund);
        }
        check (alias[2] < alias[0] - 15 && alias[1] < alias[0] - 15 && alias[2] < -40 && alias[1] < -40,
               juce::String::formatted ("aliases of a driven 5 kHz sine (36 dB): 1x %.1f dB (plain tanh), 2x %.1f dB, 4x %.1f dB below the fundamental", alias[0], alias[1], alias[2]));
        // dry / wet alignment: at mix 0.5 with a linear wet path, a 2 kHz tone sums to unity only if the dry delay matches
        set (v, "midDrive", 0); set (v, "mix", 0.5); set (v, "outGain", 0);
        for (int o = 0; o < 3; ++o)
        {
            set (v, "os", o);
            const auto a = render (v, { 1, 2000.0, 0.3 }, 1.0);
            const double g = db (toneAmplitude (a, sec (0.5), sec (0.4), 2000.0) / 0.3);
            check (std::abs (g) < 0.25, juce::String::formatted ("dry/wet aligned at %dx: a 2 kHz tone at mix 50%% reads %.2f dB", 1 << o, g));
        }
        // the shaper at drive 0 is transparent
        set (v, "mix", 1); set (v, "os", 1);
        const auto t = render (v, { 1, 1000.0, 0.1 }, 1.0);
        const double thd = db (toneAmplitude (t, sec (0.5), sec (0.4), 3000.0) / toneAmplitude (t, sec (0.5), sec (0.4), 1000.0));
        const double g0 = db (toneAmplitude (t, sec (0.5), sec (0.4), 1000.0) / 0.1);
        check (thd < -60 && std::abs (g0) < 0.05, juce::String::formatted ("drive 0 on a -20 dBFS tone: 3rd harmonic at %.1f dB, level %.3f dB", thd, g0));
    }

    // ---- stereo -----------------------------------------------------------------------------
    {
        auto v = neutral();
        set (v, "monoBelow", 120);
        // 'wide' is L saw 60 Hz, R saw 75 Hz: the side signal's 60 Hz fundamental should go, its 1020 Hz harmonic stay
        auto sideOf = [] (const Audio& x) { Audio s; s.L.resize ((size_t) x.size()); s.R = s.L; for (int i = 0; i < x.size(); ++i) s.L[(size_t) i] = 0.5f * (x.L[(size_t) i] - x.R[(size_t) i]); return s; };
        const auto with = sideOf (render (v, { 7, 60.0, 0.3 }, 1.0));
        const auto without = sideOf (render (neutral(), { 7, 60.0, 0.3 }, 1.0));
        const double low = db (toneAmplitude (with, sec (0.5), sec (0.4), 60.0) / toneAmplitude (without, sec (0.5), sec (0.4), 60.0));
        const double high = db (toneAmplitude (with, sec (0.5), sec (0.4), 1020.0) / toneAmplitude (without, sec (0.5), sec (0.4), 1020.0));
        check (low < -20 && std::abs (high) < 0.5, juce::String::formatted ("mono below 120 Hz: the side's 60 Hz %.1f dB (LR4, one octave down), its 1020 Hz %.2f dB", low, high));
        set (v, "monoBelow", 0); set (v, "stereoMode", 1);
        const auto m = render (v, { 7, 60.0, 0.3 }, 1.0);
        double diff = 0;
        for (int i = 0; i < m.size(); ++i) diff = std::max (diff, (double) std::abs (m.L[(size_t) i] - m.R[(size_t) i]));
        check (diff < 1e-6, juce::String::formatted ("Mono Sum: L and R identical (max diff %.1e)", diff));
        set (v, "stereoMode", 2); set (v, "midWob", 1); set (v, "midDepth", 0); set (v, "cutoffLo", 500); set (v, "cutoffHi", 500); set (v, "reso", 0);
        const auto ms = render (v, { 7, 60.0, 0.3 }, 1.0);
        check (finite (ms) && rms (ms, sec (0.5), sec (1.0)) > 0.02, juce::String::formatted ("Mid-Side mode processes M and S and comes back to L/R (%.1f dB RMS)", db (rms (ms, sec (0.5), sec (1.0)))));
        // stereo LFO offset: with 180 degrees the two channels wobble in opposition
        v = neutral();
        set (v, "midWob", 1); set (v, "lfoRate", 1.0); set (v, "lfoStereo", 180); set (v, "lfoSmooth", 0); set (v, "lfoShape", 4);
        set (v, "cutoffLo", 60); set (v, "cutoffHi", 8000);
        const auto st = render (v, { 2, 220.0, 0.3 }, 2.0);
        std::vector<double> el, er;
        for (int i = sec (0.5); i + sec (0.05) <= st.size(); i += sec (0.05))
        {
            double l = 0, r = 0;
            for (int j = i; j < i + sec (0.05); ++j) { l += (double) st.L[(size_t) j] * st.L[(size_t) j]; r += (double) st.R[(size_t) j] * st.R[(size_t) j]; }
            el.push_back (std::sqrt (l)); er.push_back (std::sqrt (r));
        }
        double ml = 0, mr = 0;
        for (size_t i = 0; i < el.size(); ++i) { ml += el[i]; mr += er[i]; }
        ml /= (double) el.size(); mr /= (double) er.size();
        double corr = 0, nl = 0, nr = 0;
        for (size_t i = 0; i < el.size(); ++i) { corr += (el[i] - ml) * (er[i] - mr); nl += (el[i] - ml) * (el[i] - ml); nr += (er[i] - mr) * (er[i] - mr); }
        const double rho = corr / std::sqrt (nl * nr);
        check (rho < -0.5, juce::String::formatted ("LFO stereo offset 180: L and R envelopes anti-correlated (%.2f)", rho));
    }

    // ---- ducking ----------------------------------------------------------------------------
    {
        auto v = neutral();
        set (v, "duck", 1); set (v, "lfoShape", 4); set (v, "lfoRate", 1.0); set (v, "lfoSmooth", 0);
        const auto a = render (v, { 1, 440.0, 0.3 }, 2.0);
        // square LFO 1 Hz: high (gain 1) for the first half of each second, low (gain 0) for the second half
        const double on = rms (a, sec (1.1), sec (1.4)), off = rms (a, sec (1.6), sec (1.9));
        check (on > 0.15 && off < 1e-3, juce::String::formatted ("duck 100%% with a square LFO: %.3f on, %.5f off", on, off));
    }

    // ---- stress: everything on, loud, finite and under full scale ----------------------------
    {
        auto v = defaultValues();
        set (v, "bands", 1); set (v, "hiWob", 1); set (v, "loDrive", 1); set (v, "midDrive", 1); set (v, "hiDrive", 1);
        set (v, "reso", 1); set (v, "resoLimit", 0); set (v, "os", 2); set (v, "inGain", 24); set (v, "outGain", 24); set (v, "autoIn", 0);
        set (v, "lfoSync", 0); set (v, "lfoRate", 20); set (v, "lfoShape", 5); set (v, "lfoSmooth", 0);
        const auto a = render (v, { 5, 0, 1.0 }, 3.0);
        check (finite (a) && peak (a) <= 1.0, juce::String::formatted ("full-scale noise, +24 dB in and out, Q 20, 36 dB drive x3, 4x, 20 Hz S&H: finite, peak %.3f", peak (a)));
        set (v, "filterType", 4);
        const auto f = render (v, { 5, 0, 1.0 }, 2.0);
        check (finite (f) && peak (f) <= 1.0, juce::String::formatted ("..and in formant mode: peak %.3f", peak (f)));
    }

    // ================================================================= phase 2 =====
    // ---- pitch tracking ---------------------------------------------------------------------
    {
        auto trackOf = [] (Values v, Source src, double seconds, double& conf)
        {
            auto enginePtr = std::make_unique<WubEngine>();
            auto& e = *enginePtr;
            e.setParams (makeEngineParams ([&v] (int i) { return v[(size_t) i]; }));
            e.prepare (kSr);
            TestSource g; g.prepare (src.type, src.freq, src.level, kSr);
            std::vector<float> inL ((size_t) kBlock), inR ((size_t) kBlock), oL ((size_t) kBlock), oR ((size_t) kBlock);
            const int blocks = (int) (seconds * kSr / kBlock);
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < kBlock; ++i) { double l, r; g.next (l, r); inL[(size_t) i] = (float) l; inR[(size_t) i] = (float) r; }
                e.render (inL.data(), inR.data(), oL.data(), oR.data(), kBlock);
            }
            conf = e.pitchConfidence();
            return e.pitchHz();
        };
        auto v = neutral();
        for (double f : { 41.2, 55.0, 110.0, 440.0, 1000.0 })
        {
            double conf = 0;
            const double hz = trackOf (v, { 1, f, 0.3 }, 1.0, conf);
            check (std::abs (hz / f - 1.0) < 0.005 && conf > 0.85, juce::String::formatted ("pitch of a %.1f Hz sine: %.2f Hz, confidence %.2f", f, hz, conf));
        }
        double conf = 0;
        double hz = trackOf (v, { 2, 110.0, 0.3 }, 1.0, conf);
        check (std::abs (hz / 110.0 - 1.0) < 0.005 && conf > 0.85, juce::String::formatted ("pitch of a 110 Hz saw: %.2f Hz, confidence %.2f", hz, conf));
        hz = trackOf (v, { 1, 110.0, 0.002 }, 1.0, conf);
        check (std::abs (hz / 110.0 - 1.0) < 0.005 && conf > 0.85, juce::String::formatted ("..and of a -54 dBFS sine (through the auto gain off: %.2f Hz, %.2f)", hz, conf));
        hz = trackOf (v, { 5, 0.0, 0.3 }, 1.0, conf);
        check (conf < 0.4, juce::String::formatted ("noise: confidence %.2f (no pitch)", conf));
        hz = trackOf (v, { 0, 0.0, 0.0 }, 1.0, conf);
        check (conf < 0.01, juce::String::formatted ("silence: confidence %.2f", conf));
        hz = trackOf (v, { 6, 0.0, 0.6 }, 2.0, conf);
        std::printf ("     (the drum loop tracks %.1f Hz at confidence %.2f)\n", hz, conf);
    }

    // ---- key tracking -------------------------------------------------------------------------
    {
        auto v = neutral();
        set (v, "midWob", 1); set (v, "midDepth", 0); set (v, "cutoffLo", 1000); set (v, "cutoffHi", 1000); set (v, "keyTrack", 1);
        auto cutoffFor = [&v] (Source src, int note)
        {
            auto enginePtr = std::make_unique<WubEngine>();
            auto& e = *enginePtr;
            e.setParams (makeEngineParams ([&v] (int i) { return v[(size_t) i]; }));
            e.prepare (kSr);
            if (note >= 0) e.noteOn (note, 100);
            TestSource g; g.prepare (src.type, src.freq, src.level, kSr);
            std::vector<float> inL ((size_t) kBlock), inR ((size_t) kBlock), oL ((size_t) kBlock), oR ((size_t) kBlock);
            for (int b = 0; b < 375; ++b)
            {
                for (int i = 0; i < kBlock; ++i) { double l, r; g.next (l, r); inL[(size_t) i] = (float) l; inR[(size_t) i] = (float) r; }
                e.render (inL.data(), inR.data(), oL.data(), oR.data(), kBlock);
            }
            return e.cutoffHz (1);
        };
        const double c220 = cutoffFor ({ 1, 220.0, 0.3 }, -1), c55 = cutoffFor ({ 1, 55.0, 0.3 }, -1);
        check (std::abs (c220 / 2000.0 - 1.0) < 0.02 && std::abs (c55 / 500.0 - 1.0) < 0.02,
               juce::String::formatted ("key tracking 100%% (1 kHz set): a 220 Hz input puts the cutoff at %.0f Hz, 55 Hz at %.0f Hz", c220, c55));
        const double cMidi = cutoffFor ({ 1, 55.0, 0.3 }, 69);   // A4 held: 440 Hz wins over the tracked 55 Hz
        check (std::abs (cMidi / 4000.0 - 1.0) < 0.02, juce::String::formatted ("..a held MIDI A4 wins: %.0f Hz", cMidi));
        set (v, "keyTrack", 0.5);
        const double cHalf = cutoffFor ({ 1, 440.0, 0.3 }, -1);
        check (std::abs (cHalf / 2000.0 - 1.0) < 0.02, juce::String::formatted ("..key tracking 50%% on 440 Hz (two octaves up): %.0f Hz (one octave)", cHalf));
    }

    // ---- the Auto source type -------------------------------------------------------------------
    {
        auto detect = [] (Source src, double seconds, double& rf, double& crest, double& var, double& confAvg)
        {
            auto v = defaultValues();
            auto enginePtr = std::make_unique<WubEngine>();
            auto& e = *enginePtr;
            e.setParams (makeEngineParams ([&v] (int i) { return v[(size_t) i]; }));
            e.prepare (kSr);
            TestSource g; g.prepare (src.type, src.freq, src.level, kSr);
            std::vector<float> inL ((size_t) kBlock), inR ((size_t) kBlock), oL ((size_t) kBlock), oR ((size_t) kBlock);
            const int blocks = (int) (seconds * kSr / kBlock);
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < kBlock; ++i) { double l, r; g.next (l, r); inL[(size_t) i] = (float) l; inR[(size_t) i] = (float) r; }
                e.render (inL.data(), inR.data(), oL.data(), oR.data(), kBlock);
            }
            rf = e.rmsFrequency(); crest = e.crestFactor(); var = e.envelopeVariation(); confAvg = e.averageConfidence();
            return e.detectedSource();
        };
        const char* names[] { "Bass", "Vocal", "Guitar", "Drums", "Pad", "Full Mix" };
        auto name = [&names] (int t) { return juce::String (t >= 0 && t < 6 ? names[t] : "undecided"); };
        double rf, cr, var, ca;
        auto feat = [&] { return juce::String::formatted (" (confidence %.2f, RMS frequency %.0f Hz, variation %.2f, crest %.1f)", ca, rf, var, cr); };
        int t = detect ({ 1, 55.0, 0.5 }, 2.5, rf, cr, var, ca);
        check (t == 0, "a 55 Hz sine is detected as " + name (t) + feat());
        t = detect ({ 2, 110.0, 0.4 }, 2.5, rf, cr, var, ca);
        check (t == 0, "a 110 Hz saw as " + name (t) + feat());
        t = detect ({ 6, 0.0, 0.6 }, 2.5, rf, cr, var, ca);
        check (t == 3, "the drum loop as " + name (t) + feat());
        t = detect ({ 4, 220.0, 0.4 }, 2.5, rf, cr, var, ca);
        check (t == 0 || t == 4 || t == 1 || t == 2, "a saw chord at 220 Hz as " + name (t) + feat() + " (the tracker reads the chord's common fundamental, 55 Hz)");
        t = detect ({ 1, 440.0, 0.3 }, 2.5, rf, cr, var, ca);
        check (t == 1, "a 440 Hz sine as " + name (t) + feat());
        t = detect ({ 5, 0.0, 0.3 }, 2.5, rf, cr, var, ca);
        check (t == 4 || t == 5, "white noise as " + name (t) + feat());
        t = detect ({ 0, 0.0, 0.0 }, 2.5, rf, cr, var, ca);
        check (t == -1, "silence stays " + name (t));
        t = detect ({ 1, 55.0, 0.5 }, 0.8, rf, cr, var, ca);
        check (t == -1, "..and 0.8 s is not enough to decide (" + name (t) + "): the first decision takes 1 s, a change 1.5 s");
        t = detect ({ 1, 440.0, 0.3 }, 1.2, rf, cr, var, ca);
        check (t == 1, "..the first decision on a 440 Hz sine, at 1.2 s, is already " + name (t) + feat());
    }

    // ---- harmonics -------------------------------------------------------------------------------
    {
        auto v = neutral();
        set (v, "mix", 0);   // the dry path carries the harmonics: measure them without the filter
        auto harmonicDb = [] (const Audio& a, double f, int n) { return db (toneAmplitude (a, sec (1.0), sec (0.5), f * n) / toneAmplitude (a, sec (1.0), sec (0.5), f)); };
        const auto plain = render (v, { 1, 110.0, 0.2 }, 1.5);
        set (v, "harmOn", 1); set (v, "harmMode", 1); set (v, "harmAmount", 1); set (v, "harmShape", 0);
        const auto shaped = render (v, { 1, 110.0, 0.2 }, 1.5);
        check (harmonicDb (plain, 110.0, 3) < -60 && harmonicDb (shaped, 110.0, 2) > -24 && harmonicDb (shaped, 110.0, 3) > -24,
               juce::String::formatted ("Shaper, Always, 100%% on a 110 Hz sine: 2nd %.1f dB, 3rd %.1f dB (plain: 3rd %.1f dB)", harmonicDb (shaped, 110.0, 2), harmonicDb (shaped, 110.0, 3), harmonicDb (plain, 110.0, 3)));
        set (v, "harmShape", 1);
        const auto sawed = render (v, { 1, 110.0, 0.2 }, 1.5);
        check (harmonicDb (sawed, 110.0, 2) > -20 && harmonicDb (sawed, 110.0, 3) > -24 && harmonicDb (sawed, 110.0, 5) > -30,
               juce::String::formatted ("tracked Saw on the sine: 2nd %.1f, 3rd %.1f, 5th %.1f dB", harmonicDb (sawed, 110.0, 2), harmonicDb (sawed, 110.0, 3), harmonicDb (sawed, 110.0, 5)));
        set (v, "harmShape", 2);
        const auto squared = render (v, { 1, 110.0, 0.2 }, 1.5);
        check (harmonicDb (squared, 110.0, 3) > -24 && harmonicDb (squared, 110.0, 2) < harmonicDb (squared, 110.0, 3) - 15,
               juce::String::formatted ("tracked Square on the sine: 3rd %.1f dB, 2nd %.1f dB (odd only)", harmonicDb (squared, 110.0, 3), harmonicDb (squared, 110.0, 2)));
        // Auto mode: a sine is thin (harmonics added), a saw is not (left alone)
        set (v, "harmMode", 0); set (v, "harmShape", 1);
        const auto autoSine = render (v, { 1, 110.0, 0.2 }, 1.5);
        const auto autoSaw = render (v, { 2, 110.0, 0.2 }, 1.5);
        set (v, "harmOn", 0);
        const auto plainSaw = render (v, { 2, 110.0, 0.2 }, 1.5);
        const double sawDiff = db (rms (autoSaw, sec (1.0), sec (1.5)) / rms (plainSaw, sec (1.0), sec (1.5)));
        check (harmonicDb (autoSine, 110.0, 2) > -22 && std::abs (sawDiff) < 0.5,
               juce::String::formatted ("Auto mode: the sine gets harmonics (2nd %.1f dB), the saw is left alone (%.2f dB change)", harmonicDb (autoSine, 110.0, 2), sawDiff));
        // the sub-octave
        set (v, "subOn", 1); set (v, "subAmount", 1);
        const auto sub = render (v, { 1, 110.0, 0.2 }, 1.5);
        const double subDb = db (toneAmplitude (sub, sec (1.0), sec (0.5), 55.0) / toneAmplitude (sub, sec (1.0), sec (0.5), 110.0));
        const double subPlain = db (toneAmplitude (plain, sec (1.0), sec (0.5), 55.0) / toneAmplitude (plain, sec (1.0), sec (0.5), 110.0));
        check (subDb > -12 && subPlain < -50, juce::String::formatted ("sub-octave 100%% on the 110 Hz sine: 55 Hz at %.1f dB (without: %.1f dB)", subDb, subPlain));
        // on noise nothing is added (no pitch)
        set (v, "harmOn", 1); set (v, "harmMode", 1);
        const auto noiseH = render (v, { 5, 0.0, 0.2 }, 1.5);
        set (v, "harmOn", 0); set (v, "subOn", 0);
        const auto noiseP = render (v, { 5, 0.0, 0.2 }, 1.5);
        const double nd = db (rms (noiseH, sec (1.0), sec (1.5)) / rms (noiseP, sec (1.0), sec (1.5)));
        check (std::abs (nd) < 0.5, juce::String::formatted ("harmonics + sub on noise: gated by the confidence (%.2f dB change)", nd));
    }

    // ---- transient preserve --------------------------------------------------------------------
    {
        auto v = neutral();
        set (v, "midWob", 1); set (v, "midDepth", 0); set (v, "cutoffLo", 150); set (v, "cutoffHi", 150); set (v, "reso", 0);
        auto hf = [] (const Audio& a)   // energy of the first difference: the hats
        {
            double acc = 0;
            for (int i = sec (0.5) + 1; i < a.size(); ++i) { const double d = (double) a.L[(size_t) i] - a.L[(size_t) i - 1]; acc += d * d; }
            return acc;
        };
        const auto closed = render (v, { 6, 0.0, 0.6 }, 2.0);
        set (v, "transPreserve", 1);
        const auto kept = render (v, { 6, 0.0, 0.6 }, 2.0);
        check (hf (kept) > hf (closed) * 4.0, juce::String::formatted ("transient preserve 100%% on the drum loop through a 150 Hz LP: high-frequency energy x%.1f", hf (kept) / std::max (hf (closed), 1e-12)));
        const auto steady = render (v, { 1, 440.0, 0.3 }, 1.0);
        set (v, "transPreserve", 0);
        const auto steady0 = render (v, { 1, 440.0, 0.3 }, 1.0);
        const double sd = db (rms (steady, sec (0.5), sec (1.0)) / rms (steady0, sec (0.5), sec (1.0)));
        check (std::abs (sd) < 0.5, juce::String::formatted ("..and does nothing to a steady tone (%.2f dB)", sd));
    }

    // ================================================================= phase 3 =====
    // ---- auto output gain ------------------------------------------------------------------------
    {
        auto v = neutral();
        set (v, "autoOut", 1); set (v, "midWob", 1); set (v, "midDepth", 0); set (v, "cutoffLo", 440); set (v, "cutoffHi", 440); set (v, "reso", 0.5); set (v, "resoLimit", 0);
        set (v, "outGain", -24);   // keep the clipper out of it
        auto level = [] (const Audio& a) { return db (rms (a, sec (3.0), sec (4.0)) * dbToGain (24)); };
        const auto hot = render (v, { 1, 440.0, 0.1 }, 4.0);       // a -23 dBFS RMS tone at the cutoff, Q 3.2: +10 dB without matching
        set (v, "autoOut", 0);
        const auto hot0 = render (v, { 1, 440.0, 0.1 }, 4.0);
        check (std::abs (level (hot) - (-23.0)) < 1.0 && level (hot0) > level (hot) + 8,
               juce::String::formatted ("auto output: a -23 dBFS RMS tone through Q 3.2 at the cutoff comes out at %.1f dB (without: %.1f dB)", level (hot), level (hot0)));
        set (v, "autoOut", 1); set (v, "reso", 1);
        const auto cap = render (v, { 1, 440.0, 0.1 }, 4.0);       // Q 20: +26 dB, the match can only take 12 of them
        check (std::abs (level (cap) - (3.0 - 12.0)) < 1.5, juce::String::formatted ("..through Q 20 (+26 dB) the match is capped at 12 dB: %.1f dB", level (cap)));
        set (v, "reso", 0); set (v, "cutoffLo", 20000); set (v, "cutoffHi", 20000); set (v, "midLevel", -9);
        const auto quiet = render (v, { 1, 440.0, 0.1 }, 4.0);     // a band 9 dB down: lifted back
        check (std::abs (level (quiet) - (-23.0)) < 0.5, juce::String::formatted ("..and a band at -9 dB comes back to %.1f dB", level (quiet)));
        set (v, "midLevel", 0);
        const auto sil = render (v, { 0, 0.0, 0.0 }, 2.0);
        check (peak (sil) == 0.0, "..silence stays silence (the gain holds)");
    }

    // ---- the true-peak limiter --------------------------------------------------------------------
    {
        auto v = neutral();
        set (v, "limiter", 1); set (v, "outGain", 12);
        const auto lim = render (v, { 2, 110.0, 0.5 }, 1.0);        // a saw at +12 dB: would clip hard
        // the inter-sample peak: the output 8x oversampled with a windowed sinc
        auto truePeak = [] (const Audio& a)
        {
            double p = 0;
            for (int i = sec (0.5) + 16; i < a.size() - 16; ++i)
                for (int k = 0; k < 8; ++k)
                {
                    const double t = k / 8.0;
                    double acc = 0;
                    for (int j = -8; j < 8; ++j)
                    {
                        const double u = j - t;
                        const double sinc = std::abs (u) < 1e-9 ? 1.0 : std::sin (kPi * u) / (kPi * u);
                        const double win = 0.5 + 0.5 * std::cos (kPi * u / 8.0);
                        acc += a.L[(size_t) (i + j)] * sinc * win;
                    }
                    p = std::max (p, std::abs (acc));
                }
            return p;
        };
        const double tp = truePeak (lim), sp = peak (lim);
        check (sp <= 0.985 && tp < 1.0, juce::String::formatted ("limiter on a saw at +12 dB: sample peak %.3f, true peak %.3f (ceiling 0.966)", sp, tp));
        const double loud = db (rms (lim, sec (0.5), sec (1.0)));
        check (loud > -12, juce::String::formatted ("..and it is still loud: %.1f dB RMS", loud));
        set (v, "limiter", 0);
        const auto clip = render (v, { 2, 110.0, 0.5 }, 1.0);
        check (peak (clip) > 0.99, juce::String::formatted ("without it the safety clipper takes it: peak %.3f", peak (clip)));
        // transparency: a -20 dBFS signal passes bit-exact apart from the 1 ms delay
        set (v, "outGain", 0); set (v, "limiter", 1);
        const auto on = render (v, { 2, 110.0, 0.1 }, 1.0);
        set (v, "limiter", 0);
        const auto off = render (v, { 2, 110.0, 0.1 }, 1.0);
        double err = 0;
        for (int i = sec (0.5); i < on.size(); ++i) err = std::max (err, std::abs ((double) on.L[(size_t) i] - off.L[(size_t) (i - 48)]));
        check (err < 1e-9, juce::String::formatted ("limiter under the ceiling: the output is the input 48 samples later (max diff %.1e)", err));
        check (WubEngine::totalLatency (1, true, 48000.0) == 51 && WubEngine::totalLatency (1, false, 48000.0) == 3 && WubEngine::totalLatency (0, true, 44100.0) == 44,
               "reported latency: 2x + limiter 51 samples at 48 kHz, 2x alone 3, 1x + limiter 44 at 44.1 kHz");
        // a step of +12 dB on a 60 Hz sine: the gain reduction is in place before the peak (no sample over the ceiling)
        set (v, "limiter", 1);
        const auto step = render (v, { 1, 60.0, 0.9 }, 1.0, {}, [] (int start, EngineParams& P) { P.outGain = start >= sec (0.5) ? dbToGain (12.0) : 1.0; });
        double over = 0;
        for (int i = 0; i < step.size(); ++i) over = std::max (over, (double) std::abs (step.L[(size_t) i]));
        check (over <= 0.985, juce::String::formatted ("a sudden +12 dB on a 60 Hz sine: peak %.3f (lookahead)", over));
    }

    // ================================================================= phase 4 =====
    // ---- other sample rates: 44.1 and 96 kHz behave like 48 kHz ------------------------------------
    for (double sr : { 44100.0, 96000.0 })
    {
        const auto S = [sr] (double s) { return (int) (s * sr); };
        // the LFO rate, the pitch tracker's range, the tracked sub, the limiter's lookahead
        auto v = neutral();
        set (v, "midWob", 1); set (v, "lfoRate", 1.5); set (v, "lfoSmooth", 0); set (v, "cutoffLo", 100); set (v, "cutoffHi", 4000);
        const auto a = render (v, { 2, 110.0, 0.3 }, 8.0, {}, {}, {}, sr);
        std::vector<double> env;
        for (int i = 0; i + S (0.02) <= a.size(); i += S (0.02))
        {
            double acc = 0;
            for (int j = i; j < i + S (0.02); ++j) acc += (double) a.L[(size_t) j] * a.L[(size_t) j];
            env.push_back (std::sqrt (acc / S (0.02)));
        }
        const double f = traceFrequency (env, 50.0, 0.5, 5.0);
        check (std::abs (f - 1.5) < 0.05, juce::String::formatted ("%.1f kHz: the wobble runs at %.2f Hz (1.5 set)", sr / 1000, f));

        auto enginePtr = std::make_unique<WubEngine>();
        auto& e = *enginePtr;
        auto vp = neutral();
        set (vp, "subOn", 1); set (vp, "subAmount", 1); set (vp, "mix", 0);
        e.setParams (makeEngineParams ([&vp] (int i) { return vp[(size_t) i]; }));
        e.prepare (sr);
        TestSource g; g.prepare (1, 41.2, 0.3, sr);
        const int n = S (2.0);
        Audio out; out.L.resize ((size_t) n); out.R.resize ((size_t) n);
        std::vector<float> inL ((size_t) n), inR ((size_t) n);
        for (int i = 0; i < n; ++i) { double l, r; g.next (l, r); inL[(size_t) i] = (float) l; inR[(size_t) i] = (float) r; }
        for (int p = 0; p < n; p += kBlock) e.render (inL.data() + p, inR.data() + p, out.L.data() + p, out.R.data() + p, std::min (kBlock, n - p));
        // the sub-octave at 20.6 Hz, measured with a DFT at this rate
        auto amp = [&] (double hz)
        {
            double re = 0, im = 0; const int from = S (1.0), len = S (0.9);
            for (int i = 0; i < len; ++i) { const double w = 0.5 - 0.5 * std::cos (kTwoPi * i / (len - 1)); re += out.L[(size_t) (from + i)] * w * std::cos (kTwoPi * hz * i / sr); im -= out.L[(size_t) (from + i)] * w * std::sin (kTwoPi * hz * i / sr); }
            return std::sqrt (re * re + im * im);
        };
        check (std::abs (e.pitchHz() / 41.2 - 1.0) < 0.01 && e.pitchConfidence() > 0.85 && db (amp (20.6) / amp (41.2)) > -12,
               juce::String::formatted ("%.1f kHz: a 41.2 Hz sine tracks at %.2f Hz (confidence %.2f), the sub at 20.6 Hz sits %.1f dB from it", sr / 1000, e.pitchHz(), e.pitchConfidence(), db (amp (20.6) / amp (41.2))));
        check (WubEngine::totalLatency (1, true, sr) == 3 + (int) (0.001 * sr + 0.5), juce::String::formatted ("%.1f kHz: latency %d samples (3 + 1 ms)", sr / 1000, WubEngine::totalLatency (1, true, sr)));
    }

    // ---- switching things while playing: no NaNs, no runaway, and the sizes of the steps ------------
    {
        auto v = defaultValues();
        set (v, "lfoSync", 0);
        struct Toggle { const char* id; double a, b; };
        const Toggle toggles[] { { "bands", 0, 1 }, { "os", 1, 2 }, { "filterType", 0, 4 }, { "stereoMode", 0, 2 }, { "drivePos", 0, 1 }, { "harmOn", 0, 1 }, { "limiter", 1, 0 } };
        for (const auto& t : toggles)
        {
            auto vv = v;
            set (vv, t.id, t.a);
            const int at = paramIndex (t.id);
            const auto a = render (vv, { 4, 110.0, 0.4 }, 1.0, {}, [&] (int start, EngineParams& P) { if (start >= sec (0.5)) P = makeEngineParams ([&] (int i) { return i == at ? (float) t.b : vv[(size_t) i]; }); });
            double stepAt = 0, stepElse = 0;
            for (int i = 1; i < a.size(); ++i)
            {
                const double d = std::abs ((double) a.L[(size_t) i] - a.L[(size_t) i - 1]);
                if (i >= sec (0.5) && i < sec (0.5) + 512) stepAt = std::max (stepAt, d); else if (i > sec (0.2)) stepElse = std::max (stepElse, d);   // 512: longer than a 110 Hz period
            }
            check (finite (a) && peak (a) <= 1.0, "switching " + juce::String (t.id) + juce::String::formatted (" %g -> %g while playing: finite, largest step %.3f at the switch vs %.3f elsewhere", t.a, t.b, stepAt, stepElse));
        }
    }

    // ---- block size independence: 128-sample blocks vs one 4800-sample block -------------------
    {
        auto v = defaultValues();
        set (v, "lfoSync", 0); set (v, "harmOn", 1); set (v, "harmShape", 1); set (v, "subOn", 1); set (v, "keyTrack", 0.5); set (v, "transPreserve", 0.5);
        set (v, "autoOut", 1); set (v, "limiter", 1); set (v, "outGain", 6);
        auto run = [&v] (int block)
        {
            auto enginePtr = std::make_unique<WubEngine>();
            auto& e = *enginePtr;
            e.setParams (makeEngineParams ([&v] (int i) { return v[(size_t) i]; }));
            e.prepare (kSr);
            TestSource g; g.prepare (4, 110.0, 0.4, kSr);
            const int total = 48000;
            std::vector<float> inL ((size_t) total), inR ((size_t) total), oL ((size_t) total), oR ((size_t) total);
            for (int i = 0; i < total; ++i) { double l, r; g.next (l, r); inL[(size_t) i] = (float) l; inR[(size_t) i] = (float) r; }
            for (int p = 0; p < total; p += block) e.render (inL.data() + p, inR.data() + p, oL.data() + p, oR.data() + p, std::min (block, total - p));
            return oL;
        };
        const auto a = run (128), b = run (4800), c = run (37);
        double d1 = 0, d2 = 0;
        for (size_t i = 0; i < a.size(); ++i) { d1 = std::max (d1, (double) std::abs (a[i] - b[i])); d2 = std::max (d2, (double) std::abs (a[i] - c[i])); }
        check (d1 < 1e-6 && d2 < 1e-6, juce::String::formatted ("block size independence: 128 vs 4800 samples %.1e, vs 37 %.1e", d1, d2));
    }

    std::printf (fails == 0 ? "ENGINE CHECKS OK\n" : "ENGINE CHECKS: %d FAILED\n", fails);
    return fails;
}
