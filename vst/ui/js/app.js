// ============================================================================
//  UI wiring, shared by the plugin and the web build.
//   - talks only to JuceEngine (engine-juce.js). In the plugin that is JUCE's
//     native bridge to C++; in the web build vst/webhost/web-host.js provides
//     the same bridge on top of Web Audio + Web MIDI.
//   - the host owns all parameter state; automation and preset loads flow
//     back in as events
//   - the whole panel scales to fit the window
// ============================================================================
(function () {
  'use strict';

  const $ = (sel) => document.querySelector(sel);
  function el(tag, cls, html) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (html !== undefined) e.innerHTML = html;
    return e;
  }
  const store = {
    get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* storage unavailable */ } },
  };

  // ---------------------------------------------------------------- state ---
  const values = defaultParamValues();      // replaced by the plugin's state on connect
  let presetList = [];                      // [{id, name, category, factory}] factory first, then user
  let presetFolder = '';
  let current = { id: '', name: '—', category: '' };
  let presetDirty = false;
  let activeCat = null;

  const engine = new JuceEngine();
  const knobs = {};
  const bound = {};             // id -> [controls with setValue()]
  const dragging = new Set();   // params being dragged in the UI
  const lastLocal = {};         // id -> time of last UI-side change
  const refreshers = [];        // functions run after values change: (changedIds:Set|null) => void

  // ---------------------------------------------------- parameter plumbing ---
  function applyValue(id, v) {
    values[id] = v;
    if (knobs[id]) knobs[id].setValue(v);
    (bound[id] || []).forEach(c => c.setValue(v));
  }

  function refresh(ids) { refreshers.forEach(fn => fn(ids)); }

  // A change made in this UI
  function setParam(id, v) {
    if (!replaying && values[id] !== v && !PARAM_BY_ID[id].noPreset) remember(id);
    lastLocal[id] = performance.now();
    values[id] = v;
    if (knobs[id]) knobs[id].setValue(v);
    (bound[id] || []).forEach(c => c.setValue(v));
    engine.setParam(id, v);
    if (!PARAM_BY_ID[id].noPreset && !presetDirty) { presetDirty = true; renderPresetName(); }
    refresh(new Set([id]));
  }

  const gesture = {
    begin(id) { dragging.add(id); engine.beginGesture(id); },
    end(id) { dragging.delete(id); engine.endGesture(id); },
  };
  const gestureFn = (id, begin) => (begin ? gesture.begin(id) : gesture.end(id));

  function addKnob(parent, id, label) {
    const k = new Knob(PARAM_BY_ID[id], values[id], (v) => setParam(id, v), (begin) => gestureFn(id, begin), label);
    knobs[id] = k;
    k.el.dataset.param = id;
    parent.appendChild(k.el);
    return k;
  }

  // Bind a non-knob control (ToggleButton / Segmented / SelectBox) to a param
  function control(Cls, id, ...extra) {
    const c = new Cls(PARAM_BY_ID[id], values[id], (v) => setParam(id, v), ...extra);
    (bound[id] = bound[id] || []).push(c);
    c.el.dataset.param = id;
    return c;
  }

  // Changes coming from the plugin (automation, presets, state restore)
  engine.on('params', (obj) => {
    const now = performance.now();
    const changed = new Set();
    for (const id in obj) {
      if (!PARAM_BY_ID[id]) continue;
      // don't fight the user's hand: skip params being dragged or changed here in the last 300 ms
      if (dragging.has(id) || (lastLocal[id] !== undefined && now - lastLocal[id] < 300)) continue;
      applyValue(id, obj[id]);
      changed.add(id);
    }
    if (changed.size) refresh(changed);
  });

  // ------------------------------------------------------------ presets ----
  const esc = (s) => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  engine.on('presetList', ({ presets, folder }) => {
    presetList = presets;
    presetFolder = folder;
    if (!activeCat || !categories().includes(activeCat)) activeCat = current.category || (presets[0] && presets[0].category);
    renderBrowser();
  });

  engine.on('preset', (p) => {
    const changed = p.id !== current.id || p.name !== current.name;
    current = p;
    if (changed) presetDirty = false;
    if (p.category && categories().includes(p.category)) activeCat = p.category;
    renderPresetName();
    renderBrowser();
  });

  engine.on('message', ({ text }) => log(text));

  function categories() {
    const cats = [];
    for (const p of presetList) if (!cats.includes(p.category)) cats.push(p.category);
    if (!presetList.some(p => !p.factory) && !cats.includes('User')) cats.push('User');
    return cats;
  }

  function loadPreset(p) {
    remember('preset:' + p.id);
    presetDirty = false;
    current = { id: p.id, name: p.name, category: p.category };
    for (const id in lastLocal) delete lastLocal[id]; // let all incoming values through
    renderPresetName();
    renderBrowser();
    engine.loadPreset(p.id); // the plugin applies the values and sends them back
  }

  function stepPreset(dir) {
    if (!presetList.length) return;
    const i = presetList.findIndex(p => p.id === current.id);
    const next = i < 0 ? 0 : (i + dir + presetList.length) % presetList.length;
    loadPreset(presetList[next]);
  }

  function renderPresetName() {
    $('#preset-name').textContent = current.name + (presetDirty ? ' *' : '');
    $('#preset-cat').textContent = (current.category || 'Preset').toUpperCase();
  }

  const tabsEl = $('#cat-tabs');
  const gridEl = $('#preset-grid');
  function renderBrowser() {
    const cats = categories();
    tabsEl.innerHTML = '';
    for (const c of cats) {
      const t = el('button', 'cat-tab', esc(c));
      t.type = 'button';
      t.setAttribute('role', 'tab');
      t.setAttribute('aria-selected', c === activeCat);
      t.classList.toggle('active', c === activeCat);
      t.classList.toggle('has-current', c === current.category);
      t.addEventListener('click', () => { activeCat = c; renderBrowser(); });
      tabsEl.appendChild(t);
    }
    gridEl.innerHTML = '';
    const items = presetList.filter(p => p.category === activeCat);
    if (!items.length) {
      gridEl.appendChild(el('div', 'grid-empty',
        `No user presets yet. Click <b>SAVE</b> to store the current sound. Files in <span class="path">${esc(presetFolder)}</span> show up here automatically.`));
      return;
    }
    for (const p of items) {
      const b = el('button', 'preset-btn', `<span class="pb-name">${esc(p.name)}</span>`);
      b.type = 'button';
      b.title = p.name;
      b.classList.toggle('selected', p.id === current.id);
      b.addEventListener('click', () => loadPreset(p));
      if (!p.factory) {
        const del = el('span', 'pb-del', '×');
        del.title = 'Delete preset';
        del.addEventListener('click', (e) => {
          e.stopPropagation();
          openModal({
            title: 'Delete preset', body: `Move <b>${esc(p.name)}</b> to the Recycle Bin?`,
            ok: 'DELETE', danger: true, onOk: () => engine.deletePreset(p.id),
          });
        });
        b.appendChild(del);
      }
      gridEl.appendChild(b);
    }
  }

  function savePrompt() {
    openModal({
      title: 'Save preset',
      body: `Saved to <span class="path">${esc(presetFolder)}</span>`,
      input: current.category === 'User' || !current.id.startsWith('factory:') ? current.name : `${current.name} (copy)`,
      ok: 'SAVE',
      onOk: (name) => { if (name.trim()) engine.savePreset(name.trim(), false); },
    });
  }
  engine.on('saveResult', ({ ok, exists, name, error }) => {
    if (ok) { presetDirty = false; renderPresetName(); log(`Saved preset "${name}"`); }
    else if (exists) {
      openModal({
        title: 'Replace preset?', body: `A preset named <b>${esc(name)}</b> already exists. Replace it?`,
        ok: 'REPLACE', danger: true, onOk: () => engine.savePreset(name, true),
      });
    } else log(`Save failed: ${error}`);
  });

  $('#preset-prev').addEventListener('click', () => stepPreset(-1));
  $('#preset-next').addEventListener('click', () => stepPreset(1));
  $('#preset-save').addEventListener('click', savePrompt);
  $('#preset-load').addEventListener('click', () => engine.loadPresetFile());
  $('#preset-folder').addEventListener('click', () => engine.openPresetFolder());

  // ------------------------------------------------------------- modal -----
  const modal = $('#modal'), modalInput = $('#modal-input'), modalOk = $('#modal-ok');
  let modalOnOk = null;
  function openModal({ title, body, input, ok, danger, onOk }) {
    $('#modal-title').textContent = title;
    $('#modal-body').innerHTML = body || '';
    modalOk.textContent = ok || 'OK';
    modalOk.classList.toggle('danger', !!danger);
    modalInput.style.display = input === undefined ? 'none' : '';
    modalInput.value = input || '';
    modalOnOk = onOk;
    modal.classList.remove('hidden');
    if (input !== undefined) { modalInput.focus(); modalInput.select(); } else modalOk.focus();
  }
  function closeModal(accept) {
    if (modal.classList.contains('hidden')) return;
    modal.classList.add('hidden');
    const fn = modalOnOk;
    modalOnOk = null;
    if (accept && fn) fn(modalInput.value);
  }
  modalOk.addEventListener('click', () => closeModal(true));
  $('#modal-cancel').addEventListener('click', () => closeModal(false));
  modal.addEventListener('pointerdown', (e) => { if (e.target === modal) closeModal(false); });
  modal.addEventListener('keydown', (e) => {
    e.stopPropagation();
    if (e.key === 'Enter') { e.preventDefault(); closeModal(true); }
    else if (e.key === 'Escape') { e.preventDefault(); closeModal(false); }
  });
  modal.addEventListener('keyup', (e) => e.stopPropagation());

  // ------------------------------------------------------------- pages -----
  const PAGE_HINTS = {
    wobble: 'The LFO sweeps the filter between Cutoff Low and High · Sync locks it to the host\'s beat · Resonance Limit tames loud input',
    bands: 'Single band, or a Linkwitz-Riley split: wobble and drive each band on its own · hats can stay crisp while the lows wub',
    source: 'Pick what you are feeding in, or let Auto listen · the type loads a sensible cutoff range, drive and crossover · harmonics and a sub for thin sources · transients kept',
    output: 'Stereo, mono-summed or mid/side · keep the lows mono · duck with the LFO · dry/wet and output trim · Auto Out matches the level, the true-peak limiter catches the rest',
  };
  let activePage = location.hash.slice(1) || store.get('wub.page') || 'wobble'; // #bands opens a page directly (tests)
  function showPage(name) {
    if (!PAGE_HINTS[name]) name = 'wobble';
    activePage = name;
    store.set('wub.page', name);
    document.querySelectorAll('.page-tab').forEach(t => t.classList.toggle('active', t.dataset.page === name));
    document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.dataset.page === name));
    $('#page-hint').textContent = PAGE_HINTS[name];
    refresh(null);
  }
  document.querySelectorAll('.page-tab').forEach(t => t.addEventListener('click', () => showPage(t.dataset.page)));

  function makePanel(title, extraHeader, phase) {
    const p = el('div', 'panel');
    const h = el('div', 'panel-head', `<span class="panel-title">${title}</span>`);
    if (phase) h.appendChild(el('span', 'phase-tag', `PHASE ${phase}`));
    if (extraHeader) (Array.isArray(extraHeader) ? extraHeader : [extraHeader]).forEach(x => h.appendChild(x.el || x));
    p.appendChild(h);
    return p;
  }
  function knobRow(parent, ids, cls) {
    const row = el('div', 'knob-row' + (cls ? ' ' + cls : ''));
    ids.forEach(id => (Array.isArray(id) ? addKnob(row, id[0], id[1]) : addKnob(row, id)));
    parent.appendChild(row);
    return row;
  }
  function field(label, ctl) {
    const f = el('div', 'field', `<span class="ctl-label">${label}</span>`);
    f.appendChild(ctl.el);
    return f;
  }
  function headGroup(...items) {   // a label and a control side by side, right-aligned in a panel head
    const g = el('div', 'head-group');
    items.forEach(x => g.appendChild(x.el || x));
    return g;
  }
  function fields(parent, list, three) {
    const g = el('div', 'field-grid' + (three ? ' three' : ''));
    list.forEach(([label, ctl]) => g.appendChild(field(label, ctl)));
    parent.appendChild(g);
    return g;
  }
  function onOff(panel, id) {   // panel dims while its switch is off
    refreshers.push((ids) => { if (!ids || ids.has(id)) panel.classList.toggle('off', values[id] < 0.5); });
  }

  // =============================================================== scope ====
  const scope = new Scope($('#scope'));
  window.__wubScope = scope;   // the web tests read its frame counter
  const scopeIds = ['cutoffLo', 'cutoffHi', 'bands', 'loWob', 'midWob', 'hiWob', 'filterType', 'xLow', 'xHigh'];
  refreshers.push((ids) => {
    if (ids && !scopeIds.some(id => ids.has(id))) return;
    scope.setRange({ lo: values.cutoffLo, hi: values.cutoffHi, bands: Math.round(values.bands), wob: [values.loWob >= 0.5, values.midWob >= 0.5, values.hiWob >= 0.5],
                     type: Math.round(values.filterType), xLow: values.xLow, xHigh: values.xHigh });
  });
  document.addEventListener('visibilitychange', () => scope.setVisible(!document.hidden));

  // ========================================================= WOBBLE page ====
  {
    const row = $('#wobble-row');
    {
      const sync = control(ToggleButton, 'lfoSync', 'SYNC');
      const p = makePanel('LFO', [headGroup(control(SelectBox, 'lfoRetrig', ['Retrig: Off', 'Retrig: MIDI Note']), sync)]);
      p.appendChild(control(Segmented, 'lfoShape', ['Sine', 'Tri', 'Saw ↓', 'Saw ↑', 'Square', 'S&H', 'Random']).el);
      const rateRow = el('div', 'knob-row');
      addKnob(rateRow, 'lfoRate');
      const div = control(Segmented, 'lfoDiv');
      div.el.classList.add('div-seg');
      const divField = field('Division (synced)', div);
      rateRow.appendChild(divField);
      addKnob(rateRow, 'lfoPhase');
      addKnob(rateRow, 'lfoStereo');
      addKnob(rateRow, 'lfoSmooth');
      p.appendChild(rateRow);
      p.appendChild(el('div', 'panel-note', '<b>Sync</b>: the division of the host tempo, phase-locked to the bar while the transport runs. <b>Stereo</b> offsets the right channel\'s LFO. <b>Smooth</b> rounds square / S&H edges.'));
      refreshers.push((ids) => {
        if (ids && !ids.has('lfoSync')) return;
        const s = values.lfoSync >= 0.5;
        knobs.lfoRate.el.style.opacity = s ? 0.35 : 1;
        divField.style.opacity = s ? 1 : 0.35;
      });
      row.appendChild(p);
    }
    {
      const p = makePanel('WOBBLE FILTER', control(Segmented, 'drivePos', ['Drive → Filter', 'Filter → Drive']));
      p.appendChild(control(Segmented, 'filterType', ['LP', 'BP', 'HP', 'Notch', 'Formant']).el);
      knobRow(p, ['cutoffLo', 'cutoffHi', 'reso', 'resoLimit', 'keyTrack']);
      p.appendChild(el('div', 'panel-note', 'The LFO sweeps the cutoff from <b>Low</b> to <b>High</b> (Formant: from vowel A to U). <b>Reso Limit</b> pulls the resonance down as the band gets loud, so transients don\'t blow up. <b>Key Track</b> shifts the whole range with the input\'s pitch (a held MIDI note wins): at 100% a note an octave above A2 sweeps an octave higher.'));
      row.appendChild(p);
    }
    refreshers.push(() => $('#wobble-led').classList.toggle('on', values.loWob >= 0.5 || values.midWob >= 0.5 || values.hiWob >= 0.5));
  }

  // ========================================================== BANDS page ====
  const strips = [];
  {
    const r1 = $('#bands-row-1'), r2 = $('#bands-row-2');
    {
      const p = makePanel('SPLIT', [headGroup(el('span', 'ctl-label', 'OVERSAMPLING'), control(Segmented, 'os'))]);
      const rowEl = el('div', 'knob-row');
      const bandsSeg = control(Segmented, 'bands');
      rowEl.appendChild(field('Bands', bandsSeg));
      addKnob(rowEl, 'xLow');
      addKnob(rowEl, 'xHigh');
      p.appendChild(rowEl);
      p.appendChild(el('div', 'panel-note', 'Linkwitz-Riley 4th order: the bands sum back flat. In <b>Single</b> mode the whole signal goes through the middle strip. <b>Oversampling</b> runs the drive at 2x / 4x (the plugin reports the few samples of latency to the host).'));
      r1.appendChild(p);
    }
    const RANGES = [() => `20 Hz – ${formatParam(PARAM_BY_ID.xLow, values.xLow)}`, () => `${formatParam(PARAM_BY_ID.xLow, values.xLow)} – ${formatParam(PARAM_BY_ID.xHigh, values.xHigh)}`, () => `${formatParam(PARAM_BY_ID.xHigh, values.xHigh)} – 20 kHz`];
    for (let b = 0; b < 3; b++) {
      const wob = control(ToggleButton, bandId(b, 'wob'), 'WOBBLE');
      wob.el.classList.add('wob');
      const title = el('span', 'strip-title', BAND_NAMES[b].toUpperCase());
      const range = el('span', 'strip-range', '');
      const p = el('div', 'panel strip');
      const head = el('div', 'panel-head');
      head.appendChild(title); head.appendChild(range); head.appendChild(wob.el);
      p.appendChild(head);
      knobRow(p, [bandId(b, 'depth'), bandId(b, 'drive'), bandId(b, 'tone'), bandId(b, 'level')]);
      r2.appendChild(p);
      strips.push({ p, title, range, b });
    }
    refreshers.push((ids) => {
      const three = Math.round(values.bands) === 1;
      for (const s of strips) {
        const active = three || s.b === 1;
        s.p.classList.toggle('inactive', !active);
        s.title.textContent = three ? BAND_NAMES[s.b].toUpperCase() : (s.b === 1 ? 'FULL SIGNAL' : BAND_NAMES[s.b].toUpperCase());
        s.range.textContent = three ? RANGES[s.b]() : (s.b === 1 ? '20 Hz – 20 kHz' : 'off in Single mode');
        s.p.classList.toggle('wobbling', active && values[bandId(s.b, 'wob')] >= 0.5);
      }
      $('#bands-led').classList.toggle('on', three);
    });
  }

  // ========================================================= SOURCE page ====
  let detectedEl = null;
  {
    const row = $('#source-row');
    {
      const p = makePanel('INPUT', control(ToggleButton, 'autoIn', 'AUTO GAIN'));
      knobRow(p, ['inGain']);
      p.appendChild(el('div', 'detected', 'AUTO GAIN <b id="autogain-readout">0.0 dB</b>'));
      p.appendChild(el('div', 'detected pitch-read', 'PITCH <b id="pitch-readout">—</b> <span class="mini-meter" title="Pitch confidence"><div id="pitch-conf"></div></span>'));
      p.appendChild(el('div', 'panel-note', 'A DC blocker, then the input gain. <b>Auto Gain</b> tracks the RMS level and brings any source to about -18 dBFS (±24 dB, 0.4 s), so quiet and loud material hit the drive and the filter the same way. The pitch (YIN, 30 Hz .. 1.5 kHz) feeds Key Track, the tracked harmonics and the Auto type.'));
      row.appendChild(p);
    }
    {
      // picking a type asks the host to load its defaults too
      const type = new SelectBox(PARAM_BY_ID.source, values.source, (v) => {
        lastLocal.source = performance.now(); applyValue('source', v); engine.sourceType(v);
        presetDirty = true; renderPresetName(); refresh(new Set(['source']));
      });
      type.el.dataset.param = 'source';
      (bound.source = bound.source || []).push(type);
      const p = makePanel('SOURCE TYPE', type);
      detectedEl = el('div', 'detected', 'DETECTED <b>—</b>');
      p.appendChild(detectedEl);
      const seg = el('div', 'seg', '');
      SOURCES.slice(1).forEach((name, i) => {
        const b = el('button', '', name);
        b.type = 'button';
        b.addEventListener('click', () => type.onChange(i + 1));
        seg.appendChild(b);
      });
      seg.dataset.param = 'source';
      p.appendChild(seg);
      refreshers.push((ids) => {
        if (ids && !ids.has('source')) return;
        const v = Math.round(values.source);
        seg.querySelectorAll('button').forEach((b, i) => b.classList.toggle('on', i + 1 === v));
      });
      p.appendChild(el('div', 'panel-note', 'Picking a type loads its defaults: cutoff range, resonance, drive, bands and crossover, mono-below. Everything stays editable. <b>Auto</b> listens (pitch and its confidence, the envelope\'s variation, the RMS frequency) and loads the type it hears, once it has held for a second and a half.'));
      row.appendChild(p);
    }
    {
      const p = makePanel('HARMONICS · TRANSIENTS', control(ToggleButton, 'harmOn', 'HARMONICS'));
      fields(p, [['Mode', control(Segmented, 'harmMode')], ['Shape', control(Segmented, 'harmShape')]]);
      const rowEl = el('div', 'knob-row');
      addKnob(rowEl, 'harmAmount');
      const sub = control(ToggleButton, 'subOn', 'SUB');
      addKnob(rowEl, 'subAmount');
      addKnob(rowEl, 'transPreserve');
      p.appendChild(rowEl);
      const sw = el('div', 'switch-row left');
      sw.appendChild(sub.el);
      sw.appendChild(el('div', 'detected', 'ADDING <b id="harm-readout">0%</b>'));
      p.appendChild(sw);
      p.appendChild(el('div', 'panel-note', 'Thin sources (a sine bass) get harmonics: the <b>Shaper</b>\'s clip and rectifier residue, or a saw / square locked to the tracked pitch; <b>Auto</b> adds them only as far as the source is thin, <b>Always</b> adds Amount. <b>Sub</b> is an octave below the pitch. <b>Transients</b> lets attacks pass partly unfiltered so drums keep their punch.'));
      row.appendChild(p);
    }
    refreshers.push(() => $('#source-led').classList.toggle('on', values.autoIn >= 0.5));
  }

  // ========================================================= OUTPUT page ====
  let meterIn = null, meterOut = null;
  {
    const row = $('#output-row');
    {
      const p = makePanel('STEREO');
      p.appendChild(control(Segmented, 'stereoMode').el);
      knobRow(p, ['monoBelow']);
      p.appendChild(el('div', 'panel-note', '<b>Mid-Side</b> wobbles the centre and the sides as the two channels. <b>Mono Below</b> high-passes the side signal (LR4), keeping the lows in the middle.'));
      row.appendChild(p);
    }
    {
      const p = makePanel('DUCK · MIX');
      knobRow(p, ['duck', 'mix', 'outGain']);
      p.appendChild(el('div', 'panel-note', '<b>Duck</b> lowers the level as the LFO falls (1 - Duck x (1 - LFO)): the pump of a sidechain without one. The dry signal is delayed to line up with the oversampled wet path.'));
      row.appendChild(p);
    }
    {
      const p = makePanel('SAFETY', [control(ToggleButton, 'autoOut', 'AUTO OUT'), control(ToggleButton, 'limiter', 'LIMITER')]);
      const meters = el('div', 'meter-row');
      meterIn = new Meter('IN'); meterOut = new Meter('OUT');
      meters.appendChild(meterIn.el); meters.appendChild(meterOut.el);
      p.appendChild(meters);
      const reads = el('div', 'switch-row');
      reads.appendChild(el('div', 'detected', 'AUTO OUT <b id="autoout-readout">0.0 dB</b>'));
      reads.appendChild(el('div', 'detected', 'LIMITER <b id="limiter-readout">0.0 dB</b>'));
      p.appendChild(reads);
      p.appendChild(el('div', 'panel-note', '<b>Auto Out</b> matches the wet level to the input\'s (±12 dB, over a second, ducking excluded), so resonance and drive don\'t change the loudness. The <b>Limiter</b> is true-peak (4x oversampled) with 1 ms lookahead at -0.3 dBFS; its lookahead is reported to the host as latency. A safety clipper (linear below -1.9 dBFS) stays on behind it.'));
      row.appendChild(p);
    }
    refreshers.push(() => $('#output-led').classList.toggle('on', values.duck > 0.001 || Math.round(values.stereoMode) !== 0 || values.monoBelow >= 1));
  }

  // ------------------------------------------------- undo / redo, A/B --------
  // Snapshots of every preset parameter, taken before a change made in this UI (a drag, a
  // wheel run or repeated clicks on one control within 0.8 s count as one step) and before
  // a preset load. Host automation is not recorded. Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z).
  var replaying = false;
  const hist = { undo: [], redo: [], lastId: null, lastTime: 0 };
  function snapshot() {
    const o = {};
    PARAM_DEFS.forEach(d => { if (!d.noPreset) o[d.id] = values[d.id]; });
    return o;
  }
  function remember(id) {
    const now = performance.now();
    if (id && id === hist.lastId && now - hist.lastTime < 800) { hist.lastTime = now; return; }
    hist.undo.push(snapshot());
    if (hist.undo.length > 100) hist.undo.shift();
    hist.redo.length = 0;
    hist.lastId = id; hist.lastTime = now;
    renderHistory();
  }
  function restore(snap) {
    replaying = true;
    try { for (const id in snap) if (values[id] !== snap[id]) setParam(id, snap[id]); }
    finally { replaying = false; }
    hist.lastId = null;
    refresh(null);
  }
  function undo() {
    if (!hist.undo.length) return;
    hist.redo.push(snapshot());
    restore(hist.undo.pop());
    renderHistory();
    log('Undo');
  }
  function redo() {
    if (!hist.redo.length) return;
    hist.undo.push(snapshot());
    restore(hist.redo.pop());
    renderHistory();
    log('Redo');
  }
  function renderHistory() {
    $('#undo-btn').disabled = !hist.undo.length;
    $('#redo-btn').disabled = !hist.redo.length;
  }
  $('#undo-btn').addEventListener('click', undo);
  $('#redo-btn').addEventListener('click', redo);
  window.addEventListener('keydown', (e) => {
    if (!(e.ctrlKey || e.metaKey) || /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName)) return;
    const k = e.key.toLowerCase();
    if (k === 'z' && !e.shiftKey) { e.preventDefault(); undo(); }
    else if (k === 'y' || (k === 'z' && e.shiftKey)) { e.preventDefault(); redo(); }
  });

  // A/B compare: two complete settings; switching stores the current one in its slot.
  const ab = { active: 'A', A: null, B: null };
  function abSwitch(slot) {
    if (slot === ab.active) return;
    ab[ab.active] = snapshot();
    if (!ab[slot]) ab[slot] = snapshot();
    remember('ab');
    restore(ab[slot]);
    ab.active = slot;
    renderAb();
    log(`Comparing: ${slot}`);
  }
  function renderAb() {
    $('#ab-a').classList.toggle('on', ab.active === 'A');
    $('#ab-b').classList.toggle('on', ab.active === 'B');
    $('#ab-copy').textContent = ab.active === 'A' ? 'A→B' : 'B→A';
  }
  $('#ab-a').addEventListener('click', () => abSwitch('A'));
  $('#ab-b').addEventListener('click', () => abSwitch('B'));
  $('#ab-copy').addEventListener('click', () => {
    const other = ab.active === 'A' ? 'B' : 'A';
    ab[other] = snapshot();
    log(`Copied ${ab.active} to ${other}`);
  });

  // ---------------------------------------------------------- status / stats ---
  const monitor = $('#monitor');
  function log(text) { monitor.textContent = text; }
  $('#retrig-btn').addEventListener('click', () => { engine.retrig(); log('LFO restarted'); });

  const midiLed = $('#midi-led'), playLed = $('#play-led'), bpmEl = $('#bpm-readout');
  let lastNote = -1, limHold = 0;
  engine.on('stats', (s) => {
    scope.push(s);
    if (meterIn) meterIn.setLevel(s.peakIn || 0);
    if (meterOut) meterOut.setLevel(s.peakOut || 0);
    const ag = $('#autogain-readout');
    if (ag && typeof s.autoGain === 'number') ag.textContent = `${s.autoGain > 0.05 ? '+' : ''}${s.autoGain.toFixed(1)} dB`;
    if (detectedEl) detectedEl.innerHTML = `DETECTED <b>${s.detected >= 0 ? esc(SOURCES[s.detected + 1] || '—') : '—'}</b>${Math.round(values.source) === 0 && s.detected >= 0 ? ' · loaded' : ''}`;
    const pr = $('#pitch-readout'), pc = $('#pitch-conf');
    if (pr && typeof s.pitch === 'number') {
      const conf = clamp(s.pitchConf || 0, 0, 1);
      pr.textContent = s.pitch > 0 && conf > 0.3 ? `${midiNoteName(Math.round(69 + 12 * Math.log2(s.pitch / 440)))} · ${s.pitch.toFixed(s.pitch < 100 ? 1 : 0)} Hz` : '—';
      if (pc) pc.style.width = `${conf * 100}%`;
    }
    const hr = $('#harm-readout');
    if (hr && typeof s.harm === 'number') hr.textContent = `${Math.round(s.harm * 100)}%`;
    const ao = $('#autoout-readout'), lr = $('#limiter-readout');
    if (ao && typeof s.autoOut === 'number') ao.textContent = values.autoOut >= 0.5 ? `${s.autoOut > 0.05 ? '+' : ''}${s.autoOut.toFixed(1)} dB` : 'off';
    if (lr && typeof s.limiter === 'number') {
      limHold = Math.min(limHold * 0.85, s.limiter);   // show the deepest reduction, falling back
      lr.textContent = values.limiter >= 0.5 ? `${limHold.toFixed(1)} dB` : 'off';
      lr.classList.toggle('hot', limHold < -0.5);
    }
    if (typeof s.bpm === 'number') bpmEl.textContent = `${s.bpm.toFixed(s.bpm === Math.round(s.bpm) ? 0 : 1)} BPM`;
    playLed.classList.toggle('on', !!s.playing);
    const note = typeof s.note === 'number' ? s.note : -1;
    midiLed.classList.toggle('on', note >= 0);
    if (note >= 0 && note !== lastNote) log(`MIDI note ${midiNoteName(note)}${Math.round(values.lfoRetrig) === 1 ? ' (LFO retriggered)' : ''}`);
    lastNote = note;
  });

  let connectedOnce = false;
  engine.on('config', ({ sampleRate, blockSize, wrapper, audio, running, startPage }) => {
    if (startPage && PAGE_HINTS[startPage] && !connectedOnce) showPage(startPage);
    const web = wrapper === 'Web';
    document.body.classList.toggle('web', web);
    const sr = sampleRate > 0 ? `${(sampleRate / 1000).toFixed(1)} kHz · ${blockSize} smp` : 'not playing yet';
    $('#audio-info').textContent = audio || `${wrapper} · ${sr}`;
    $('#audio-led').classList.toggle('on', running !== false);
    if (!connectedOnce) log(web ? 'Connected: pick an INPUT at the top right (a test tone, the microphone, a browser tab or a file)' : 'Connected: feed the plugin some audio and it wubs');
    connectedOnce = true;
    fit();
  });

  // ---- input source + tempo (web build) -----------------------------------------
  const inputSelect = $('#input-select');
  engine.on('inputs', ({ inputs, selected }) => {
    inputSelect.innerHTML = '';
    inputs.forEach(d => inputSelect.appendChild(new Option(d.name, d.id)));
    inputSelect.value = Array.from(inputSelect.options).some(o => o.value === selected) ? selected : inputs[0].id;
  });
  inputSelect.addEventListener('change', () => engine.selectInput(inputSelect.value));
  const tempoInput = $('#tempo-input');
  tempoInput.addEventListener('change', () => { const b = clamp(Number(tempoInput.value) || 120, 20, 300); tempoInput.value = b; engine.setTempo(b); });
  tempoInput.addEventListener('keydown', (e) => { e.stopPropagation(); if (e.key === 'Enter') tempoInput.blur(); });

  // --------------------------------------------------------- fit window ----
  const synth = $('.synth');
  function fit() {
    const w = synth.offsetWidth + 24, h = synth.offsetHeight + 24;
    const s = Math.min(window.innerWidth / w, window.innerHeight / h);
    synth.style.transform = `translateX(-50%) scale(${s})`;
  }
  window.addEventListener('resize', fit);

  // Right-click on any control = the host's parameter menu (automation, link to controller...).
  // Elsewhere no browser menu; Shift+right-click still opens it (Inspect).
  document.addEventListener('contextmenu', (e) => {
    if (!engine.connected || e.shiftKey) return;
    e.preventDefault();
    const ctl = e.target.closest('[data-param]');
    if (ctl) engine.paramMenu(ctl.dataset.param, e.clientX, e.clientY);
  });

  // -------------------------------------------------------------- init -----
  renderHistory();
  renderAb();
  renderPresetName();
  renderBrowser();
  showPage(activePage);
  refresh(null);
  fit();
  if (engine.connected) engine.uiReady();
  else log('Not running inside the plugin: no audio engine connected (UI preview only)');
})();
