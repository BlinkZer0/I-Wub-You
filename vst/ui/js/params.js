// ============================================================================
//  Parameter definitions. Must match vst/Source/Params.h exactly (IDs, ranges,
//  defaults, skew, choice lists, ORDER); tests/check-params.ps1 compares them.
//  New parameters are only appended. The web build is generated from this file.
// ============================================================================
'use strict';

const SOURCES = ['Auto', 'Bass', 'Vocal', 'Guitar', 'Drums', 'Pad', 'Full Mix'];
const HARM_MODES = ['Auto', 'Always'];
const HARM_SHAPES = ['Shaper', 'Saw', 'Square'];
const BANDS = ['Single', '3-Band'];
const OVERSAMPLE = ['1x', '2x', '4x'];
const DRIVE_POS = ['Pre Filter', 'Post Filter'];
const FILTER_TYPES = ['Low-pass', 'Band-pass', 'High-pass', 'Notch', 'Formant'];
const LFO_SHAPES = ['Sine', 'Triangle', 'Saw Down', 'Saw Up', 'Square', 'Sample & Hold', 'Smooth Random'];
const LFO_DIVS = ['1/1', '1/2', '1/2T', '1/4', '1/4D', '1/4T', '1/8', '1/8D', '1/8T', '1/16', '1/16D', '1/16T', '1/32'];
const LFO_DIV_BEATS = [4, 2, 4 / 3, 1, 1.5, 2 / 3, 0.5, 0.75, 1 / 3, 0.25, 0.375, 1 / 6, 0.125];
const LFO_RETRIGS = ['Off', 'MIDI Note'];
const STEREO_MODES = ['Stereo', 'Mono Sum', 'Mid-Side'];
const BAND_IDS = ['lo', 'mid', 'hi'];
const BAND_NAMES = ['Low', 'Mid', 'High'];

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
function midiNoteName(n) { return NOTE_NAMES[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 1); }

// ---- builders (same shapes as Params.h) ----------------------------------------
const P = (id, name, group, min, max, def, unit, extra) =>
  Object.assign({ id, name, group, type: 'float', min, max, def, unit: unit || '' }, extra || {});
const L = (id, name, group, min, max, def, unit) => P(id, name, group, min, max, def, unit, { skew: 'log' });
const B = (id, name, group, def) => ({ id, name, group, type: 'bool', min: 0, max: 1, step: 1, def: def ? 1 : 0, unit: '' });
const C = (id, name, group, choices, def) =>
  ({ id, name, group, type: 'choice', min: 0, max: choices.length - 1, step: 1, def, choices, unit: '' });

// the five controls of one band strip
function bandParams(b) {
  const id = BAND_IDS[b], name = BAND_NAMES[b];
  return [
    B(`${id}Wob`, `${name} Wobble`, 'band', b !== 2 ? 1 : 0),
    P(`${id}Depth`, 'Depth', 'band', 0, 1, 1, '%'),
    P(`${id}Drive`, 'Drive', 'band', 0, 1, 0.3, '%'),
    P(`${id}Tone`, 'Tone', 'band', 0, 1, 0.7, '%'),
    P(`${id}Level`, 'Level', 'band', -24, 12, 0, 'dB'),
  ];
}
// band 0 low, 1 mid, 2 high; field: wob | depth | drive | tone | level
function bandId(band, field) { return BAND_IDS[band] + field[0].toUpperCase() + field.slice(1); }

