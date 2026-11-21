// Engine tools for the console test app:
//   --dump-params                              the parameter table as JSON (tests/check-params.ps1 compares it with params.js)
//   --render <preset> <source> <out.wav>       offline render of a test source through a preset; preset = index or "Category/Name";
//                                              source = sine | saw | square | chord | noise | drums | wide [@freq]  (e.g. saw@110)
//   --bench                                    worst-case CPU
//   --loudness                                 loudness of every factory preset on its category's test source (level matching)
//   --export-presets <folder>                  every factory preset as a complete .wubpreset file, <folder>\<Category>\<NN Name>.wubpreset
#include "TestRender.h"

using namespace wub;
using namespace wub::test;

namespace
{
const char* kindName (Kind k)
{
    switch (k) { case Kind::Bool: return "bool"; case Kind::Choice: return "choice"; case Kind::Int: return "int"; default: return "float"; }
}

const char* unitName (Unit u)
{
    switch (u)
    {
        case Unit::Seconds: return "s";       case Unit::Hz: return "Hz";        case Unit::LfoHz: return "lfohz";
        case Unit::Semitones: return "st";    case Unit::Cents: return "ct";     case Unit::Percent: return "%";
        case Unit::BipolarPercent: return "bip"; case Unit::Decibels: return "dB"; case Unit::Degrees: return "deg";
        case Unit::Ms: return "ms";           case Unit::Count: return "count";  case Unit::Note: return "note";
        case Unit::Octaves: return "oct";     default: return "";
    }
}

const PresetData* findPreset (const PresetManager& pm, const juce::String& spec)
{
    const auto& f = pm.factory();
    if (spec.containsOnly ("0123456789"))
    {
        const int i = spec.getIntValue();
        return i >= 0 && i < (int) f.size() ? &f[(size_t) i] : nullptr;
    }
    for (const auto& p : f)
        if (p.category + "/" + p.name == spec || p.name == spec) return &p;
    return nullptr;
}

Source parseSource (const juce::String& spec)
{
    const juce::StringArray names { "silence", "sine", "saw", "square", "chord", "noise", "drums", "wide" };
    Source s;
    const auto name = spec.upToFirstOccurrenceOf ("@", false, false);
    s.type = std::max (0, names.indexOf (name));
    if (spec.contains ("@")) s.freq = spec.fromFirstOccurrenceOf ("@", false, false).getDoubleValue();
    else s.freq = s.type == 1 ? 55.0 : 110.0;
    s.level = s.type == 6 ? 0.6 : 0.4;
    return s;
}
} // namespace

int runDumpParams()
{
    juce::Array<juce::var> list;
    for (const auto& d : paramDefs())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", d.id);
        o->setProperty ("type", kindName (d.kind));
        o->setProperty ("min", d.min);
        o->setProperty ("max", d.max);
        o->setProperty ("def", d.def);
        o->setProperty ("unit", unitName (d.unit));
        o->setProperty ("log", d.logSkew);
        o->setProperty ("noPreset", d.noPreset);
        juce::Array<juce::var> ch;
        for (const auto& c : d.choices) ch.add (c);
        o->setProperty ("choices", ch);
        list.add (juce::var (o));
    }
    std::printf ("%s\n", juce::JSON::toString (juce::var (list), true).toRawUTF8());
    return 0;
}

int runRender (const juce::String& spec, const juce::String& sourceSpec, const juce::File& out)
{
    PresetManager pm;
    const auto* p = findPreset (pm, spec);
    if (p == nullptr) { std::printf ("unknown preset %s\n", spec.toRawUTF8()); return 2; }
    const auto v = presetValues (*p);
    const auto a = render (v, parseSource (sourceSpec), 8.0, {}, {}, { 120, true, 0 });
    if (! writeWav (out, a)) { std::printf ("cannot write %s\n", out.getFullPathName().toRawUTF8()); return 2; }
    std::printf ("%s/%s on %s -> %s  (8.0 s, peak %.3f)\n", p->category.toRawUTF8(), p->name.toRawUTF8(), sourceSpec.toRawUTF8(),
                 out.getFullPathName().toRawUTF8(), peak (a));
    return 0;
}

