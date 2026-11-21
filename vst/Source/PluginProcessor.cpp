#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/Sources.h"

using namespace wub;

namespace
{
// the factory template that carries each source type's defaults (index = source choice - 1)
const char* kSourceTemplates[] { "srcBass", "srcVocal", "srcGuitar", "srcDrums", "srcPad", "srcMix" };
}

IWubYouProcessor::IWubYouProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "IWubYouState", createParameterLayout())
{
    for (int i = 0; i < kNumParams; ++i)
    {
        const auto& id = paramDefs()[(size_t) i].id;
        params[(size_t) i] = apvts.getParameter (id);
        rawValues[(size_t) i] = apvts.getRawParameterValue (id);
        jassert (params[(size_t) i] != nullptr && rawValues[(size_t) i] != nullptr);
    }
    setCurrentProgram (0); // a fresh instance starts on the first factory preset
    startTimerHz (50);
}

//==============================================================================
void IWubYouProcessor::prepareToPlay (double sampleRate, int)
{
    engine.setParams (readParams());
    engine.prepare (sampleRate);
    reportedLatency = engine.totalLatency();
    setLatencySamples (reportedLatency);
}

void IWubYouProcessor::reset()
{
    if (getSampleRate() > 0) engine.prepare (getSampleRate());
}

void IWubYouProcessor::applySourceType (int type)
{
    type = juce::jlimit (0, 6, type);
    auto* sp = params[(size_t) source];
    sp->beginChangeGesture();
    sp->setValueNotifyingHost (sp->convertTo0to1 ((float) type));
    sp->endChangeGesture();
    appliedDetected = -1;    // Auto picked again: its next decision loads afresh
    if (type == 0) return;   // Auto: the analysis loads the defaults once it has decided (timerCallback)
    applySourceDefaults (type);
}

void IWubYouProcessor::applySourceDefaults (int type)
{
    if (type < 1 || type > 6) return;
    const auto values = presetManager.templateValues (kSourceTemplates[type - 1]);
    for (const auto& [id, v] : values)
    {
        bool wanted = false;
        for (const char* key : kSourceKeys) if (id == key) wanted = true;
        if (! wanted) continue;
        for (int i = 0; i < kNumParams; ++i)
            if (paramDefs()[(size_t) i].id == id)
            {
                auto* p = params[(size_t) i];
                p->beginChangeGesture();
                p->setValueNotifyingHost (p->convertTo0to1 (v));
                p->endChangeGesture();
            }
    }
}

bool IWubYouProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    return in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono();
}

EngineParams IWubYouProcessor::readParams() const
{
    return makeEngineParams ([this] (int i) { return paramValue (i); });
}

void IWubYouProcessor::timerCallback()
{
    // the drive's oversampling filters and the limiter's lookahead delay the output: tell the host
    if (const int l = WubEngine::totalLatency (juce::roundToInt (paramValue (os)), paramValue (limiter) >= 0.5f, getSampleRate() > 0 ? getSampleRate() : 48000.0);
        l != reportedLatency)
    {
        reportedLatency = l;
        setLatencySamples (l);
    }
    // Source Type = Auto: the analysis' decision loads that type's defaults (once per decision)
    if (juce::roundToInt (paramValue (source)) == 0)
    {
        const int d = statDetected.load();
        if (d >= 0 && d != appliedDetected)
        {
            appliedDetected = d;
            applySourceDefaults (d + 1);
        }
    }
    else appliedDetected = -1;
}

void IWubYouProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    engine.setParams (readParams());

    // host transport (tempo sync, phase lock)
    double bpm = statBpm.load(), ppq = 0;
    bool playing = false;
    if (auto* ph = getPlayHead())
        if (const auto pos = ph->getPosition())
        {
            if (const auto b = pos->getBpm()) bpm = *b;
            if (const auto q = pos->getPpqPosition()) ppq = *q;
            playing = pos->getIsPlaying();
        }
    engine.setTransport (bpm, ppq, playing);
    statBpm.store ((float) bpm);
    statPlaying.store (playing);

    if (retrigRequested.exchange (false))
        engine.retrigger();

    const int inCh = getTotalNumInputChannels();
    const float* inL = buffer.getReadPointer (0);
    const float* inR = inCh > 1 ? buffer.getReadPointer (1) : nullptr;
    float* outL = buffer.getWritePointer (0);
    float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    // the engine reads each input sample before it writes the output sample at the same index,
    // so in-place processing on the host's buffer is safe

    // Sample-accurate MIDI: render up to each event, then apply it.
    int pos = 0;
    for (const auto meta : midi)
    {
        const int t = juce::jlimit (0, n, meta.samplePosition);
        if (t > pos)
        {
            engine.render (inL + pos, inR != nullptr ? inR + pos : nullptr, outL + pos, outR != nullptr ? outR + pos : nullptr, t - pos);
            pos = t;
        }
        const auto msg = meta.getMessage();
        if (msg.isNoteOn()) engine.noteOn (msg.getNoteNumber(), msg.getVelocity());
        else if (msg.isNoteOff()) engine.noteOff (msg.getNoteNumber());
        else if (msg.isAllNotesOff() || msg.isAllSoundOff()) engine.noteOff (engine.heldNote());
    }
    if (pos < n)
        engine.render (inL + pos, inR != nullptr ? inR + pos : nullptr, outL + pos, outR != nullptr ? outR + pos : nullptr, n - pos);
    midi.clear();

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, n);

    statLfo.store ((float) engine.lfoValue (0));
    statLfoR.store ((float) engine.lfoValue (1));
    for (int b = 0; b < 3; ++b) statCutoff[b].store ((float) engine.cutoffHz (b));
    statAutoGain.store ((float) engine.autoGainDb());
    statDetected.store (engine.detectedSource());
    statNote.store (engine.heldNote());
    statPitch.store ((float) engine.pitchHz());
    statPitchConf.store ((float) engine.pitchConfidence());
    statHarm.store ((float) engine.harmonicsBlend());
    statAutoOut.store ((float) engine.autoOutputDb());
    const float gr = (float) engine.takeLimiterReductionDb();
    if (gr < statLimiter.load()) statLimiter.store (gr);
    const float pi = (float) engine.takePeakIn(), po = (float) engine.takePeakOut();
    if (pi > statPeakIn.load()) statPeakIn.store (pi);
    if (po > statPeakOut.load()) statPeakOut.store (po);
}

//==============================================================================
// Sets every parameter. Anything the preset doesn't specify
// (e.g. an older file saved before a parameter existed) goes to its default.
void IWubYouProcessor::applyPreset (const PresetData& preset)
{
    for (int i = 0; i < kNumParams; ++i)
    {
        if (paramDefs()[(size_t) i].noPreset) continue;
        const float stored = preset.values[(size_t) i];
        const float v = std::isnan (stored) ? paramDefs()[(size_t) i].def : stored;
        auto* p = params[(size_t) i];
        const float norm = p->convertTo0to1 (v);
        if (std::abs (p->getValue() - norm) < 1e-7f) continue; // unchanged: don't spam the host
        p->beginChangeGesture();
        p->setValueNotifyingHost (norm);
        p->endChangeGesture();
    }
}

void IWubYouProcessor::setPresetInfo (const juce::String& id, const juce::String& name, const juce::String& category)
{
    {
        const juce::ScopedLock sl (presetLock);
        presetInfo = { id, name, category };
    }
    ++presetVersion;
}

IWubYouProcessor::PresetInfo IWubYouProcessor::currentPreset() const
{
    const juce::ScopedLock sl (presetLock);
    return presetInfo;
}

