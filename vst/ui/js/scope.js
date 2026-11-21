// ============================================================================
//  The wobble scope: a canvas that draws the LFO's recent history (left and
//  right), the sweep range and the current cutoff of each active band on a
//  log frequency axis, and the input / output levels. Fed by the host's
//  `stats` events at ~30 Hz; drawn with requestAnimationFrame, paused while
//  hidden. Colours come from the CSS custom properties.
// ============================================================================
'use strict';

class Scope {
  constructor(canvas) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.hist = new Float32Array(240).fill(0);   // ~8 s of LFO at 30 Hz
    this.histR = new Float32Array(240).fill(0);
    this.pos = 0;
    this.stats = { lfo: 0, lfoR: 0, cutoff: [0, 0, 0], peakIn: 0, peakOut: 0 };
    this.range = { lo: 120, hi: 4000, bands: 0, wob: [1, 1, 0], type: 0, xLow: 200, xHigh: 2500 };
    this.frames = 0;
    this.visible = true;
    this.colors = null;
    this.raf = 0;
    this.tick = this.tick.bind(this);
    this.raf = requestAnimationFrame(this.tick);
  }

  // the formant table of dsp/Filters.h (A E I O U), morphed by a 0..1 position
  static formants(pos) {
    const HZ = [[800, 1150, 2900], [400, 1600, 2700], [350, 1700, 2700], [450, 800, 2830], [325, 700, 2700]];
    const AMP = [[1, 0.63, 0.1], [1, 0.25, 0.03], [1, 0.2, 0.03], [1, 0.5, 0.05], [1, 0.3, 0.03]];
    const m = clamp(pos, 0, 1) * 4;
    const v0 = Math.min(Math.trunc(m), 3), t = m - v0;
    return {
      hz: [0, 1, 2].map(i => HZ[v0][i] * Math.pow(HZ[v0 + 1][i] / HZ[v0][i], t)),
      amp: [0, 1, 2].map(i => AMP[v0][i] + (AMP[v0 + 1][i] - AMP[v0][i]) * t),
    };
  }
  static vowelName(pos) { return ['A', 'E', 'I', 'O', 'U'][Math.round(clamp(pos, 0, 1) * 4)]; }

  // called from app.js whenever the filter / band parameters change
  setRange(r) { Object.assign(this.range, r); }
  setVisible(v) { this.visible = v; if (v && !this.raf) this.raf = requestAnimationFrame(this.tick); }

  push(s) {
    this.stats = s;
    this.hist[this.pos] = s.lfo;
    this.histR[this.pos] = s.lfoR;
    this.pos = (this.pos + 1) % this.hist.length;
  }

  readColors() {
    const cs = getComputedStyle(document.documentElement);
    const v = (n, d) => (cs.getPropertyValue(n) || d).trim();
    this.colors = {
      grid: v('--scope-grid', '#1b2d55'), lfo: v('--accent', '#ff8c2a'), lfoR: v('--blue-hi', '#5aa0ff'),
      sweep: v('--scope-sweep', 'rgba(59,130,255,0.18)'), cutoff: v('--red', '#ff3b3b'), text: v('--muted', '#8da3c9'),
      band: v('--scope-band', 'rgba(255,140,42,0.10)'), meter: v('--accent', '#ff8c2a'), hot: v('--red', '#ff3b3b'),
    };
  }

  tick() {
    this.raf = 0;
    if (!this.visible) return;
    this.draw();
    this.frames++;
    this.raf = requestAnimationFrame(this.tick);
  }

  draw() {
    const c = this.canvas, g = this.ctx;
    const W = c.width, H = c.height;
    if (!this.colors) this.readColors();
    const col = this.colors;
    g.clearRect(0, 0, W, H);

    // ---- layout: LFO history on the left 60%, the spectrum strip on the right
    const lfoW = Math.floor(W * 0.58), specX = lfoW + 14, specW = W - specX - 8;
    const top = 8, bottom = H - 18;

    // grid
    g.strokeStyle = col.grid; g.lineWidth = 1;
    g.beginPath();
    for (let i = 0; i <= 4; i++) { const y = top + (bottom - top) * i / 4; g.moveTo(0, y + 0.5); g.lineTo(lfoW, y + 0.5); }
    g.stroke();

    // LFO history (oldest left, newest right); L in orange, R in blue
    const n = this.hist.length;
    const trace = (arr, color, width) => {
      g.strokeStyle = color; g.lineWidth = width;
      g.beginPath();
      for (let i = 0; i < n; i++) {
        const v = arr[(this.pos + i) % n];
        const x = i / (n - 1) * lfoW, y = bottom - v * (bottom - top);
        if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
      }
      g.stroke();
    };
    trace(this.histR, col.lfoR, 1.2);
    trace(this.hist, col.lfo, 2);
    // the current value as a dot
    const cur = this.hist[(this.pos + n - 1) % n];
    g.fillStyle = col.cutoff;
    g.beginPath(); g.arc(lfoW - 2, bottom - cur * (bottom - top), 3.5, 0, Math.PI * 2); g.fill();

    // ---- frequency strip: 20 Hz .. 20 kHz log
    const fx = (hz) => specX + Math.log(clamp(hz, 20, 20000) / 20) / Math.log(1000) * specW;
    g.strokeStyle = col.grid;
    g.beginPath();
    for (const hz of [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]) { const x = Math.round(fx(hz)) + 0.5; g.moveTo(x, top); g.lineTo(x, bottom); }
    g.stroke();
    g.fillStyle = col.text; g.font = '9px Bahnschrift, "Segoe UI", sans-serif'; g.textAlign = 'center';
    for (const [hz, t] of [[50, '50'], [200, '200'], [1000, '1k'], [5000, '5k'], [20000, '20k']]) g.fillText(t, fx(hz), H - 5);
    g.textAlign = 'left';
    g.fillText('LFO  L / R', 4, H - 5);

    // band ranges (3-band mode): tint each active band's region
    const r = this.range;
    if (r.bands === 1) {
      const edges = [20, r.xLow, r.xHigh, 20000];
      for (let b = 0; b < 3; b++) {
        if (!r.wob[b]) continue;
        g.fillStyle = col.band;
        g.fillRect(fx(edges[b]), top, fx(edges[b + 1]) - fx(edges[b]), bottom - top);
      }
    }
    // the sweep range
    g.fillStyle = col.sweep;
    g.fillRect(fx(r.lo), top, Math.max(2, fx(r.hi) - fx(r.lo)), bottom - top);
    // the current cutoff of each active band; in Formant mode the three formants of the current vowel instead
    const s = this.stats;
    for (let b = 0; b < 3; b++) {
      const active = r.bands === 1 ? r.wob[b] : (b === 1 && r.wob[1]);
      if (!active || !s.cutoff || !s.cutoff[b]) continue;
      if (r.type === 4) {
        // the engine reports the swept cutoff; its position in the Low..High range is the vowel position
        const pos = clamp(Math.log(s.cutoff[b] / r.lo) / Math.log(Math.max(r.hi / r.lo, 1.0001)), 0, 1);
        const f = Scope.formants(pos);
        for (let i = 0; i < 3; i++) {
          const x = fx(f.hz[i]);
          g.strokeStyle = col.cutoff; g.lineWidth = 2; g.globalAlpha = 0.35 + 0.65 * f.amp[i];
          g.beginPath(); g.moveTo(x, bottom); g.lineTo(x, top + (bottom - top) * (1 - f.amp[i]) * 0.6); g.stroke();
          g.globalAlpha = 1;
        }
        g.fillStyle = col.text; g.textAlign = 'center'; g.font = 'bold 11px Bahnschrift, "Segoe UI", sans-serif';
        g.fillText(Scope.vowelName(pos), fx(f.hz[0]), top + 11);
        g.font = '9px Bahnschrift, "Segoe UI", sans-serif'; g.textAlign = 'left';
        continue;
      }
      const x = fx(s.cutoff[b]);
      g.strokeStyle = col.cutoff; g.lineWidth = 2;
      g.beginPath(); g.moveTo(x, top); g.lineTo(x, bottom); g.stroke();
      // a little resonance bump
      g.fillStyle = col.cutoff;
      g.beginPath(); g.arc(x, top + 6, 3, 0, Math.PI * 2); g.fill();
    }

    // meters: in / out as two thin bars at the far right edge of the LFO area
    const mx = lfoW + 3, mw = 4;
    const bar = (x, peak) => {
      const db = 20 * Math.log10(Math.max(peak, 1e-6));
      const h = clamp((db + 60) / 60, 0, 1) * (bottom - top);
      g.fillStyle = db > -1 ? col.hot : col.meter;
      g.fillRect(x, bottom - h, mw, h);
    };
    g.fillStyle = col.grid; g.fillRect(mx, top, mw, bottom - top); g.fillRect(mx + 6, top, mw, bottom - top);
    bar(mx, s.peakIn || 0); bar(mx + 6, s.peakOut || 0);
  }
}
