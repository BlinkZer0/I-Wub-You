# I Wub You: build plan

Status (2026-11-20): **phases 1–4 built, v1.0.0** (the core wobble chain, the plugin, the web build, the tests;
pitch tracking, key tracking, the Auto source type, harmonics + sub, transient preserve; auto output gain, the
true-peak limiter, level-matched presets; validation, other sample rates, DAW-style host checks, Chrome). What is
left is listening: the hand-tests listed under each phase and in the README's release notes.

Phase 4 notes (verification and release):
- **Validation like a host's scanner**: `tests\validate.ps1` runs Steinberg's VST3 SDK validator and pluginval at
  strictness 10 (editor tests included) on the built VST3; both tools are shared with Pedal=Faster. Steinberg's
  validator: **47 tests passed, 0 failed**. pluginval: see below (filled in from the run).
- **The host check's automation sweep must touch only the plugin's own parameters**: a VST3 exposes JUCE's bypass
  and 2048 MIDI CC parameters too, and parking the bypass at 1 broke the state round-trip comparison. The tool now
  finds the 51 by their host names.
- **pluginval: 24 of its 25 test groups pass; the "Plugin state restoration" group reports two switches stale.**
  Its sequence is: random values, save, other random values, restore, compare what the host reads. A host that
  sets a switch to 0.27 keeps 0.27 in its own cache and only learns the plugin's snapped value when something
  notifies it; a restore that puts the switch back where it already was changes nothing on the plugin side, and
  the host keeps reading its own 0.27. The failing parameters are always switches whose two random draws snapped
  the same way. What was established:
  - `--host-check` reproduces the sequence and then **saves the state again and compares it with the saved one:
    identical for all 51 parameters**. The plugin restores correctly; the stale values live in the JUCE host's
    parameter cache.
  - The same two-switch failure appears with the plugin built against **JUCE 9.0.5** (a separate build in
    `%LOCALAPPDATA%\IWubYouVST\build-juce9`), so it is not a JUCE 8.0.17 matter; Steinberg's validator passes
    47 / 47 on both builds.
  - `setStateInformation` now sends a value-changed notification for every parameter after `replaceState`
    (harmless, and it cut the stale readings in the reproduction from 12 to 4), but the wrapper ignores
    notifications while a state is being set, so it cannot close the gap for switches that did not change.
  `tests\validate.ps1` stays strict and reports the group as failed; a DAW re-reads every parameter from the
  plugin's controller after a project load, which is what the state comparison shows to be right.
- **Sample rates.** Two things only worked at 44.1 / 48 kHz and were fixed: the pitch tracker decimated by a fixed
  4 (at 96 kHz its lowest pitch would have been 60 Hz), now by `sr / 12 kHz` rounded (4, 4, 8 at 44.1 / 48 / 96);
  the limiter's lookahead buffer capped at 64 samples (0.67 ms at 96 kHz), now 256. New checks at 44.1 and 96 kHz:
  the wobble rate, a 41.2 Hz sine tracked with its sub-octave, the reported latency (3 + 1 ms: 47 / 51 / 99).
- **DAW behaviour**: `reset()` re-prepares the engine (stop / relocate clears the filters, the followers and the
  analysis). The host check now also automates every parameter at once for 100 blocks and parks them all at 0 and
  at 1 (finite, never over full scale), and runs a mono-in / mono-out instance at 44.1 kHz with 1-, 37- and
  64-sample blocks.
- **Switching while playing**: a check measures the largest sample step at each switch (bands, oversampling, filter
  type, stereo mode, drive position, harmonics, the limiter) against the steps elsewhere; all finite and under
  full scale. Toggling the limiter or the oversampling changes the latency, which is reported to the host but is
  a step in the audio by nature.
- **Browsers**: the web tests run in Chrome as well as Edge (`run-web-tests.ps1 -Browser Chrome`).
- Version 1.0.0 in `CMakeLists.txt` (the plugin's reported version and the web build's banner).

Phase 3 notes (output), no new parameters:
- **Auto Output** (`dsp/Limiter.h`, `AutoOutput`): the wet sum (before ducking and before the dry / wet mix) and the
  conditioned input are both RMS-tracked over a second; the wet signal is scaled by input / wet, ±12 dB, smoothed over
  a second, held while either is under −80 dBFS. Measured: a −23 dBFS tone at the cutoff through Q 20 (+26 dB on its
  own) comes out at the input level within the 12 dB the match allows; a band at −9 dB comes back to within 0.5 dB.