void IWubYouProcessor::setCurrentProgram (int index)
{
    index = juce::jlimit (0, getNumPrograms() - 1, index);
    currentProgram = index;
    const auto& preset = presetManager.factory()[(size_t) index];
    applyPreset (preset);
    setPresetInfo ("factory:" + juce::String (index), preset.name, preset.category);
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

const juce::String IWubYouProcessor::getProgramName (int index)
{
    if (index < 0 || index >= getNumPrograms()) return {};
    const auto& p = presetManager.factory()[(size_t) index];
    return p.category + " - " + p.name;
}

bool IWubYouProcessor::loadPresetById (const juce::String& id, juce::String& error)
{
    if (id.startsWith ("factory:"))
    {
        const int index = id.fromFirstOccurrenceOf (":", false, false).getIntValue();
        if (index < 0 || index >= getNumPrograms()) { error = "Unknown preset"; return false; }
        setCurrentProgram (index);
        return true;
    }

    presetManager.rescanIfChanged();
    if (const auto* entry = presetManager.findUser (id))
    {
        PresetData data;
        if (! PresetManager::readFile (entry->file, data, error)) return false;
        applyPreset (data);
        setPresetInfo (entry->id, entry->name, entry->category);
        return true;
    }
    error = "Preset not found (was it moved or deleted?)";
    return false;
}

bool IWubYouProcessor::loadPresetFile (const juce::File& file, juce::String& error)
{
    PresetData data;
    if (! PresetManager::readFile (file, data, error)) return false;
    applyPreset (data);
    const auto userId = presetManager.idForFile (file);
    if (userId.isNotEmpty())
        setPresetInfo (userId, file.getFileNameWithoutExtension(), presetManager.categoryForFile (file));
    else
        setPresetInfo ("file:" + file.getFullPathName(), data.name, "File");
    return true;
}

bool IWubYouProcessor::saveUserPreset (const juce::String& rawName, bool overwrite, bool& alreadyExists, juce::String& error)
{
    alreadyExists = false;
    const auto name = rawName.trim();
    if (name.isEmpty()) { error = "Enter a name for the preset"; return false; }

    const auto file = presetManager.fileForName (name);
    if (file.existsAsFile() && ! overwrite) { alreadyExists = true; return false; }

    PresetData data;
    data.name = name;
    data.category = "User";
    for (int i = 0; i < kNumParams; ++i)
        if (! paramDefs()[(size_t) i].noPreset) data.values[(size_t) i] = paramValue (i);

    if (! PresetManager::writeFile (file, data, error)) return false;
    presetManager.rescanIfChanged();
    setPresetInfo (presetManager.idForFile (file), file.getFileNameWithoutExtension(), presetManager.categoryForFile (file));
    return true;
}

bool IWubYouProcessor::deleteUserPreset (const juce::String& id, juce::String& error)
{
    presetManager.rescanIfChanged();
    const auto* entry = presetManager.findUser (id);
    if (entry == nullptr) { error = "Only user presets can be deleted"; return false; }
    const auto file = entry->file;
    if (! file.moveToTrash() && ! file.deleteFile()) { error = "Couldn't delete " + file.getFullPathName(); return false; }
    presetManager.rescanIfChanged();
    if (currentPreset().id == id)                       // keep the sound, drop the link to the file
        setPresetInfo ({}, currentPreset().name, "User");
    return true;
}

//==============================================================================
void IWubYouProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    const auto info = currentPreset();
    state.setProperty ("program", currentProgram.load(), nullptr);
    state.setProperty ("presetId", info.id, nullptr);
    state.setProperty ("presetName", info.name, nullptr);
    state.setProperty ("presetCategory", info.category, nullptr);
    state.setProperty ("stateVersion", kPresetVersion, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void IWubYouProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            currentProgram = juce::jlimit (0, getNumPrograms() - 1, (int) state.getProperty ("program", 0));
            const auto name = state.getProperty ("presetName").toString();
            if (name.isEmpty())
            {
                const auto& p = presetManager.factory()[(size_t) currentProgram.load()];
                setPresetInfo ("factory:" + juce::String (currentProgram.load()), p.name, p.category);
            }
            else
            {
                setPresetInfo (state.getProperty ("presetId").toString(), name,
                               state.getProperty ("presetCategory").toString());
            }
            apvts.replaceState (state);
            // Tell the host about every parameter, changed or not: a host that set a discrete parameter to a raw
            // value (0.27 for a switch) caches that raw value, and only hears the plugin's snapped value when the
            // plugin notifies; a restore that leaves such a parameter where it was would otherwise leave the host's
            // cache stale (found by pluginval's state restoration test).
            for (auto* p : params)
                p->sendValueChangedMessageToListeners (p->getValue());
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* IWubYouProcessor::createEditor()
{
    return new IWubYouEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new IWubYouProcessor();
}
