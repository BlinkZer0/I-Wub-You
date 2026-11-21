#pragma once
// ============================================================================
//  Presets
//   - factory presets, compiled in from presets/factory_presets.json
//     (templates + per-preset overrides; also exposed as programs)
//   - user presets: *.wubpreset JSON files in Documents\I Wub You\Presets
//     (shared by every DAW / instance; sub-folders become categories,
//      the folder is re-scanned automatically)
//   - files carry a format version; older versions are migrated on load
// ============================================================================
#include "Params.h"
#include <map>
#include <vector>

namespace wub
{
constexpr int kPresetVersion = 1;

struct PresetData
{
    juce::String name, category;
    std::array<float, kNumParams> values; // NaN = not in preset -> parameter default
    PresetData() { values.fill (NAN); }
};

struct PresetEntry
{
    juce::String id;        // "user:<relative/path.wubpreset>"
    juce::String name, category;
    juce::File file;
};

class PresetManager
{
public:
    PresetManager();

    static juce::File userFolder();
    static constexpr const char* fileExtension = ".wubpreset";
    static constexpr const char* formatName = "I Wub You Preset";

    // Factory (immutable, any thread)
    const std::vector<PresetData>& factory() const { return factoryPresets; }
    // The resolved values of a factory template (what picking a source type loads), or an empty map.
    std::map<juce::String, float> templateValues (const juce::String& name) const;

    // User library (message thread only)
    bool rescanIfChanged();                     // true if the list changed
    const std::vector<PresetEntry>& userPresets() const { return user; }
    const PresetEntry* findUser (const juce::String& id) const;
    juce::File fileForName (const juce::String& name) const;
    juce::String idForFile (const juce::File& f) const;  // "" if not inside the user folder
    juce::String categoryForFile (const juce::File& f) const;

    static bool readFile (const juce::File&, PresetData&, juce::String& error);
    static bool readJson (const juce::var& json, PresetData&, juce::String& error);
    static bool writeFile (const juce::File&, const PresetData&, juce::String& error);
    static juce::var toJson (const PresetData&);

    // Brings a preset object written by an older version up to kPresetVersion (in place).
    static void migrate (juce::var& json);

private:
    void loadFactory();

    juce::var factoryJson;
    std::vector<PresetData> factoryPresets;
    std::vector<PresetEntry> user;
    juce::String signature;
    bool scanned = false;
};
} // namespace wub
