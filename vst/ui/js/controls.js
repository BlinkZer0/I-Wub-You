// ============================================================================
//  Parameter controls besides the knob. Same conventions as Knob:
//  setValue(v) updates the display, onChange(v) fires on user edits.
//    ToggleButton - bool param   (click; double-click = default)
//    Segmented    - choice param (click; wheel / arrow keys step; double-click = default)
//    SelectBox    - choice param (native dropdown; wheel steps; double-click = default)
//    Meter        - a level meter (not a control): setLevel(0..1 peak)
// ============================================================================
'use strict';

class ToggleButton {
  constructor(def, value, onChange, label) {
    this.def = def;
    this.onChange = onChange;
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'power';
    b.innerHTML = `<span class="led"></span><span>${label || 'ON'}</span>`;
    b.addEventListener('click', () => this.onChange(this.value >= 0.5 ? 0 : 1));
    this.el = b;
    this.setValue(value);
  }
  setValue(v) {
    this.value = v;
    this.el.classList.toggle('on', v >= 0.5);
    this.el.setAttribute('aria-pressed', v >= 0.5);
  }
}

class Segmented {
  constructor(def, value, onChange, labels) {
    this.def = def;
    this.onChange = onChange;
    const wrap = document.createElement('div');
    wrap.className = 'seg';
    wrap.setAttribute('role', 'radiogroup');
    wrap.setAttribute('aria-label', def.name);
    this.buttons = (labels || def.choices).map((label, i) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.textContent = label;
      b.setAttribute('role', 'radio');
      b.addEventListener('click', () => this.pick(i));
      wrap.appendChild(b);
      return b;
    });
    wrap.addEventListener('wheel', (e) => { e.preventDefault(); this.pick(this.value + (e.deltaY < 0 ? 1 : -1)); }, { passive: false });
    wrap.addEventListener('keydown', (e) => {
      const dir = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key];
      if (dir) { e.preventDefault(); e.stopPropagation(); this.pick(this.value + dir); this.buttons[this.value].focus(); }
    });
    wrap.addEventListener('dblclick', () => this.pick(this.def.def));
    this.el = wrap;
    this.setValue(value);
  }
  pick(i) {
    i = clamp(Math.round(i), 0, this.buttons.length - 1);
    if (this.buttons[i].disabled) return;
    if (i !== this.value) this.onChange(i);
  }
  setValue(v) {
    this.value = clamp(Math.round(v), 0, this.buttons.length - 1);
    this.buttons.forEach((b, i) => { b.classList.toggle('on', i === this.value); b.setAttribute('aria-checked', i === this.value); });
  }
}

class SelectBox {
  constructor(def, value, onChange, labels) {
    this.def = def;
    this.onChange = onChange;
    const s = document.createElement('select');
    s.className = 'param-select';
    s.setAttribute('aria-label', def.name);
    (labels || def.choices).forEach((label, i) => s.appendChild(new Option(label, String(i))));
    s.addEventListener('change', () => this.onChange(Number(s.value)));
    s.addEventListener('wheel', (e) => {
      e.preventDefault();
      const i = clamp(this.value + (e.deltaY < 0 ? -1 : 1), 0, s.options.length - 1);
      if (i !== this.value) this.onChange(i);
    }, { passive: false });
    s.addEventListener('dblclick', () => { if (this.value !== this.def.def) this.onChange(this.def.def); });
    s.addEventListener('keydown', (e) => e.stopPropagation()); // arrow keys belong to the dropdown
    this.el = s;
    this.setValue(value);
  }
  setValue(v) {
    this.value = clamp(Math.round(v), 0, this.el.options.length - 1);
    this.el.value = String(this.value);
  }
}

// A vertical peak meter with a slow-falling peak mark. setLevel(peak 0..1) at the stats rate.
class Meter {
  constructor(label) {
    const el = document.createElement('div');
    el.className = 'meter';
    el.innerHTML = `<div class="meter-well"><div class="meter-fill"></div><div class="meter-peak"></div></div><span class="ctl-label">${label}</span><span class="meter-db">-∞</span>`;
    this.el = el;
    this.fill = el.querySelector('.meter-fill');
    this.peakEl = el.querySelector('.meter-peak');
    this.db = el.querySelector('.meter-db');
    this.level = 0; this.hold = 0; this.holdAge = 0;
  }
  // dBFS -> 0..1 of the meter height, -60 dB at the bottom
  static pos(db) { return clamp((db + 60) / 60, 0, 1); }
  setLevel(peak) {
    const db = 20 * Math.log10(Math.max(peak, 1e-6));
    const p = Meter.pos(db);
    this.level = Math.max(p, this.level * 0.8);
    if (p >= this.hold) { this.hold = p; this.holdAge = 0; } else if (++this.holdAge > 30) this.hold = Math.max(p, this.hold - 0.01);
    this.fill.style.height = `${this.level * 100}%`;
    this.peakEl.style.bottom = `${this.hold * 100}%`;
    this.peakEl.classList.toggle('hot', this.hold > Meter.pos(-1));
    this.db.textContent = peak < 1e-5 ? '-∞' : `${db.toFixed(0)}`;
  }
}
