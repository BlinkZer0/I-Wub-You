# I Wub You: User Manual

I Wub You is a wobble effect: a tempo-synced LFO sweeps a resonant filter over whatever you feed it, with drive,
and it adapts to the source. It works on a sine bass, a vocal, a guitar, a drum loop, a pad or a whole mix, mono
or stereo. This manual is for playing it; the technical notes are in the repository's `vst/README.md`.

## Getting started

**In a DAW:** run `install.bat`, rescan your plugins, and insert **I Wub You** on a track. Press play: the plugin
takes the tempo and the beat position from your DAW, so a synced wobble lines up with the bar.

**Standalone:** start `I Wub You.exe`, open **Options → Audio/MIDI Settings**, pick your audio interface, enable
its input and choose a small buffer. The standalone has no transport, so Sync runs from its own clock at 120 BPM.

**In a browser:** open `I Wub You Web.html` in Chrome or Edge. Click anywhere to enable audio, then pick an
**INPUT** at the top right: a built-in test source (a sine bass, a saw, a chord, a drum loop, noise), your
microphone or line-in, a browser tab or the screen (share its audio), or an audio file that loops. The **BPM**
field sets the tempo.

**First sound:** load a preset from the category that matches what you are feeding in (BASS, VOCAL, GUITAR,
DRUMS, PAD, FULL MIX), and play. Every preset is level-matched, so they are all about the same loudness.

## The window

```
┌ I WUB YOU   < preset >   SAVE LOAD FOLDER   undo redo A B   (INPUT · BPM)   ● transport  ● MIDI   RETRIG ┐
│ BASS  VOCAL  GUITAR  DRUMS  PAD  FULL MIX  USER          16 presets of the chosen category            │
│ the scope: the LFO's recent movement (left orange, right blue) · the sweep range and the live cutoff   │
│ ● WOBBLE   ● BANDS · DRIVE   ● SOURCE   ● OUTPUT                                                       │
│                              the page's panels                                                        │
└ status line · audio status ────────────────────────────────────────────────────────────────────────────┘
```

**Top bar.** `<` and `>` step through the presets; a `*` after the name means you changed something. **SAVE**
stores your own preset (it asks for a name, and asks again before replacing one). **LOAD** opens a `.wubpreset`
file. **FOLDER** opens your preset folder (`Documents\I Wub You\Presets`; in the browser it links a folder on
disk). **Undo / redo** (also Ctrl+Z / Ctrl+Y) step through your edits. **A / B** hold two complete settings to
compare, `A→B` copies one into the other. The **transport** lamp lights while the DAW plays and shows its tempo;
the **MIDI** lamp lights while a note is held. **RETRIG** restarts the wobble from the top of its cycle.

**The preset browser.** One tab per category plus USER for your own. Click a preset to load it. Your presets show a
`×` to delete them (they go to the Recycle Bin). Files you drop into the preset folder appear by themselves.

**The scope.** The left part draws the last seconds of the LFO (left channel orange, right channel blue, so a
stereo offset shows as two traces). The right part is a frequency axis from 20 Hz to 20 kHz: the shaded block is
the sweep range (Cutoff Low to High), the red line is where the filter is right now, one per wobbling band; in
Formant mode it draws the three formants of the current vowel instead. Two small bars at the join are the input
and output levels.

**Knobs.** Drag up or down (hold Shift for fine moves), use the mouse wheel or the arrow keys, double-click to
reset. Right-click a control for your DAW's automation menu.

## The pages

### WOBBLE

**LFO.** The shape: Sine, Tri, Saw ↓, Saw ↑, Square, S&H (a new random value every cycle), Random (a smooth
random drift). **SYNC** on: the **Division** sets the cycle as a note length of the DAW tempo, from 1/1 (a bar)
to 1/32, with dotted (D) and triplet (T) versions; while the DAW plays, the wobble is locked to the bar, so it
sounds the same every time round. **SYNC** off: **Rate** in Hz. **Phase** shifts the cycle; **Stereo** offsets the
right channel's LFO (90° gives movement across the stereo field, 180° makes the sides wobble against each other);
**Smooth** rounds the corners of Square and S&H (0 is instant, higher is softer). **Retrig: MIDI Note** makes any
MIDI note restart the wobble, so you can play it in time from a keyboard or a MIDI clip.

**WOBBLE FILTER.** The type: **LP** (the classic wub: it opens and closes), **BP** (a wah-like band), **HP**
(the bass drops out and returns), **Notch** (a phaser-like dip), **Formant** (vowels: the sweep goes A → E →
I → O → U). The LFO sweeps the cutoff from **Cutoff Low** (the closed end) to **Cutoff High** (the open end).
**Resonance** is the filter's emphasis at the cutoff: more is a sharper, more vocal wub. **Reso Limit** is the
safety: as the input gets loud, it turns the resonance down before the peak can blow up; leave it around half
on anything with transients. **Key Track** moves the whole sweep range with the pitch you play, so a bass line
wobbles the same way on every note; a MIDI note held into the plugin takes over from the tracked pitch.
**Drive → Filter / Filter → Drive** sets whether the distortion comes before or after the filter.

### BANDS · DRIVE

