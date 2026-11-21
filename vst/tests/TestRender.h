#pragma once
// Offline rendering + analysis helpers shared by the console tests and tools.
#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include "EngineParamsFromValues.h"
#include "PresetManager.h"
#include "dsp/TestSignals.h"

namespace wub::test
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 128;

using Values = std::array<float, kNumParams>;

inline Values defaultValues()
{
    Values v {};
    for (int i = 0; i < kNumParams; ++i) v[(size_t) i] = paramDefs()[(size_t) i].def;
    return v;
}

inline void applyPresetData (Values& v, const PresetData& p)
{
    for (int i = 0; i < kNumParams; ++i)
        if (! std::isnan (p.values[(size_t) i])) v[(size_t) i] = p.values[(size_t) i];
}

inline int paramIndex (const juce::String& id)
{
    for (int i = 0; i < kNumParams; ++i)
        if (paramDefs()[(size_t) i].id == id) return i;
    return -1;
}

inline Values presetValues (const PresetData& p)
{
    auto v = defaultValues();
    applyPresetData (v, p);
    return v;
}

inline void set (Values& v, const char* id, double value)
{
    const int i = paramIndex (id);
    jassert (i >= 0);
    if (i >= 0) v[(size_t) i] = (float) value;
}

// Test input: a TestSource type (dsp/TestSignals.h), frequency, level.
struct Source
{
    int type = 1;          // 0 silence 1 sine 2 saw 3 square 4 chord 5 noise 6 drums 7 wide
    double freq = 55.0, level = 0.5;
};

// Timed events (sample position).
struct Event
{
    enum Type { NoteOn, NoteOff, Retrig };
    int time;
    Type type;
    int a = 0;       // note
};

struct Audio
{
    std::vector<float> L, R;
    int size() const { return (int) L.size(); }
};

struct Transport { double bpm = 120; bool playing = false; double ppq0 = 0; };

// Renders `seconds` of the source through the engine, applying events at their exact sample (like
// the plugin). `onBlock` (optional) runs before each block, e.g. to change parameters mid-render.
// `sampleRate` defaults to kSr (the sec() helpers assume kSr; the sample-rate checks convert themselves).
inline Audio render (const Values& values, Source src, double seconds, std::vector<Event> events = {},
                     std::function<void (int, EngineParams&)> onBlock = {}, Transport tr = {}, double sampleRate = kSr)
{
    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b) { return a.time < b.time; });
    auto enginePtr = std::make_unique<WubEngine>();
    auto& engine = *enginePtr;
    auto P = makeEngineParams ([&values] (int i) { return values[(size_t) i]; });
    engine.setParams (P);
    engine.prepare (sampleRate);
    TestSource gen;
    gen.prepare (src.type, src.freq, src.level, sampleRate);
    const int total = (int) (seconds * sampleRate);
    Audio out;
    out.L.assign ((size_t) total, 0.0f);
    out.R.assign ((size_t) total, 0.0f);
    std::vector<float> inL ((size_t) total), inR ((size_t) total);
    for (int i = 0; i < total; ++i)
    {
        double l, r;
        gen.next (l, r);
        inL[(size_t) i] = (float) l;
        inR[(size_t) i] = (float) r;
    }
    size_t ei = 0;
    for (int start = 0; start < total; start += kBlock)
    {
        const int end = std::min (total, start + kBlock);
        if (onBlock) onBlock (start, P);
        engine.setParams (P);
        engine.setTransport (tr.bpm, tr.ppq0 + start / sampleRate * tr.bpm / 60.0, tr.playing);
        int pos = start;
        auto run = [&] (int to)
        {
            if (to > pos)
            {
                engine.render (inL.data() + pos, inR.data() + pos, out.L.data() + pos, out.R.data() + pos, to - pos);
                pos = to;
            }
        };
        while (ei < events.size() && events[ei].time < end)
        {
            run (std::max (pos, events[ei].time));
            const auto& ev = events[ei++];
            if (ev.type == Event::NoteOn) engine.noteOn (ev.a, 100);
            else if (ev.type == Event::NoteOff) engine.noteOff (ev.a);
            else engine.retrigger();
        }
        run (end);
    }
    return out;
}

