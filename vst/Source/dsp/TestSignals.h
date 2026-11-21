#pragma once
// ============================================================================
//  Deterministic test sources, shared by the C++ tests, the parity tool and the
//  web build's built-in sources (twin in dsp-worklet.js). An effect needs an
//  input; these make the same samples in both engines.
//    0 silence · 1 sine · 2 saw · 3 square · 4 chord (3 saws: f, 5/4 f, 3/2 f) ·
//    5 noise (seeded) · 6 drums (kick / snare / hats pattern at 120 BPM) ·
//    7 wide (L saw at f, R saw at 5/4 f: a stereo source with side content)
//  Sines come from a rotating phasor, decays from a per-sample multiply, so
//  the two languages stay bit-close.
// ============================================================================
#include "Common.h"

namespace wub
{
struct TestSource
{
    int type = 1;
    double freq = 55.0, level = 0.5, sr = 48000.0;
    double c = 1, s = 0, dc = 1, ds = 0;    // sine phasor
    double ph = 0, ph2 = 0, ph3 = 0;        // saw / square phases
    Rng rng;
    // drums
    int pos = 0;
    double kickC = 1, kickS = 0, kickEnv = 0, snareEnv = 0, hatEnv = 0, hatPrev = 0, snareC = 1, snareS = 0;

    void prepare (int typeIn, double freqIn, double levelIn, double sampleRate)
    {
        type = typeIn; freq = freqIn; level = levelIn; sr = sampleRate;
        rng.seed (0x57554231u); // "WUB1"
        c = 1; s = 0;
        const double w = kTwoPi * freq / sr;
        dc = std::cos (w); ds = std::sin (w);
        ph = ph2 = ph3 = 0;
        pos = 0; kickC = 1; kickS = 0; kickEnv = snareEnv = hatEnv = 0; hatPrev = 0; snareC = 1; snareS = 0;
    }

    static double saw (double& p, double inc)
    {
        p += inc;
        if (p >= 1.0) p -= 1.0;
        return 2.0 * p - 1.0;
    }

    void next (double& L, double& R)
    {
        double l = 0, r = 0;
        const double inc = freq / sr;
        switch (type)
        {
            case 1:
            {
                const double nc = c * dc - s * ds, ns = s * dc + c * ds;
                c = nc; s = ns;
                const double n = 0.5 * (3.0 - (c * c + s * s));   // renormalise
                c *= n; s *= n;
                l = r = s;
                break;
            }
            case 2: l = r = saw (ph, inc); break;
            case 3: { saw (ph, inc); l = r = ph < 0.5 ? 1.0 : -1.0; break; }
            case 4: l = r = (saw (ph, inc) + saw (ph2, inc * 1.25) + saw (ph3, inc * 1.5)) / 3.0; break;
            case 5: l = r = rng.bi(); break;
            case 6:
            {
                // 120 BPM: a beat every 0.5 s; kick on 1 and 3, snare on 2 and 4, hats every eighth
                const int beat = (int) (0.5 * sr), eighth = beat / 2;
                const int inBar = pos % (4 * beat);
                if (inBar % beat == 0)
                {
                    const int b = inBar / beat;
                    if (b == 0 || b == 2) { kickEnv = 1.0; kickC = 1; kickS = 0; }
                    else { snareEnv = 1.0; }
                }
                if (pos % eighth == 0) hatEnv = 1.0;
                // kick: 55 Hz phasor, decays to -60 dB over 0.35 s
                const double kw = kTwoPi * 55.0 / sr, kc = std::cos (kw), ks = std::sin (kw);
                const double nc = kickC * kc - kickS * ks, ns = kickS * kc + kickC * ks;
                kickC = nc; kickS = ns;
                kickEnv *= std::exp (-6.9078 / (0.35 * sr));
                // snare: noise + 200 Hz tone, 0.15 s
                const double sw = kTwoPi * 200.0 / sr, sc = std::cos (sw), ss = std::sin (sw);
                const double nsc = snareC * sc - snareS * ss, nss = snareS * sc + snareC * ss;
                snareC = nsc; snareS = nss;
                snareEnv *= std::exp (-6.9078 / (0.15 * sr));
                const double noise = rng.bi();
                // hats: first-differenced noise (bright), 0.05 s
                hatEnv *= std::exp (-6.9078 / (0.05 * sr));
                const double hat = (noise - hatPrev) * 0.5;
                hatPrev = noise;
                l = r = kickS * kickEnv * 1.0 + (noise * 0.6 + snareS * 0.4) * snareEnv * 0.7 + hat * hatEnv * 0.5;
                ++pos;
                break;
            }
            case 7: l = saw (ph, inc); r = saw (ph2, inc * 1.25); break;
            default: break;
        }
        L = l * level;
        R = r * level;
    }
};
} // namespace wub
