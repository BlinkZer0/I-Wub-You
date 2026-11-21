#pragma once
// ============================================================================
//  Parameter table. Mirrors vst/ui/js/params.js (IDs, ranges, defaults, skew,
//  choice lists, ORDER). `IWubYouTests --dump-params` prints this table and
//  tests/check-params.ps1 fails on any difference from params.js.
//
//  Stability rules (same as The Ham / Brass Knuckles / E-Piano):
//   - the order below is the storage order; new parameters are only APPENDED
//   - IDs never change; ParameterID version is 1
//   - parameters appended after v1.0 must default to a neutral value
//   - choice lists are append-only too (a host lane maps values onto them)
//  The whole table for every phase is defined in phase 1 and frozen, so test
//  projects keep loading. Parameters for later phases exist but do nothing yet
//  (harmonics, sub, key tracking, transient preserve: phase 2; auto output
//  gain and the true-peak limiter: phase 3).
// ============================================================================
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <vector>

namespace wub
{
constexpr int kNumBands = 3;   // low, mid, high

enum ParamIndex
{
    // ---- input conditioning -----------------------------------------------------
    source, autoIn, inGain,
    // ---- harmonic generation (phase 2) --------------------------------------------
    harmOn, harmMode, harmShape, harmAmount, subOn, subAmount,
    // ---- multiband split + drive ---------------------------------------------------
    bands, xLow, xHigh, os, drivePos,
    loWob, loDepth, loDrive, loTone, loLevel,
    midWob, midDepth, midDrive, midTone, midLevel,
    hiWob, hiDepth, hiDrive, hiTone, hiLevel,
    // ---- wobble filter -----------------------------------------------------------
    filterType, cutoffLo, cutoffHi, reso, resoLimit, keyTrack,
    // ---- LFO ---------------------------------------------------------------------
    lfoShape, lfoSync, lfoRate, lfoDiv, lfoPhase, lfoStereo, lfoSmooth, lfoRetrig,
    // ---- transient preserve (phase 2) ---------------------------------------------
    transPreserve,
    // ---- stereo ------------------------------------------------------------------
    stereoMode, monoBelow,
    // ---- output ------------------------------------------------------------------
    duck, mix, outGain, autoOut, limiter,
    kNumParams
};

// band 0 low, 1 mid, 2 high; field 0 wob, 1 depth, 2 drive, 3 tone, 4 level
inline int bandParam (int band, int field) { return loWob + band * 5 + field; }

enum class Kind { Float, Bool, Choice, Int };
enum class Unit { None, Seconds, Hz, LfoHz, Semitones, Cents, Percent, BipolarPercent, Decibels, Degrees,
                  Ms, Count, Note, Octaves };

struct ParamDef
{
    juce::String id;
    juce::String hostName;
    Kind kind;
    float min, max, def;
    Unit unit;
    bool logSkew;
    juce::StringArray choices; // Kind::Choice only
    bool noPreset = false;     // not stored in presets (none so far)
};

// Choice lists: the order is the stored value; must match params.js and the engine enums.
inline juce::StringArray sourceChoices()     { return { "Auto", "Bass", "Vocal", "Guitar", "Drums", "Pad", "Full Mix" }; }
inline juce::StringArray harmModeChoices()   { return { "Auto", "Always" }; }
inline juce::StringArray harmShapeChoices()  { return { "Shaper", "Saw", "Square" }; }
inline juce::StringArray bandsChoices()      { return { "Single", "3-Band" }; }
inline juce::StringArray oversampleChoices() { return { "1x", "2x", "4x" }; }
inline juce::StringArray drivePosChoices()   { return { "Pre Filter", "Post Filter" }; }
inline juce::StringArray filterTypeChoices() { return { "Low-pass", "Band-pass", "High-pass", "Notch", "Formant" }; }
inline juce::StringArray lfoShapeChoices()   { return { "Sine", "Triangle", "Saw Down", "Saw Up", "Square", "Sample & Hold", "Smooth Random" }; }
inline juce::StringArray lfoDivChoices()     { return { "1/1", "1/2", "1/2T", "1/4", "1/4D", "1/4T", "1/8", "1/8D", "1/8T", "1/16", "1/16D", "1/16T", "1/32" }; }
inline juce::StringArray lfoRetrigChoices()  { return { "Off", "MIDI Note" }; }
inline juce::StringArray stereoModeChoices() { return { "Stereo", "Mono Sum", "Mid-Side" }; }

inline const std::vector<ParamDef>& paramDefs()
{
    static const std::vector<ParamDef> defs = []
    {
        std::vector<ParamDef> d;
        auto add = [&d] (juce::String id, juce::String name, Kind kind, float mn, float mx, float def,
                         Unit unit = Unit::None, bool log = false)
        {
            d.push_back ({ id, name, kind, mn, mx, def, unit, log, {}, false });
        };
        auto flt  = [&add] (juce::String id, juce::String name, float mn, float mx, float def, Unit u = Unit::None)
                    { add (id, name, Kind::Float, mn, mx, def, u, false); };
        auto logf = [&add] (juce::String id, juce::String name, float mn, float mx, float def, Unit u)
                    { add (id, name, Kind::Float, mn, mx, def, u, true); };
        auto bln  = [&add] (juce::String id, juce::String name, bool def)
                    { add (id, name, Kind::Bool, 0, 1, def ? 1.0f : 0.0f); };
        auto choice = [&d] (juce::String id, juce::String name, juce::StringArray c, int def)
        {
            d.push_back ({ id, name, Kind::Choice, 0.0f, (float) (c.size() - 1), (float) def, Unit::None, false, c, false });
        };

        // ---- input
        choice ("source", "Source Type", sourceChoices(), 0);
        bln  ("autoIn", "Auto Input Gain", true);
        flt  ("inGain", "Input Gain", -24, 24, 0, Unit::Decibels);

        // ---- harmonics (phase 2)
        bln  ("harmOn", "Harmonics", false);
        choice ("harmMode", "Harmonics Mode", harmModeChoices(), 0);
        choice ("harmShape", "Harmonics Shape", harmShapeChoices(), 0);
        flt  ("harmAmount", "Harmonics Amount", 0, 1, 0.5f, Unit::Percent);
        bln  ("subOn", "Sub Octave", false);
        flt  ("subAmount", "Sub Amount", 0, 1, 0.5f, Unit::Percent);

        // ---- bands
        choice ("bands", "Bands", bandsChoices(), 0);
        logf ("xLow", "Crossover Low", 60, 1000, 200, Unit::Hz);
        logf ("xHigh", "Crossover High", 800, 12000, 2500, Unit::Hz);
        choice ("os", "Oversampling", oversampleChoices(), 1);
        choice ("drivePos", "Drive Position", drivePosChoices(), 0);
        {
            const char* ids[]   { "lo", "mid", "hi" };
            const char* names[] { "Low", "Mid", "High" };
            const bool wobDef[] { true, true, false };
            for (int b = 0; b < kNumBands; ++b)
            {
                const juce::String id (ids[b]), name (names[b]);
                bln  (id + "Wob", name + " Wobble", wobDef[b]);
                flt  (id + "Depth", name + " Depth", 0, 1, 1, Unit::Percent);
                flt  (id + "Drive", name + " Drive", 0, 1, 0.3f, Unit::Percent);
                flt  (id + "Tone", name + " Tone", 0, 1, 0.7f, Unit::Percent);
                flt  (id + "Level", name + " Level", -24, 12, 0, Unit::Decibels);
            }
        }

        // ---- wobble filter
        choice ("filterType", "Filter Type", filterTypeChoices(), 0);
        logf ("cutoffLo", "Cutoff Low", 20, 20000, 120, Unit::Hz);
        logf ("cutoffHi", "Cutoff High", 20, 20000, 4000, Unit::Hz);
        flt  ("reso", "Resonance", 0, 1, 0.5f, Unit::Percent);
        flt  ("resoLimit", "Resonance Limit", 0, 1, 0.5f, Unit::Percent);
        flt  ("keyTrack", "Key Tracking", 0, 1, 0, Unit::Percent);

        // ---- LFO
        choice ("lfoShape", "LFO Shape", lfoShapeChoices(), 0);
        bln  ("lfoSync", "LFO Sync", true);
        logf ("lfoRate", "LFO Rate", 0.05f, 20, 2, Unit::LfoHz);
        choice ("lfoDiv", "LFO Division", lfoDivChoices(), 6);
        flt  ("lfoPhase", "LFO Phase", 0, 360, 0, Unit::Degrees);
        flt  ("lfoStereo", "LFO Stereo Offset", 0, 180, 0, Unit::Degrees);
        flt  ("lfoSmooth", "LFO Smooth", 0, 1, 0.1f, Unit::Percent);
        choice ("lfoRetrig", "LFO Retrigger", lfoRetrigChoices(), 0);

        // ---- transient preserve (phase 2)
        flt  ("transPreserve", "Transient Preserve", 0, 1, 0, Unit::Percent);

        // ---- stereo
        choice ("stereoMode", "Stereo Mode", stereoModeChoices(), 0);
        flt  ("monoBelow", "Mono Below", 0, 300, 0, Unit::Hz);

        // ---- output
        flt  ("duck", "Duck", 0, 1, 0, Unit::Percent);
        flt  ("mix", "Mix", 0, 1, 1, Unit::Percent);
        flt  ("outGain", "Output Gain", -24, 24, 0, Unit::Decibels);
        bln  ("autoOut", "Auto Output Gain", false);
        bln  ("limiter", "Limiter", true);

        jassert ((int) d.size() == kNumParams);
        return d;
    }();
    return defs;
}

inline juce::NormalisableRange<float> paramRange (const ParamDef& d)
{
    if (d.logSkew)
    {
        // log-skewed: equal octaves per unit of travel (what the knob and the host lane see)
        const float mn = d.min, mx = d.max;
        return juce::NormalisableRange<float> (mn, mx,
            [mn, mx] (float, float, float n) { return mn * std::pow (mx / mn, juce::jlimit (0.0f, 1.0f, n)); },
            [mn, mx] (float, float, float v) { return juce::jlimit (0.0f, 1.0f, std::log (juce::jmax (v, 1e-9f) / mn) / std::log (mx / mn)); },
            [mn, mx] (float, float, float v) { return juce::jlimit (mn, mx, v); });
    }
    return juce::NormalisableRange<float> (d.min, d.max);
}

inline juce::String formatValue (const ParamDef& d, float v)
{
    switch (d.kind)
    {
        case Kind::Bool:   return v >= 0.5f ? "On" : "Off";
        case Kind::Choice: return d.choices[juce::jlimit (0, d.choices.size() - 1, juce::roundToInt (v))];
        case Kind::Int:    return juce::String (juce::roundToInt (v));
        default: break;
    }
    switch (d.unit)
    {
        case Unit::Seconds:  return v < 1 ? juce::String (juce::roundToInt (v * 1000)) + " ms" : juce::String (v, 2) + " s";
        case Unit::Hz:       return v >= 1000 ? juce::String (v / 1000, v >= 10000 ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
        case Unit::LfoHz:    return juce::String (v, v < 1 ? 3 : v < 10 ? 2 : 1) + " Hz";
        case Unit::Semitones: return juce::String (juce::roundToInt (v)) + " st";
        case Unit::Cents:    return juce::String (juce::roundToInt (v)) + " ct";
        case Unit::Percent:  return juce::String (juce::roundToInt (v * 100)) + "%";
        case Unit::BipolarPercent: return (v > 0.004f ? "+" : "") + juce::String (juce::roundToInt (v * 100)) + "%";
        case Unit::Decibels: return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB";
        case Unit::Degrees:  return juce::String (juce::roundToInt (v)) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
        case Unit::Ms:       return juce::String (juce::roundToInt (v)) + " ms";
        case Unit::Count:    return juce::String (juce::roundToInt (v));
        case Unit::Note:     return juce::MidiMessage::getMidiNoteName (juce::roundToInt (v), true, true, 3);
        case Unit::Octaves:  return juce::String (v, 1) + " oct";
        default:             return juce::String (v, 2);
    }
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& d : paramDefs())
    {
        const juce::ParameterID pid { d.id, 1 };
        switch (d.kind)
        {
            case Kind::Bool:
                layout.add (std::make_unique<juce::AudioParameterBool> (pid, d.hostName, d.def >= 0.5f));
                break;
            case Kind::Choice:
                layout.add (std::make_unique<juce::AudioParameterChoice> (pid, d.hostName, d.choices, (int) d.def));
                break;
            case Kind::Int:
                layout.add (std::make_unique<juce::AudioParameterInt> (pid, d.hostName, (int) d.min, (int) d.max, (int) d.def));
                break;
            case Kind::Float:
            {
                auto attrs = juce::AudioParameterFloatAttributes().withStringFromValueFunction (
                    [d] (float v, int) { return formatValue (d, v); });
                layout.add (std::make_unique<juce::AudioParameterFloat> (pid, d.hostName, paramRange (d), d.def, attrs));
                break;
            }
        }
    }
    return layout;
}
} // namespace wub
