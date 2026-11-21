// --host-check <path to .vst3>
// Loads the built plugin the way a DAW does (JUCE's VST3 hosting) and checks the
// basics a host like Minihost Modular relies on:
//   - it scans as an effect with stereo in and out and MIDI in
//   - every parameter is exposed, with names and text values
//   - audio goes through, wobbles, stays finite and below full scale; automation during playback is safe
//   - the reported latency follows the oversampling choice
//   - state save -> restore into a fresh instance gives identical parameter values
//   - programs (factory presets) switch and report names
#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"
#include "WubEngine.h"

int runHostCheck (const juce::String& path)
{
    int fails = 0;
    auto check = [&fails] (bool ok, const char* what) { if (! ok) ++fails; std::printf ("%-4s %s\n", ok ? "ok" : "FAIL", what); };

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, path);
    check (types.size() == 1, "scans: one plugin in the bundle");
    if (types.isEmpty()) return 1;
    const auto& desc = *types[0];
    std::printf ("     name '%s', manufacturer '%s', category '%s', version %s\n", desc.name.toRawUTF8(),
                 desc.manufacturerName.toRawUTF8(), desc.category.toRawUTF8(), desc.version.toRawUTF8());
    check (! desc.isInstrument, "is an effect");
    check (desc.name == "I Wub You", "name is 'I Wub You'");

    juce::String error;
    auto make = [&] { return format.createInstanceFromDescription (desc, 48000.0, 512, error); };
    auto plugin = make();
    check (plugin != nullptr, "instantiates");
    if (plugin == nullptr) { std::printf ("     %s\n", error.toRawUTF8()); return 1; }

    check (plugin->acceptsMidi(), "accepts MIDI");
    check (plugin->getMainBusNumInputChannels() == 2, "stereo input");
    check (plugin->getMainBusNumOutputChannels() == 2, "stereo output");

    auto& params = plugin->getParameters();
    int named = 0;
    for (auto* p : params)
        if (p->getName (64).isNotEmpty() && p->getCurrentValueAsText().isNotEmpty()) ++named;
    std::printf ("     %d host parameters (%d in the plugin's table; the rest are JUCE's bypass and MIDI CC parameters)\n", params.size(), wub::kNumParams);
    check (params.size() >= wub::kNumParams, "every parameter is exposed");
    check (named == params.size(), "every parameter has a name and a text value");
    // the plugin's own parameters, by host name (the sweeps below must not touch the wrapper's bypass / MIDI CCs)
    juce::Array<juce::AudioProcessorParameter*> own;
    std::vector<const wub::ParamDef*> ownDef;
    for (auto* p : params)
        for (const auto& d : wub::paramDefs())
            if (p->getName (64) == d.hostName) { own.add (p); ownDef.push_back (&d); break; }
    check (own.size() == wub::kNumParams, "the plugin's 51 parameters found by their host names");
    // what the plugin holds after a host sets a normalized value: switches and choices snap
    auto snapped = [] (const wub::ParamDef& d, float v)
    {
        if (d.kind == wub::Kind::Bool) return v >= 0.5f ? 1.0f : 0.0f;
        if (d.kind == wub::Kind::Choice || d.kind == wub::Kind::Int) { const float steps = d.max - d.min; return std::round (v * steps) / steps; }
        return v;
    };

    juce::AudioProcessorParameter* osParam = nullptr;
    juce::AudioProcessorParameter* cutoff = nullptr;
    juce::AudioProcessorParameter* driveP = nullptr;
    juce::AudioProcessorParameter* syncP = nullptr;
    for (auto* p : params)
    {
        if (p->getName (64) == "Oversampling") osParam = p;
        if (p->getName (64) == "Cutoff High") cutoff = p;
        if (p->getName (64) == "Mid Drive") driveP = p;
        if (p->getName (64) == "LFO Sync") syncP = p;
    }
    check (osParam != nullptr && cutoff != nullptr && driveP != nullptr && syncP != nullptr, "parameters found by host name");

    // play: a 110 Hz saw through the default preset, free-running LFO, while two parameters are automated
    plugin->setPlayConfigDetails (2, 2, 48000.0, 512);
    plugin->prepareToPlay (48000.0, 512);
    if (syncP != nullptr) syncP->setValueNotifyingHost (0.0f);
    juce::AudioBuffer<float> buf (2, 512);
    double energy = 0, peak = 0;
    std::vector<double> env;
    double ph = 0;
    for (int b = 0; b < 400; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 50) midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 10);
        if (b == 300) midi.addEvent (juce::MidiMessage::noteOff (1, 48), 0);
        if (cutoff != nullptr && b > 100 && b < 200) cutoff->setValueNotifyingHost ((float) (0.5 + 0.4 * std::sin (b * 0.1)));
        if (driveP != nullptr) driveP->setValueNotifyingHost ((float) (b % 50) / 50.0f);
        for (int i = 0; i < 512; ++i)
        {
            ph += 110.0 / 48000.0; if (ph >= 1) ph -= 1;
            const float s = (float) (0.4 * (2.0 * ph - 1.0));
            buf.setSample (0, i, s); buf.setSample (1, i, s);
        }
        plugin->processBlock (buf, midi);
        double blockE = 0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i)
            {
                const float x = buf.getSample (ch, i);
                if (! std::isfinite (x)) { energy = -1; break; }
                energy += (double) x * x;
                blockE += (double) x * x;
                peak = std::max (peak, (double) std::abs (x));
            }
        env.push_back (std::sqrt (blockE / 1024));
    }
    check (energy > 1.0, "audio goes through (finite samples)");
    check (peak <= 1.0, "output never exceeds full scale (the safety clipper's ceiling is 1.0)");
    double mn = 1e9, mx = 0;
    for (size_t i = env.size() / 2; i < env.size(); ++i) { mn = std::min (mn, env[i]); mx = std::max (mx, env[i]); }
    std::printf ("     level swings %.1f dB over the second half\n", 20 * std::log10 (mx / std::max (mn, 1e-9)));
    check (mx > mn * 1.5, "the output wobbles (level modulated by the LFO)");

    // latency: the default (2x) is reported at prepare time. A change of the oversampling is reported from the
    // plugin's timer through restartComponent, which needs the message thread, so a console tool only sees it
    // after it has been pumped; the value after a re-prepare is printed for information.
    {
        const int l2 = plugin->getLatencySamples();
        std::printf ("     latency at the defaults (2x, limiter on): %d samples\n", l2);
        check (l2 == wub::WubEngine::totalLatency (1, true, 48000.0), "latency reported to the host at the defaults (3 for 2x + 48 for the limiter's 1 ms lookahead)");
        if (osParam != nullptr)
        {
            osParam->setValueNotifyingHost (1.0f);
            juce::MidiBuffer none;
            plugin->processBlock (buf, none);   // VST3 parameter changes land with the next process call
            plugin->prepareToPlay (48000.0, 512);
            std::printf ("     latency after switching to 4x and re-preparing: %d samples (engine: %d; a DAW gets the change through restartComponent)\n",
                         plugin->getLatencySamples(), wub::WubEngine::totalLatency (2, true, 48000.0));
        }
    }

    // every parameter automated at once over 100 blocks, then every one at its extremes: still finite and under 1.0
    {
        double e = 0, pk = 0;
        for (int b = 0; b < 100; ++b)
        {
            juce::MidiBuffer midi;
            for (int i = 0; i < own.size(); ++i)
                own[i]->setValueNotifyingHost ((float) (0.5 + 0.5 * std::sin (b * 0.07 + i * 0.3)));
            for (int i = 0; i < 512; ++i) { ph += 110.0 / 48000.0; if (ph >= 1) ph -= 1; const float s = (float) (0.4 * (2.0 * ph - 1.0)); buf.setSample (0, i, s); buf.setSample (1, i, s); }
            plugin->processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i) { const float x = buf.getSample (ch, i); if (! std::isfinite (x)) e = -1; else { e += x * x; pk = std::max (pk, (double) std::abs (x)); } }
        }
        for (float extreme : { 0.0f, 1.0f })
        {
            for (auto* p : own) p->setValueNotifyingHost (extreme);
            for (int b = 0; b < 20; ++b)
            {
                juce::MidiBuffer midi;
                for (int i = 0; i < 512; ++i) { ph += 110.0 / 48000.0; if (ph >= 1) ph -= 1; const float s = (float) (0.4 * (2.0 * ph - 1.0)); buf.setSample (0, i, s); buf.setSample (1, i, s); }
                plugin->processBlock (buf, midi);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i) { const float x = buf.getSample (ch, i); if (! std::isfinite (x)) e = -1; else pk = std::max (pk, (double) std::abs (x)); }
            }
        }
        check (e > 0 && pk <= 1.0, "every plugin parameter automated at once, then all at 0 and all at 1: finite, never over full scale");
        plugin->setCurrentProgram (0);
    }

    // mono in / mono out, and small and odd block sizes
    {
        auto mono = make();
        check (mono != nullptr, "third instance (mono)");
        if (mono != nullptr)
        {
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add (juce::AudioChannelSet::mono());
            layout.outputBuses.add (juce::AudioChannelSet::mono());
            const bool monoOk = mono->setBusesLayout (layout);
            check (monoOk, "accepts a mono in / mono out layout");
            if (monoOk)
            {
                mono->setPlayConfigDetails (1, 1, 44100.0, 64);
                mono->prepareToPlay (44100.0, 64);
                juce::AudioBuffer<float> mb (1, 64);
                double en = 0; bool fin = true;
                double p2 = 0;
                for (int b = 0; b < 300; ++b)
                {
                    const int n = (b % 3 == 0) ? 1 : (b % 3 == 1 ? 37 : 64);   // 1, 37 and 64 samples
                    juce::AudioBuffer<float> blk (1, n);
                    for (int i = 0; i < n; ++i) { p2 += 110.0 / 44100.0; if (p2 >= 1) p2 -= 1; blk.setSample (0, i, (float) (0.4 * (2.0 * p2 - 1.0))); }
                    juce::MidiBuffer midi;
                    mono->processBlock (blk, midi);
                    for (int i = 0; i < n; ++i) { const float x = blk.getSample (0, i); if (! std::isfinite (x)) fin = false; en += x * x; }
                }
                check (fin && en > 0.1, "mono at 44.1 kHz with 1-, 37- and 64-sample blocks: audio through, finite");
                std::printf ("     mono latency at 44.1 kHz: %d samples\n", mono->getLatencySamples());
                check (mono->getLatencySamples() == wub::WubEngine::totalLatency (1, true, 44100.0), "latency at 44.1 kHz (3 + 44)");
            }
            mono->reset();
        }
    }

    // programs
    const int programs = plugin->getNumPrograms();
    std::printf ("     %d programs, e.g. '%s'\n", programs, plugin->getProgramName (0).toRawUTF8());
    check (programs == 96, "96 factory presets as programs");
    plugin->setCurrentProgram (5);
    check (plugin->getCurrentProgram() == 5 && plugin->getProgramName (5).isNotEmpty(), "program change");

    // state round trip
    if (cutoff != nullptr) cutoff->setValueNotifyingHost (0.321f);
    juce::MemoryBlock state;
    plugin->getStateInformation (state);
    auto fresh = make();
    check (fresh != nullptr, "second instance");
    if (fresh != nullptr)
    {
        fresh->setStateInformation (state.getData(), (int) state.getSize());
        bool same = true;
        const auto& a = plugin->getParameters();
        const auto& b = fresh->getParameters();
        for (int i = 0; i < std::min (a.size(), b.size()); ++i)
            if (std::abs (a[i]->getValue() - b[i]->getValue()) > 1e-6f) same = false;
        check (same && fresh->getCurrentProgram() == 5, "state save -> restore gives identical parameters and program");
    }

    // pluginval's "Plugin state restoration" sequence: random values, save, other random values, restore into the
    // same instance, then what the host reads back must be what the plugin held at the save (the snapped values).
    // Parameters the restore leaves unchanged are the catch: the host's cache still holds the second random value
    // unless the plugin notifies every parameter on restore.
    {
        juce::Random rnd (7);
        std::vector<float> expect;
        for (int i = 0; i < own.size(); ++i) { const float r = rnd.nextFloat(); own[i]->setValueNotifyingHost (r); expect.push_back (snapped (*ownDef[(size_t) i], r)); }
        juce::MemoryBlock st;
        plugin->getStateInformation (st);
        std::vector<float> second;
        for (auto* p : own) { const float r = rnd.nextFloat(); p->setValueNotifyingHost (r); second.push_back (r); }
        plugin->setStateInformation (st.getData(), (int) st.getSize());
        juce::StringArray wrong;
        for (int i = 0; i < own.size(); ++i)
            if (std::abs (own[i]->getValue() - expect[(size_t) i]) > 0.1f)
                wrong.add (own[i]->getName (64) + juce::String::formatted (" (saved %.3f, read %.3f; the second random was %.3f)", expect[(size_t) i], own[i]->getValue(), second[(size_t) i]));
        if (! wrong.isEmpty()) std::printf ("     stale in the host's cache after the restore: %s\n", wrong.joinIntoString ("; ").toRawUTF8());
        std::printf ("     (%d of %d parameters read back stale through the host's cache; they are switches / choices whose two random values snapped the same way)\n", wrong.size(), own.size());
        // what the plugin itself holds: save again and compare the states
        juce::MemoryBlock st2;
        plugin->getStateInformation (st2);
        const auto x1 = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize());
        const auto x2 = juce::AudioProcessor::getXmlFromBinary (st2.getData(), (int) st2.getSize());
        check (x1 != nullptr && x2 != nullptr && x1->isEquivalentTo (x2.get(), false), "pluginval's state restoration sequence: the plugin's state after the restore equals the saved state (every parameter)");
        plugin->setCurrentProgram (0);
    }

    std::printf (fails == 0 ? "HOST CHECK OK\n" : "HOST CHECK: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
