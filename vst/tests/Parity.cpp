// --parity <scenarios.json>: renders every scenario with the C++ engine and prints the
// results as JSON. tests/run-parity.ps1 renders the same scenarios with the JS engine
// (vst/webhost/dsp-worklet.js) in headless Edge and compares the two.
//
// Scenario format (tests/parity_scenarios.json):
//   { "name": "...", "preset": "Category/Name"?, "params": { id: value }?, "blocks": N,
//     "source": { "type": 0..7 | "sine".., "freq": Hz, "level": 0..1 },
//     "transport": { "bpm": 120, "playing": true, "ppq": 0 }?,
//     "events": [ [block, type, args...], ... ] }
// Blocks are 128 samples at 48 kHz. Events are applied at the start of their block, after the
// block's parameter snapshot (as in the plugin). Types:
//   param id value (a parameter change, as host automation) · on note [vel] · off note · retrig ·
//   transport bpm playing [ppq]
// Output per scenario: RMS of each channel in 16 equal windows, the peak, and the render time.
#include "TestRender.h"

using namespace wub;
using namespace wub::test;

namespace
{
constexpr int kWindows = 16;

const PresetData* findFactory (const PresetManager& pm, const juce::String& spec)
{
    for (const auto& p : pm.factory())
        if (p.category + "/" + p.name == spec || p.name == spec) return &p;
    return nullptr;
}

int argInt (const juce::var& ev, int i, int def = 0) { return ev.size() > i ? (int) ev[i] : def; }
double argNum (const juce::var& ev, int i, double def = 0) { return ev.size() > i ? (double) ev[i] : def; }

bool setValue (Values& values, const juce::String& id, double v)
{
    const int i = paramIndex (id);
    if (i < 0) { std::printf ("unknown param %s\n", id.toRawUTF8()); return false; }
    values[(size_t) i] = (float) v;
    return true;
}

int sourceType (const juce::var& t)
{
    if (t.isString())
    {
        const juce::StringArray names { "silence", "sine", "saw", "square", "chord", "noise", "drums", "wide" };
        return std::max (0, names.indexOf (t.toString()));
    }
    return (int) t;
}
} // namespace

int runParity (const juce::File& scenarioFile)
{
    const auto json = juce::JSON::parse (scenarioFile);
    if (! json.isArray()) { std::printf ("cannot read %s\n", scenarioFile.getFullPathName().toRawUTF8()); return 2; }

    PresetManager pm;
    juce::Array<juce::var> results;

    for (const auto& sc : *json.getArray())
    {
        auto values = defaultValues();
        if (sc.hasProperty ("preset"))
        {
            const auto* p = findFactory (pm, sc["preset"].toString());
            if (p == nullptr) { std::printf ("unknown preset %s\n", sc["preset"].toString().toRawUTF8()); return 2; }
            applyPresetData (values, *p);
        }
        if (auto* obj = sc["params"].getDynamicObject())
            for (const auto& prop : obj->getProperties())
                if (! setValue (values, prop.name.toString(), (double) prop.value)) return 2;

        const int blocks = (int) sc["blocks"];
        const auto* events = sc["events"].getArray();
        const auto src = sc["source"];
        TestSource gen;
        gen.prepare (src.isVoid() ? 1 : sourceType (src["type"]), src.hasProperty ("freq") ? (double) src["freq"] : 55.0,
                     src.hasProperty ("level") ? (double) src["level"] : 0.5, kSr);
        double bpm = 120, ppq = 0;
        bool playing = false;
        if (const auto tr = sc["transport"]; tr.isObject())
        {
            bpm = tr.hasProperty ("bpm") ? (double) tr["bpm"] : 120.0;
            playing = (bool) tr["playing"];
            ppq = tr.hasProperty ("ppq") ? (double) tr["ppq"] : 0.0;
        }

        auto P = makeEngineParams ([&values] (int i) { return values[(size_t) i]; });
        auto engine = std::make_unique<WubEngine>();   // fresh per scenario, like the JS runner
        engine->setParams (P);
        engine->prepare (kSr);
        std::array<float, kBlock> inL {}, inR {}, L {}, R {};
        const int per = blocks * kBlock / kWindows;
        double sl[kWindows] {}, sr[kWindows] {}, peakv = 0;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int b = 0; b < blocks; ++b)
        {
            engine->setParams (P);
            engine->setTransport (bpm, ppq + b * kBlock / kSr * bpm / 60.0, playing);
            if (events != nullptr)
                for (const auto& ev : *events)
                    if ((int) ev[0] == b)
                    {
                        const auto type = ev[1].toString();
                        if (type == "param") { if (setValue (values, ev[2].toString(), argNum (ev, 3))) { P = makeEngineParams ([&values] (int i) { return values[(size_t) i]; }); engine->setParams (P); } }
                        else if (type == "on") engine->noteOn (argInt (ev, 2), argInt (ev, 3, 100));
                        else if (type == "off") engine->noteOff (argInt (ev, 2));
                        else if (type == "retrig") engine->retrigger();
                        else if (type == "transport")
                        {
                            bpm = argNum (ev, 2, 120); playing = argInt (ev, 3) != 0;
                            ppq = argNum (ev, 4, 0) - b * kBlock / kSr * bpm / 60.0;   // so the running ppq lands on the given value now
                            engine->setTransport (bpm, argNum (ev, 4, 0), playing);
                        }
                        else std::printf ("unknown event type %s\n", type.toRawUTF8());
                    }
            for (int i = 0; i < kBlock; ++i) { double l, r; gen.next (l, r); inL[(size_t) i] = (float) l; inR[(size_t) i] = (float) r; }
            engine->render (inL.data(), inR.data(), L.data(), R.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                const int w = std::min (kWindows - 1, (b * kBlock + i) / std::max (per, 1));
                sl[w] += (double) L[(size_t) i] * L[(size_t) i];
                sr[w] += (double) R[(size_t) i] * R[(size_t) i];
                peakv = std::max (peakv, (double) std::max (std::abs (L[(size_t) i]), std::abs (R[(size_t) i])));
            }
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;

        juce::Array<juce::var> rl, rr;
        for (int w = 0; w < kWindows; ++w)
        {
            rl.add (std::sqrt (sl[w] / std::max (per, 1)));
            rr.add (std::sqrt (sr[w] / std::max (per, 1)));
        }
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", sc["name"]);
        o->setProperty ("rmsL", rl);
        o->setProperty ("rmsR", rr);
        o->setProperty ("peak", peakv);
        o->setProperty ("ms", ms);
        o->setProperty ("audioMs", blocks * kBlock / kSr * 1000.0);
        results.add (juce::var (o));
    }
    std::printf ("%s\n", juce::JSON::toString (juce::var (results), true, 9).toRawUTF8());
    return 0;
}