const PARAM_DEFS = [
  // ---- input
  C('source', 'Source Type', 'input', SOURCES, 0),
  B('autoIn', 'Auto Gain', 'input', 1),
  P('inGain', 'Input', 'input', -24, 24, 0, 'dB'),
  // ---- harmonics (phase 2)
  B('harmOn', 'Harmonics', 'harm', 0),
  C('harmMode', 'Mode', 'harm', HARM_MODES, 0),
  C('harmShape', 'Shape', 'harm', HARM_SHAPES, 0),
  P('harmAmount', 'Amount', 'harm', 0, 1, 0.5, '%'),
  B('subOn', 'Sub Octave', 'harm', 0),
  P('subAmount', 'Sub', 'harm', 0, 1, 0.5, '%'),
  // ---- bands
  C('bands', 'Bands', 'bands', BANDS, 0),
  L('xLow', 'X-Over Low', 'bands', 60, 1000, 200, 'Hz'),
  L('xHigh', 'X-Over High', 'bands', 800, 12000, 2500, 'Hz'),
  C('os', 'Oversampling', 'bands', OVERSAMPLE, 1),
  C('drivePos', 'Drive Position', 'bands', DRIVE_POS, 0),
  ...bandParams(0), ...bandParams(1), ...bandParams(2),
  // ---- wobble filter
  C('filterType', 'Filter', 'filter', FILTER_TYPES, 0),
  L('cutoffLo', 'Cutoff Low', 'filter', 20, 20000, 120, 'Hz'),
  L('cutoffHi', 'Cutoff High', 'filter', 20, 20000, 4000, 'Hz'),
  P('reso', 'Resonance', 'filter', 0, 1, 0.5, '%'),
  P('resoLimit', 'Reso Limit', 'filter', 0, 1, 0.5, '%'),
  P('keyTrack', 'Key Track', 'filter', 0, 1, 0, '%'),
  // ---- LFO
  C('lfoShape', 'Shape', 'lfo', LFO_SHAPES, 0),
  B('lfoSync', 'Sync', 'lfo', 1),
  L('lfoRate', 'Rate', 'lfo', 0.05, 20, 2, 'lfohz'),
  C('lfoDiv', 'Division', 'lfo', LFO_DIVS, 6),
  P('lfoPhase', 'Phase', 'lfo', 0, 360, 0, 'deg'),
  P('lfoStereo', 'Stereo', 'lfo', 0, 180, 0, 'deg'),
  P('lfoSmooth', 'Smooth', 'lfo', 0, 1, 0.1, '%'),
  C('lfoRetrig', 'Retrigger', 'lfo', LFO_RETRIGS, 0),
  // ---- transient preserve (phase 2)
  P('transPreserve', 'Transients', 'trans', 0, 1, 0, '%'),
  // ---- stereo
  C('stereoMode', 'Stereo Mode', 'stereo', STEREO_MODES, 0),
  P('monoBelow', 'Mono Below', 'stereo', 0, 300, 0, 'Hz'),
  // ---- output
  P('duck', 'Duck', 'output', 0, 1, 0, '%'),
  P('mix', 'Mix', 'output', 0, 1, 1, '%'),
  P('outGain', 'Output', 'output', -24, 24, 0, 'dB'),
  B('autoOut', 'Auto Output', 'output', 0),
  B('limiter', 'Limiter', 'output', 1),
];

const PARAM_BY_ID = Object.fromEntries(PARAM_DEFS.map(d => [d.id, d]));

function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

function defaultParamValues() {
  return Object.fromEntries(PARAM_DEFS.map(d => [d.id, d.def]));
}

// value <-> 0..1 normalized (same mapping the VST host sees)
function paramToNorm(d, v) {
  if (d.skew === 'log') return clamp(Math.log(v / d.min) / Math.log(d.max / d.min), 0, 1);
  return clamp((v - d.min) / (d.max - d.min), 0, 1);
}

function paramFromNorm(d, n) {
  n = clamp(n, 0, 1);
  let v = d.skew === 'log' ? d.min * Math.pow(d.max / d.min, n) : d.min + n * (d.max - d.min);
  if (d.step) v = Math.round(v / d.step) * d.step;
  return clamp(v, d.min, d.max);
}

function formatParam(d, v) {
  if (d.type === 'choice') return d.choices[clamp(Math.round(v), 0, d.choices.length - 1)];
  if (d.type === 'bool') return v >= 0.5 ? 'On' : 'Off';
  const sgn = (x) => (x > 0 ? '+' : '');
  switch (d.unit) {
    case 's':     return v < 1 ? `${Math.round(v * 1000)} ms` : `${v.toFixed(2)} s`;
    case 'Hz':    if (d.id === 'monoBelow' && v < 1) return 'Off';
                  return v >= 1000 ? `${(v / 1000).toFixed(v >= 10000 ? 1 : 2)} kHz` : `${Math.round(v)} Hz`;
    case 'lfohz': return `${v.toFixed(v < 1 ? 3 : v < 10 ? 2 : 1)} Hz`;
    case 'st':    return `${sgn(v)}${Math.round(v)} st`;
    case 'ct':    return `${sgn(v)}${Math.round(v)} ct`;
    case '%':     return `${Math.round(v * 100)}%`;
    case 'bip':   return `${v > 0.004 ? '+' : ''}${Math.round(v * 100)}%`;
    case 'dB':    return `${v > 0.05 ? '+' : ''}${v.toFixed(1)} dB`;
    case 'deg':   return `${Math.round(v)}°`;
    case 'ms':    return `${Math.round(v)} ms`;
    case 'count': return String(Math.round(v));
    case 'note':  return midiNoteName(Math.round(v));
    case 'oct':   return `${v > 0.005 ? '+' : ''}${v.toFixed(1)} oct`;
    default:      return v.toFixed(2);
  }
}
