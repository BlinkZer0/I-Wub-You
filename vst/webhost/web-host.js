// ============================================================================
//  Web host: stands in for the plugin's C++ side when the UI runs in a
//  browser. It provides window.__JUCE__.backend with exactly the events the
//  JUCE editor uses (see PluginEditor.cpp), so vst/ui runs unchanged:
//    audio    Web Audio: AudioWorklet (or ScriptProcessor fallback on file://)
//             running dsp-worklet.js, the JS twin of WubEngine.h
//    input    built-in test sources (sine bass, saw, chord, drum loop, noise:
//             generated inside the engine), the microphone / line-in, a browser
//             tab or the whole screen's audio, or an audio file (looped)
//    tempo    a BPM field (the plugin takes the host's tempo and transport)
//    MIDI     Web MIDI, all inputs: notes retrigger the LFO (Retrigger: MIDI Note)
//    presets  factory (FACTORY_JSON via factory.js), this browser (localStorage),
//             or a linked folder on disk (File System Access API), the same
//             .wubpreset format as the plugin, rescanned every 2 s
//  Needs params.js, factory.js, dsp-worklet.js and FACTORY_JSON loaded first.
// ============================================================================
(function () {
  'use strict';

  // ---------------------------------------------------------- the bridge ---
  const listeners = {};
  const handlers = {};
  const toUi = (ev, data) => queueMicrotask(() => (listeners[ev] || []).forEach(f => f(data)));
  window.__JUCE__ = {
    backend: {
      addEventListener(ev, fn) { (listeners[ev] = listeners[ev] || []).push(fn); },
      // handled synchronously so file pickers / permission prompts keep the click's user activation
      emitEvent(ev, data) { const h = handlers[ev]; if (h) h(data || {}); },
    },
  };
  const message = (text) => toUi('message', { text });
  const store = {
    get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set(k, v) { try { localStorage.setItem(k, v); return true; } catch (e) { return false; } },
  };

  // ------------------------------------------------------ factory presets ---
  const FACTORY = wubFactoryPresets(FACTORY_JSON).map((p, i) =>
    ({ id: `factory:${i}`, name: p.name, category: p.category, factory: true, values: p.values }));

  // ---------------------------------------------------------------- state ---
  const values = defaultParamValues();
  let current = { id: '', name: '', category: '' };
  let bpm = clamp(Number(store.get('wub.bpm')) || 120, 20, 300);

  // Every preset parameter: the preset's value, else the default (like PluginProcessor::applyPreset).
  // Stepped parameters snap like the plugin's int / choice params.
  function presetValues(pv) {
    const out = {};
    for (const d of PARAM_DEFS) {
      if (d.noPreset) continue;
      let v = Number(pv[d.id]);
      if (pv[d.id] === undefined || pv[d.id] === null || isNaN(v)) v = d.def;
      v = clamp(v, d.min, d.max);
      if (d.step) v = Math.round(v / d.step) * d.step;
      out[d.id] = v;
    }
    return out;
  }
  function applyPreset(pv, info) {
    const all = presetValues(pv);
    Object.assign(values, all);
    audio.post({ type: 'params', values: all });
    current = info;
    toUi('params', Object.assign({}, values));
    toUi('preset', current);
  }

  // a fresh instance starts on the first factory preset, like the plugin
  Object.assign(values, presetValues(FACTORY[0].values));
  current = { id: FACTORY[0].id, name: FACTORY[0].name, category: FACTORY[0].category };

  // a parameter change made by the host side (source type defaults): the engine and the UI both see it
  function hostSetParam(id, v) {
    const d = PARAM_BY_ID[id];
    if (!d) return;
    v = clamp(Number(v), d.min, d.max);
    if (d.step) v = Math.round(v / d.step) * d.step;
    if (values[id] === v) return;
    values[id] = v;
    audio.post({ type: 'param', id, value: v });
    toUi('params', { [id]: v });
  }

  // ---------------------------------------------------------------- audio ---
  // built-in sources are generated inside the engine (type = TestSource type); the rest feed the worklet's input
  const BUILTIN = [
    { id: 'tone', name: 'Test: sine bass (55 Hz)', type: 1, freq: 55, level: 0.5 },
    { id: 'saw', name: 'Test: saw (110 Hz)', type: 2, freq: 110, level: 0.4 },
    { id: 'chord', name: 'Test: saw chord', type: 4, freq: 110, level: 0.4 },
    { id: 'drums', name: 'Test: drum loop (120 BPM)', type: 6, freq: 0, level: 0.6 },
    { id: 'noise', name: 'Test: white noise', type: 5, freq: 0, level: 0.3 },
  ];
  const LIVE = [
    { id: 'mic', name: 'Microphone / line-in' },
    { id: 'tab', name: 'A browser tab or the screen (share its audio)' },
    { id: 'file', name: 'Audio file (looped)…' },
  ];
  const audio = {
    ctx: null, node: null, port: null, mode: '', error: '', input: store.get('wub.input') || 'tone', liveNode: null, liveStream: null, fileName: '',
    post(m) { if (this.port) this.port.postMessage(m); },
    get running() { return !!this.ctx && this.ctx.state === 'running'; },

    async start() {
      const ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
      this.ctx = ctx;
      ctx.onstatechange = sendConfig;
      const src = `(${wubWorkletModule.toString()})();`;
      const builtin = BUILTIN.find(b => b.id === this.input) || BUILTIN[0];
      const options = { params: values, bpm, source: { gen: builtin.type, freq: builtin.freq, level: builtin.level } };
      let built = null;
      if (ctx.audioWorklet && location.protocol !== 'file:') {
        try {
          const url = URL.createObjectURL(new Blob([src], { type: 'application/javascript' }));
          try { await ctx.audioWorklet.addModule(url); } finally { URL.revokeObjectURL(url); }
          const node = new AudioWorkletNode(ctx, 'i-wub-you', {
            numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], channelCount: 2, channelCountMode: 'explicit', processorOptions: options,
          });
          node.port.onmessage = (e) => onDspMessage(e.data);
          // an exception inside the audio thread silences the node: say so instead of failing quietly
          node.onprocessorerror = (e) => {
            audio.error = 'the audio engine stopped (processor error); reload the page';
            console.error('AudioWorklet processor error', e);
            message(`Audio engine error: ${(e && e.message) || 'processor error'}. Reload the page.`);
            sendConfig();
          };
          built = { node, port: node.port, mode: 'AudioWorklet' };
        } catch (e) { console.warn('AudioWorklet unavailable, using ScriptProcessor', e); }
      }
      if (!built) built = this.scriptProcessor(ctx, src, options);
      this.node = built.node; this.port = built.port; this.mode = built.mode;
      this.node.connect(ctx.destination);
      this.post({ type: 'params', values: Object.assign({}, values) }); // anything changed while starting
      this.post({ type: 'tempo', bpm });
      if (!BUILTIN.some(b => b.id === this.input)) this.selectInput(this.input, false);
      sendConfig();
    },

    // Chrome/Edge block worklet modules on file://: run the same DSP on the main thread
    scriptProcessor(ctx, src, options) {
      class AudioWorkletProcessorShim { constructor() { this.port = { postMessage: onDspMessage, onmessage: null }; } }
      let Processor = null;
      new Function('AudioWorkletProcessor', 'registerProcessor', 'sampleRate', src)(
        AudioWorkletProcessorShim, (name, cls) => { Processor = cls; }, ctx.sampleRate);
      const proc = new Processor({ processorOptions: options });
      const L128 = new Float32Array(128), R128 = new Float32Array(128), iL = new Float32Array(128), iR = new Float32Array(128);
      const node = ctx.createScriptProcessor(1024, 2, 2);
      node.onaudioprocess = (e) => {
        const L = e.outputBuffer.getChannelData(0), R = e.outputBuffer.getChannelData(1);
        const inL = e.inputBuffer.getChannelData(0), inR = e.inputBuffer.numberOfChannels > 1 ? e.inputBuffer.getChannelData(1) : inL;
        for (let off = 0; off < L.length; off += 128) {
          iL.set(inL.subarray(off, off + 128)); iR.set(inR.subarray(off, off + 128));
          proc.process([[iL, iR]], [[L128, R128]]);
          L.set(L128, off); R.set(R128, off);
        }
      };
      return { node, port: { postMessage: (m) => proc.port.onmessage({ data: m }) }, mode: 'ScriptProcessor' };
    },

    // the input source: a built-in generator (inside the engine), or a live node into the worklet
    async selectInput(id, announce) {
      const ctx = this.ctx;
      if (!ctx) { this.input = id; return; }
      const builtin = BUILTIN.find(b => b.id === id);
      const stopLive = () => {
        if (this.liveNode) { try { this.liveNode.disconnect(); } catch (e) { /* already */ } this.liveNode = null; }
        if (this.liveStream) { this.liveStream.getTracks().forEach(t => t.stop()); this.liveStream = null; }
      };
      try {
        if (builtin) {
          stopLive();
          // the message type is 'source'; the generator type travels as `gen`
          this.post({ type: 'source', gen: builtin.type, freq: builtin.freq, level: builtin.level });
        } else if (id === 'mic') {
          const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false }, video: false });
          stopLive();
          this.liveStream = stream;
          this.liveNode = ctx.createMediaStreamSource(stream);
          this.liveNode.connect(this.node);
          this.post({ type: 'source', gen: -1 });
          if (announce) message('Input: microphone / line-in (use headphones). Auto Gain brings it to -18 dBFS.');
        } else if (id === 'tab') {
          const stream = await navigator.mediaDevices.getDisplayMedia({ video: true, audio: true });
          stream.getVideoTracks().forEach(t => t.stop());
          if (!stream.getAudioTracks().length) throw new Error('no audio was shared: tick "Share tab audio" / "Share system audio" in the picker');
          stopLive();
          this.liveStream = stream;
          this.liveNode = ctx.createMediaStreamSource(stream);
          this.liveNode.connect(this.node);
          this.post({ type: 'source', gen: -1 });
          stream.getAudioTracks()[0].onended = () => { if (this.input === 'tab') { this.selectInput('tone', true); toUi('inputs', inputList()); } };
          if (announce) message('Input: the shared tab / screen audio. Note the original keeps playing too unless you muted the tab.');
        } else if (id === 'file') {
          const file = await pickFile();
          if (!file) { toUi('inputs', inputList()); return; }
          const buffer = await ctx.decodeAudioData(await file.arrayBuffer());
          stopLive();
          const node = ctx.createBufferSource();
          node.buffer = buffer;
          node.loop = true;
          node.connect(this.node);
          node.start();
          this.liveNode = node;
          this.fileName = file.name;
          this.post({ type: 'source', gen: -1 });
          if (announce) message(`Input: ${file.name} (${buffer.duration.toFixed(1)} s, looped)`);
        }
        this.input = id;
        store.set('wub.input', id);
      } catch (e) {
        if (!(e && e.name === 'AbortError')) message(`Input: ${e.message || e}`);
        this.input = BUILTIN[0].id;
        this.post({ type: 'source', gen: BUILTIN[0].type, freq: BUILTIN[0].freq, level: BUILTIN[0].level });
      }
      toUi('inputs', inputList());
    },
  };

  function inputList() {
    const list = BUILTIN.map(b => ({ id: b.id, name: b.name })).concat(LIVE.map(l => ({ id: l.id, name: l.id === 'file' && audio.fileName ? `File: ${audio.fileName}` : l.name })));
    return { inputs: list, selected: audio.input };
  }

  const fileInputAudio = document.createElement('input');
  fileInputAudio.type = 'file';
  fileInputAudio.accept = 'audio/*,.wav,.mp3,.flac,.ogg,.m4a';
  fileInputAudio.style.display = 'none';
  let filePick = null;
  fileInputAudio.addEventListener('change', () => { const f = fileInputAudio.files && fileInputAudio.files[0]; fileInputAudio.value = ''; if (filePick) filePick(f || null); filePick = null; });
  function pickFile() {
    return new Promise((res) => { filePick = res; fileInputAudio.click(); });
  }

  // what the engine reports: stats for the scope, the meters and the lamps; and, with Source Type = Auto,
  // the analysis' decision loads that type's defaults (once per decision), like the plugin's timer
  let appliedDetected = -1;
  function onDspMessage(m) {
    if (m.type !== 'stats') return;
    toUi('stats', { peakIn: m.peakIn, peakOut: m.peakOut, lfo: m.lfo, lfoR: m.lfoR, cutoff: m.cutoff, autoGain: m.autoGain, detected: m.detected,
                    bpm: m.bpm, playing: m.playing, note: m.note, pitch: m.pitch, pitchConf: m.pitchConf, harm: m.harm, autoOut: m.autoOut, limiter: m.limiter });
    if (Math.round(values.source) === 0) {
      if (m.detected >= 0 && m.detected !== appliedDetected) {
        appliedDetected = m.detected;
        const defaults = wubSourceDefaults(FACTORY_JSON, m.detected + 1);
        for (const id of Object.keys(defaults)) if (id !== 'source') hostSetParam(id, defaults[id]);
        message(`Auto: ${SOURCES[m.detected + 1]} detected, its defaults loaded`);
      }
    } else appliedDetected = -1;
  }

  function sendConfig() {
    const c = audio.ctx;
    let text;
    if (audio.error) text = `Audio error: ${audio.error}`;
    else if (!audio.running) text = 'Click or tap anywhere to enable audio';
    else text = `Web · ${audio.mode} · ${(c.sampleRate / 1000).toFixed(1)} kHz · ~${(((c.baseLatency || 0) + (c.outputLatency || 0)) * 1000).toFixed(1)} ms`;
    toUi('config', { wrapper: 'Web', sampleRate: c ? c.sampleRate : 0, blockSize: 128, audio: text, running: audio.running, startPage: '' });
  }

  const started = audio.start().catch((e) => { audio.error = e.message || String(e); console.error(e); sendConfig(); });
  function unlockAudio() {
    if (audio.running || audio.error) return;
    started.then(() => audio.ctx && audio.ctx.resume()).then(sendConfig).catch(() => {});
  }
  document.addEventListener('pointerdown', unlockAudio, true);
  document.addEventListener('keydown', unlockAudio, true);
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', () => document.body.appendChild(fileInputAudio));
  else document.body.appendChild(fileInputAudio);

  // ----------------------------------------------------------------- MIDI ---
  if (typeof navigator.requestMIDIAccess === 'function') {
    navigator.requestMIDIAccess({ sysex: false }).then((access) => {
      const bind = () => { for (const input of access.inputs.values()) input.onmidimessage = (e) => audio.post({ type: 'midi', data: Array.from(e.data) }); };
      access.onstatechange = bind;
      bind();
      const n = Array.from(access.inputs.values()).length;
      if (n) message(`MIDI ready: ${n} input(s). Notes retrigger the LFO when Retrigger is set to MIDI Note.`);
    }).catch(() => {});
  }

  // -------------------------------------------------------------- presets ---
  const LS_KEY = 'wub.userPresets.v1';
  const local = {
    read() { try { return JSON.parse(store.get(LS_KEY)) || {}; } catch (e) { return {}; } },
    write(obj) { return store.set(LS_KEY, JSON.stringify(obj)); },
  };
  const localPresets = () => Object.keys(local.read()).sort((a, b) => a.localeCompare(b, undefined, { numeric: true }))
    .map(name => ({ id: `local:${name}`, name, category: 'User', factory: false }));

  const fsSupported = typeof window.showDirectoryPicker === 'function';
  const folder = { handle: null, granted: false, files: [], sig: '' };
  const idb = {
    open() {
      return new Promise((res, rej) => {
        const r = indexedDB.open('i-wub-you', 1);
        r.onupgradeneeded = () => r.result.createObjectStore('kv');
        r.onsuccess = () => res(r.result);
        r.onerror = () => rej(r.error);
      });
    },
    async get(key) {
      const db = await this.open();
      return new Promise((res) => { const r = db.transaction('kv').objectStore('kv').get(key); r.onsuccess = () => res(r.result); r.onerror = () => res(undefined); });
    },
    async set(key, val) {
      const db = await this.open();
      return new Promise((res) => { const tx = db.transaction('kv', 'readwrite'); tx.objectStore('kv').put(val, key); tx.oncomplete = () => res(true); tx.onerror = () => res(false); });
    },
  };

  async function scanFolder() {
    if (!folder.handle || !folder.granted) return false;
    const files = [];
    async function walk(dir, path) {
      for await (const [name, h] of dir.entries()) {
        const rel = path ? `${path}/${name}` : name;
        if (h.kind === 'directory') await walk(h, rel);
        else if (name.toLowerCase().endsWith(WUB_PRESET_EXT)) {
          const f = await h.getFile();
          files.push({ id: `user:${rel}`, name: name.slice(0, -WUB_PRESET_EXT.length), category: path || 'User',
                       factory: false, handle: h, parent: dir, fileName: name, stamp: `${f.size}:${f.lastModified}` });
        }
      }
    }
    try { await walk(folder.handle, ''); } catch (e) { folder.granted = false; return true; }
    files.sort((a, b) => a.category.localeCompare(b.category, undefined, { numeric: true }) || a.name.localeCompare(b.name, undefined, { numeric: true }));
    const sig = files.map(f => `${f.id}|${f.stamp}`).join(';');
    if (sig === folder.sig) return false;
    folder.sig = sig;
    folder.files = files;
    return true;
  }

  function allPresets() { return [...FACTORY, ...folder.files, ...localPresets()]; }
  function sendPresetList() {
    toUi('presetList', {
      presets: allPresets().map(({ id, name, category, factory }) => ({ id, name, category, factory })),
      folder: folder.granted ? `${folder.handle.name} (linked folder)` : 'this browser: click FOLDER to link a folder on disk',
    });
  }

  async function linkFolder(handle, ask) {
    folder.handle = handle;
    folder.sig = '';
    let state = await handle.queryPermission({ mode: 'readwrite' });
    if (state !== 'granted' && ask) state = await handle.requestPermission({ mode: 'readwrite' });
    folder.granted = state === 'granted';
    await idb.set('presetFolder', handle);
    if (folder.granted) await scanFolder();
    sendPresetList();
    return folder.granted;
  }
  if (fsSupported && window.indexedDB) {
    idb.get('presetFolder').then(async (h) => {
      if (h && !(await linkFolder(h, false))) message(`Click FOLDER to reconnect the preset folder "${h.name}"`);
    }).catch(() => {});
  }
  setInterval(async () => { if (await scanFolder()) sendPresetList(); }, 2000); // autoload, like the plugin

  // Same rules as PresetManager::readJson: migrate, then at least one known parameter.
  function parsePresetText(text, fallbackName) {
    let json;
    try { json = JSON.parse(text); } catch (e) { throw new Error(`Not a valid preset file: ${fallbackName}`); }
    if (!json || typeof json !== 'object') throw new Error(`Not an I Wub You preset: ${fallbackName}`);
    wubMigratePreset(json);
    const params = {};
    if (wubApplyParamObject(json.params, params) === 0) throw new Error(`No I Wub You settings found in ${fallbackName}`);
    return { name: String(json.name || '').trim() || fallbackName, category: String(json.category || ''), params };
  }
  function presetText(name, category) {
    const params = {};
    for (const d of PARAM_DEFS) {
      if (d.noPreset) continue;
      params[d.id] = +Number(values[d.id]).toPrecision(6);   // like PresetManager::toJson: 6 significant digits
    }
    return JSON.stringify({ format: WUB_PRESET_FORMAT, version: WUB_PRESET_VERSION, name, category, params }, null, 2);
  }
  const legalName = (s) => s.trim().replace(/[\\/:*?"<>|]/g, '_').slice(0, 60);

  async function loadPresetById(id) {
    const p = allPresets().find(x => x.id === id);
    if (!p) { message('Preset not found (was it moved or deleted?)'); toUi('preset', current); return; }
    try {
      const info = { id, name: p.name, category: p.category };
      if (p.factory) applyPreset(p.values, info);
      else {
        const text = id.startsWith('local:') ? local.read()[p.name] : await (await p.handle.getFile()).text();
        const parsed = parsePresetText(text, p.name);
        applyPreset(parsed.params, info);
      }
    } catch (e) { message(e.message || String(e)); toUi('preset', current); }
  }

  async function savePreset(rawName, overwrite) {
    const name = legalName(rawName);
    const result = (r) => toUi('saveResult', Object.assign({ name, ok: false, exists: false, error: '' }, r));
    if (!name) return result({ error: 'Enter a name for the preset' });
    try {
      if (folder.granted) {
        const fileName = name + WUB_PRESET_EXT;
        if (!overwrite) {
          let exists = true;
          try { await folder.handle.getFileHandle(fileName); } catch (e) { exists = false; }
          if (exists) return result({ exists: true });
        }
        const w = await (await folder.handle.getFileHandle(fileName, { create: true })).createWritable();
        await w.write(presetText(name, 'User'));
        await w.close();
        await scanFolder();
        current = { id: `user:${fileName}`, name, category: 'User' };
      } else {
        const all = local.read();
        if (!overwrite && all[name]) return result({ exists: true });
        all[name] = presetText(name, 'User');
        if (!local.write(all)) throw new Error('browser storage is unavailable (private window?)');
        current = { id: `local:${name}`, name, category: 'User' };
      }
      sendPresetList();
      toUi('preset', current);
      result({ ok: true });
    } catch (e) { result({ error: e.message || String(e) }); }
  }

  async function deletePreset(id) {
    const p = allPresets().find(x => x.id === id);
    try {
      if (!p || p.factory) throw new Error('Only user presets can be deleted');
      if (id.startsWith('local:')) { const all = local.read(); delete all[p.name]; local.write(all); }
      else { await p.parent.removeEntry(p.fileName); await scanFolder(); }
      if (current.id === id) current = { id: '', name: current.name, category: 'User' };
      message(`Deleted preset "${p.name}"`);
    } catch (e) { message(e.message || String(e)); }
    sendPresetList();
    toUi('preset', current);
  }

  const fileInput = document.createElement('input');
  fileInput.type = 'file';
  fileInput.accept = `${WUB_PRESET_EXT},application/json`;
  fileInput.style.display = 'none';
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', () => document.body.appendChild(fileInput));
  else document.body.appendChild(fileInput);   // dev.html loads this after the document is ready
  fileInput.addEventListener('change', async () => {
    const f = fileInput.files && fileInput.files[0];
    fileInput.value = '';
    if (!f) return;
    try {
      const p = parsePresetText(await f.text(), f.name.replace(/\.[^.]+$/, ''));
      applyPreset(p.params, { id: `file:${f.name}`, name: p.name, category: 'File' });
      message(`Loaded ${f.name}`);
    } catch (e) { message(e.message || String(e)); }
  });

  async function folderButton() {
    if (!fsSupported) { message('Linking a preset folder needs Chrome or Edge: presets are saved in this browser instead.'); return; }
    try {
      if (folder.handle && !folder.granted) { if (await linkFolder(folder.handle, true)) message(`Reconnected "${folder.handle.name}"`); return; }
      const handle = await window.showDirectoryPicker({ id: 'wub-presets', mode: 'readwrite', startIn: 'documents' });
      await linkFolder(handle, true);
      message(`Preset folder linked: ${handle.name}, ${folder.files.length} preset(s). Link Documents\\I Wub You\\Presets to share presets with the VST.`);
    } catch (e) { if (e && e.name !== 'AbortError') message(`Folder: ${e.message || e}`); }
  }

  // ------------------------------------------------- UI -> host events ---
  Object.assign(handlers, {
    uiReady() {
      sendConfig();
      sendPresetList();
      toUi('params', Object.assign({}, values));
      toUi('preset', current);
      toUi('inputs', inputList());
      toUi('stats', { peakIn: 0, peakOut: 0, lfo: 0, lfoR: 0, cutoff: [0, 0, 0], autoGain: 0, detected: -1, bpm, playing: true, note: -1, pitch: 0, pitchConf: 0, harm: 0, autoOut: 0, limiter: 0 });
    },
    paramMenu() {}, // no host automation menu in a browser
    setParam({ id, value }) {
      const d = PARAM_BY_ID[id];
      if (!d) return;
      let v = clamp(Number(value), d.min, d.max);
      if (d.step) v = Math.round(v / d.step) * d.step;
      values[id] = v;
      audio.post({ type: 'param', id, value: v });
    },
    gesture() {},
    retrig() { audio.post({ type: 'retrig' }); },
    sourceType({ type }) {
      appliedDetected = -1;   // Auto picked again: its next decision loads afresh
      const defaults = wubSourceDefaults(FACTORY_JSON, type);
      for (const id of Object.keys(defaults)) hostSetParam(id, defaults[id]);
    },
    selectInput({ id }) { if (!audio.running) message('Audio is off: click anywhere on the page once to enable it'); audio.selectInput(id, true); },
    setTempo({ bpm: b }) { bpm = clamp(Number(b) || 120, 20, 300); store.set('wub.bpm', String(bpm)); audio.post({ type: 'tempo', bpm }); },
    selectMidi() {},
    loadPreset({ id }) { loadPresetById(id); },
    savePreset({ name, overwrite }) { savePreset(String(name || ''), overwrite); },
    deletePreset({ id }) { deletePreset(id); },
    loadPresetFile() { fileInput.click(); },
    openPresetFolder() { folderButton(); },
  });

  // Test hook (tests/run-web-tests.ps1): a read-only view of the host state, a tap on what goes to
  // the DSP, and the MIDI entry point (as if a device sent the bytes).
  window.__wubWebHost = {
    values, factory: FACTORY, get current() { return current; }, get audioMode() { return audio.mode; }, get input() { return audio.input; },
    get bpm() { return bpm; },
    sent: [], tap: false,
    midi(bytes) { audio.post({ type: 'midi', data: Array.from(bytes) }); },
  };
  const post = audio.post.bind(audio);
  audio.post = (m) => { if (window.__wubWebHost.tap) window.__wubWebHost.sent.push(m); post(m); };
})();