**SPLIT.** **Single** runs everything through one filter. **3-Band** splits the signal at **X-Over Low** and
**X-Over High** into three bands with their own strips, so a drum loop can wobble its lows and mids while the
hats stay crisp (switch WOBBLE off on the HIGH strip), or a mix can wobble only its mids. **Oversampling**
(1x / 2x / 4x) is for the drive: 2x is the default and good for most things; 4x for bright sources with heavy
drive; 1x for the lowest CPU and no latency from it.

**The strips** (LOW, MID, HIGH; in Single mode the middle one is the whole signal). **WOBBLE** lights red when
the band is being swept. **Depth** scales how far the sweep goes on this band (100% is the whole Low..High
range, less keeps it nearer the middle). **Drive** is a soft clip from clean to a hard 36 dB; **Tone** darkens it;
**Level** balances the band.

### SOURCE

**INPUT.** **AUTO GAIN** brings any input to the same working level (about −18 dBFS), so a quiet DI bass and a
loud mix hit the filter and the drive the same way; it holds still on silence. **Input** is a manual trim on top.
Below them the plugin shows the gain it is applying and the **PITCH** it hears (the note, the frequency and a
confidence bar).

**SOURCE TYPE.** Tell the plugin what it is listening to, or let **Auto** decide. Picking a type loads sensible
starting points for it (the cutoff range, the resonance, the drive, single or three bands and where they split,
whether the lows stay mono); everything stays editable. **Auto** listens for about a second, shows what it hears
under DETECTED, and loads that type's settings; it changes its mind only after hearing something else for a
second and a half. Pick a type by hand to stop it.

**HARMONICS · TRANSIENTS.** A wobble needs harmonics to work on: a pure sine bass has none, so the filter has
nothing to sweep over. **HARMONICS** adds them: **Shaper** (a soft clip and rectifier, the gentlest), **Saw** or
**Square** (an oscillator locked to the tracked pitch, the fattest). **Auto** adds them only as far as the source
is thin (a saw gets nothing); **Always** adds the full **Amount**. **SUB** adds an octave below the pitch. Both
need a tracked pitch, so chords and drums get nothing. **Transients** lets the attacks of drums and plucked
strings through the filter unfiltered, so a closed wobble keeps its punch.

### OUTPUT

**STEREO.** **Stereo** wobbles left and right (with the LFO's Stereo offset between them); **Mono Sum** folds the
input to mono first; **Mid-Side** wobbles the centre and the sides as the two channels. **Mono Below** keeps
everything under that frequency in the middle, for club systems.

**DUCK · MIX.** **Duck** lowers the level as the LFO falls, the pump of a sidechain compressor without one (100%
with a Square LFO is a gate). **Mix** blends the dry signal back in; **Output** is the final trim.

**SAFETY.** **AUTO OUT** keeps the loudness steady while you change the resonance or the drive (it matches the
wet level to the input's over about a second). The **LIMITER** is a true-peak limiter at −0.3 dBFS with 1 ms of
lookahead; the readout shows how much it is holding back. Leave both on unless you are after a zero-latency path.
The IN and OUT meters show the levels.

## Recipes

- **The classic dubstep wub on a bass line:** BASS → Classic Wub. Sync 1/8 for the standard pace, 1/4 for a
  slower one, 1/16 for a buzz. Turn Resonance up for more bite, Drive on the strip for grit. On a sine bass,
  switch HARMONICS on (Saw, Always) first.
- **A vocal that talks:** VOCAL → Vowel Wobble, or any preset with the Formant filter. Slow divisions (1/2, 1/1)
  for words, Random for babble.
- **Drums that wobble without losing the hats:** DRUMS → Wobbly Breaks: 3-Band with the HIGH strip's wobble
  off, Transients up so the kicks and snares still hit.
- **Pumping a whole mix:** FULL MIX → Fake Sidechain or Quarter Pump: every wobble off, Duck does the work,
  Smooth sets how round the pump is.
- **A riser or a drop:** FULL MIX → Build Riser (Saw ↑ over a bar) or Tape Stop Dip (Saw ↓). Set the division to
  the length of the build.
- **Keeping it in tune on a melody:** Key Track up (BASS → Key Tracked Wub). The sweep range follows the note.
- **Wide, moving pads:** PAD → Wide Wobble or Random Drift: the Stereo offset puts the movement across the field.

## Tips and troubleshooting

- **Too loud or clipping:** switch AUTO OUT on, keep the LIMITER on, or turn Output down. The presets are matched
  to a sensible level on typical material; a hotter input will be louder.
- **Nothing happens on a chord or a loop with HARMONICS or SUB on:** they need a single tracked pitch (watch the
  confidence bar on the SOURCE page).
- **The wobble drifts against the beat:** SYNC must be on and the DAW playing; stopped, it free-runs at the
  division's rate. In the browser, set the BPM field.
- **Auto picks the wrong type:** pick it by hand; Auto stays off until you choose Auto again.
- **Latency:** 2x oversampling and the limiter add about 1 ms, which your DAW compensates. 1x with the limiter
  off is zero latency.
- **Switching Bands, the filter type, the stereo mode, the oversampling or the limiter while playing** makes a
  small step in the sound; set them before the take.
- **Sample rates:** anything works; the pitch tracker covers 30 Hz to 1.5 kHz at every rate.
