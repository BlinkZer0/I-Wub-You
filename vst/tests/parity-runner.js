// ============================================================================
//  JS half of the DSP parity test. Renders tests/parity_scenarios.json with the
//  JS engine (webhost/dsp-worklet.js), through the same WubProcessor and
//  message path the web build uses, and returns the same statistics as the C++
//  tool (IWubYouTests --parity): RMS per channel in 16 windows + peak.
//  The test source is generated inside the processor (the `source` message),
//  as the web build's built-in sources are, from the same TestSource twin.
//
//  Loaded by run-parity.ps1 into headless Edge (after params.js, factory.js and
//  dsp-worklet.js). Also runs under Node for quick checks:
//    node tests/parity-runner.js            (prints the JSON)
// ============================================================================
'use strict';

function wubParityRun(scenarios, factoryJson, Proc) {
  const WINDOWS = 16, BLOCK = 128;
  const factory = {};
  for (const p of wubFactoryPresets(factoryJson)) factory[`${p.category}/${p.name}`] = p;
  const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());
  const SOURCE_NAMES = ['silence', 'sine', 'saw', 'square', 'chord', 'noise', 'drums', 'wide'];
  const sourceType = (t) => (typeof t === 'string' ? Math.max(0, SOURCE_NAMES.indexOf(t)) : (t | 0));

  const out = [];
  for (const sc of scenarios) {
    const values = defaultParamValues();
    if (sc.preset) {
      if (!factory[sc.preset]) throw new Error(`unknown preset ${sc.preset}`);
      Object.assign(values, factory[sc.preset].values);
    }
    for (const [id, v] of Object.entries(sc.params || {})) {
      if (!(id in values)) throw new Error(`unknown param ${id}`);
      values[id] = v;
    }
    const src = sc.source || {};
    const tr = sc.transport || {};
    const bpm0 = tr.bpm === undefined ? 120 : tr.bpm, playing0 = !!tr.playing, ppq0 = tr.ppq || 0;
    const p = new Proc({ processorOptions: { params: values, bpm: bpm0,
      source: { gen: src.type === undefined ? 1 : sourceType(src.type), freq: src.freq === undefined ? 55 : src.freq, level: src.level === undefined ? 0.5 : src.level } } });
    const send = (m) => p.port.onmessage({ data: m });
    const param = (id, value) => { if (!(id in values)) throw new Error(`unknown param ${id}`); send({ type: 'param', id, value }); };
    const L = new Float32Array(BLOCK), R = new Float32Array(BLOCK);
    const per = Math.max(1, Math.floor(sc.blocks * BLOCK / WINDOWS));
    const sl = new Float64Array(WINDOWS), sr = new Float64Array(WINDOWS);
    let peak = 0;
    const byBlock = new Map();
    for (const ev of sc.events || []) { if (!byBlock.has(ev[0])) byBlock.set(ev[0], []); byBlock.get(ev[0]).push(ev); }
    // like Parity.cpp: the transport is set every block from a running ppq
    let bpm = bpm0, playing = playing0, ppq = ppq0;
    const t0 = now();
    for (let b = 0; b < sc.blocks; b++) {
      send({ type: 'transport', bpm, ppq: ppq + b * BLOCK / 48000 * bpm / 60, playing });
      for (const ev of byBlock.get(b) || []) {
        const a = ev.slice(2);
        switch (ev[1]) {
          case 'param': param(a[0], a[1]); break;
          case 'on': send({ type: 'noteOn', note: a[0], vel: a[1] === undefined ? 100 : a[1] }); break;
          case 'off': send({ type: 'noteOff', note: a[0] }); break;
          case 'retrig': send({ type: 'retrig' }); break;
          case 'transport':
            bpm = a[0] === undefined ? 120 : a[0]; playing = !!a[1];
            ppq = (a[2] || 0) - b * BLOCK / 48000 * bpm / 60;
            send({ type: 'transport', bpm, ppq: a[2] || 0, playing });
            break;
          default: throw new Error(`unknown event type ${ev[1]}`);
        }
      }
      p.process([[]], [[L, R]]);
      for (let i = 0; i < BLOCK; i++) {
        const w = Math.min(WINDOWS - 1, Math.floor((b * BLOCK + i) / per));
        sl[w] += L[i] * L[i]; sr[w] += R[i] * R[i];
        peak = Math.max(peak, Math.abs(L[i]), Math.abs(R[i]));
      }
    }
    const ms = now() - t0;
    out.push({ name: sc.name, rmsL: [...sl].map(x => Math.sqrt(x / per)), rmsR: [...sr].map(x => Math.sqrt(x / per)), peak,
               ms, audioMs: sc.blocks * BLOCK / 48 });
  }
  return out;
}

// ---- Node entry point ----------------------------------------------------------------
if (typeof module !== 'undefined' && require.main === module) {
  const fs = require('fs'), path = require('path'), vm = require('vm');
  const vst = path.join(__dirname, '..');
  // main context: a vm sandbox makes every global lookup (Math.sin...) ~10x slower
  globalThis.sampleRate = 48000;
  const run = (file, tail) => vm.runInThisContext(fs.readFileSync(path.join(vst, file), 'utf8') + (tail || ''), { filename: path.join(vst, file) });
  run('ui/js/params.js', ';globalThis.defaultParamValues = defaultParamValues;');
  run('webhost/factory.js', ';globalThis.wubFactoryPresets = wubFactoryPresets;');
  run('webhost/dsp-worklet.js', `
    ;globalThis.AudioWorkletProcessor = class { constructor() { this.port = { postMessage() {}, onmessage: null }; } };
    globalThis.registerProcessor = (n, c) => { globalThis.Proc = c; };
    wubWorkletModule();`);
  const scenarios = JSON.parse(fs.readFileSync(path.join(__dirname, 'parity_scenarios.json'), 'utf8'));
  const factory = JSON.parse(fs.readFileSync(path.join(vst, 'presets/factory_presets.json'), 'utf8'));
  const res = wubParityRun(scenarios, factory, globalThis.Proc);
  process.stdout.write(JSON.stringify(res));
}