// The test source that stands in for each preset category (the level matching uses it).
Source categorySource (const juce::String& category)
{
    if (category == "Bass")   return { 1, 55.0, 0.5 };      // a sine bass
    if (category == "Vocal")  return { 4, 220.0, 0.3 };     // a chord of saws around a vocal range
    if (category == "Guitar") return { 4, 110.0, 0.4 };
    if (category == "Drums")  return { 6, 0.0, 0.6 };       // the drum loop
    if (category == "Pad")    return { 4, 165.0, 0.3 };
    return { 6, 0.0, 0.6 };                                 // Full Mix: the drum loop too
}

// Loudness of a preset: RMS of its output over the last 2 s of a 4 s render of its category source
// (the wobble averages out over 2 s), at 120 BPM playing. The target is -18 dBFS, like the auto input gain.
double presetLoudness (const PresetData& p)
{
    const auto v = presetValues (p);
    const auto a = render (v, categorySource (p.category), 4.0, {}, {}, { 120, true, 0 });
    return rms (a, sec (2.0), sec (4.0));
}

int runLoudness()
{
    PresetManager pm;
    std::printf ("[\n");
    const auto& f = pm.factory();
    for (size_t pi = 0; pi < f.size(); ++pi)
    {
        const auto v = presetValues (f[pi]);
        const double loud = db (presetLoudness (f[pi]));
        std::printf ("  { \"index\": %d, \"name\": \"%s\", \"category\": \"%s\", \"loudnessDb\": %.2f, \"outGain\": %.2f, \"suggested\": %.1f }%s\n",
                     (int) pi, f[pi].name.toRawUTF8(), f[pi].category.toRawUTF8(), loud, v[(size_t) outGain],
                     juce::jlimit (-24.0, 24.0, std::round ((v[(size_t) outGain] + (-18.0 - loud)) * 10.0) / 10.0), pi + 1 < f.size() ? "," : "");
    }
    std::printf ("]\n");
    return 0;
}

// The factory presets as files (complete: every parameter resolved), for sharing and for the web build's linked folder.
int runExportPresets (const juce::File& folder)
{
    PresetManager pm;
    int index = 0, written = 0;
    for (const auto& p : pm.factory())
    {
        PresetData full = p;
        const auto v = presetValues (p);
        for (int i = 0; i < kNumParams; ++i) full.values[(size_t) i] = v[(size_t) i];
        const auto file = folder.getChildFile (juce::File::createLegalFileName (p.category))
                                .getChildFile (juce::String (index + 1).paddedLeft ('0', 2) + " " + juce::File::createLegalFileName (p.name) + PresetManager::fileExtension);
        juce::String error;
        if (PresetManager::writeFile (file, full, error)) ++written;
        else std::printf ("%s\n", error.toRawUTF8());
        ++index;
    }
    std::printf ("%d of %d presets written to %s\n", written, index, folder.getFullPathName().toRawUTF8());
    return written == index ? 0 : 1;
}

// Worst case: three bands, every wobble on in formant mode, 4x drive on all of them, mono-below on, stereo.
int runBench()
{
    auto v = defaultValues();
    set (v, "bands", 1); set (v, "loWob", 1); set (v, "midWob", 1); set (v, "hiWob", 1); set (v, "filterType", 4);
    set (v, "os", 2); set (v, "loDrive", 0.8); set (v, "midDrive", 0.8); set (v, "hiDrive", 0.8); set (v, "monoBelow", 120);
    set (v, "lfoSync", 0); set (v, "lfoRate", 4);
    set (v, "harmOn", 1); set (v, "harmMode", 1); set (v, "harmShape", 1); set (v, "subOn", 1); set (v, "keyTrack", 1); set (v, "transPreserve", 1);
    set (v, "autoOut", 1); set (v, "limiter", 1);
    const double seconds = 10.0;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto a = render (v, { 4, 110.0, 0.4 }, seconds);
    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
    std::printf ("bench: 3 bands, formant wobble on all, 4x drive on all, mono-below, stereo, tracked saw + sub, key tracking, transients, auto out, limiter: %.1f%% of one core (%.0f ms for %.0f s of audio), peak %.2f\n",
                 100.0 * ms / (seconds * 1000.0), ms, seconds, peak (a));
    auto v1 = defaultValues();
    set (v1, "lfoSync", 0);
    const auto t1 = juce::Time::getMillisecondCounterHiRes();
    const auto b = render (v1, { 4, 110.0, 0.4 }, seconds);
    const double ms1 = juce::Time::getMillisecondCounterHiRes() - t1;
    std::printf ("bench: defaults (single band LP, 2x): %.1f%% of one core, peak %.2f\n", 100.0 * ms1 / (seconds * 1000.0), peak (b));
    return 0;
}
