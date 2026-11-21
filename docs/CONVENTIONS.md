# Conventions (inherited from The Ham, Brass Knuckles and E-PianoVST)

I Wub You follows the house style of [The Ham](https://github.com/BlinkZer0/The-Ham) (local clone `../The Ham`),
which follows [Brass Knuckles](https://github.com/BlinkZer0/Brass-Knuckles) v1.0 and
[E-PianoVST](https://github.com/BlinkZer0/E-PianoVST) v1.0. Everything in The Ham's `docs/CONVENTIONS.md` applies.
This file restates the rules briefly and, under **I Wub You:**, lists every place where this project changes or
extends a pattern. The big one: this is an **effect**, not an instrument. It has an audio input, no voices, no
keyboard, and its tests feed it generated signals.

## 1. Repository layout

```
README.md                    user-facing: features, screenshots, build/install, release history, layout
.gitignore                   .vs/  build/  *.user
.gitattributes               * text=auto; *.bat and *.ps1 eol=crlf
start.bat -> serve.ps1       PowerShell HttpListener serving web/ on localhost:8080 (-Dev, -Ui as in The Ham)
docs/                        CONVENTIONS.md, PLAN.md, screenshots/, index.html (GitHub Pages copy of the web build)
web/index.html               GENERATED single-file web build (never edited by hand)
vst/
  CMakeLists.txt  build.bat  install.bat  README.md
  Source/                    C++ (see 3)
  ui/                        HTML/CSS/SVG/JS UI, compiled into the binary as BinaryData
  webhost/                   JS twin of the DSP, browser stand-in for the C++ bridge, build-web.ps1
  presets/factory_presets.json
  tests/
```

`build-web.ps1` writes `web/index.html` **and** `docs/index.html` (byte-identical), as The Ham does.

## 2. Build system

- CMake 3.22+, C++17, **JUCE 8.0.17** (`GIT_TAG 8.0.17-security`), `FetchContent`, shallow. The cache variable
  `WUB_JUCE_DIR` points at a local JUCE checkout instead.
- `juce_add_plugin`: `FORMATS VST3 Standalone`, **`IS_SYNTH FALSE`**, `NEEDS_MIDI_INPUT TRUE` (notes retrigger the
  LFO and, in phase 2, feed key tracking), `NEEDS_WEBVIEW2 TRUE`, `VST3_CATEGORIES Fx Filter Modulation Distortion`,
  `MICROPHONE_PERMISSION_ENABLED TRUE` (the Standalone takes a live input), `COPY_PLUGIN_AFTER_BUILD FALSE`.
- Defines `JUCE_WEB_BROWSER=1`, `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1`, `JUCE_USE_CURL=0`,
  `JUCE_VST3_CAN_REPLACE_VST2=0`, `JUCE_DISPLAY_SPLASH_SCREEN=0`.
- One `juce_add_binary_data` target holds `ui/*.html|css|js|svg` and `presets/factory_presets.json`. BinaryData is
  looked up by file name, so every UI file name must be unique.
- Console app `IWubYouTests` compiles the preset manager and the test sources, and includes the header-only engine.
  It is built with `JUCE_PLUGINHOST_VST3=1` for `--host-check`.
- `build.bat`: work dir `%LOCALAPPDATA%\IWubYouVST` (build + deps). It finds CMake on PATH or through `vswhere`,
  adds Git to PATH if needed, reuses an existing JUCE checkout from The Ham, Brass Knuckles or E-Piano, and builds
  `IWubYouVST_VST3`, `IWubYouVST_Standalone` and `IWubYouTests` with `--parallel`. Messages are prefixed `[I Wub You]`.
- `install.bat`: copies `I Wub You.vst3` to `%CommonProgramFiles%\VST3` through an elevated PowerShell;
  `uninstall.bat` removes it (presets kept). `package.ps1` builds `dist\IWubYou-<version>-win64.zip` (VST3,
  Standalone, the scripts, the presets as files via `IWubYouTests --export-presets`, the web build, the manual,
  `LICENSES.md`); `dist/` is git-ignored. This is Pedal=Faster's packaging, without its Start Menu install.

Identity: product `I Wub You`, target `IWubYouVST`, company `IWubYou`, manufacturer code `Iwub`, plugin code `Wub1`.

## 3. C++ source structure

| File | Role |
|---|---|
| `Params.h` | Parameter table: `enum ParamIndex` (order = storage order) + `paramDefs()` of `{id, hostName, Kind, min, max, def, Unit, logSkew, choices}`, `formatValue`, `paramRange` (log ranges round-trip through the host's 0..1), `createParameterLayout()` with `ParameterID{id, 1}`. Namespace `wub`. |
| `WubEngine.h` + `dsp/*.h` | Header-only DSP. No JUCE, no allocation after `prepare()`, no locks. `EngineParams` snapshot, deterministic `Rng` (the same LCG), `prepare / setParams / setTransport / noteOn / noteOff / retrigger / render(inL, inR, outL, outR, n)`. |
| `EngineParamsFromValues.h` | `makeEngineParams(get)`: raw parameter values to `EngineParams`. Shared by the plugin and the tests. |
| `PresetManager.*` | Factory presets from BinaryData, user folder with a signature rescan, read/write/`migrate()`, `templateValues()` (what a source type loads). |
| `PluginProcessor.*` | APVTS, cached parameter pointers and atomics, sample-accurate MIDI, host transport, programs, state, latency reporting, `applySourceType`. |
| `PluginEditor.*` | `WebBrowserComponent` (WebView2), resource provider, bridge events, 30 Hz timer that pushes only changed values and the stats. |

Rules:
- `juce::ScopedNoDenormals` in `processBlock`. Feedback paths (SVF integrators, biquads, the tone one-pole, the
  followers) flush values below 1e-30 to exactly 0 once per chunk.
- The engine runs in **double** precision; the plugin reads **float** parameters, so the JS twin passes every
  parameter through `Math.fround` first.
- **Control rate `kChunk = 32` samples.** The LFOs, the cutoffs, the resonance limit, the gains are evaluated once
  per chunk and ramped linearly across it (`ChunkRamp`). Every ramp is `set()` every chunk and `next()` every sample.
- The output safety clipper is exactly linear below −1.9 dBFS (Brass Knuckles').
- Parameter changes made by presets or by picking a source type go through
  `beginChangeGesture / setValueNotifyingHost / endChangeGesture`.
- Inner loops use only `+ - * /`, `sqrt`, `tanh`, `exp`, `log`, `tan` and `pow`, each once per sample or once per
  chunk, so the C++ and JS engines stay bit-close (the parity tolerance is 1e-4; measured 1e-6-ish).

**I Wub You:**
- **`render` takes an input.** `render (inL, inR, outL, outR, n)`: `inR` may be null (a mono input feeds both
  sides), `outR` may be null. The engine reads each input sample before it writes the output sample at the same
  index, so in-place processing on the host's buffer is safe.
- **No voices, no MIDI routing header.** MIDI is three calls: `noteOn` (records the note, retriggers the LFO when
  Retrigger is MIDI Note), `noteOff`, all-notes-off → `noteOff(heldNote())`. Phase 2 uses the held note for key
  tracking.
- **Transport.** `setTransport (bpm, ppq, playing)` once per block from the host's `AudioPlayHead`; the engine
  advances its own ppq copy per chunk inside the block. Synced and playing, the LFO phase is `frac(ppq / beats)`,
  so every bar wobbles the same way and two instances agree; synced but stopped it free-runs at the division's
  rate. `setTempo (bpm)` changes only the tempo (the web build has no transport and keeps the engine's clock running).
- **Oversampling** wraps only the per-band drive shaper (the HIIR half-bands of The Ham). The dry path is delayed by
  the same 0 / 3 / 4 samples so dry/wet mixes stay aligned. **The limiter's 1 ms lookahead is latency too**:
  `WubEngine::totalLatency (os, limiterOn, sr)` is what the plugin reports with `setLatencySamples` (at prepare and
  from its 50 Hz timer when either changes); the dry delay inside stays the oversampling delay only, because the
  limiter sits after the mix.
- **The limiter reuses the drive's half-bands for true peaks** (4x, the larger channel), so the JS twin computes
  the same peaks; its state lives in `dsp/Limiter.h` with the auto output gain.
- **1x is plain tanh, 2x / 4x use ADAA.** First-order ADAA averages two samples: at the base rate that is a 3 dB
  loss at 12 kHz, so 1x keeps the response flat and accepts aliasing; the oversampled rates get both.
- **The shaper is `4 tanh(x / 4)`**: linear to 0.02 dB at the working level with Drive 0, a hard clip at Drive 100%
  (36 dB in). Make-up `1 / sqrt(pre-gain)`.
- **Single-band mode runs the MID strip.** The three band parameter sets (`lo*`, `mid*`, `hi*`) always exist; with
  Bands = Single the whole signal goes through band 1 (the UI relabels the strip FULL SIGNAL and dims the others).
- **Test signals are part of the twin.** `dsp/TestSignals.h` / the `TestSource` class in `dsp-worklet.js` generate
  the same sine, saw, square, chord, noise, drum loop and "wide" stereo source in both languages (phasors and
  per-sample multiplies only). The C++ tests, the parity tool and the web build's built-in sources all use them.
- Model data lives in `dsp/Sources.h` (the source types' defaults, `kSourceKeys`: what picking a type sets) and the
  formant table in `dsp/Filters.h`.
- **Nothing assumes 48 kHz.** Decimation factors, lookahead buffers and window lengths come from the sample rate
  at `prepare()` (the tracker decimates to about 12 kHz, the limiter's lookahead is 1 ms up to 256 kHz); the engine
  checks run the rate-dependent parts at 44.1 and 96 kHz too. The parity test stays at 48 kHz.
- **`setStateInformation` notifies every parameter after `replaceState`**, changed or not. A host caches the raw
  value it last set (0.27 for a switch) until the plugin reports its snapped value; a restore that leaves a switch
  where it was reports nothing, and a JUCE host keeps reading 0.27 (pluginval's state restoration test shows it).
  The notification helps where the wrapper lets it through; the plugin's own state is right either way, which
  `--host-check` proves by saving again after the restore and comparing.
- **`reset()` re-prepares the engine**: a host's stop / relocate clears the filters, the followers, the analysis
  and the Auto decision (the plugin's timer then loads the next decision's defaults again).
- **Analysis is sample-count based, never block based.** The pitch tracker analyses every 1024 input samples from
  its own counter, the Auto type counts analyses, so results are identical at any block size (the block-size
  independence check covers key tracking, harmonics and transients).
- **Running statistics start from a settled measurement**, not from zero (a one-pole mean and mean-square that
  both ramp from 0 fake a variance, and so does the measured envelope's own start-up ramp; an averaged confidence
  that ramps from 0 fakes "unpitched"). The envelope statistics start 100 ms in, the confidence at the first analysis.
- **Two followers with different attack times disagree on a steady tone's level**: a transient detector compares a
  follower with its own lagged copy, not two followers.

## 4. Parameters

- **Append-only.** New parameters go at the end of `ParamIndex`. Indices and string IDs never change.
  `ParameterID` version is 1.
- **New parameters default to neutral** (off, zero, or "as before"), so older presets and projects sound unchanged.
- Groups come from helpers (`bandParams(b)` / `bandId(b, field)` in JS, `bandParam(b, field)` in C++).
- `Params.h` and `ui/js/params.js` are mirrored by hand. `IWubYouTests --dump-params` prints the C++ table as JSON,
  and `tests/check-params.ps1` fails on any difference.
- The **whole table is defined and frozen in Phase 1**, so test projects keep loading. Phase 2 filled in
  `harmOn harmMode harmShape harmAmount subOn subAmount keyTrack transPreserve` and phase 3 `autoOut limiter`
  without touching the table (the safety clipper is always on behind the limiter).

**I Wub You:**
- **No `master`, nothing excluded from presets.** An effect's output trim belongs to the preset (`outGain`), and
  there is no separate master. `noPreset` exists in the table for the day one is needed, and the code honours it.
- Log-skewed floats (`cutoffLo`, `cutoffHi`, `xLow`, `xHigh`, `lfoRate`) use a `NormalisableRange` with explicit
  log mapping functions, so the host's 0..1 travels in octaves (the knobs do the same in `paramToNorm`).
- Everything the engine exposes to the UI that is not a parameter travels in `stats` (see 5).

## 5. UI ↔ C++ bridge

Inherited: `ui/js/engine-juce.js` wraps `window.__JUCE__.backend` in a `JuceEngine` class.
- UI → host: `uiReady`, `setParam {id,value}`, `gesture {id,begin}`, `loadPreset {id}`, `savePreset {name,overwrite}`,
  `deletePreset {id}`, `loadPresetFile`, `openPresetFolder`, `paramMenu {id,x,y}`.
- Host → UI (30 Hz, only changes): `params`, `config`, `presetList`, `preset`, `saveResult`, `message`.
- On `uiReady` the editor clears its `lastSent` cache, so everything is sent again.
- Drags send `gesture begin/end`. One-shot changes are wrapped in a gesture by the editor.
- Right-click a control opens the host's parameter menu, with a *Reset to default* fallback. Shift+right-click
  opens DevTools.
- WebView2 data folder: `%TEMP%\IWubYouVST-WebView2`. Dev mode: env var **`WUB_UI_DIR`** loads the UI from disk,
  `WUB_UI_PAGE` opens a page directly (for screenshots).

**I Wub You:**
- UI → host: **`retrig`** (restart the LFOs at the next block), **`sourceType {type}`** (the host sets `source`
  and loads the type's defaults from the factory template, as one undoable set of gestures). Web only:
  `selectInput {id}`, `setTempo {bpm}`.
- Host → UI: **`stats {peakIn, peakOut, lfo, lfoR, cutoff[3], autoGain, detected, bpm, playing, note, pitch,
  pitchConf, harm}`** every tick: the scope draws the LFO history and the cutoffs from it, the meters the peaks, the
  SOURCE page the auto gain in dB, the pitch (note, Hz, a confidence bar), the detected type (−1 until decided) and
  the harmonics blend, the header the tempo, the transport lamp and the held MIDI note. Web only:
  `inputs {inputs[], selected}`.
- **The Auto source type is applied by the host side, not the engine**: the engine only decides (`detectedSource()`),
  the processor's timer (and `onDspMessage` in the web host) loads the decided type's `kSourceKeys` once per decision
  while `source` is Auto, through the same gestures as a manual pick, so the UI and the host see it.
- No keyboards, no MIDI learn, no registrations.

## 6. UI code

- Plain ES, `'use strict'`, no framework, no build step. Scripts load in order:
  `params.js, engine-juce.js, knob.js, controls.js, scope.js, app.js`.
- All controls are SVG. `Knob` is the reference widget (drag, Shift for fine, wheel, arrows, double-click reset,
  gestures). Panels are built in `app.js` (`makePanel`, `addKnob`, `control(Class, id)`) and every control carries
  `data-param`. Pages are tabs with an LED. The whole panel scales to fit the window.
- CSS custom properties on `:root`.

**I Wub You:**
- New identity, same widget family: **a blue chassis and plate, orange lamps and value arcs, red highlights**
  (`--chassis-*`, `--plate*`, `--panel*` blues; `--accent` orange; `--red` for knob pointers, the cutoff line in
  the scope, the WOBBLE lamps and RETRIG; `--blue` / `--blue-hi` for tracks and the right channel's LFO trace).
- Pages: **WOBBLE** (LFO, filter), **BANDS · DRIVE** (split, three strips), **SOURCE** (input, source type,
  the phase-2 harmonics/transients), **OUTPUT** (stereo, duck/mix, safety with meters).
- **`Scope`** (`scope.js`): a canvas strip above the pages, drawn with `requestAnimationFrame`, paused while
  hidden: the LFO history (L orange, R blue), the sweep range and each active band's cutoff on a 20 Hz .. 20 kHz
  log axis (red; in Formant mode the three formants of the current vowel and its letter, from a copy of the
  formant table), in / out level bars. It reads its colours from the CSS tokens.
- **Band strips** are panels with a WOBBLE toggle that lights red; in Single mode the middle one is relabelled
  FULL SIGNAL and the others dim.
- The console is laid out at 1240 px wide and about 740 px tall; the plugin window is 1120 × 760.
- Header, web only: the INPUT picker (built-in test sources, microphone, a tab's audio, a file) and a BPM field.

## 7. Presets

- Factory presets: `presets/factory_presets.json`, with **templates and per-preset overrides**. The resolved values
  are defaults, then the template chain, then the preset.
- User presets: `Documents\I Wub You\Presets`, subfolders = categories, rescanned every ~2 s by signature. Delete
  moves the file to the Recycle Bin. SAVE asks before it replaces a file.
- File: `.wubpreset`, plain JSON `{ "format": "I Wub You Preset", "version": 1, "name", "category", "params": {id: value} }`.
  Missing parameters load at their defaults, values are clamped. `PresetManager::migrate()` and `wubMigratePreset`
  (JS) bring old versions up to date.
- Factory presets are also host programs (`"<Category> - <Name>"`).

**I Wub You:** 96 presets (6 source categories × 16; the browser shows a category's 16 as 8 × 2, user presets
scroll below). The templates `srcBass … srcMix` carry each source type's
defaults from `dsp/Sources.h` (the preset tests fail if they drift), on a `base` template (auto gain on, 2x,
limiter on, Auto Out off, mix 100%). Picking a type in the UI loads only the `kSourceKeys` of its template (not
mix / output). **Level-matched** with `IWubYouTests --loudness` and `tests/level-match.ps1`, each preset on its
category's test source (`categorySource()` in `EngineTools.cpp`), to within 1 dB of −18 dBFS (2 dB for a sweep
that is nearly closed for part of a bar and already on the limiter when open; at most four such); the preset's own
`outGain` carries the match (an effect's level depends on its input, so the test source is the reference). The
matching takes several passes once the limiter is in the chain.

## 8. Web build

Inherited: `build-web.ps1` inlines the CSS and scripts, and injects `FACTORY_JSON`, `factory.js`,
`dsp-worklet.js` and `web-host.js` just before `engine-juce.js`. It refuses code containing `</script` and adds a
GENERATED banner and the version from `CMakeLists.txt`. `web-host.js` provides `window.__JUCE__.backend`. Audio
runs in an AudioWorklet when served and through a ScriptProcessor on `file://`. Presets live in localStorage or a
linked folder. `dsp-worklet.js` is one self-contained function and a **line-for-line twin** of `WubEngine.h` and
`dsp/*.h`.

**I Wub You:**
- **Inputs.** The worklet node has one input. Built-in sources (sine bass, saw, chord, drum loop, noise) are
  generated *inside* the worklet by the `TestSource` twin (`source {gen, freq, level}` message; `gen -1` = use the
  input), so they are identical to the C++ tests'. Live inputs connect to the node: `getUserMedia` (no echo
  cancellation / noise suppression / AGC), `getDisplayMedia` for a tab or the screen (the video track is stopped),
  or a decoded audio file in a looping `AudioBufferSourceNode`. The choice is remembered (`wub.input`).
- **Tempo.** A BPM field (`wub.bpm`) → `tempo` message → `engine.setTempo`; the worklet starts the engine with
  `setTransport (bpm, 0, true)` so the synced LFO runs from its own clock.
- Web MIDI: all inputs, notes straight to the engine (retrigger).
- Test hook `window.__wubWebHost` (values, factory, current, audioMode, input, bpm, sent[] tap, midi()).

## 9. Tests

| Command | What |
|---|---|
| `IWubYouTests.exe` | parameter table (incl. log-range round trips), 96 presets (categories, templates = `Sources.h`, every preset renders a chord and a drum loop finite and audible, level-matched within 1 dB), file round trip, partial / garbage / old files, then the engine checks |
| `--engine` | the engine checks alone: dry path and latency, auto gain (target, cap, hold), LFO rates free / synced / stopped, transport lock, MIDI retrigger, LR4 flatness (tones and noise), SVF responses (LP / HP / BP / notch / resonance), formant, the wobble's rate, the resonance limit, aliasing at 1x / 2x / 4x, dry/wet alignment, drive-0 transparency, mono-below, Mono Sum, Mid-Side, stereo LFO offset, ducking, a stress case; phase 2: pitch accuracy and confidence on sines / a saw / noise / silence, key tracking (tracked and MIDI), the Auto type on each test source and its timing, the harmonics of each shape and mode, the sub, the confidence gate, transients on the drum loop and a steady tone; phase 3: auto output on a hot resonance, a quiet band and silence, the limiter's sample and true peak at +12 dB, its loudness, the clipper without it, transparency under the ceiling, the reported latencies, a sudden +12 dB step; phase 4: the wobble rate, the tracked 41.2 Hz sine with its sub and the latency at 44.1 and 96 kHz, every switch thrown while playing (finite, ≤ 1.0, the step sizes printed); block-size independence |
| `--dump-params` | C++ parameter table as JSON (diffed by `check-params.ps1`) |
| `--parity <scenarios.json>` | C++ render of each scenario: RMS per window + peak as JSON |
| `--render <preset> <source[@Hz]> <out.wav>` | 8 s of a test source through a preset |
| `--bench` | worst-case and default CPU |
| `--loudness` + `tests\level-match.ps1` | preset level matching (see 7) |
| `--host-check <.vst3>` | loads the built plugin like a DAW: scan, buses, parameters, audio wobbles and stays ≤ 1.0 (also with everything automated at once and parked at 0 / 1), latency, mono in / out at 44.1 kHz with 1-, 37- and 64-sample blocks, programs, state |
| `tests\validate.ps1` | Steinberg's VST3 validator and pluginval (strictness 10, editor tests) on the built VST3; the tools live in `%LOCALAPPDATA%\PedalFaster\tools`. Strict: it reports pluginval's state-restoration group as failed (the host-cache reading, see the plan) |
| `tests\check-params.ps1` | `Params.h` vs `params.js` |
| `tests\run-parity.ps1` | the scenarios in headless Edge through `dsp-worklet.js`, compared per window (tolerance 1e-4) |
| `tests\run-web-tests.ps1 [-Browser Chrome]` | the built web build headless in Edge or Chrome, from disk and served: the bridge, presets, source-type defaults, tempo, MIDI, undo / redo, save / reload / delete; served: the AudioWorklet plays a saw, the LFO moves, the scope draws, the cutoff follows, the pitch is tracked and shown, Auto decides Bass and loads its defaults, the harmonics report their blend |

Scenarios: `{ name, preset?, params?, blocks, source {type, freq, level}, transport {bpm, playing, ppq}?, events: [[block, type, …]] }`
with event types `param id value`, `on note [vel]`, `off note`, `retrig`, `transport bpm playing [ppq]`.

## 10. Docs and release style

- README: version in the title, a one-table build overview, screenshots, "What it does" bullet lists, per-build
  sections, release history with "still to confirm by hand", known limitations, layout.
- `vst/README.md`: signal flow, host behaviour, feature tables, preset format, tests table, layout, bridge events,
  troubleshooting.
- Plain, concrete language. Numbers are measured, not guessed.