- **Limiter** (`TruePeakLimiter`): stereo-linked, 4x oversampled true peaks from the same HIIR half-bands as the
  drive, 1 ms lookahead (48 samples at 48 kHz: a delay line and a sliding minimum of the gain each sample needs),
  0.3 ms attack, 50 ms release, ceiling −0.3 dBFS; the audio is delayed by the lookahead and the plugin reports the
  oversampling delay plus the lookahead as latency (51 samples at the defaults, 2x + limiter; 44 at 44.1 kHz with
  1x). Measured: a saw at +12 dB leaves with its sample peak at or under 0.985 and its 8x-interpolated true peak
  under 1.0 while still loud; under the ceiling it is the input 48 samples later to 1e-9; a sudden +12 dB step on a
  60 Hz sine never passes 0.985 (the lookahead has the gain in place). The safety clipper stays behind it.
- **Preset level matching**: `--loudness` renders each preset on its category's test source (Bass: a 55 Hz sine;
  Vocal: a saw chord at 220 Hz; Guitar: at 110 Hz; Drums and Full Mix: the drum loop; Pad: a chord at 165 Hz),
  4 s at 120 BPM, RMS of the last 2 s; `tests\level-match.ps1` writes the `outGain` that puts each at −18 dBFS (the
  auto input gain's target) into `factory_presets.json`; the preset test fails beyond ±1 dB. Auto Output stays
  off in the presets (it changes the dynamics; the user switches it on).
- UI: the OUTPUT page's SAFETY panel shows the auto output gain and the limiter's deepest recent reduction (red
  when working); in Formant mode the scope draws the three formants of the current vowel (with its letter) instead
  of a cutoff line.
- Talking Bass needed its tracked Saw harmonics (Always, 80%): a formant filter on a 55 Hz sine passes almost
  nothing (the formants start at 325 Hz), and the level match ran into its +24 dB cap.
- CPU: defaults 1.7% of one core, worst case 4.4% (the limiter's half-bands and the tracker run always; the same
  binary read 1.5% / 4.0% on a quieter run of the machine).
- Verification: ALL TESTS PASSED (69 engine checks, presets within 0.26 dB of −18 dBFS), HOST CHECK OK (latency
  51 at the defaults), PARITY OK on 108 scenarios (worst 3.94e-6), WEB TESTS PASSED (64 checks), PARAMS OK.
- Tests: 9 engine checks, the loudness check on 24 presets, 10 parity scenarios (auto out hot / quiet / toggled,
  the limiter at +12 / +18 dB, off, toggled, stereo-linked, transparent, everything on), 2 web checks (the limiter
  holding +18 dB under full scale and reporting, Auto Out reporting).

Phase 2 notes (source intelligence), all in the engine and its JS twin, no new parameters:
- **Pitch tracker** (`dsp/Pitch.h`): YIN on a 4x decimated, 2.5 kHz low-passed copy of the conditioned mono input,
  512-sample window, lags 8..400 (30 Hz .. 1.5 kHz), every 1024 input samples, parabolic refinement, about 200
  multiply-adds per sample. Measured: sines from 41.2 Hz to 1 kHz within 0.3% (1000 → 1003.3 Hz: the lag
  quantisation at 12 kHz) at confidence 1.00, a saw 110.00 Hz, a −54 dBFS sine the same, noise confidence 0.14,
  silence 0.
- **Key tracking**: the sweep range (and the formants) shift by `(pitch / 110 Hz) ^ Key Track`, a held MIDI note
  winning over the tracked pitch, smoothed over 20 ms in the log domain. Measured: 100% with 1 kHz set, a 220 Hz
  input → 2000 Hz, 55 Hz → 500 Hz, a held A4 → 4000 Hz; 50% on 440 Hz → 2001 Hz.
- **Auto source type**: features are the pitch confidence averaged over a second, the tracked pitch, the RMS
  frequency (from the first difference: a cheap centroid), the 20 ms envelope's coefficient of variation over a
  second, the crest factor and the energy ratio below 200 Hz. Rules (`classifySource`): variation > 0.6 with low
  confidence → Drums; confident and pitch < 130 Hz → Bass, < 1 kHz → Guitar if the crest passes 5 else Vocal, above
  → Vocal; unpitched and all low → Bass; steady → Pad, moving → Full Mix. The first decision waits 1 s, a change
  needs 1.5 s of agreement; the processor's timer (and the web host) loads the decided type's `kSourceKeys` while
  Source Type is Auto. Measured on the test sources: a 55 Hz sine and a 110 Hz saw → Bass, the drum loop → Drums
  (variation 2.1), a 440 Hz sine → Vocal, the saw chord → Bass (its common fundamental is 55 Hz), noise → Pad.
  Two things found and fixed: the crest factor over 300 ms never separated the drum loop (2.5) from a chord (2.8),
  hence the envelope variation; and statistics ramping from zero looked like variation 0.3 on a steady tone (enough
  to flip a candidate and delay the first decision), so the envelope statistics start 100 ms in, from the settled
  envelope, and the averaged confidence from the first analysis.
- **Harmonics** (`dsp/Harmonics.h`): Shaper = the residue of a soft clip and a rectifier (3rd and 2nd harmonics,
  no fundamental, no DC); Saw / Square = a PolyBLEP oscillator at the tracked pitch, phase-locked to the input's
  rising zero crossings (at most once per period), scaled by the input's peak envelope; Sub = a flip-flop at the
  locks through a one-pole at the pitch. Auto mode scales Amount by the thinness (RMS frequency over the pitch: 1
  for a sine, 0 for a saw); everything is gated by the pitch confidence. Measured on a 110 Hz sine at 100%:
  Shaper 2nd +1.4 dB / 3rd −8.6 dB re the fundamental, Saw 2nd −1.6 / 3rd −5.2 / 5th −9.4 dB, Square 3rd −14.8 dB
  with the 2nd 60 dB lower, Sub 55 Hz at +1.0 dB; Auto leaves a saw within 0.00 dB; noise gets nothing.
- **Transient preserve**: a peak follower (instant attack, 20 ms release) against its own 60 ms lagged copy; the
  attack = `clamp((fast / slow − 1) / 2)` blends the dry signal into the wet sum. Measured: the drum loop through a
  150 Hz low-pass keeps 10,000× the high-frequency energy at 100%, a steady tone changes by −0.16 dB (a 5 ms-attack
  follower against a 20 ms one leaked 2.9 dB: followers with different attack times disagree on a sine's level).
- UI: the SOURCE page shows the pitch (note, Hz, confidence bar), the detected type ("· loaded" when Auto applied
  it) and how much harmonic content is being added; the PHASE 2 tags are gone.
- Tests: 20 new engine checks, 14 parity scenarios (key tracking with a MIDI note, every harmonics mode and shape,
  the sub, transients on the drum loop and a preset, Auto on drums, everything at once), 6 web checks (the tracked
  pitch of the built-in saw reaches the UI, Auto decides Bass and loads its defaults, the harmonics readout).
- CPU: defaults 1.2% of one core (the tracker runs always), worst case 3.8%.

## The brief

A source-adaptive wobble effect: it must work on anything (sine bass, vocals, guitar, drums, pads, full mixes,
mono or stereo). The signal chain from the brief, and where each item lands:

| # | Brief | Phase |
|---|---|---|
| 1 | DC blocker + auto input gain (RMS-tracked, −18 dBFS target, toggle) | 1 |
| 1 | Source-type selector Bass / Vocal / Guitar / Drums / Pad / Full Mix loading cutoff range, drive, crossover | 1 |
| 1 | **Auto** mode: spectral centroid, crest factor, pitch confidence → type | 2 |
| 2 | Harmonic generation for thin sources: waveshaper + pitch-tracked saw / square resynthesis (YIN), amount; sub-octave | 2 |
| 3 | Linkwitz-Riley 3-band split, per-band wobble on/off and depth, Single-band mode | 1 |
| 4 | Drive per band: tanh soft clip, amount + tone, 2x / 4x oversampling | 1 |
| 5 | Wobble filter LP / BP / formant (vowel morph), stable SVF / ZDF (+ HP and notch) | 1 |
| 5 | Key tracking: cutoff follows the detected pitch | 2 |
| 5 | Resonance auto-limited as the input level rises | 1 |
| 6 | Transient preserve: attacks pass partly unfiltered | 2 |
| 7 | Stereo / Mono-sum / Mid-Side; stereo LFO phase offset; mono below a frequency | 1 |
| 8 | LFO-driven ducking, output gain, dry/wet, latency-compensated and reported | 1 |
| 9 | Auto output gain matching (toggle) + true-peak limiter | 3 |

The LFO itself (not in the brief, but the wobble needs one): sine / triangle / saw down / saw up / square / sample &
hold / smooth random, free Hz or tempo-synced note divisions (1/1 … 1/32 with dotted and triplet), phase, stereo
offset, edge smoothing, MIDI-note retrigger, phase-locked to the host transport (phase 1).

## Phase 1: the core chain (built)

- **Parameter table** (51 parameters) defined and frozen, including the phase-2 / 3 ones: `vst/Source/Params.h`,
  mirrored in `ui/js/params.js`, checked by `check-params.ps1` (PARAMS OK, 51 match).
- **Engine** `WubEngine.h` + `dsp/`: input conditioning, LR4 split (sums flat to 0.05 dB from 50 Hz to 12 kHz,
  white noise −0.003 dB), per-band drive with The Ham's HIIR half-bands (latency 0 / 3 / 4 samples, dry path delayed
  to match: a 2 kHz tone at mix 50% reads within 0.02 dB), ZDF SVF (LP / BP / HP / notch; Q 0.5 … 20, +23 dB at Q 20),
  three-formant vowel morph, resonance limit (a −6 dBFS tone at the cutoff with Q 20: peak 9.96 → 0.5 allowed),
  the LFO (free 2 Hz measures 2.000, synced 1/4 at 120 BPM 2.000, stopped 1/8T at 150 BPM free-runs at 7.500;
  transport lock reproducible to 1e-9), ducking, stereo modes, mono-below (LR4: the side's 60 Hz −24 dB, 1 kHz
  untouched), safety clip.
- **Decisions made while building** (changed from the first cut, see `docs/CONVENTIONS.md` §3):
  - The auto gain tracks the level **before** its own gain (a feedback loop only applied half the correction in dB).
  - **1x drive is plain tanh.** First-order ADAA at the base rate averages two samples: a 3 dB loss at 12 kHz
    even with Drive at 0, which failed the flatness check. 2x / 4x keep ADAA at the oversampled rate.
    Measured aliases of a 5 kHz sine driven 36 dB: 1x −15 dB, 2x −53 dB, 4x −52 dB below the fundamental.
  - The shaper is `4 tanh(x / 4)` (Drive 0 is linear: 3rd harmonic −86 dB, level −0.003 dB on a −20 dBFS tone);
    Tone at 100% bypasses its one-pole.
  - The resonance limit uses a true peak follower (instant attack, 120 ms release); a 5 ms attack undershot a
    440 Hz tone by 10 dB and did almost nothing.
  - Preset base template at **output −6 dB**: the defaults (auto gain to −18 dBFS RMS, +5 dB of drive make-up,
    +10 dB resonant peak) otherwise sit on the safety clipper. Phase 3's auto output gain is the real fix.
- **Plugin**: effect with stereo in / out, MIDI in, host transport, 24 programs, state, latency reporting, the
  WebView2 UI (blue / orange / red), `--host-check` loads the built VST3: scans as an effect, 51 parameters named,
  audio wobbles 18 dB and never exceeds 1.0, programs and state round-trip.
- **Web build**: `web/index.html` (one file) with the JS twin, built-in sources, microphone / tab / file inputs, a
  BPM field, presets in the browser or a linked folder. **Parity**: PARITY OK on 84 scenarios (every preset, every
  LFO shape and sync case, every filter type, drive at each rate, bands, stereo modes, ducking, automation, stress),
  worst relative error 3.77e-6 against a tolerance of 1e-4; JS renders at 3.1× the C++ time (about 3% of real time
  in headless Edge). Web tests: 57 checks from disk and served (AudioWorklet playing, LFO moving, scope drawing).
- **CPU** (`--bench`, Release, one core): defaults 0.8%; worst case (3 bands, formant wobble on all, 4x drive on
  all, mono-below, stereo) 3.4%.

What to try by hand (phase 1):
- `vst\build.bat`, `vst\install.bat`, then the plugin on a bass line, a vocal, a drum loop and a full mix. Pick the
  type on the SOURCE page and listen to the defaults it loads; then WOBBLE: shapes, divisions, Sync with the
  transport running and stopped, Resonance Limit on loud material; BANDS: 3-band on drums with the HIGH strip's
  wobble off (hats stay crisp).
- `start.bat`: the same in a browser with the built-in drum loop, then the microphone or a YouTube tab.
- Listen for: zipper noise on fast knob moves (everything is ramped per 32 samples), clicks when switching
  oversampling or bands (states are kept; the crossovers are recomputed), the level of the defaults.

## Phase 2: source intelligence (built, see the notes at the top)

Pitch tracking (YIN) → key tracking; the Auto source type from the pitch confidence, the envelope variation and
the RMS frequency; harmonic generation (shaper residue, or a phase-locked saw / square at the tracked pitch) and the
sub-octave for thin sources; transient preserve. What to try by hand: a plain sine bass with HARMONICS on (Auto
adds, Always adds more; Saw vs Square vs Shaper), a drum loop with TRANSIENTS up through a low cutoff, Key Track on a
bass line, and Source Type = Auto on each kind of material (the SOURCE page shows what it hears and when it loads).

## Phase 3: output (built, see the notes at the top)

Auto output gain (±12 dB, 1 s, ducking excluded), the true-peak lookahead limiter (reported as latency), the
presets level-matched on their category's test source, the formant display in the scope. What to try by hand: a
bass line with Auto Out on while sweeping Resonance and Drive (the loudness should hold), the limiter on a drum loop
with Output pushed up (no overs, the readout working), and whether −18 dBFS presets sit right in a mix.

v1.0.1 (presets): the library quadrupled from 24 to **96, 16 per category**. Each category keeps its four originals and
adds twelve that cover the range: sine / triangle wubs at the common divisions, square and sample-and-hold chops
(1/16, 1/32, triplets), one-bar sweeps and risers (saw up and down with smoothing), the five filter types including
formant vowels and notch phasing, key-tracked and sub-octave variants, stereo-offset and mid-side movement, 3-band
splits with one band off, pure ducking "sidechain" presets with every wobble off, and a subtle low-mix one. All 96
are level-matched on their category's test source (four passes of `level-match.ps1`: with the limiter in the
chain the level responds less than linearly to the output gain, so each pass closes part of the gap), the preset
browser shows a category's 16 at once (8 columns × 2 rows; user presets scroll below), the web and host checks
count 96. Two Full Mix sweeps (Radio Filter, DJ Sweep Up: a one-bar band-pass / high-pass sweep over the drum
loop) settle 1.3 and 1.7 dB under the target: nearly closed for part of each bar, their 2 s RMS stays low while
their open moments already sit on the limiter, and more gain only squashes. The loudness check allows 2 dB
(1 dB for all but a few sweeps) rather than pushing them into the limiter.

Phase 5 (release packaging): `vst\package.ps1` stages and zips `dist\IWubYou-<version>-win64.zip` with the VST3
bundle, the Standalone, `install.bat` / `uninstall.bat`, the 96 presets as `.wubpreset` files
(`IWubYouTests --export-presets`, complete files with every parameter resolved, one folder per category), the web
build renamed `I Wub You Web.html`, the user manual (`docs/USER_MANUAL.md`, written for players: the window, the
four pages, recipes, troubleshooting) and `LICENSES.md` (JUCE, the VST3 SDK, the HIIR design, WebView2). `dist/`
is ignored by git. Pedal=Faster's package layout, kept to this family's single-VST3 install (no Start Menu entry).

## Phase 4: verification and release (built, see the notes at the top)

Automated: validation (Steinberg + pluginval), 44.1 / 96 kHz, DAW-style host checks (automation of everything, mono,
odd block sizes, reset), switching while playing, Chrome. By hand, still open: Minihost Modular / a DAW with
automation, tempo changes, an offline render, a mono track, the pumping on a mix; the web build on a laptop with a
tab's audio and the microphone. Release notes and known limitations are in the README (v1.0).
