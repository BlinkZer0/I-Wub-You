# I Wub You: VST3 plugin (JUCE 8)

This folder is the source of truth for I Wub You:
- **C++ DSP**: the plugin's engine (`Source/WubEngine.h`, `Source/dsp/`).
- **HTML/SVG UI**: shown inside the plugin through WebView2.
- **Web host** (`webhost/`): the same UI in a browser with a JS twin of the engine (`dsp-worklet.js`), checked
  against the C++ engine by the parity test.

## Build

```
build.bat            VST3 + Standalone + IWubYouTests (Release)
build.bat Debug
install.bat          copies "I Wub You.vst3" to C:\Program Files\Common Files\VST3 (asks for admin)
```

Build files go to `%LOCALAPPDATA%\IWubYouVST` so OneDrive doesn't sync them. An existing JUCE 8.0.17 download from
The Ham, Brass Knuckles or E-Piano is reused.

- `...\build\IWubYouVST_artefacts\Release\VST3\I Wub You.vst3`
- `...\build\IWubYouVST_artefacts\Release\Standalone\I Wub You.exe`: *Options → Audio/MIDI Settings* for the
  audio input and output and a MIDI input
- `...\build\IWubYouTests_artefacts\Release\IWubYouTests.exe`

## Signal flow (phase 3)

```
L, R (a mono input feeds both)
  -> Stereo mode: Stereo = L and R; Mono Sum = (L + R) / 2 once; Mid-Side = M = (L + R) / 2 and S = (L - R) / 2
  -> per channel: DC blocker (5 Hz) -> x Input gain x auto gain
       auto gain: the RMS before any gain (0.3 s window) is brought to -18 dBFS, +-24 dB, 0.4 s time constant,
       held when the input is under -80 dBFS
  -> the conditioned mono sum feeds the analysis (every sample; decisions every 32 samples):
       YIN pitch tracker (2.5 kHz low-pass, 4x decimated, 512-sample window, lags 8..400 = 30 Hz .. 1.5 kHz,
       every 1024 samples, parabolic refinement) -> pitch, confidence (30 ms smoothed; a 1 s average for Auto)
       features: RMS frequency (from the first difference), 20 ms envelope variation over 1 s, crest, energy < 200 Hz
       key tracking: (pitch or the held MIDI note) / 110 Hz, ^ Key Track, 20 ms smoothed in the log domain
       Auto source type: classifySource() on every analysis; the first decision after 1 s, a change after 1.5 s
         of agreement; the plugin's timer / the web host loads that type's defaults while Source Type is Auto
       the harmonic oscillator: PolyBLEP saw / square at the pitch, phase reset at a rising zero crossing of the
         low-passed input at most once per period; the sub-octave = a flip-flop at those resets -> one-pole at the pitch
  -> harmonics: x += blend x (Shaper residue of this channel | saw | square) x peak envelope (instant attack, 50 ms)
       Shaper residue = 3 (0.2 tanh 5x - x) + 1.2 (|x| - mean|x|): odd + even harmonics, no fundamental, no DC
       blend = Amount (Always), or Amount x thinness (Auto; thinness = (1.8 - RMS frequency / pitch) / 0.6, 0..1),
       x the confidence gate (not for the Shaper); x += Sub x sub-octave x envelope x gate
  -> Single band (the MID strip takes the whole signal), or Linkwitz-Riley 4th-order split at X-Over Low / High
     (the low band passes the upper crossover's all-pass, so the three bands sum flat: measured 0.05 dB)
  -> per band: a true-peak follower (instant attack, 120 ms release) for the resonance limit
     drive: x pre (0..36 dB) -> 4 tanh(x / 4) at 1x (plain), or with first-order ADAA at 2x / 4x
            (HIIR polyphase half-bands, latency 0 / 3 / 4 samples) -> x 1 / sqrt(pre) -> tone one-pole
            (800 Hz .. 20 kHz; 100% = bypass)
     wobble filter (if the band's WOBBLE is on): ZDF SVF, g = tan(pi fc / sr) ramped per sample,
            cutoff = Low x keyMul x (High / Low) ^ (0.5 + Depth x (LFO - 0.5)); Q = 0.5 .. 20 (log), divided by
            1 + Limit x max(0, Q x peak / 0.5 - 1); LP / BP / HP / notch; Formant = 3 band-passes at the
            formants of A E I O U (x keyMul; morphed by the same position, Q 4 .. 20) summed x 1.5
     drive and filter in either order (Drive Position) -> x Level
  -> sum of the bands -> + Transients x attack x (dry - sum)
       attack = clamp((fast / slow - 1) / 2): a peak follower (instant attack, 20 ms release) over its 60 ms lagged copy
  -> x auto output gain (Auto Out on): input RMS / wet RMS, both over 1 s and measured here (before the duck and the
       mix), +-12 dB, smoothed over 1 s, held under -80 dBFS
  -> x (1 - Duck x (1 - LFO))
  -> dry / wet: the dry path (with the harmonics) delayed by the oversampling latency -> back to L, R
  -> Mono Below (if > 0 Hz): the side signal high-passed (LR4), the mid untouched
  -> x Output
  -> true-peak limiter (Limiter on): the larger channel's 4x oversampled peak (the drive's half-bands) -> the gain
       it needs for a -0.3 dBFS ceiling -> the minimum over the 1 ms lookahead window, 0.3 ms attack, 50 ms
       release, stereo-linked, applied to the audio delayed by the lookahead
  -> safety clip (linear below -1.9 dBFS, tanh knee to 1.0) -> L, R
Control rate: every 32 samples the LFOs (one per channel, the right one offset by LFO Stereo), the cutoffs, the
resonance limit, the drive gains and the auto gain are evaluated and ramped linearly across the chunk.
LFO: phase = frac(ppq / beats) while synced and playing (beats per cycle from the division), else
phase += rate x dt (rate from the division and the tempo when synced); shapes on the phase + Phase offset;
sample & hold and smooth random draw from a seeded LCG at each cycle wrap; Smooth = a one-pole, 0 .. 250 ms.
```

