#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "Params.h"
#include "PresetManager.h"
#include "WubEngine.h"
#include "EngineParamsFromValues.h"

class IWubYouProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    IWubYouProcessor();
    ~IWubYouProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;   // the host stopped / relocated: clear the filters, the followers, the analysis
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }   // filter ring-out

    // The factory presets are exposed to the host as programs.
    int getNumPrograms() override { return (int) presetManager.factory().size(); }
    int getCurrentProgram() override { return currentProgram.load(); }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Presets (message thread)
    struct PresetInfo { juce::String id, name, category; };
    PresetInfo currentPreset() const;
    std::atomic<int> presetVersion { 0 };   // bumps whenever currentPreset() changes

    bool loadPresetById (const juce::String& id, juce::String& error);
    bool loadPresetFile (const juce::File& file, juce::String& error);
    bool saveUserPreset (const juce::String& name, bool overwrite, bool& alreadyExists, juce::String& error);
    bool deleteUserPreset (const juce::String& id, juce::String& error);

    wub::PresetManager presetManager;

    //==============================================================================
    // Used by the editor (message thread)
    juce::AudioProcessorValueTreeState apvts;
    juce::RangedAudioParameter* param (int index) const { return params[(size_t) index]; }
    float paramValue (int index) const { return rawValues[(size_t) index]->load(); }

    // RETRIG button: restart the LFOs at the next block
    void requestRetrigger() { retrigRequested = true; }

    // A source type picked in the UI: the type and its defaults (cutoff range, drive,
    // crossover, bands...) as one undoable set of parameter changes (from the factory template).
    void applySourceType (int type);

    std::atomic<float> statPeakIn { 0.0f }, statPeakOut { 0.0f };
    std::atomic<float> statLfo { 0.0f }, statLfoR { 0.0f };
    std::atomic<float> statCutoff[3] { 0.0f, 0.0f, 0.0f };
    std::atomic<float> statAutoGain { 0.0f };    // dB applied by the auto input gain
    std::atomic<int>   statDetected { -1 };      // the Auto analysis' source type (0 Bass .. 5 Full Mix, -1 undecided)
    std::atomic<float> statPitch { 0.0f }, statPitchConf { 0.0f }, statHarm { 0.0f };
    std::atomic<float> statAutoOut { 0.0f };     // dB applied by the auto output gain
    std::atomic<float> statLimiter { 0.0f };     // the limiter's deepest gain reduction since the editor last read it (dB, <= 0)
    std::atomic<float> statBpm { 120.0f };
    std::atomic<bool>  statPlaying { false };
    std::atomic<int>   statNote { -1 };

private:
    void applyPreset (const wub::PresetData&);
    void setPresetInfo (const juce::String& id, const juce::String& name, const juce::String& category);
    wub::EngineParams readParams() const;
    void timerCallback() override;   // latency reporting, the Auto source type
    void applySourceDefaults (int type);   // the kSourceKeys of a type's template (type 1..6), as gestures
    int reportedLatency = -1;
    int appliedDetected = -1;        // the detected type whose defaults were loaded while Source Type is Auto

    std::array<juce::RangedAudioParameter*, wub::kNumParams> params {};
    std::array<std::atomic<float>*, wub::kNumParams> rawValues {};
    wub::WubEngine engine;
    std::atomic<int> currentProgram { 0 };
    std::atomic<bool> retrigRequested { false };

    mutable juce::CriticalSection presetLock;
    PresetInfo presetInfo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IWubYouProcessor)
};
