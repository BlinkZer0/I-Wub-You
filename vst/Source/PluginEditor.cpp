#include "PluginEditor.h"
#include "BinaryData.h"

using namespace wub;

namespace
{
const juce::Colour kBackground { 0xff060a14 };

const char* mimeForExtension (const juce::String& ext)
{
    if (ext == "html") return "text/html";
    if (ext == "js")   return "text/javascript";
    if (ext == "css")  return "text/css";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "json") return "application/json";
    if (ext == "png")  return "image/png";
    return "application/octet-stream";
}

std::vector<std::byte> toBytes (const void* data, size_t size)
{
    const auto* p = static_cast<const std::byte*> (data);
    return { p, p + size };
}

juce::var makeObject (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* obj = new juce::DynamicObject();
    for (const auto& [k, v] : props) obj->setProperty (k, v);
    return juce::var (obj);
}
} // namespace

//==============================================================================
IWubYouEditor::IWubYouEditor (IWubYouProcessor& p)
    : AudioProcessorEditor (p), processor (p), browser (makeBrowserOptions())
{
    lastSent.fill (std::numeric_limits<float>::quiet_NaN());

    addAndMakeVisible (browser);
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    setResizable (true, true);
    setResizeLimits (720, 480, 2600, 2200);
    setSize (1120, 760);
    startTimerHz (30);
}

IWubYouEditor::~IWubYouEditor()
{
    stopTimer();
    // Close any knob gestures the UI didn't get to finish.
    for (int i = 0; i < kNumParams; ++i)
        if (inGesture[(size_t) i]) processor.param (i)->endChangeGesture();
}

void IWubYouEditor::paint (juce::Graphics& g) { g.fillAll (kBackground); }
void IWubYouEditor::resized() { browser.setBounds (getLocalBounds()); }

//==============================================================================
juce::WebBrowserComponent::Options IWubYouEditor::makeBrowserOptions()
{
    using Opts = juce::WebBrowserComponent::Options;

    // WebView2 needs a writable data folder; the default (next to the host's exe) usually isn't.
    const auto userData = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("IWubYouVST-WebView2");

    return Opts{}
        .withBackend (Opts::Backend::webview2)
        .withWinWebView2Options (Opts::WinWebView2{}
                                     .withUserDataFolder (userData)
                                     .withBackgroundColour (kBackground)
                                     .withStatusBarDisabled())
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withResourceProvider ([this] (const juce::String& url) { return getResource (url); })
        .withEventListener ("uiReady",    [this] (const juce::var&) { onUiReady(); })
        .withEventListener ("setParam",   [this] (const juce::var& e) { onSetParam (e); })
        .withEventListener ("gesture",    [this] (const juce::var& e) { onGesture (e); })
        .withEventListener ("retrig",     [this] (const juce::var&) { processor.requestRetrigger(); })
        // a source type picked in the UI loads its defaults (cutoff range, drive, crossover, bands)
        .withEventListener ("sourceType", [this] (const juce::var& e) { processor.applySourceType ((int) e["type"]); })
        .withEventListener ("loadPreset", [this] (const juce::var& e)
        {
            juce::String error;
            if (! processor.loadPresetById (e["id"].toString(), error)) sendMessage (error);
            sendPreset();
        })
        .withEventListener ("savePreset", [this] (const juce::var& e)
        {
            const auto name = e["name"].toString().trim();
            bool exists = false;
            juce::String error;
            const bool ok = processor.saveUserPreset (name, (bool) e["overwrite"], exists, error);
            browser.emitEventIfBrowserIsVisible ("saveResult", makeObject ({
                { "ok", ok }, { "exists", exists }, { "name", name }, { "error", error } }));
            if (ok) { sendPresetList(); sendPreset(); }
        })
        .withEventListener ("deletePreset", [this] (const juce::var& e)
        {
            juce::String error;
            if (processor.deleteUserPreset (e["id"].toString(), error)) sendMessage ("Preset moved to the Recycle Bin");
            else sendMessage (error);
            sendPresetList();
            sendPreset();
        })
        .withEventListener ("loadPresetFile", [this] (const juce::var&) { chooseAndLoadPresetFile(); })
        // right-click on a control: the host's own parameter menu (automation, link to controller...).
        // Hosts without one get a small fallback menu.
        .withEventListener ("paramMenu", [this] (const juce::var& e) { showParamMenu (e); })
        .withEventListener ("openPresetFolder", [] (const juce::var&)
        {
            const auto folder = PresetManager::userFolder();
            folder.createDirectory();
            folder.startAsProcess();
        });
}

