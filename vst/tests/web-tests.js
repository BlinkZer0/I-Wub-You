// ============================================================================
//  Headless tests of the web build (run by tests/run-web-tests.ps1, which appends
//  this script to a copy of web/index.html and loads it in headless Edge, once
//  from disk and once served over http). They drive the real UI and check what
//  reaches the web host and the DSP (window.__wubWebHost taps the messages):
//  the bridge, presets, source-type defaults, tempo, MIDI retrigger, saving and
//  reloading a preset in this browser, and (served) the AudioWorklet playing
//  with the LFO moving and the scope drawing.
//  Results: JSON in a pre element (id wub-test-out); the title becomes "done".
//  Served over http they are also POSTed to the test server.
// ============================================================================
(function () {
  'use strict';
  const results = [];
  const check = (name, ok, detail) => results.push({ name, ok: !!ok, detail: detail === undefined ? '' : String(detail) });
  const sleep = (ms) => new Promise(r => setTimeout(r, ms));
  const $ = (s) => document.querySelector(s);
  const $$ = (s) => Array.from(document.querySelectorAll(s));
  const host = window.__wubWebHost;
  const sent = (type) => host.sent.filter(m => m.type === type);
  const lastSent = (type) => sent(type).slice(-1)[0];
  const clearSent = () => { host.sent.length = 0; };
  const bridge = window.__JUCE__.backend;
  const got = {};
  for (const ev of ['params', 'preset', 'presetList', 'stats', 'message', 'saveResult', 'inputs', 'config'])
    bridge.addEventListener(ev, (d) => { (got[ev] = got[ev] || []).push(d); });
  const last = (ev) => (got[ev] || []).slice(-1)[0];
  const detectedEl = () => { const e = $$('.detected').find(x => /DETECTED/.test(x.textContent)); return e ? e.textContent : ''; };

  async function run() {
    await sleep(300);
    for (let i = 0; i < 100 && !host.audioMode; i++) await sleep(50);   // the AudioWorklet module loads asynchronously
    host.tap = true;
    // the start-up events went out before this script's listeners existed: ask for them again
    bridge.emitEvent('uiReady');
    await sleep(50);

    // ---- start-up ------------------------------------------------------------
    check('no script errors during start-up', window.__wubErrors.length === 0, window.__wubErrors.join(' | '));
    check('bridge connected (config received, web mode)', document.body.classList.contains('web'), document.body.className);
    check('audio engine built (ScriptProcessor on file://, AudioWorklet served)', host.audioMode === 'ScriptProcessor' || host.audioMode === 'AudioWorklet', host.audioMode || 'none');
    check('96 factory presets in 6 categories', host.factory.length === 96 && new Set(host.factory.map(p => p.category)).size === 6, `${host.factory.length} presets`);
    check('starts on the first factory preset', host.current.id === 'factory:0' && $('#preset-name').textContent.startsWith(host.factory[0].name), $('#preset-name').textContent);
    check('the browser lists the presets (16 per category, all visible)', $$('#cat-tabs .cat-tab').length === 7 && last('presetList') && last('presetList').presets.length === 96
          && $$('#preset-grid .preset-btn').length === 16 && $('#preset-grid').scrollHeight <= $('#preset-grid').clientHeight + 1,
          `${$$('#cat-tabs .cat-tab').length} tabs, ${$$('#preset-grid .preset-btn').length} buttons, grid ${$('#preset-grid').scrollHeight}/${$('#preset-grid').clientHeight}`);
    const noControl = PARAM_DEFS.filter(d => !d.noPreset && !$(`[data-param="${d.id}"]`)).map(d => d.id);
    check('every parameter has a control', noControl.length === 0, noControl.join(', '));
    check('the input picker lists the built-in sources and the live inputs', last('inputs') && last('inputs').inputs.length === 8 && $('#input-select').options.length === 8,
          last('inputs') && last('inputs').inputs.map(i => i.id).join(','));

    // ---- a control reaches the DSP --------------------------------------------
    clearSent();
    const knob = $('.knob[data-param="reso"] svg');
    knob.dispatchEvent(new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }));
    check('resonance knob wheel -> param message to the engine', lastSent('param') && lastSent('param').id === 'reso' && lastSent('param').value > 0.5,
          JSON.stringify(lastSent('param')));
    clearSent();
    $('.seg[data-param="filterType"] button:nth-child(5)').click();
    check('filter type segment -> Formant (4)', lastSent('param') && lastSent('param').id === 'filterType' && lastSent('param').value === 4 && host.values.filterType === 4,
          JSON.stringify(lastSent('param')));

    // ---- presets ---------------------------------------------------------------
    clearSent();
    const breaks = host.factory.find(p => p.name === 'Wobbly Breaks');
    bridge.emitEvent('loadPreset', { id: breaks.id });
    await sleep(50);
    check('loading a factory preset sends every parameter to the engine and the UI',
          lastSent('params') && Object.keys(lastSent('params').values).length === PARAM_DEFS.filter(d => !d.noPreset).length
          && host.current.name === 'Wobbly Breaks' && Math.round(host.values.bands) === 1 && Math.round(host.values.source) === 4 && last('preset').name === 'Wobbly Breaks',
          `${host.current.name}, bands ${host.values.bands}, source ${host.values.source}`);
    check('the preset display follows', $('#preset-name').textContent.startsWith('Wobbly Breaks'), $('#preset-name').textContent);
    check('the band strips show the 3-band ranges', $$('.panel.strip').length === 3 && !$$('.panel.strip')[0].classList.contains('inactive') && $$('.strip-range')[0].textContent.includes('180'),
          $$('.strip-range').map(e => e.textContent).join(' | '));

    // picking a source type loads its template defaults (and leaves the preset's own output trim alone)
    clearSent();
    const outBefore = host.values.outGain;
    bridge.emitEvent('sourceType', { type: 1 });
    await sleep(30);
    check('picking Bass loads its defaults (single band, 60 Hz .. 2.5 kHz, drive 45%, mono below 100)',
          Math.round(host.values.source) === 1 && Math.round(host.values.bands) === 0 && host.values.cutoffLo === 60 && host.values.cutoffHi === 2500
          && Math.abs(host.values.midDrive - 0.45) < 1e-9 && host.values.monoBelow === 100 && sent('param').length >= 5,
          `source ${host.values.source}, bands ${host.values.bands}, cutoff ${host.values.cutoffLo}-${host.values.cutoffHi}, drive ${host.values.midDrive}`);
    check('..and leaves the output alone (mix, out gain)', host.values.mix === 1 && host.values.outGain === outBefore, `mix ${host.values.mix}, out ${host.values.outGain}`);
    check('..the UI follows (strips collapse to FULL SIGNAL)', $$('.strip-title')[1].textContent === 'FULL SIGNAL' && $$('.panel.strip')[0].classList.contains('inactive'),
          $$('.strip-title').map(e => e.textContent).join(' | '));

    // ---- tempo and MIDI -----------------------------------------------------------
    clearSent();
    bridge.emitEvent('setTempo', { bpm: 140 });
    check('BPM field -> tempo message', lastSent('tempo') && lastSent('tempo').bpm === 140 && host.bpm === 140, JSON.stringify(lastSent('tempo')));
    clearSent();
    bridge.emitEvent('setParam', { id: 'lfoRetrig', value: 1 });
    host.midi([0x90, 48, 100]);
    host.midi([0x80, 48, 0]);
    check('MIDI notes reach the engine as raw MIDI', sent('midi').length === 2 && sent('midi')[0].data[1] === 48, JSON.stringify(sent('midi')));
    clearSent();
    $('#retrig-btn').click();
    check('RETRIG button -> retrig message', sent('retrig').length === 1, JSON.stringify(host.sent));

    // ---- the input picker ----------------------------------------------------------
    clearSent();
    bridge.emitEvent('selectInput', { id: 'drums' });
    await sleep(30);
    check('a built-in source is picked (drum loop: generator 6 inside the engine)', host.input === 'drums' && lastSent('source') && lastSent('source').gen === 6,
          JSON.stringify(lastSent('source')));

    // ---- undo ---------------------------------------------------------------------
    await sleep(400);   // let the UI's 300 ms "don't fight the user's hand" window on the earlier knob expire
    clearSent();
    const mixKnob = $('.knob[data-param="mix"] svg');
    const before = host.values.mix;
    mixKnob.dispatchEvent(new WheelEvent('wheel', { deltaY: 100, bubbles: true, cancelable: true }));
    const after = host.values.mix;
    await sleep(900);   // past the 0.8 s grouping window
    $('#undo-btn').click();
    check('undo restores the previous value', after < before && Math.abs(host.values.mix - before) < 1e-9 && lastSent('param').id === 'mix', `${before} -> ${after} -> ${host.values.mix}`);
    $('#redo-btn').click();
    check('redo brings it back', Math.abs(host.values.mix - after) < 1e-9, `${host.values.mix}`);

    // ---- save / reload in this browser ------------------------------------------------
    clearSent();
    bridge.emitEvent('setParam', { id: 'cutoffHi', value: 3210 });
    bridge.emitEvent('savePreset', { name: 'Web Test', overwrite: false });
    await sleep(50);
    check('save a preset in this browser', last('saveResult') && last('saveResult').ok && host.current.id === 'local:Web Test' && last('presetList').presets.some(p => p.id === 'local:Web Test'),
          JSON.stringify(last('saveResult')));
    bridge.emitEvent('setParam', { id: 'cutoffHi', value: 500 });
    bridge.emitEvent('loadPreset', { id: 'local:Web Test' });
    await sleep(50);
    check('reload it: the saved value comes back', host.values.cutoffHi === 3210 && host.current.name === 'Web Test', `cutoffHi ${host.values.cutoffHi}`);
    bridge.emitEvent('deletePreset', { id: 'local:Web Test' });
    await sleep(50);
    check('delete it', !last('presetList').presets.some(p => p.id === 'local:Web Test'), '');

    // ---- served: the AudioWorklet plays and the UI follows the stats ------------------------
    if (location.protocol !== 'file:') {
      let ctx = null;
      try { ctx = new AudioContext(); await ctx.resume(); } catch (e) { /* no audio device */ }
      const framesBefore = window.__wubScope.frames;
      const statsBefore = (got.stats || []).length;
      bridge.emitEvent('selectInput', { id: 'saw' });
      bridge.emitEvent('setParam', { id: 'lfoSync', value: 0 });
      bridge.emitEvent('setParam', { id: 'lfoRate', value: 4 });
      for (let i = 0; i < 30; i++) { await fetch('ping').catch(() => {}); await sleep(100); }   // ~3 s of real time
      const stats = (got.stats || []).slice(statsBefore);
      const lfos = stats.map(s => s.lfo);
      check('served: the AudioWorklet runs', host.audioMode === 'AudioWorklet', host.audioMode);
      check('served: stats arrive from the engine (~30 Hz)', stats.length > 40, `${stats.length} in 3 s`);
      check('served: the LFO moves and the saw makes sound', Math.max(...lfos) - Math.min(...lfos) > 0.5 && stats.some(s => s.peakIn > 0.05 && s.peakOut > 0.01),
            `lfo ${Math.min(...lfos).toFixed(2)}..${Math.max(...lfos).toFixed(2)}, peakIn ${Math.max(...stats.map(s => s.peakIn)).toFixed(3)}, peakOut ${Math.max(...stats.map(s => s.peakOut)).toFixed(3)}`);
      check('served: the scope draws (frames advance)', window.__wubScope.frames > framesBefore + 20, `${window.__wubScope.frames - framesBefore} frames`);
      check('served: the cutoff readout follows the LFO', stats.some(s => s.cutoff[1] > 1000) && stats.some(s => s.cutoff[1] < 300),
            `cutoff ${Math.min(...stats.map(s => s.cutoff[1])).toFixed(0)}..${Math.max(...stats.map(s => s.cutoff[1])).toFixed(0)} Hz`);
      // phase 2: the pitch of the 110 Hz saw is tracked and shown; Auto detects a bass and loads its defaults
      const tracked = stats.filter(s => s.pitchConf > 0.8 && Math.abs(s.pitch - 110) < 1);
      check('served: the pitch tracker reads the 110 Hz saw', tracked.length > 10, `${tracked.length} stats at 110 Hz; last ${(stats.slice(-1)[0] || {}).pitch}`);
      check('served: the SOURCE page shows it', /A2 · 110 Hz/.test($('#pitch-readout').textContent), $('#pitch-readout').textContent);
      clearSent();
      bridge.emitEvent('sourceType', { type: 0 });   // Auto
      bridge.emitEvent('setParam', { id: 'bands', value: 1 });   // something a Bass decision will undo
      let detected = -1;
      for (let i = 0; i < 40 && Math.round(host.values.bands) !== 0; i++) { await fetch('ping').catch(() => {}); await sleep(100); detected = (last('stats') || {}).detected; }
      check('served: Auto listens to the saw, decides Bass and loads its defaults (single band, 60..2500 Hz)',
            detected === 0 && Math.round(host.values.bands) === 0 && host.values.cutoffLo === 60 && Math.round(host.values.source) === 0 && (got.params || []).some(p => p.cutoffLo === 60),
            `detected ${detected}, bands ${host.values.bands}, cutoffLo ${host.values.cutoffLo}`);
      check('served: ..and says so', /loaded/.test($('#monitor').textContent) || /loaded/.test(detectedEl()), $('#monitor').textContent);
      bridge.emitEvent('setParam', { id: 'harmOn', value: 1 });
      bridge.emitEvent('setParam', { id: 'harmMode', value: 1 });
      for (let i = 0; i < 10; i++) { await fetch('ping').catch(() => {}); await sleep(100); }
      check('served: harmonics report their blend', (last('stats') || {}).harm > 0.3 && /\d+%/.test($('#harm-readout').textContent), `${(last('stats') || {}).harm} / ${$('#harm-readout').textContent}`);
      // phase 3: the limiter works at +18 dB and reports its gain reduction; Auto Out reports its gain
      bridge.emitEvent('setParam', { id: 'outGain', value: 18 });
      if (host.values.autoOut < 0.5) $('.power[data-param="autoOut"]').click();   // through the UI, so its readout knows
      const statsBefore3 = (got.stats || []).length;
      for (let i = 0; i < 15; i++) { await fetch('ping').catch(() => {}); await sleep(100); }
      const stats3 = (got.stats || []).slice(statsBefore3);
      check('served: the limiter holds +18 dB under full scale and reports its reduction', stats3.some(s => s.limiter < -3) && stats3.every(s => s.peakOut <= 1.0) && /-\d/.test($('#limiter-readout').textContent),
            `deepest ${Math.min(...stats3.map(s => s.limiter)).toFixed(1)} dB, peak out ${Math.max(...stats3.map(s => s.peakOut)).toFixed(3)}, readout ${$('#limiter-readout').textContent}`);
      check('served: Auto Out reports its gain', stats3.some(s => typeof s.autoOut === 'number') && /dB/.test($('#autoout-readout').textContent), $('#autoout-readout').textContent);
      if (ctx) ctx.close();
    }

    check('no script errors during the tests', window.__wubErrors.length === 0, window.__wubErrors.join(' | '));
    finish();
  }

  function finish() {
    const pre = document.createElement('pre');
    pre.id = 'wub-test-out';
    pre.textContent = JSON.stringify(results);
    document.body.appendChild(pre);
    document.title = 'done';
    if (location.protocol !== 'file:') fetch('result', { method: 'POST', body: JSON.stringify(results) }).catch(() => {});
  }

  run().catch((e) => { check('test script ran to the end', false, e && (e.stack || e.message || e)); finish(); });
})();