## Host behaviour

- Buses: stereo or mono in, stereo or mono out. MIDI in: a note-on retriggers the LFO when Retrigger is
  MIDI Note and takes over key tracking; note-off / all-notes-off clear the held note.
- Sample rates: anything; the pitch tracker decimates to about 12 kHz (by 4 at 44.1 / 48 kHz, 8 at 96 kHz) so its
  range stays 30 Hz .. 1.5 kHz, the limiter's lookahead stays 1 ms. `reset()` (stop / relocate) clears all state.
- Transport: tempo, ppq position and play state from the host's `AudioPlayHead` every block; without a host
  tempo the engine assumes 120 BPM.
- Latency: 0 / 3 / 4 samples at 1x / 2x / 4x for the drive (the dry path inside the plugin is delayed by the same
  amount), plus 1 ms (48 samples at 48 kHz) while the limiter is on; reported at prepare and when either choice
  changes (the plugin's 50 Hz timer). The defaults report 51 samples.
- Programs: the 96 factory presets. State: every parameter plus the preset name / category.
- The UI is a WebView2 page compiled into the binary. Set **`WUB_UI_DIR`** to `vst\ui` to load it from disk while
  editing; **`WUB_UI_PAGE`** (`wobble`, `bands`, `source`, `output`) opens a page directly. Shift+right-click opens
  DevTools.

## Parameters

| Group | Parameters |
|---|---|
| Input | Source Type (Auto / Bass / Vocal / Guitar / Drums / Pad / Full Mix), Auto Gain, Input (±24 dB) |
| Harmonics | Harmonics, Mode (Auto / Always), Shape (Shaper / Saw / Square), Amount, Sub Octave, Sub |
| Bands | Bands (Single / 3-Band), X-Over Low 60–1000 Hz, X-Over High 0.8–12 kHz, Oversampling 1x/2x/4x, Drive Position; per band: Wobble, Depth, Drive, Tone, Level (−24..+12 dB) |
| Filter | Filter (LP / BP / HP / Notch / Formant), Cutoff Low / High 20 Hz–20 kHz, Resonance, Reso Limit, Key Track |
| LFO | Shape, Sync, Rate 0.05–20 Hz, Division (1/1 … 1/32, D, T), Phase 0–360°, Stereo 0–180°, Smooth, Retrigger (Off / MIDI Note) |
| Transients | Transients |
| Stereo | Stereo Mode (Stereo / Mono Sum / Mid-Side), Mono Below 0–300 Hz (0 = off) |
| Output | Duck, Mix, Output (±24 dB), Auto Output, Limiter |

Picking a Source Type (other than Auto) sets: bands, cutoffLo, cutoffHi, reso, the three Wobble switches, the three
Drives, xLow, xHigh, filterType, monoBelow, transPreserve, harmOn, harmAmount (from `dsp/Sources.h`, mirrored by the
factory templates `srcBass` … `srcMix`). With **Auto**, the engine's decision (`stats.detected`) loads the same
keys from the plugin's timer, once per decision; the SOURCE page says "DETECTED <type> · loaded". Picking a type by
hand stops that until Auto is picked again.

## Preset format

`Documents\I Wub You\Presets\*.wubpreset` (subfolders are categories), plain JSON:

```json
{ "format": "I Wub You Preset", "version": 1, "name": "My Wub", "category": "User",
  "params": { "lfoShape": 0, "lfoDiv": 6, "cutoffLo": 120, "cutoffHi": 4000, "reso": 0.5, "...": 0 } }
```

Missing parameters load at their defaults, values are clamped, unknown keys are ignored. Factory presets:
`presets/factory_presets.json` (templates + overrides; see `docs/CONVENTIONS.md` §7).

## Tests

| Command | What |
|---|---|
| `IWubYouTests.exe` | parameter table, presets, files, every preset renders, the engine checks |
| `IWubYouTests.exe --engine` | the engine checks alone (82 measurements: phase 1, then pitch, key tracking, Auto, harmonics, sub, transients, auto output, the limiter, 44.1 / 96 kHz, switches while playing) |
| `IWubYouTests.exe --loudness` | each preset's level on its category's test source and the outGain that would put it at -18 dBFS |
| `IWubYouTests.exe --export-presets <folder>` | the 96 factory presets as complete `.wubpreset` files, one folder per category (the release package's Presets folder) |
| `package.ps1` | the release zip in `dist\` (see the top-level README) |
| `tests\level-match.ps1` | writes those gains into `presets/factory_presets.json` (rebuild afterwards) |
| `IWubYouTests.exe --dump-params` | the parameter table as JSON |
| `IWubYouTests.exe --render <preset> <source[@Hz]> <out.wav>` | 8 s of a test source (sine, saw, square, chord, noise, drums, wide) through a preset |
| `IWubYouTests.exe --bench` | CPU: defaults and the worst case |
| `IWubYouTests.exe --host-check "<path>\I Wub You.vst3"` | loads the built plugin like a DAW |
| `IWubYouTests.exe --parity tests\parity_scenarios.json` | the C++ half of the parity test |
| `tests\check-params.ps1` | `Params.h` vs `params.js` |
| `tests\run-parity.ps1 [-Full]` | C++ vs JS on 108 scenarios, tolerance 1e-4 |
| `tests\run-web-tests.ps1 [-Browser Chrome]` | the web build headless, from disk and served, in Edge (default) or Chrome |
| `tests\validate.ps1 [-Strictness 10] [-SkipGui]` | Steinberg's VST3 validator + pluginval on the built VST3 (tools shared with Pedal=Faster) |
| `tests\screenshots.ps1` | the four pages → `docs/screenshots/` |

## Layout

```
Source/   Params.h  WubEngine.h  EngineParamsFromValues.h  PresetManager.h/.cpp  PluginProcessor.h/.cpp  PluginEditor.h/.cpp
          dsp/  Common.h (Rng, ChunkRamp, Biquad, safety clip)  Filters.h (SVF, formants, LR4, DC blocker, followers, delay)
                Oversampler.h (HIIR half-bands, the shaper)  Lfo.h  Pitch.h (YIN, features, classifySource)
                Harmonics.h (PolyBLEP osc, sub, shaper residue)  Limiter.h (auto output, true-peak limiter)
                Sources.h (source-type defaults)  TestSignals.h
ui/       index.html  css/style.css  js/params.js  engine-juce.js  knob.js  controls.js  scope.js  app.js
webhost/  dsp-worklet.js (the JS twin)  web-host.js  factory.js  build-web.ps1  dev.html
presets/  factory_presets.json
tests/    PresetTests.cpp (main)  EngineTests.cpp  EngineTools.cpp  Parity.cpp  HostCheck.cpp  TestRender.h
          parity_scenarios.json  parity-runner.js  parity-compare.js  run-parity.ps1  check-params.ps1
          run-web-tests.ps1  web-tests.js  screenshots.ps1  level-match.ps1  validate.ps1
```

## Bridge events

UI → host: `uiReady`, `setParam {id,value}`, `gesture {id,begin}`, `retrig`, `sourceType {type}`,
`loadPreset {id}`, `savePreset {name,overwrite}`, `deletePreset {id}`, `loadPresetFile`, `openPresetFolder`,
`paramMenu {id,x,y}`; web only `selectInput {id}`, `setTempo {bpm}`.
Host → UI: `params {id: value}`, `stats {peakIn, peakOut, lfo, lfoR, cutoff[3], autoGain, detected, bpm, playing, note, pitch, pitchConf, harm, autoOut, limiter}`,
`config`, `presetList`, `preset`, `saveResult`, `message`; web only `inputs`.

## Troubleshooting

- **No sound / a quiet wobble on a quiet track**: Auto Gain needs about half a second to settle; with Auto Gain off,
  the Input gain is yours. The SOURCE page shows the gain being applied.
- **It clips / it's too loud**: switch Auto Out on (the wet level follows the input's) and leave the Limiter on
  (−0.3 dBFS true peak). The presets sit at −18 dBFS on their category's material. The safety clipper never lets a
  sample pass 1.0 even with both off.
- **The level drifts for a second after play starts**: that is Auto Out settling (1 s); switch it off for a fixed gain.
- **The LFO doesn't line up with the beat**: Sync must be on and the transport running; stopped, it free-runs at the
  division's rate. The phase is locked to the host's ppq position, so a tempo change re-aligns it at once.
- **The hats wobble too**: BANDS → 3-Band, switch the HIGH strip's WOBBLE off (the Drums and Full Mix types do this).
- **Aliasing on a bright source with heavy drive**: use 2x or 4x oversampling (1x is plain tanh).
- **Latency**: 3 samples at 2x, 4 at 4x, reported to the host; 0 at 1x.
- **Harmonics or Sub do nothing**: they need a tracked pitch (the SOURCE page's confidence bar); chords and noise
  don't give one, and in Auto mode a source that is already rich (a saw) gets nothing by design: use Always.
- **Auto keeps picking the wrong type**: pick it by hand (that stops Auto until Auto is picked again). The Vocal /
  Guitar line is the crest factor; a steady pitched source under 130 Hz is always Bass.
- **The filter sweeps the wrong range on a melody**: Key Track shifts the range by the tracked pitch relative to
  A2 (110 Hz); set the Cutoff Low / High for the lowest note, or lower Key Track.
