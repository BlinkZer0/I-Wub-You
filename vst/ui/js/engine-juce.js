// ============================================================================
//  JUCE bridge: talks to the C++ processor through JUCE 8's native
//  integration (window.__JUCE__.backend). In the web build,
//  vst/webhost/web-host.js provides the same backend on top of Web Audio.
// ============================================================================
'use strict';

class JuceEngine {
  constructor() {
    this.backend = (window.__JUCE__ && window.__JUCE__.backend) || null;
    this.connected = !!this.backend;
    this.handlers = {};
    if (this.backend) {
      for (const ev of ['params', 'stats', 'config', 'presetList', 'preset', 'saveResult', 'message', 'inputs', 'midiInputs']) {
        this.backend.addEventListener(ev, (data) => { if (this.handlers[ev]) this.handlers[ev](data); });
      }
    }
  }

  on(event, fn) { this.handlers[event] = fn; }
  emit(id, payload) { if (this.backend) this.backend.emitEvent(id, payload || {}); }

  uiReady() { this.emit('uiReady'); }

  // parameters
  setParam(id, value) { this.emit('setParam', { id, value }); }
  beginGesture(id) { this.emit('gesture', { id, begin: true }); }
  endGesture(id) { this.emit('gesture', { id, begin: false }); }

  // restart the LFOs (the RETRIG button)
  retrig() { this.emit('retrig'); }

  // pick a source type: the host sets it and loads its defaults (cutoff range, drive, crossover, bands)
  sourceType(type) { this.emit('sourceType', { type }); }

  // right-click on a control -> the host's parameter menu (automation, MIDI learn...)
  paramMenu(id, x, y) { this.emit('paramMenu', { id, x: Math.round(x), y: Math.round(y) }); }

  // web build only (the plugin takes audio and tempo from the host)
  selectInput(id) { this.emit('selectInput', { id }); }
  setTempo(bpm) { this.emit('setTempo', { bpm }); }
  selectMidi(id) { this.emit('selectMidi', { id }); }

  // presets
  loadPreset(id) { this.emit('loadPreset', { id }); }
  savePreset(name, overwrite) { this.emit('savePreset', { name, overwrite: !!overwrite }); }
  deletePreset(id) { this.emit('deletePreset', { id }); }
  loadPresetFile() { this.emit('loadPresetFile'); }
  openPresetFolder() { this.emit('openPresetFolder'); }
}
