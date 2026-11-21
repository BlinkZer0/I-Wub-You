#pragma once
// Maps raw parameter values (indexed by ParamIndex) to the engine's snapshot.
// Shared by the plugin (atomic APVTS values) and the console tests. Twin: makeEngineParams in dsp-worklet.js.
#include "Params.h"
#include "WubEngine.h"

namespace wub
{
template <typename GetValue> // float (int paramIndex)
EngineParams makeEngineParams (GetValue&& get)
{
    auto num    = [&get] (int index) { return (double) get (index); };
    auto choice = [&get] (int index, int maxIndex) { return juce::jlimit (0, maxIndex, (int) std::lround (get (index))); };
    auto on     = [&get] (int index) { return get (index) >= 0.5f; };

    EngineParams p;
    p.source     = choice (source, 6);
    p.autoIn     = on (autoIn);
    p.inGain     = dbToGain (num (inGain));
    p.harmOn     = on (harmOn);
    p.harmMode   = choice (harmMode, 1);
    p.harmShape  = choice (harmShape, 2);
    p.harmAmount = num (harmAmount);
    p.subOn      = on (subOn);
    p.subAmount  = num (subAmount);
    p.bands      = choice (bands, 1);
    p.xLow       = num (xLow);
    p.xHigh      = num (xHigh);
    p.os         = choice (os, 2);
    p.drivePos   = choice (drivePos, 1);
    for (int b = 0; b < kNumBands; ++b)
    {
        p.wob[b]   = on (bandParam (b, 0));
        p.depth[b] = num (bandParam (b, 1));
        p.drive[b] = num (bandParam (b, 2));
        p.tone[b]  = num (bandParam (b, 3));
        p.level[b] = dbToGain (num (bandParam (b, 4)));
    }
    p.filterType = choice (filterType, 4);
    p.cutoffLo   = num (cutoffLo);
    p.cutoffHi   = num (cutoffHi);
    p.reso       = num (reso);
    p.resoLimit  = num (resoLimit);
    p.keyTrack   = num (keyTrack);
    p.lfoShape   = choice (lfoShape, 6);
    p.lfoSync    = on (lfoSync);
    p.lfoRate    = num (lfoRate);
    p.lfoDiv     = choice (lfoDiv, kNumDivs - 1);
    p.lfoPhase   = num (lfoPhase);
    p.lfoStereo  = num (lfoStereo);
    p.lfoSmooth  = num (lfoSmooth);
    p.lfoRetrig  = choice (lfoRetrig, 1);
    p.transPreserve = num (transPreserve);
    p.stereoMode = choice (stereoMode, 2);
    p.monoBelow  = num (monoBelow);
    p.duck       = num (duck);
    p.mix        = num (mix);
    p.outGain    = dbToGain (num (outGain));
    p.autoOut    = on (autoOut);
    p.limiter    = on (limiter);
    return p;
}
} // namespace wub