void IWubYouEditor::chooseAndLoadPresetFile()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load I Wub You preset", PresetManager::userFolder(),
                                                        juce::String ("*") + PresetManager::fileExtension);
    const auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    fileChooser->launchAsync (chooserFlags, [safeThis = juce::Component::SafePointer<IWubYouEditor> (this)] (const juce::FileChooser& fc)
    {
        if (safeThis == nullptr) return;
        const auto file = fc.getResult();
        if (file == juce::File()) return; // cancelled
        juce::String error;
        if (safeThis->processor.loadPresetFile (file, error)) safeThis->sendMessage ("Loaded " + file.getFileName());
        else safeThis->sendMessage (error);
        safeThis->sendPreset();
    });
}

void IWubYouEditor::showParamMenu (const juce::var& e)
{
    const int i = indexForId (e["id"].toString());
    if (i < 0) return;
    auto* p = processor.param (i);
    const juce::Point<int> pos ((int) e["x"], (int) e["y"]);

    if (auto* host = getHostContext())
        if (auto menu = host->getContextMenuForParameter (p))
        {
            menu->showNativeMenu (pos);
            return;
        }

    // No host menu (e.g. Standalone): minimal fallback
    juce::PopupMenu m;
    m.addSectionHeader (p->getName (64) + ": " + p->getCurrentValueAsText());
    m.addItem (1, "Reset to default");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                                .withTargetScreenArea (localAreaToGlobal (juce::Rectangle<int> (pos.x, pos.y, 1, 1))),
                     [safeThis = juce::Component::SafePointer<IWubYouEditor> (this), i] (int result)
                     {
                         if (safeThis == nullptr || result != 1) return;
                         auto* param = safeThis->processor.param (i);
                         param->beginChangeGesture();
                         param->setValueNotifyingHost (param->getDefaultValue());
                         param->endChangeGesture();
                     });
}

void IWubYouEditor::sendMessage (const juce::String& text)
{
    browser.emitEventIfBrowserIsVisible ("message", makeObject ({ { "text", text } }));
}

void IWubYouEditor::sendPresetList()
{
    auto& pm = processor.presetManager;
    pm.rescanIfChanged();

    juce::Array<juce::var> list;
    const auto& factory = pm.factory();
    for (size_t i = 0; i < factory.size(); ++i)
        list.add (makeObject ({ { "id", "factory:" + juce::String ((int) i) }, { "name", factory[i].name },
                                { "category", factory[i].category }, { "factory", true } }));
    for (const auto& e : pm.userPresets())
        list.add (makeObject ({ { "id", e.id }, { "name", e.name }, { "category", e.category }, { "factory", false } }));

    browser.emitEventIfBrowserIsVisible ("presetList", makeObject ({
        { "presets", list }, { "folder", PresetManager::userFolder().getFullPathName() } }));
}

void IWubYouEditor::sendPreset()
{
    const auto info = processor.currentPreset();
    lastPresetVersion = processor.presetVersion.load();
    browser.emitEventIfBrowserIsVisible ("preset", makeObject ({
        { "id", info.id }, { "name", info.name }, { "category", info.category } }));
}

std::optional<juce::WebBrowserComponent::Resource> IWubYouEditor::getResource (const juce::String& url)
{
    auto path = url.upToFirstOccurrenceOf ("?", false, false).trimCharactersAtStart ("/");
    if (path.isEmpty()) path = "index.html";
    const auto fileName = path.fromLastOccurrenceOf ("/", false, false);
    const auto mime = mimeForExtension (fileName.fromLastOccurrenceOf (".", false, false).toLowerCase());

    // Dev mode: set WUB_UI_DIR to the vst/ui folder to load the UI from disk
    // (edit HTML/CSS/JS and just reopen the editor, no rebuild needed).
    const auto devDir = juce::SystemStats::getEnvironmentVariable ("WUB_UI_DIR", {});
    if (devDir.isNotEmpty())
    {
        const auto f = juce::File (devDir).getChildFile (path);
        juce::MemoryBlock mb;
        if (f.existsAsFile() && f.loadFileAsData (mb))
            return juce::WebBrowserComponent::Resource { toBytes (mb.getData(), mb.getSize()), mime };
    }

    // Embedded UI. BinaryData is keyed by file name (all UI file names are unique).
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        if (fileName == BinaryData::originalFilenames[i])
        {
            int size = 0;
            if (const auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size))
                return juce::WebBrowserComponent::Resource { toBytes (data, (size_t) size), mime };
        }
    }
    return std::nullopt;
}

