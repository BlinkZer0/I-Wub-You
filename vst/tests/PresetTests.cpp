// Console tests (no audio device / window needed).
// Build:  vst\build.bat   (target IWubYouTests)
// Run without arguments for the full test suite; see EngineTools.cpp for the tool commands.
#include "TestRender.h"
#include "dsp/Sources.h"

using namespace wub;
using namespace wub::test;

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

int runDumpParams();
int runRender (const juce::String&, const juce::String&, const juce::File&);
int runBench();
int runLoudness();
int runExportPresets (const juce::File& folder);
double presetLoudness (const PresetData&);
int runHostCheck (const juce::String& path);
int runEngineTests();
int runParity (const juce::File& scenarioFile);

static void testParamTable()
{
    const auto& defs = paramDefs();
    CHECK ((int) defs.size() == kNumParams);
    std::printf ("parameters: %d\n", kNumParams);
    juce::StringArray ids;
    for (const auto& d : defs)
    {
        CHECK (! ids.contains (d.id));
        ids.add (d.id);
        CHECK (d.min < d.max);
        CHECK (d.def >= d.min && d.def <= d.max);
        if (d.kind == Kind::Choice) CHECK (d.choices.size() == (int) d.max + 1);
        if (d.logSkew) CHECK (d.min > 0);
    }
    // index helpers line up with the table
    CHECK (defs[(size_t) bandParam (0, 0)].id == "loWob");
    CHECK (defs[(size_t) bandParam (1, 2)].id == "midDrive");
    CHECK (defs[(size_t) bandParam (2, 4)].id == "hiLevel" && filterType == bandParam (2, 4) + 1);
    CHECK (defs[(size_t) limiter].id == "limiter" && limiter == kNumParams - 1);
    CHECK (defs[(size_t) lfoDiv].choices.size() == kNumDivs);
    CHECK (defs[(size_t) source].choices.size() == kNumSourceTypes + 1);
    // the log ranges round-trip through the host's 0..1
    for (const auto& d : defs)
        if (d.kind == Kind::Float)
        {
            const auto r = paramRange (d);
            for (float v : { d.min, d.def, d.max })
                CHECK (std::abs (r.convertFrom0to1 (r.convertTo0to1 (v)) - v) < 1e-3f * std::max (1.0f, std::abs (v)));
        }
}

static void testFactoryPresets (const PresetManager& pm)
{
    const auto& f = pm.factory();
    std::printf ("factory presets: %d\n", (int) f.size());
    CHECK (f.size() == 96);   // 6 source categories x 16
    juce::StringArray cats, names;
    for (const auto& p : f)
    {
        cats.addIfNotAlreadyThere (p.category);
        CHECK (p.name.isNotEmpty());
        CHECK (! names.contains (p.name));
        names.add (p.name);
        for (int i = 0; i < kNumParams; ++i)
        {
            const float v = p.values[(size_t) i];
            if (! std::isnan (v)) CHECK (v >= paramDefs()[(size_t) i].min && v <= paramDefs()[(size_t) i].max);
        }
        CHECK (! std::isnan (p.values[(size_t) source]));   // every preset names its source type
    }
    std::printf ("categories (%d): %s\n", cats.size(), cats.joinIntoString (", ").toRawUTF8());
    const juce::StringArray expected { "Bass", "Vocal", "Guitar", "Drums", "Pad", "Full Mix" };
    CHECK (cats == expected);

    // the source templates equal dsp/Sources.h (what picking a type in the UI loads)
    const char* templates[] { "srcBass", "srcVocal", "srcGuitar", "srcDrums", "srcPad", "srcMix" };
    for (int t = 0; t < kNumSourceTypes; ++t)
    {
        const auto v = pm.templateValues (templates[t]);
        const auto& S = sourceDefaults (t);
        auto eq = [&v] (const char* id, double expectedValue)
        {
            const auto it = v.find (id);
            return it != v.end() && std::abs (it->second - expectedValue) < 1e-4;
        };
        CHECK (eq ("source", t + 1));
        CHECK (eq ("bands", S.bands));
        CHECK (eq ("cutoffLo", S.cutoffLo) && eq ("cutoffHi", S.cutoffHi) && eq ("reso", S.reso));
        CHECK (eq ("loWob", S.wob[0]) && eq ("midWob", S.wob[1]) && eq ("hiWob", S.wob[2]));
        CHECK (eq ("loDrive", S.drive[0]) && eq ("midDrive", S.drive[1]) && eq ("hiDrive", S.drive[2]));
        CHECK (eq ("xLow", S.xLow) && eq ("xHigh", S.xHigh) && eq ("filterType", S.filterType));
        CHECK (eq ("monoBelow", S.monoBelow) && eq ("transPreserve", S.transPreserve));
        CHECK (eq ("harmOn", S.harmOn) && eq ("harmAmount", S.harmAmount));
        // the category's presets use their type's template
        for (const auto& p : f)
            if (p.category == juce::String (S.name)) CHECK (std::lround (p.values[(size_t) source]) == t + 1);
    }
}

