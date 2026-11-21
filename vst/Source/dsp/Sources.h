#pragma once
// ============================================================================
//  Source types and the defaults each one loads (Source Type on the SOURCE
//  page: picking Bass / Vocal / Guitar / Drums / Pad / Full Mix sets these,
//  like picking a cabinet does in The Ham; Auto (phase 2) picks one from the
//  input's spectral centroid, crest factor and pitch confidence).
//  The factory preset templates srcBass .. srcMix carry the same values: the
//  preset tests fail if they drift apart.
//  Index = the `source` choice minus 1 (0 = Auto has no defaults).
// ============================================================================
#include "Common.h"

namespace wub
{
constexpr int kNumSourceTypes = 6;

struct SourceDefaults
{
    const char* name;
    int bands;                 // 0 single, 1 three-band
    double cutoffLo, cutoffHi; // Hz
    double reso;
    int wob[3];                // per band wobble on
    double drive[3];
    double xLow, xHigh;        // crossover Hz
    int filterType;            // 0 LP 1 BP 2 HP 3 notch 4 formant
    double monoBelow;          // Hz, 0 off
    double transPreserve;      // phase 2
    int harmOn;                // phase 2: harmonics on
    double harmAmount;
};

// The parameters a source type sets (the rest of the preset template, e.g. mix and output, is left alone).
// Twin: WUB_SOURCE_KEYS in webhost/factory.js.
constexpr const char* kSourceKeys[] { "bands", "cutoffLo", "cutoffHi", "reso", "loWob", "midWob", "hiWob", "loDrive", "midDrive", "hiDrive",
                                      "xLow", "xHigh", "filterType", "monoBelow", "transPreserve", "harmOn", "harmAmount" };

inline const SourceDefaults& sourceDefaults (int type)
{
    static const SourceDefaults table[kNumSourceTypes] {
        //  name        bands  lo    hi    reso  wob        drive            xLow xHigh  type mono trans harm amount
        { "Bass",       0,     60,   2500, 0.55, {1, 1, 0}, {0.45, 0.45, 0.3}, 200, 2500, 0,   100,  0.0,  1,   0.5 },
        { "Vocal",      0,     200,  5000, 0.45, {1, 1, 0}, {0.15, 0.15, 0.1}, 200, 3500, 1,   0,    0.3,  0,   0.3 },
        { "Guitar",     0,     150,  4000, 0.5,  {1, 1, 0}, {0.35, 0.35, 0.2}, 250, 3000, 0,   0,    0.4,  0,   0.3 },
        { "Drums",      1,     100,  3000, 0.4,  {1, 1, 0}, {0.2,  0.2,  0.1}, 180, 4000, 0,   120,  0.7,  0,   0.3 },
        { "Pad",        0,     100,  6000, 0.6,  {1, 1, 1}, {0.1,  0.1,  0.1}, 200, 2500, 4,   0,    0.0,  0,   0.3 },
        { "Full Mix",   1,     120,  3500, 0.4,  {1, 1, 0}, {0.15, 0.15, 0.05}, 150, 3000, 0,  120,  0.5,  0,   0.3 },
    };
    return table[type < 0 ? 0 : (type >= kNumSourceTypes ? kNumSourceTypes - 1 : type)];
}
} // namespace wub