inline int sec (double s) { return (int) (s * kSr); }

inline double rms (const Audio& a, int from, int to)
{
    from = std::max (0, from); to = std::min (a.size(), to);
    double acc = 0;
    for (int i = from; i < to; ++i) acc += 0.5 * ((double) a.L[(size_t) i] * a.L[(size_t) i] + (double) a.R[(size_t) i] * a.R[(size_t) i]);
    return to > from ? std::sqrt (acc / (to - from)) : 0.0;
}

inline double rmsL (const Audio& a, int from, int to)
{
    from = std::max (0, from); to = std::min (a.size(), to);
    double acc = 0;
    for (int i = from; i < to; ++i) acc += (double) a.L[(size_t) i] * a.L[(size_t) i];
    return to > from ? std::sqrt (acc / (to - from)) : 0.0;
}

inline double peak (const Audio& a)
{
    double p = 0;
    for (int i = 0; i < a.size(); ++i) p = std::max (p, (double) std::max (std::abs (a.L[(size_t) i]), std::abs (a.R[(size_t) i])));
    return p;
}

inline bool finite (const Audio& a)
{
    for (int i = 0; i < a.size(); ++i)
        if (! std::isfinite (a.L[(size_t) i]) || ! std::isfinite (a.R[(size_t) i])) return false;
    return true;
}

inline double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-12)); }

// Amplitude of the component at f Hz in [from, from + n) of the left channel (Hann-windowed
// single-bin DFT, scaled so a sine of amplitude A reads A).
inline double toneAmplitude (const Audio& a, int from, int n, double f)
{
    double re = 0, im = 0, wsum = 0;
    const double w = kTwoPi * f / kSr;
    for (int i = 0; i < n; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (kTwoPi * i / (n - 1));
        const double x = a.L[(size_t) (from + i)] * win;
        re += x * std::cos (w * i);
        im -= x * std::sin (w * i);
        wsum += win;
    }
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

// Frequency of the strongest component in [lo, hi] Hz of a sequence of values sampled at `rate`
// (for LFO / envelope traces): Hann-windowed DFT on a grid, refined by a parabola.
inline double traceFrequency (const std::vector<double>& x, double rate, double lo, double hi, double step = 0.01)
{
    const int n = (int) x.size();
    double mean = 0;
    for (double v : x) mean += v;
    mean /= std::max (1, n);
    auto power = [&] (double f)
    {
        double re = 0, im = 0;
        const double w = kTwoPi * f / rate;
        for (int i = 0; i < n; ++i)
        {
            const double win = 0.5 - 0.5 * std::cos (kTwoPi * i / (n - 1));
            const double v = (x[(size_t) i] - mean) * win;
            re += v * std::cos (w * i);
            im -= v * std::sin (w * i);
        }
        return re * re + im * im;
    };
    double best = lo, bestP = -1;
    for (double f = lo; f <= hi; f += 0.05) { const double p = power (f); if (p > bestP) { bestP = p; best = f; } }
    double fineBest = best;
    bestP = -1;
    for (double f = best - 0.05; f <= best + 0.05; f += step) { const double p = power (f); if (p > bestP) { bestP = p; fineBest = f; } }
    return fineBest;
}

inline bool writeWav (const juce::File& f, const Audio& a)
{
    f.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (f);
    const auto opts = juce::AudioFormatWriterOptions{}.withSampleRate (kSr).withNumChannels (2).withBitsPerSample (24);
    auto w = fmt.createWriterFor (out, opts);
    if (w == nullptr) return false;
    juce::AudioBuffer<float> b (2, a.size());
    b.copyFrom (0, 0, a.L.data(), a.size());
    b.copyFrom (1, 0, a.R.data(), a.size());
    return w->writeFromAudioSampleBuffer (b, 0, a.size());
}
} // namespace wub::test