// every factory preset processes a chord and a drum loop without blowing up, and does something
static void testPresetsRender (const PresetManager& pm)
{
    int i = 0;
    for (const auto& p : pm.factory())
    {
        auto v = presetValues (p);
        const auto a = render (v, { 4, 110.0, 0.4 }, 2.0, {}, {}, { 120, true, 0 });
        CHECK (finite (a));
        CHECK (peak (a) <= 1.0);
        const double level = rms (a, sec (0.5), sec (2.0));
        if (! (level > 0.01)) std::printf ("  preset %d (%s) is quiet: %.1f dB\n", i, p.name.toRawUTF8(), db (level));
        CHECK (level > 0.01);
        const auto d = render (v, { 6, 0, 0.6 }, 2.0, {}, {}, { 120, true, 0 });
        CHECK (finite (d) && peak (d) <= 1.0);
        ++i;
    }
    // level-matched on their category's test source (tests/level-match.ps1 sets outGain): -18 dBFS +- 2 dB.
    // (1 dB for most; a one-bar band-pass / high-pass sweep over the drum loop is nearly closed for part of each
    // bar, so its 2 s RMS stays under the target while its open moments already sit on the limiter: up to 2 dB.)
    double worst = 0;
    int over1 = 0;
    for (const auto& p : pm.factory())
    {
        const double l = db (presetLoudness (p));
        worst = std::max (worst, std::abs (l + 18.0));
        if (std::abs (l + 18.0) > 1.0) { ++over1; std::printf ("  %s/%s: %.1f dB%s\n", p.category.toRawUTF8(), p.name.toRawUTF8(), l, std::abs (l + 18.0) > 2.0 ? " (run tests\\level-match.ps1)" : " (a sweep on the limiter: within 2 dB)"); }
    }
    std::printf ("preset loudness: worst %.2f dB from -18 dBFS, %d preset(s) beyond 1 dB\n", worst, over1);
    CHECK (worst <= 2.0);
    CHECK (over1 <= 4);
}

static void testPresetFiles()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("iwubyou-tests");
    dir.deleteRecursively();
    dir.createDirectory();
    juce::String error;

    // round trip
    PresetData d;
    d.name = "Round Trip";
    d.category = "User";
    for (int i = 0; i < kNumParams; ++i) d.values[(size_t) i] = paramDefs()[(size_t) i].min + 0.37f * (paramDefs()[(size_t) i].max - paramDefs()[(size_t) i].min);
    const auto file = dir.getChildFile ("round.wubpreset");
    CHECK (PresetManager::writeFile (file, d, error));
    PresetData back;
    CHECK (PresetManager::readFile (file, back, error));
    CHECK (back.name == "Round Trip");
    for (int i = 0; i < kNumParams; ++i)
        CHECK (std::abs (back.values[(size_t) i] - d.values[(size_t) i]) < 1e-4f * std::max (1.0f, std::abs (d.values[(size_t) i])));

    // partial file: unknown parameters are ignored, missing ones stay NaN (= default), values are clamped
    const auto partial = dir.getChildFile ("partial.wubpreset");
    partial.replaceWithText (R"({ "format": "I Wub You Preset", "version": 1, "name": "Partial", "params": { "reso": 7, "nonsense": 3, "lfoRate": 1.5 } })");
    PresetData pd;
    CHECK (PresetManager::readFile (partial, pd, error));
    CHECK (pd.values[(size_t) reso] == 1.0f);
    CHECK (std::abs (pd.values[(size_t) lfoRate] - 1.5f) < 1e-6f);
    CHECK (std::isnan (pd.values[(size_t) cutoffLo]));

    // garbage
    const auto garbage = dir.getChildFile ("garbage.wubpreset");
    garbage.replaceWithText ("this is not json");
    PresetData gd;
    CHECK (! PresetManager::readFile (garbage, gd, error));
    const auto empty = dir.getChildFile ("empty.wubpreset");
    empty.replaceWithText (R"({ "format": "I Wub You Preset", "params": { "unknown": 1 } })");
    CHECK (! PresetManager::readFile (empty, gd, error));

    // an old version migrates
    juce::var old = juce::JSON::parse (R"({ "version": 0, "params": { "mix": 0.5 } })");
    PresetManager::migrate (old);
    CHECK ((int) old["version"] == kPresetVersion);

    dir.deleteRecursively();
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;   // JSON / File on the message thread
    const juce::StringArray args (argv + 1, argc - 1);
    if (args.contains ("--dump-params")) return runDumpParams();
    if (args.contains ("--render"))
    {
        const int i = args.indexOf ("--render");
        if (args.size() < i + 4) { std::printf ("usage: --render <preset index or Category/Name> <source: sine|saw|square|chord|noise|drums|wide> <out.wav>\n"); return 2; }
        return runRender (args[i + 1], args[i + 2], juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 3]));
    }
    if (args.contains ("--bench")) return runBench();
    if (args.contains ("--loudness")) return runLoudness();
    if (args.contains ("--export-presets"))
    {
        const int i = args.indexOf ("--export-presets");
        if (args.size() < i + 2) { std::printf ("usage: --export-presets <folder>\n"); return 2; }
        return runExportPresets (juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 1]));
    }
    if (args.contains ("--engine")) return runEngineTests();
    if (args.contains ("--host-check"))
    {
        const int i = args.indexOf ("--host-check");
        if (args.size() < i + 2) { std::printf ("usage: --host-check <path to I Wub You.vst3>\n"); return 2; }
        return runHostCheck (args[i + 1]);
    }
    if (args.contains ("--parity"))
    {
        const int i = args.indexOf ("--parity");
        if (args.size() < i + 2) { std::printf ("usage: --parity <scenarios.json>\n"); return 2; }
        return runParity (juce::File::getCurrentWorkingDirectory().getChildFile (args[i + 1]));
    }

    std::printf ("I Wub You tests\n");
    testParamTable();
    PresetManager pm;
    testFactoryPresets (pm);
    testPresetFiles();
    testPresetsRender (pm);
    failures += runEngineTests();
    if (failures == 0) std::printf ("ALL TESTS PASSED\n");
    else std::printf ("%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