//==============================================================================
int IWubYouEditor::indexForId (const juce::String& id) const
{
    for (int i = 0; i < kNumParams; ++i)
        if (id == paramDefs()[(size_t) i].id) return i;
    return -1;
}

void IWubYouEditor::onUiReady()
{
    uiReady = true;
    lastSent.fill (std::numeric_limits<float>::quiet_NaN()); // force a full resend
    sendConfig();
    sendPresetList();
    sendPreset();
    timerCallback();
}

void IWubYouEditor::onSetParam (const juce::var& e)
{
    const int i = indexForId (e["id"].toString());
    if (i < 0) return;
    auto* p = processor.param (i);
    const float v = (float) (double) e["value"];
    const bool wrap = ! inGesture[(size_t) i];   // wheel / keys / double-click: one-shot gesture
    if (wrap) p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (v));
    if (wrap) p->endChangeGesture();
    lastSent[(size_t) i] = processor.paramValue (i); // don't echo it back to the UI
}

void IWubYouEditor::onGesture (const juce::var& e)
{
    const int i = indexForId (e["id"].toString());
    if (i < 0) return;
    const bool begin = (bool) e["begin"];
    if (begin == inGesture[(size_t) i]) return;
    inGesture[(size_t) i] = begin;
    if (begin) processor.param (i)->beginChangeGesture();
    else       processor.param (i)->endChangeGesture();
}

void IWubYouEditor::sendConfig()
{
    const auto wrapper = juce::String (juce::AudioProcessor::getWrapperTypeDescription (processor.wrapperType));
    browser.emitEventIfBrowserIsVisible ("config", makeObject ({
        { "sampleRate", processor.getSampleRate() },
        { "blockSize",  processor.getBlockSize() },
        { "wrapper",    wrapper },
        // dev / screenshots: WUB_UI_PAGE=bands (etc.) opens that page first
        { "startPage",  juce::SystemStats::getEnvironmentVariable ("WUB_UI_PAGE", {}) },
    }));
}

void IWubYouEditor::timerCallback()
{
    if (! uiReady) return;

    // Parameter changes (host automation, preset loads, state restore)
    auto* changed = new juce::DynamicObject();
    for (int i = 0; i < kNumParams; ++i)
    {
        const float v = processor.paramValue (i);
        if (v != lastSent[(size_t) i]) // NaN != anything -> always sent after uiReady
        {
            changed->setProperty (paramDefs()[(size_t) i].id, v);
            lastSent[(size_t) i] = v;
        }
    }
    juce::var changedVar (changed);
    if (changed->getProperties().size() > 0)
        browser.emitEventIfBrowserIsVisible ("params", changedVar);

    // Preset changed outside the UI (host program change, project reload)
    if (processor.presetVersion.load() != lastPresetVersion)
        sendPreset();

    // Autoload: pick up preset files added/removed/edited in the preset folder (every ~2 s)
    if (++ticks % 60 == 0 && processor.presetManager.rescanIfChanged())
        sendPresetList();

    juce::Array<juce::var> cutoff;
    for (int b = 0; b < 3; ++b) cutoff.add (processor.statCutoff[b].load());
    browser.emitEventIfBrowserIsVisible ("stats", makeObject ({
        { "peakIn",   processor.statPeakIn.exchange (0.0f) },
        { "peakOut",  processor.statPeakOut.exchange (0.0f) },
        { "lfo",      processor.statLfo.load() },
        { "lfoR",     processor.statLfoR.load() },
        { "cutoff",   cutoff },
        { "autoGain", processor.statAutoGain.load() },
        { "detected", processor.statDetected.load() },
        { "bpm",      processor.statBpm.load() },
        { "playing",  processor.statPlaying.load() },
        { "note",     processor.statNote.load() },
        { "pitch",    processor.statPitch.load() },
        { "pitchConf", processor.statPitchConf.load() },
        { "harm",     processor.statHarm.load() },
        { "autoOut",  processor.statAutoOut.load() },
        { "limiter",  processor.statLimiter.exchange (0.0f) },
    }));
}
