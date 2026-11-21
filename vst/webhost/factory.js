// ============================================================================
//  Factory presets for the web build: resolves vst/presets/factory_presets.json
//  exactly like PresetManager::loadFactory (C++):
//    preset values = parameter defaults <- template chain (base first) <- own params
//  Values are clamped to the parameter range (not rounded), like applyParamObject.
//  Also: the preset-file migration steps (twin of PresetManager::migrate) and the
//  source types' defaults (what picking a type loads), taken from the srcBass ..
//  srcMix templates (the C++ tests keep those equal to dsp/Sources.h).
//  Needs params.js (PARAM_BY_ID, clamp).
// ============================================================================
'use strict';

const WUB_PRESET_VERSION = 1;
const WUB_PRESET_FORMAT = 'I Wub You Preset';
const WUB_PRESET_EXT = '.wubpreset';

// Copies every known { paramId: number } into out. Returns how many were found.
function wubApplyParamObject(obj, out) {
  let found = 0;
  if (!obj || typeof obj !== 'object') return 0;
  for (const id of Object.keys(obj)) {
    const d = PARAM_BY_ID[id];
    const raw = obj[id];
    if (!d || d.noPreset || !(typeof raw === 'number' || typeof raw === 'boolean')) continue;
    out[id] = clamp(Number(raw), d.min, d.max);
    ++found;
  }
  return found;
}

// Resolves a template chain into out (base first, then the template's own params).
function wubApplyTemplate(templates, name, out, depth) {
  if (!name || depth > 8) return;
  const t = templates[name];
  if (!t || typeof t !== 'object') { console.warn(`unknown preset template "${name}"`); return; }
  wubApplyTemplate(templates, t.base, out, depth + 1);
  wubApplyParamObject(t.params, out);
}

// [{ name, category, values }] where values holds only what the template chain + preset set.
function wubFactoryPresets(json) {
  const templates = (json && json.templates) || {};
  const list = [];
  for (const cat of (json && json.categories) || []) {
    for (const p of cat.presets || []) {
      const values = {};
      wubApplyTemplate(templates, p.base, values, 0);
      wubApplyParamObject(p.params, values);
      list.push({ name: String(p.name), category: String(cat.name), values });
    }
  }
  if (!list.length) list.push({ name: 'Init', category: 'Factory', values: {} });
  return list;
}

// What picking a source type loads (PluginProcessor::applySourceType): the type and its
// defaults, read from that type's factory template. Twin of kSourceKeys in dsp/Sources.h.
const WUB_SOURCE_TEMPLATES = ['srcBass', 'srcVocal', 'srcGuitar', 'srcDrums', 'srcPad', 'srcMix'];
const WUB_SOURCE_KEYS = ['bands', 'cutoffLo', 'cutoffHi', 'reso', 'loWob', 'midWob', 'hiWob', 'loDrive', 'midDrive', 'hiDrive',
                         'xLow', 'xHigh', 'filterType', 'monoBelow', 'transPreserve', 'harmOn', 'harmAmount'];
function wubSourceDefaults(json, type) {
  type = clamp(Math.round(type), 0, 6);
  const out = { source: type };
  if (type === 0) return out;   // Auto: nothing to load (phase 2 analyses the input)
  const all = {};
  wubApplyTemplate((json && json.templates) || {}, WUB_SOURCE_TEMPLATES[type - 1], all, 0);
  for (const k of WUB_SOURCE_KEYS) if (all[k] !== undefined) out[k] = all[k];
  return out;
}

// Brings a preset object written by an older version up to WUB_PRESET_VERSION (in place).
// Same steps as PresetManager::migrate. Plain parameter additions need no step.
function wubMigratePreset(json) {
  if (!json || typeof json !== 'object') return json;
  let version = Number.isFinite(Number(json.version)) ? Math.trunc(Number(json.version)) : 1;
  version = Math.max(version, 1);
  // while (version < WUB_PRESET_VERSION) { switch (version) { case 1: /* 1 -> 2 */ break; } ++version; }
  if (version < WUB_PRESET_VERSION) version = WUB_PRESET_VERSION;
  json.version = version;
  return json;
}
