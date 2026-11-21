// ============================================================================
//  I Wub You DSP engine: JavaScript twin of vst/Source/WubEngine.h and
//  vst/Source/dsp/*.h. Kept line-for-line equivalent (same control rate, same
//  RNG, same maths, same order of operations) and checked against the C++
//  build by vst/tests/run-parity.ps1 (tolerance 1e-4).
//
//  This whole function is stringified and loaded as an AudioWorklet module
//  (or run on the main thread as a ScriptProcessor fallback), so it must not
//  reference anything outside itself.
//
//  Every parameter goes through Math.fround first, because the plugin reads
//  float parameters. Nothing allocates on the audio path.
// ============================================================================
function wubWorkletModule() {
  'use strict';

  // ======================================================== dsp/Common.h ===
  const kPi = 3.141592653589793, kTwoPi = 6.283185307179586, kChunk = 32;
  const clampd = (v, lo, hi) => (v < lo ? lo : (v > hi ? hi : v));
  const jlimit = (lo, hi, v) => (v < lo ? lo : (v > hi ? hi : v));
  const dbToGain = (db) => Math.pow(10, db / 20);
  const gainToDb = (g) => 20 * Math.log10(Math.max(g, 1e-12));
  const lround = (x) => (x < 0 ? -Math.round(-x) : Math.round(x)); // std::lround: halves away from zero

  class Rng {
    constructor() { this.s = 0x2545F491; }
    seed(v) { this.s = v >>> 0; }
    next() { this.s = (Math.imul(this.s, 1664525) + 1013904223) >>> 0; return this.s; }
    uni() { return (this.next() >>> 8) * (1 / 16777216); }
    bi() { return this.uni() * 2 - 1; }
  }

  function safetyClip(x) {
    const a = Math.abs(x);
    if (a <= 0.8) return x;
    const y = 0.8 + 0.2 * Math.tanh((a - 0.8) / 0.2);
    return x < 0 ? -y : y;
  }

  class ChunkRamp {
    constructor() { this.cur = 0; this.target = 0; this.inc = 0; }
    set(t) { this.cur = this.target; this.target = t; this.inc = (t - this.cur) / kChunk; }
    jump(t) { this.cur = this.target = t; this.inc = 0; }
    next() { const v = this.cur; this.cur += this.inc; return v; }
  }

  // transposed direct form II; RBJ designs. The coefficients live on the instance, as in the C++ struct.
  class Biquad {
    constructor() { this.b0 = 1; this.b1 = 0; this.b2 = 0; this.a1 = 0; this.a2 = 0; this.s1 = 0; this.s2 = 0; }
    process(x) {
      const y = this.b0 * x + this.s1;
      this.s1 = this.b1 * x - this.a1 * y + this.s2;
      this.s2 = this.b2 * x - this.a2 * y;
      return y;
    }
    reset() { this.s1 = this.s2 = 0; }
    copyCoefs(o) { this.b0 = o.b0; this.b1 = o.b1; this.b2 = o.b2; this.a1 = o.a1; this.a2 = o.a2; }
    setLowpass(f0, q, sr) {
      const w = kTwoPi * f0 / sr, alpha = Math.sin(w) / (2 * q), cw = Math.cos(w), a0 = 1 + alpha;
      this.b0 = (1 - cw) / 2 / a0; this.b1 = (1 - cw) / a0; this.b2 = this.b0;
      this.a1 = -2 * cw / a0; this.a2 = (1 - alpha) / a0;
    }
    setHighpass(f0, q, sr) {
      const w = kTwoPi * f0 / sr, alpha = Math.sin(w) / (2 * q), cw = Math.cos(w), a0 = 1 + alpha;
      this.b0 = (1 + cw) / 2 / a0; this.b1 = -(1 + cw) / a0; this.b2 = this.b0;
      this.a1 = -2 * cw / a0; this.a2 = (1 - alpha) / a0;
    }
  }

  const onePoleCoef = (hz, sr) => 1 - Math.exp(-kTwoPi * hz / sr);
  const tauCoef = (tau, dt) => (tau <= 0 ? 1 : 1 - Math.exp(-dt / tau));

  // ======================================================= dsp/Filters.h ===
  class Svf {
    constructor() { this.ic1 = 0; this.ic2 = 0; this.lp = 0; this.bp = 0; this.hp = 0; this.notch = 0; }
    reset() { this.ic1 = this.ic2 = 0; }
    // writes lp / bp / hp / notch onto the instance (no allocation)
    process(x, g, k) {
      const a1 = 1 / (1 + g * (g + k));
      const a2 = g * a1;
      const a3 = g * a2;
      const v3 = x - this.ic2;
      const v1 = a1 * this.ic1 + a2 * v3;
      const v2 = this.ic2 + a2 * this.ic1 + a3 * v3;
      this.ic1 = 2 * v1 - this.ic1;
      this.ic2 = 2 * v2 - this.ic2;
      this.lp = v2;
      this.bp = v1 * k;
      this.hp = x - k * v1 - v2;
      this.notch = x - k * v1;
    }
  }

  const svfG = (fc, sr) => Math.tan(kPi * clampd(fc, 10, 0.45 * sr) / sr);
  const resoToQ = (reso) => 0.5 * Math.pow(10, clampd(reso, 0, 1) * 1.60206);

  const kNumVowels = 5;
  const kFormantHz = [[800, 1150, 2900], [400, 1600, 2700], [350, 1700, 2700], [450, 800, 2830], [325, 700, 2700]];
  const kFormantAmp = [[1.0, 0.63, 0.10], [1.0, 0.25, 0.03], [1.0, 0.20, 0.03], [1.0, 0.50, 0.05], [1.0, 0.30, 0.03]];
  const formantOut = { hz: 0, amp: 0 };
  function formantAt(morph, i) {
    const m = clampd(morph, 0, 1) * (kNumVowels - 1);
    let v0 = Math.trunc(m);
    if (v0 > kNumVowels - 2) v0 = kNumVowels - 2;
    const t = m - v0;
    formantOut.hz = kFormantHz[v0][i] * Math.pow(kFormantHz[v0 + 1][i] / kFormantHz[v0][i], t);
    formantOut.amp = kFormantAmp[v0][i] + (kFormantAmp[v0 + 1][i] - kFormantAmp[v0][i]) * t;
  }

  class Lr4 {
    constructor() { this.lp1 = new Biquad(); this.lp2 = new Biquad(); this.hp1 = new Biquad(); this.hp2 = new Biquad(); this.lo = 0; this.hi = 0; }
    set(fc, sr) {
      const q = 0.7071067811865476;
      this.lp1.setLowpass(fc, q, sr); this.lp2.copyCoefs(this.lp1);
      this.hp1.setHighpass(fc, q, sr); this.hp2.copyCoefs(this.hp1);
    }
    reset() { this.lp1.reset(); this.lp2.reset(); this.hp1.reset(); this.hp2.reset(); }
    split(x) {   // writes lo / hi onto the instance
      this.lo = this.lp2.process(this.lp1.process(x));
      this.hi = this.hp2.process(this.hp1.process(x));
    }
    allpass(x) { this.split(x); return this.lo + this.hi; }
  }

  class DcBlocker {
    constructor() { this.x1 = 0; this.y1 = 0; this.r = 0.9993; }
    prepare(sr) { this.r = 1 - kTwoPi * 5 / sr; this.x1 = this.y1 = 0; }
    process(x) { const y = x - this.x1 + this.r * this.y1; this.x1 = x; this.y1 = y; return y; }
  }

  class EnvFollower {
    constructor() { this.env = 0; this.aUp = 1; this.aDown = 0.01; }
    prepare(attackSec, releaseSec, sr) { this.aUp = tauCoef(attackSec, 1 / sr); this.aDown = tauCoef(releaseSec, 1 / sr); this.env = 0; }
    process(x) {
      const a = Math.abs(x);
      this.env += (a > this.env ? this.aUp : this.aDown) * (a - this.env);
      return this.env;
    }
  }

  class RmsTracker {
    constructor() { this.ms = 0; this.a = 0.001; }
    prepare(windowSec, sr) { this.a = tauCoef(windowSec, 1 / sr); this.ms = 0; }
    push(x) { this.ms += this.a * (x * x - this.ms); }
    rms() { return Math.sqrt(this.ms); }
  }

  class DelayLine {
    constructor() { this.buf = new Float64Array(16); this.pos = 0; }
    reset() { this.buf.fill(0); this.pos = 0; }
    process(x, delay) {
      this.buf[this.pos] = x;
      const y = this.buf[(this.pos - delay + 16) & 15];
      this.pos = (this.pos + 1) & 15;
      return y;
    }
  }

  // =================================================== dsp/Oversampler.h ===
  class HalfbandDesign {
    constructor(numCoefs, transition) {
      this.n = numCoefs;
      this.c = new Float64Array(12);
      const n = this.n, order = n * 2 + 1;
      let k = Math.tan((1 - 2 * transition) * kPi / 4);
      k *= k;
      const kk = Math.sqrt(Math.sqrt(1 - k * k));
      const e = 0.5 * (1 - kk) / (1 + kk);
      const e4 = e * e * e * e;
      const q = e * (1 + e4 * (2 + e4 * (15 + 150 * e4)));
      for (let idx = 0; idx < n; idx++) {
        const cc = idx + 1;
        let num = 0, den = 0, t;
        let i = 0, j = 1;
        do { t = Math.pow(q, i * (i + 1)) * Math.sin((i * 2 + 1) * cc * kPi / order) * j; num += t; j = -j; i++; } while (Math.abs(t) > 1e-100);
        num *= Math.pow(q, 0.25);
        i = 1; j = -1;
        do { t = Math.pow(q, i * i) * Math.cos(i * 2 * cc * kPi / order) * j; den += t; j = -j; i++; } while (Math.abs(t) > 1e-100);
        den += 0.5;
        const ww = num / den, w2 = ww * ww;
        const x = Math.sqrt((1 - w2 * k) * (1 - w2 / k)) / (1 + w2);
        this.c[idx] = (1 - x) / (1 + x);
      }
    }
  }
  const halfbandA = new HalfbandDesign(8, 0.04);
  const halfbandB = new HalfbandDesign(4, 0.14);

  class HalfbandState {
    constructor() { this.x = new Float64Array(12); this.y = new Float64Array(12); this.s0 = 0; this.s1 = 0; }
    reset() { this.x.fill(0); this.y.fill(0); }
    // runs the chains on this.s0 / this.s1 in place
    run(d) {
      const x = this.x, y = this.y, c = d.c;
      let s0 = this.s0, s1 = this.s1;
      let i = 0;
      for (; i + 1 < d.n; i += 2) {
        const t0 = (s0 - y[i]) * c[i] + x[i];
        const t1 = (s1 - y[i + 1]) * c[i + 1] + x[i + 1];
        x[i] = s0; x[i + 1] = s1;
        y[i] = t0; y[i + 1] = t1;
        s0 = t0; s1 = t1;
      }
      if (i < d.n) {
        const t0 = (s0 - y[i]) * c[i] + x[i];
        x[i] = s0; y[i] = t0; s0 = t0;
      }
      this.s0 = s0; this.s1 = s1;
    }
    // one input -> two outputs (this.s0, this.s1) at twice the rate
    up(d, inp) { this.s0 = inp; this.s1 = inp; this.run(d); }
    // two inputs -> one output at half the rate
    down(d, in0, in1) {
      this.s0 = in1; this.s1 = in0;
      this.run(d);
      return 0.5 * (this.s0 + this.s1);
    }
  }

  class TanhShaper {
    constructor() { this.x1 = 0; }
    reset() { this.x1 = 0; }
    static f(x) { return 4 * Math.tanh(0.25 * x); }
    static F(x) { const a = Math.abs(x); return 16 * (0.25 * a + Math.log(1 + Math.exp(-0.5 * a)) - 0.6931471805599453); }
    process(x) {
      const d = x - this.x1;
      let y;
      if (Math.abs(d) < 1e-6) y = TanhShaper.f(0.5 * (x + this.x1));
      else y = (TanhShaper.F(x) - TanhShaper.F(this.x1)) / d;
      this.x1 = x;
      return y;
    }
    direct(x) { this.x1 = x; return TanhShaper.f(x); }
  }

  class OversampledShaper {
    constructor() {
      this.upA = new HalfbandState(); this.upB = new HalfbandState(); this.downA = new HalfbandState(); this.downB = new HalfbandState();
      this.s1 = new TanhShaper(); this.s2 = new TanhShaper(); this.s4 = new TanhShaper();
    }
    reset() { this.upA.reset(); this.upB.reset(); this.downA.reset(); this.downB.reset(); this.s1.reset(); this.s2.reset(); this.s4.reset(); }
    static latency(osChoice) { return osChoice >= 2 ? 4 : (osChoice === 1 ? 3 : 0); }
    process(x, osChoice) {
      if (osChoice <= 0) return this.s1.direct(x);
      const A = halfbandA;
      this.upA.up(A, x);
      const a0 = this.upA.s0, a1 = this.upA.s1;
      if (osChoice === 1) {
        const y0 = this.s2.process(a0);
        const y1 = this.s2.process(a1);
        return this.downA.down(A, y0, y1);
      }
      const B = halfbandB;
      this.upB.up(B, a0);
      const b0 = this.upB.s0, b1 = this.upB.s1;
      this.upB.up(B, a1);
      const b2 = this.upB.s0, b3 = this.upB.s1;
      const y0 = this.s4.process(b0);
      const y1 = this.s4.process(b1);
      const y2 = this.s4.process(b2);
      const y3 = this.s4.process(b3);
      const c0 = this.downB.down(B, y0, y1);
      const c1 = this.downB.down(B, y2, y3);
      return this.downA.down(A, c0, c1);
    }
  }

  const drivePreGain = (drive) => dbToGain(36 * clampd(drive, 0, 1));
  const driveMakeup = (pre) => 1 / Math.sqrt(pre);

  // =========================================================== dsp/Lfo.h ===
  const kNumDivs = 13;
  const kDivBeats = [4.0, 2.0, 4.0 / 3.0, 1.0, 1.5, 2.0 / 3.0, 0.5, 0.75, 1.0 / 3.0, 0.25, 0.375, 1.0 / 6.0, 0.125];
  const fracPart = (x) => x - Math.floor(x);

  class Lfo {
    constructor() {
      this.phase = 0; this.lastPos = 0; this.shValue = 0; this.rndFrom = 0; this.rndTo = 0.5; this.smoothed = 0; this.started = false;
      this.rng = new Rng();
    }
    reset(seed) {
      this.phase = 0; this.lastPos = 0; this.smoothed = 0; this.started = false;
      this.rng.seed(seed);
      this.shValue = this.rng.uni();
      this.rndFrom = this.rng.uni();
      this.rndTo = this.rng.uni();
    }
    retrigger() { this.phase = 0; this.lastPos = 0; }
    static rateHz(sync, rate, div, bpm) {
      if (!sync) return rate;
      const d = div < 0 ? 0 : (div >= kNumDivs ? kNumDivs - 1 : div);
      return (bpm / 60) / kDivBeats[d];
    }
    next(shape, sync, rate, div, phaseDeg, smooth, dt, bpm, ppq, playing) {
      const hz = Lfo.rateHz(sync, rate, div, bpm);
      if (sync && playing) {
        const d = div < 0 ? 0 : (div >= kNumDivs ? kNumDivs - 1 : div);
        this.phase = fracPart(ppq / kDivBeats[d]);
      } else {
        this.phase = fracPart(this.phase + hz * dt);
      }
      const pos = fracPart(this.phase + phaseDeg / 360);
      const wrapped = this.started && pos < this.lastPos;
      this.lastPos = pos;
      if (wrapped) { this.shValue = this.rng.uni(); this.rndFrom = this.rndTo; this.rndTo = this.rng.uni(); }

      let raw;
      switch (shape) {
        case 1: raw = pos < 0.5 ? 2 * pos : 2 - 2 * pos; break;
        case 2: raw = 1 - pos; break;
        case 3: raw = pos; break;
        case 4: raw = pos < 0.5 ? 1 : 0; break;
        case 5: raw = this.shValue; break;
        case 6: raw = this.rndFrom + (this.rndTo - this.rndFrom) * pos; break;
        default: raw = 0.5 - 0.5 * Math.cos(kTwoPi * pos); break;
      }
      if (!this.started) { this.smoothed = raw; this.started = true; }
      const tau = smooth * smooth * 0.25;
      this.smoothed += tauCoef(tau, dt) * (raw - this.smoothed);
      return this.smoothed;
    }
  }

  // ========================================================= dsp/Pitch.h ===
  class PitchTracker {
    constructor() {
      this.lp = new Biquad();
      this.ring = new Float64Array(1024);
      this.d = new Float64Array(402);
      this.pos = 0; this.decimCount = 0; this.sinceHop = 0;
      this.filled = 0;
      this.decim = 4;
      this.srDec = 12000;
      this.pitch = 0; this.conf = 0; this.lastLp = 0; this.analysed = false;
    }
    prepare(sr) {
      this.lp.setLowpass(2500, 0.7071067811865476, sr);
      this.lp.reset();
      this.ring.fill(0);
      this.pos = this.decimCount = this.sinceHop = 0;
      this.filled = 0;
      this.decim = Math.trunc(sr / 12000 + 0.5);
      if (this.decim < 1) this.decim = 1;
      this.srDec = sr / this.decim;
      this.pitch = 0; this.conf = 0; this.lastLp = 0; this.analysed = false;
    }
    push(x) {
      const y = this.lp.process(x);
      this.lastLp = y;
      if (++this.decimCount < this.decim) return;
      this.decimCount = 0;
      this.ring[this.pos] = y;
      this.pos = (this.pos + 1) & 1023;
      if (this.filled < 912) this.filled++;
      if (++this.sinceHop >= 256) { this.sinceHop = 0; this.analyse(); }
    }
    analyse() {
      const kWin = 512, kMaxLag = 400, kMinLag = 8, kThreshold = 0.15;
      const ring = this.ring, d = this.d;
      if (this.filled < kWin + kMaxLag) return;
      this.analysed = true;
      const start = (this.pos - (kWin + kMaxLag) + 1024) & 1023;
      let energy = 0;
      for (let i = 0; i < kWin; i++) { const v = ring[(start + i) & 1023]; energy += v * v; }
      if (energy < 1e-7 * kWin) { this.conf = 0; return; }

      let running = 0;
      let best = -1, firstDip = -1;
      let bestVal = 1e9;
      for (let tau = 1; tau <= kMaxLag; tau++) {
        let acc = 0;
        for (let i = 0; i < kWin; i++) {
          const diff = ring[(start + i) & 1023] - ring[(start + i + tau) & 1023];
          acc += diff * diff;
        }
        running += acc;
        d[tau] = running > 0 ? acc * tau / running : 1;
        if (tau >= kMinLag) {
          if (firstDip < 0 && d[tau] < kThreshold) firstDip = tau;
          if (d[tau] < bestVal) { bestVal = d[tau]; best = tau; }
        }
      }
      let tau = firstDip;
      if (tau >= 0) {
        while (tau + 1 <= kMaxLag && d[tau + 1] < d[tau]) tau++;
        while (tau - 1 >= kMinLag && d[tau - 1] < d[tau]) tau--;
      } else tau = best;
      if (tau < kMinLag) { this.conf = 0; return; }
      let period = tau;
      if (tau > kMinLag && tau < kMaxLag) {
        const a = d[tau - 1], b = d[tau], c = d[tau + 1];
        const den = a - 2 * b + c;
        if (den > 1e-12) period = tau + 0.5 * (a - c) / den;
      }
      this.pitch = this.srDec / period;
      this.conf = clampd(1 - d[tau], 0, 1);
    }
  }

  class SourceFeatures {
    constructor() {
      this.msX = 0; this.msD = 0; this.prev = 0; this.aFast = 0.001;
      this.peak = 0; this.ms300 = 0; this.aPeakDown = 0.001; this.a300 = 0.001;
      this.lowState = 0; this.aLow = 0.03; this.msLow = 0;
      this.ms20 = 0; this.a20 = 0.001; this.envMean = 0; this.envMs = 0; this.a1s = 0.0001;
      this.started = false; this.startCount = 0; this.startAfter = 4800;
      this.sr = 48000;
    }
    prepare(sampleRate) {
      this.sr = sampleRate;
      this.aFast = tauCoef(0.1, 1 / this.sr);
      this.aPeakDown = tauCoef(0.3, 1 / this.sr);
      this.a300 = tauCoef(0.3, 1 / this.sr);
      this.aLow = onePoleCoef(200, this.sr);
      this.a20 = tauCoef(0.02, 1 / this.sr);
      this.a1s = tauCoef(1.0, 1 / this.sr);
      this.msX = this.msD = this.prev = this.peak = this.ms300 = this.lowState = this.msLow = this.ms20 = this.envMean = this.envMs = 0;
      this.started = false; this.startCount = 0; this.startAfter = Math.trunc(0.1 * this.sr);
    }
    push(x) {
      const dx = x - this.prev;
      this.prev = x;
      this.msX += this.aFast * (x * x - this.msX);
      this.msD += this.aFast * (dx * dx - this.msD);
      const a = Math.abs(x);
      if (a > this.peak) this.peak = a; else this.peak += this.aPeakDown * (a - this.peak);
      this.ms300 += this.a300 * (x * x - this.ms300);
      this.lowState += this.aLow * (x - this.lowState);
      this.msLow += this.a300 * (this.lowState * this.lowState - this.msLow);
      this.ms20 += this.a20 * (x * x - this.ms20);
      const e = Math.sqrt(this.ms20);
      if (!this.started) { if (++this.startCount >= this.startAfter) { this.envMean = e; this.envMs = e * e; this.started = true; } }
      else {
        this.envMean += this.a1s * (e - this.envMean);
        this.envMs += this.a1s * (e * e - this.envMs);
      }
    }
    variation() {
      if (this.envMean < 1e-6) return 0;
      return Math.sqrt(Math.max(this.envMs - this.envMean * this.envMean, 0)) / this.envMean;
    }
    rmsFreq() {
      if (this.msX < 1e-12) return 0;
      const r = clampd(Math.sqrt(this.msD / this.msX) * 0.5, 0, 1);
      return 2 * Math.asin(r) * this.sr / kTwoPi;
    }
    crest() { return this.ms300 > 1e-12 ? this.peak / Math.sqrt(this.ms300) : 0; }
    lowRatio() { return this.ms300 > 1e-12 ? clampd(this.msLow / this.ms300, 0, 1) : 0; }
    rms() { return Math.sqrt(this.ms300); }
    flush() {
      if (this.msX < 1e-30) this.msX = 0;
      if (this.msD < 1e-30) this.msD = 0;
      if (this.ms300 < 1e-30) this.ms300 = 0;
      if (this.msLow < 1e-30) this.msLow = 0;
      if (Math.abs(this.lowState) < 1e-30) this.lowState = 0;
      if (this.peak < 1e-30) this.peak = 0;
      if (this.ms20 < 1e-30) this.ms20 = 0;
      if (this.envMean < 1e-30) this.envMean = 0;
      if (this.envMs < 1e-30) this.envMs = 0;
    }
  }

  function classifySource(confAvg, pitch, rmsFreq, variation, crest, lowRatio) {
    if (variation > 1.2) return 3;
    if (variation > 0.6 && confAvg < 0.6) return 3;
    if (confAvg > 0.6) {
      if (pitch < 130) return 0;
      if (pitch < 1000) return crest > 5 ? 2 : 1;
      return 1;
    }
    if (rmsFreq < 300 && lowRatio > 0.6) return 0;
    return variation < 0.25 ? 4 : 5;
  }

  // ===================================================== dsp/Harmonics.h ===
  function polyBlep(t, dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
  }

  class HarmOsc {
    constructor() { this.phase = 0; this.inc = 0; this.prevX = 0; this.sinceLock = 0; this.subFlip = 1; this.subLp = 0; this.subCoef = 0.01; this.saw = 0; this.square = 0; this.sub = 0; }
    reset() { this.phase = 0; this.inc = 0; this.prevX = 0; this.sinceLock = 0; this.subFlip = 1; this.subLp = 0; this.saw = this.square = this.sub = 0; }
    setPitch(hz, sr) {
      this.inc = hz > 0 ? hz / sr : 0;
      this.subCoef = onePoleCoef(hz > 0 ? clampd(hz, 20, 2000) : 100, sr);
    }
    process(x) {
      this.sinceLock++;
      if (this.inc > 0 && this.prevX <= 0 && x > 0 && this.sinceLock >= Math.trunc(0.8 / this.inc)) {
        this.phase = 0;
        this.sinceLock = 0;
        this.subFlip = -this.subFlip;
      }
      this.prevX = x;
      if (this.inc > 0) {
        this.phase += this.inc;
        if (this.phase >= 1) this.phase -= 1;
        this.saw = 2 * this.phase - 1 - polyBlep(this.phase, this.inc);
        let sq = (this.phase < 0.5 ? 1 : -1) + polyBlep(this.phase, this.inc);
        let t2 = this.phase + 0.5;
        if (t2 >= 1) t2 -= 1;
        sq -= polyBlep(t2, this.inc);
        this.square = sq;
      } else { this.saw = 0; this.square = 0; }
      this.subLp += this.subCoef * (this.subFlip - this.subLp);
      this.sub = this.subLp;
    }
  }

  function shaperResidue(x, meanAbs) {
    const odd = 3 * (0.2 * Math.tanh(5 * x) - x);
    const even = 1.2 * (Math.abs(x) - meanAbs);
    return odd + even;
  }

  // ======================================================= dsp/Limiter.h ===
  class AutoOutput {
    constructor() { this.inRms = new RmsTracker(); this.wetRms = new RmsTracker(); this.gain = 1; this.a = 0.001; }
    prepare(sr) {
      this.inRms.prepare(1.0, sr);
      this.wetRms.prepare(1.0, sr);
      this.gain = 1;
      this.a = tauCoef(1.0, kChunk / sr);
    }
    update(enabled) {
      const inp = this.inRms.rms(), wet = this.wetRms.rms();
      let target = 1;
      if (enabled) {
        const lim = dbToGain(12);
        target = (inp > 1e-4 && wet > 1e-4) ? clampd(inp / wet, 1 / lim, lim) : this.gain;
      }
      this.gain += this.a * (target - this.gain);
      return this.gain;
    }
    flush() {
      if (this.inRms.ms < 1e-30) this.inRms.ms = 0;
      if (this.wetRms.ms < 1e-30) this.wetRms.ms = 0;
    }
  }

  class TruePeakLimiter {
    constructor() {
      this.upA = [new HalfbandState(), new HalfbandState()]; this.upB = [new HalfbandState(), new HalfbandState()];
      this.delay = [new Float64Array(256), new Float64Array(256)];
      this.need = new Float64Array(256).fill(1);
      this.pos = 0; this.look = 48;
      this.env = 1; this.aAttack = 0.1; this.aRelease = 0.001;
      this.reduction = 1;
      this.L = 0; this.R = 0;
    }
    static lookaheadSamples(sr) { const n = Math.trunc(0.001 * sr + 0.5); return n > 256 ? 256 : (n < 1 ? 1 : n); }
    prepare(sr) {
      this.look = TruePeakLimiter.lookaheadSamples(sr);
      for (let c = 0; c < 2; c++) { this.upA[c].reset(); this.upB[c].reset(); this.delay[c].fill(0); }
      this.need.fill(1);
      this.pos = 0;
      this.env = 1;
      this.aAttack = tauCoef(0.0003, 1 / sr);
      this.aRelease = tauCoef(0.05, 1 / sr);
      this.reduction = 1;
    }
    truePeak(c, x) {
      const A = this.upA[c], B = this.upB[c];
      A.up(halfbandA, x);
      const a0 = A.s0, a1 = A.s1;
      B.up(halfbandB, a0);
      const b0 = B.s0, b1 = B.s1;
      B.up(halfbandB, a1);
      const b2 = B.s0, b3 = B.s1;
      let p = Math.abs(b0);
      if (Math.abs(b1) > p) p = Math.abs(b1);
      if (Math.abs(b2) > p) p = Math.abs(b2);
      if (Math.abs(b3) > p) p = Math.abs(b3);
      return p;
    }
    // writes this.L / this.R
    process(L, R) {
      const kCeiling = 0.9660;
      const pk = Math.max(this.truePeak(0, L), this.truePeak(1, R));
      const need = this.need, look = this.look, pos = this.pos;
      need[pos] = pk > kCeiling ? kCeiling / pk : 1;
      let target = 1;
      for (let i = 0; i < look; i++) if (need[i] < target) target = need[i];
      this.env += (target < this.env ? this.aAttack : this.aRelease) * (target - this.env);
      if (this.env < this.reduction) this.reduction = this.env;
      const outL = this.delay[0][pos], outR = this.delay[1][pos];
      this.delay[0][pos] = L; this.delay[1][pos] = R;
      this.pos = (pos + 1) % look;
      this.L = outL * this.env;
      this.R = outR * this.env;
    }
    takeReductionDb() { const r = this.reduction; this.reduction = 1; return gainToDb(r); }
    flush() {
      for (let c = 0; c < 2; c++)
        for (let i = 0; i < 12; i++) {
          if (Math.abs(this.upA[c].x[i]) < 1e-30) this.upA[c].x[i] = 0;
          if (Math.abs(this.upA[c].y[i]) < 1e-30) this.upA[c].y[i] = 0;
          if (Math.abs(this.upB[c].x[i]) < 1e-30) this.upB[c].x[i] = 0;
          if (Math.abs(this.upB[c].y[i]) < 1e-30) this.upB[c].y[i] = 0;
        }
    }
  }

  // =================================================== dsp/TestSignals.h ===
  class TestSource {
    constructor() {
      this.type = 1; this.freq = 55; this.level = 0.5; this.sr = 48000;
      this.c = 1; this.s = 0; this.dc = 1; this.ds = 0; this.ph = 0; this.ph2 = 0; this.ph3 = 0;
      this.rng = new Rng();
      this.pos = 0; this.kickC = 1; this.kickS = 0; this.kickEnv = 0; this.snareEnv = 0; this.hatEnv = 0; this.hatPrev = 0; this.snareC = 1; this.snareS = 0;
      this.L = 0; this.R = 0;
    }
    prepare(type, freq, level, sampleRate) {
      this.type = type; this.freq = freq; this.level = level; this.sr = sampleRate;
      this.rng.seed(0x57554231);
      this.c = 1; this.s = 0;
      const w = kTwoPi * freq / sampleRate;
      this.dc = Math.cos(w); this.ds = Math.sin(w);
      this.ph = this.ph2 = this.ph3 = 0;
      this.pos = 0; this.kickC = 1; this.kickS = 0; this.kickEnv = this.snareEnv = this.hatEnv = 0; this.hatPrev = 0; this.snareC = 1; this.snareS = 0;
    }
    saw(which, inc) {   // which: 0 ph, 1 ph2, 2 ph3
      let p = which === 0 ? this.ph : (which === 1 ? this.ph2 : this.ph3);
      p += inc;
      if (p >= 1) p -= 1;
      if (which === 0) this.ph = p; else if (which === 1) this.ph2 = p; else this.ph3 = p;
      return 2 * p - 1;
    }
    next() {   // writes this.L / this.R
      let l = 0, r = 0;
      const inc = this.freq / this.sr;
      switch (this.type) {
        case 1: {
          const nc = this.c * this.dc - this.s * this.ds, ns = this.s * this.dc + this.c * this.ds;
          this.c = nc; this.s = ns;
          const n = 0.5 * (3 - (this.c * this.c + this.s * this.s));
          this.c *= n; this.s *= n;
          l = r = this.s;
          break;
        }
        case 2: l = r = this.saw(0, inc); break;
        case 3: { this.saw(0, inc); l = r = this.ph < 0.5 ? 1 : -1; break; }
        case 4: l = r = (this.saw(0, inc) + this.saw(1, inc * 1.25) + this.saw(2, inc * 1.5)) / 3; break;
        case 5: l = r = this.rng.bi(); break;
        case 6: {
          const sr = this.sr;
          const beat = Math.trunc(0.5 * sr), eighth = Math.trunc(beat / 2);
          const inBar = this.pos % (4 * beat);
          if (inBar % beat === 0) {
            const b = Math.trunc(inBar / beat);
            if (b === 0 || b === 2) { this.kickEnv = 1; this.kickC = 1; this.kickS = 0; }
            else { this.snareEnv = 1; }
          }
          if (this.pos % eighth === 0) this.hatEnv = 1;
          const kw = kTwoPi * 55 / sr, kc = Math.cos(kw), ks = Math.sin(kw);
          const nc = this.kickC * kc - this.kickS * ks, ns = this.kickS * kc + this.kickC * ks;
          this.kickC = nc; this.kickS = ns;
          this.kickEnv *= Math.exp(-6.9078 / (0.35 * sr));
          const sw = kTwoPi * 200 / sr, sc = Math.cos(sw), ss = Math.sin(sw);
          const nsc = this.snareC * sc - this.snareS * ss, nss = this.snareS * sc + this.snareC * ss;
          this.snareC = nsc; this.snareS = nss;
          this.snareEnv *= Math.exp(-6.9078 / (0.15 * sr));
          const noise = this.rng.bi();
          this.hatEnv *= Math.exp(-6.9078 / (0.05 * sr));
          const hat = (noise - this.hatPrev) * 0.5;
          this.hatPrev = noise;
          l = r = this.kickS * this.kickEnv * 1.0 + (noise * 0.6 + this.snareS * 0.4) * this.snareEnv * 0.7 + hat * this.hatEnv * 0.5;
          this.pos++;
          break;
        }
        case 7: l = this.saw(0, inc); r = this.saw(1, inc * 1.25); break;
        default: break;
      }
      this.L = l * this.level;
      this.R = r * this.level;
    }
  }

  // ======================================================== WubEngine.h ===
  function newEngineParams() {
    return {
      source: 0, autoIn: true, inGain: 1,
      harmOn: false, harmMode: 0, harmShape: 0, harmAmount: 0.5, subOn: false, subAmount: 0.5,
      bands: 0, xLow: 200, xHigh: 2500, os: 1, drivePos: 0,
      wob: [true, true, false], depth: [1, 1, 1], drive: [0.3, 0.3, 0.3], tone: [0.7, 0.7, 0.7], level: [1, 1, 1],
      filterType: 0, cutoffLo: 120, cutoffHi: 4000, reso: 0.5, resoLimit: 0.5, keyTrack: 0,
      lfoShape: 0, lfoSync: true, lfoRate: 2, lfoDiv: 6, lfoPhase: 0, lfoStereo: 0, lfoSmooth: 0.1, lfoRetrig: 0,
      transPreserve: 0, stereoMode: 0, monoBelow: 0,
      duck: 0, mix: 1, outGain: 1, autoOut: false, limiter: true,
    };
  }

  // EngineParamsFromValues.h: raw parameter values (floats in the plugin: Math.fround) -> the snapshot, in place
  const BAND_IDS = ['lo', 'mid', 'hi'];
  function makeEngineParams(values, p) {
    const num = (id) => Math.fround(values[id]);
    const choice = (id, maxIndex) => jlimit(0, maxIndex, lround(Math.fround(values[id])));
    const on = (id) => Math.fround(values[id]) >= 0.5;
    p.source = choice('source', 6);
    p.autoIn = on('autoIn');
    p.inGain = dbToGain(num('inGain'));
    p.harmOn = on('harmOn');
    p.harmMode = choice('harmMode', 1);
    p.harmShape = choice('harmShape', 2);
    p.harmAmount = num('harmAmount');
    p.subOn = on('subOn');
    p.subAmount = num('subAmount');
    p.bands = choice('bands', 1);
    p.xLow = num('xLow');
    p.xHigh = num('xHigh');
    p.os = choice('os', 2);
    p.drivePos = choice('drivePos', 1);
    for (let b = 0; b < 3; b++) {
      const id = BAND_IDS[b];
      p.wob[b] = on(id + 'Wob');
      p.depth[b] = num(id + 'Depth');
      p.drive[b] = num(id + 'Drive');
      p.tone[b] = num(id + 'Tone');
      p.level[b] = dbToGain(num(id + 'Level'));
    }
    p.filterType = choice('filterType', 4);
    p.cutoffLo = num('cutoffLo');
    p.cutoffHi = num('cutoffHi');
    p.reso = num('reso');
    p.resoLimit = num('resoLimit');
    p.keyTrack = num('keyTrack');
    p.lfoShape = choice('lfoShape', 6);
    p.lfoSync = on('lfoSync');
    p.lfoRate = num('lfoRate');
    p.lfoDiv = choice('lfoDiv', kNumDivs - 1);
    p.lfoPhase = num('lfoPhase');
    p.lfoStereo = num('lfoStereo');
    p.lfoSmooth = num('lfoSmooth');
    p.lfoRetrig = choice('lfoRetrig', 1);
    p.transPreserve = num('transPreserve');
    p.stereoMode = choice('stereoMode', 2);
    p.monoBelow = num('monoBelow');
    p.duck = num('duck');
    p.mix = num('mix');
    p.outGain = dbToGain(num('outGain'));
    p.autoOut = on('autoOut');
    p.limiter = on('limiter');
    return p;
  }

  class Band {
    constructor() {
      this.shaper = new OversampledShaper();
      this.toneState = 0; this.toneCoef = 1;
      this.svf = [new Svf(), new Svf(), new Svf()];
      this.env = new EnvFollower();
      this.g = new ChunkRamp(); this.k = new ChunkRamp(); this.pre = new ChunkRamp(); this.makeup = new ChunkRamp(); this.level = new ChunkRamp();
      this.fg = [new ChunkRamp(), new ChunkRamp(), new ChunkRamp()];
      this.famp = [new ChunkRamp(), new ChunkRamp(), new ChunkRamp()];
      this.fk = 0.1;
    }
  }
  class Channel {
    constructor() {
      this.dc = new DcBlocker();
      this.xa = new Lr4(); this.xb = new Lr4(); this.apb = new Lr4();
      this.band = [new Band(), new Band(), new Band()];
      this.lfo = new Lfo();
      this.duckGain = new ChunkRamp();
      this.dry = new DelayLine();
      this.lfoVal = 0;
      this.meanAbs = 0;
      this.fastEnv = new EnvFollower(); this.slowEnv = 0; this.aSlow = 0.001;
      this.transGain = new ChunkRamp();
    }
  }

  class WubEngine {
    constructor() {
      this.params = newEngineParams();
      this.sr = 48000;
      this.ch = [new Channel(), new Channel()];
      this.monoSplit = new Lr4();
      this.rms = new RmsTracker();
      this.agGain = 1;
      this.inGainRamp = new ChunkRamp(); this.outGainRamp = new ChunkRamp(); this.mixRamp = new ChunkRamp();
      this.lastXLow = -1; this.lastXHigh = -1; this.lastMono = -1;
      this.chunkLeft = 0;
      this.bpm = 120; this.ppq = 0; this.playing = false;
      this.peakIn = 0; this.peakOut = 0;
      this.cutoffStat = [0, 0, 0];
      this.lastNote = -1;
      // analysis (phase 2)
      this.dcMono = new DcBlocker();
      this.tracker = new PitchTracker();
      this.features = new SourceFeatures();
      this.osc = new HarmOsc();
      this.harmEnv = new EnvFollower();
      this.pitchSm = 110; this.confSm = 0; this.confAvg = 0; this.confStarted = false; this.thinSm = 0; this.gateSm = 0; this.ktLog = 0; this.aMeanAbs = 0.001;
      this.harmRamp = new ChunkRamp(); this.subRamp = new ChunkRamp();
      this.detected = -1; this.candidate = -1; this.candidateCount = 0;
      // output (phase 3)
      this.autoOut = new AutoOutput();
      this.autoOutRamp = new ChunkRamp();
      this.limiter = new TruePeakLimiter();
      this.fg = new Float64Array(3); this.fa = new Float64Array(3);   // per-sample formant ramps (scratch)
      this.bandIn = new Float64Array(3);
      this.xin = new Float64Array(2);
      this.o = new Float64Array(2);
    }
    static latencySamples(osChoice) { return OversampledShaper.latency(osChoice); }
    static totalLatency(osChoice, limiterOn, sr) { return WubEngine.latencySamples(osChoice) + (limiterOn ? TruePeakLimiter.lookaheadSamples(sr) : 0); }
    static kRmsTarget() { return 0.12589254117941673; }

    prepare(sampleRate) {
      const sr = this.sr = sampleRate;
      this.autoOut.prepare(sr);
      this.limiter.prepare(sr);
      this.autoOutRamp.jump(1);
      for (let c = 0; c < 2; c++) {
        const C = this.ch[c];
        C.dc.prepare(sr);
        C.xa.reset(); C.xb.reset(); C.apb.reset();
        C.lfo.reset((0x57554231 + c) >>> 0);
        C.dry.reset();
        C.duckGain.jump(1);
        C.lfoVal = 0;
        C.meanAbs = 0;
        C.fastEnv.prepare(0, 0.02, sr);
        C.slowEnv = 0; C.aSlow = tauCoef(0.06, 1 / sr);
        C.transGain.jump(0);
        for (let b = 0; b < 3; b++) {
          const B = C.band[b];
          B.shaper.reset();
          B.toneState = 0;
          B.toneCoef = 1;
          for (const f of B.svf) f.reset();
          B.env.prepare(0, 0.12, sr);
          B.g.jump(svfG(1000, sr)); B.k.jump(1);
          B.pre.jump(1); B.makeup.jump(1); B.level.jump(1);
          for (let i = 0; i < 3; i++) { B.fg[i].jump(svfG(1000, sr)); B.famp[i].jump(0); }
          B.fk = 0.1;
        }
      }
      this.monoSplit.reset();
      this.rms.prepare(0.3, sr);
      this.agGain = 1;
      this.inGainRamp.jump(this.params.inGain);
      this.outGainRamp.jump(this.params.outGain);
      this.mixRamp.jump(this.params.mix);
      this.lastXLow = this.lastXHigh = this.lastMono = -1;
      this.chunkLeft = 0;
      this.bpm = 120; this.ppq = 0; this.playing = false;
      this.peakIn = this.peakOut = 0;
      this.cutoffStat[0] = this.cutoffStat[1] = this.cutoffStat[2] = 0;
      this.lastNote = -1;
      this.dcMono.prepare(sr);
      this.tracker.prepare(sr);
      this.features.prepare(sr);
      this.osc.reset();
      this.harmEnv.prepare(0, 0.05, sr);
      this.pitchSm = 110; this.confSm = 0; this.confAvg = 0; this.confStarted = false; this.thinSm = 0; this.gateSm = 0; this.ktLog = 0;
      this.aMeanAbs = tauCoef(0.05, 1 / sr);
      this.harmRamp.jump(0); this.subRamp.jump(0);
      this.detected = -1; this.candidate = -1; this.candidateCount = 0;
      this.updateCrossovers();
    }
    setParams(p) { this.params = p; }
    setTransport(bpm, ppq, playing) { this.bpm = bpm > 1 ? bpm : 120; this.ppq = ppq; this.playing = playing; }
    setTempo(bpm) { this.bpm = bpm > 1 ? bpm : 120; }
    noteOn(note) { this.lastNote = note; if (this.params.lfoRetrig === 1) this.retrigger(); }
    noteOff(note) { if (note === this.lastNote) this.lastNote = -1; }
    retrigger() { this.ch[0].lfo.retrigger(); this.ch[1].lfo.retrigger(); }

    takePeakIn() { const p = this.peakIn; this.peakIn = 0; return p; }
    takePeakOut() { const p = this.peakOut; this.peakOut = 0; return p; }
    lfoValue(c) { return this.ch[c & 1].lfoVal; }
    cutoffHz(b) { return this.cutoffStat[b < 0 ? 0 : (b > 2 ? 2 : b)]; }
    autoGainDb() { return gainToDb(this.agGain); }
    detectedSource() { return this.detected; }
    bandActive(b) { return this.params.bands === 1 || b === 1; }
    heldNote() { return this.lastNote; }
    pitchHz() { return this.tracker.pitch; }
    pitchConfidence() { return this.confSm; }
    harmonicsBlend() { return this.harmRamp.target; }
    rmsFrequency() { return this.features.rmsFreq(); }
    crestFactor() { return this.features.crest(); }
    autoOutputDb() { return gainToDb(this.autoOut.gain); }
    takeLimiterReductionDb() { return this.limiter.takeReductionDb(); }

    drive(B, y, pre, mk) {
      y = B.shaper.process(y * pre, this.params.os) * mk;
      B.toneState += B.toneCoef * (y - B.toneState);
      return B.toneState;
    }
    filter(B, y, g, k, fg, fa) {
      const p = this.params;
      if (p.filterType === 4) {
        let acc = 0;
        for (let f = 0; f < 3; f++) {
          const s = B.svf[f];
          s.process(y, fg[f], B.fk);
          acc += s.bp * fa[f];
        }
        return acc * 1.5;
      }
      const s = B.svf[0];
      s.process(y, g, k);
      switch (p.filterType) {
        case 1: return s.bp;
        case 2: return s.hp;
        case 3: return s.notch;
        default: return s.lp;
      }
    }
    updateCrossovers() {
      const p = this.params;
      if (p.xLow !== this.lastXLow || p.xHigh !== this.lastXHigh) {
        this.lastXLow = p.xLow; this.lastXHigh = p.xHigh;
        for (const C of this.ch) { C.xa.set(p.xLow, this.sr); C.xb.set(p.xHigh, this.sr); C.apb.set(p.xHigh, this.sr); }
      }
      if (p.monoBelow !== this.lastMono) {
        this.lastMono = p.monoBelow;
        this.monoSplit.set(p.monoBelow < 1 ? 1 : p.monoBelow, this.sr);
      }
    }
    static flushSvf(s) {
      if (Math.abs(s.ic1) < 1e-30) s.ic1 = 0;
      if (Math.abs(s.ic2) < 1e-30) s.ic2 = 0;
    }
    static flushBiquad(b) {
      if (Math.abs(b.s1) < 1e-30) b.s1 = 0;
      if (Math.abs(b.s2) < 1e-30) b.s2 = 0;
    }
    static flushLr4(x) { WubEngine.flushBiquad(x.lp1); WubEngine.flushBiquad(x.lp2); WubEngine.flushBiquad(x.hp1); WubEngine.flushBiquad(x.hp2); }

    analysis(dt) {
      const p = this.params, tracker = this.tracker, features = this.features;
      const a = tauCoef(0.03, dt);
      this.confSm += a * (tracker.conf - this.confSm);
      if (tracker.analysed && !this.confStarted) { this.confAvg = tracker.conf; this.confStarted = true; }
      this.confAvg += tauCoef(1.0, dt) * (tracker.conf - this.confAvg);
      if (tracker.conf > 0.5 && tracker.pitch > 0) this.pitchSm += a * (tracker.pitch - this.pitchSm);
      let keyPitch = this.pitchSm;
      if (this.lastNote >= 0) keyPitch = 440 * Math.pow(2, (this.lastNote - 69) / 12);
      const ktTarget = p.keyTrack * Math.log(clampd(keyPitch, 20, 5000) / 110);
      this.ktLog += tauCoef(0.02, dt) * (ktTarget - this.ktLog);
      const rf = features.rmsFreq();
      const thin = (this.confSm > 0.5 && this.pitchSm > 0) ? clampd((1.8 - rf / this.pitchSm) / 0.6, 0, 1) : 0;
      this.thinSm += tauCoef(0.1, dt) * (thin - this.thinSm);
      const gate = this.confSm > 0.5 ? 1 : 0;
      this.gateSm += tauCoef(0.05, dt) * (gate - this.gateSm);
      const blend = p.harmOn ? p.harmAmount * (p.harmMode === 0 ? this.thinSm : 1) * (p.harmShape === 0 ? 1 : this.gateSm) : 0;
      this.harmRamp.set(blend);
      this.subRamp.set(p.subOn ? p.subAmount * this.gateSm : 0);
      this.osc.setPitch(this.pitchSm, this.sr);

      if (tracker.analysed) {
        tracker.analysed = false;
        if (features.rms() > 1e-4) {
          const type = classifySource(this.confAvg, this.pitchSm, rf, features.variation(), features.crest(), features.lowRatio());
          if (type === this.candidate) this.candidateCount++; else { this.candidate = type; this.candidateCount = 1; }
          if (this.candidateCount >= 70 || (this.detected < 0 && this.candidateCount >= 48)) this.detected = this.candidate;
        }
      }
      features.flush();
      if (Math.abs(this.osc.subLp) < 1e-30) this.osc.subLp = 0;
    }

    control() {
      const p = this.params, sr = this.sr;
      const dt = kChunk / sr;
      this.updateCrossovers();

      let target = 1;
      if (p.autoIn) {
        const r = this.rms.rms();
        target = r > 1e-4 ? clampd(0.12589254117941673 / r, 1 / 15.848931924611133, 15.848931924611133) : this.agGain;
      }
      this.agGain += tauCoef(0.4, dt) * (target - this.agGain);
      this.inGainRamp.set(p.inGain * this.agGain);
      this.outGainRamp.set(p.outGain);
      this.mixRamp.set(p.mix);

      if (this.playing) this.ppq += dt * this.bpm / 60;

      this.analysis(dt);
      const ktMul = Math.exp(this.ktLog);
      this.autoOutRamp.set(this.autoOut.update(p.autoOut));
      this.autoOut.flush();
      this.limiter.flush();

      const q0 = resoToQ(p.reso);
      const fk = 1 / (4 + 16 * p.reso);
      for (let c = 0; c < 2; c++) {
        const C = this.ch[c];
        C.lfoVal = C.lfo.next(p.lfoShape, p.lfoSync, p.lfoRate, p.lfoDiv, p.lfoPhase + (c === 1 ? p.lfoStereo : 0),
                              p.lfoSmooth, dt, this.bpm, this.ppq, this.playing);
        C.duckGain.set(1 - p.duck * (1 - C.lfoVal));
        const attack = C.slowEnv > 1e-6 ? clampd((C.fastEnv.env / C.slowEnv - 1) * 0.5, 0, 1) : 0;
        C.transGain.set(p.transPreserve * attack);
        WubEngine.flushLr4(C.xa); WubEngine.flushLr4(C.xb); WubEngine.flushLr4(C.apb);
        if (C.fastEnv.env < 1e-30) C.fastEnv.env = 0;
        if (C.slowEnv < 1e-30) C.slowEnv = 0;
        if (C.meanAbs < 1e-30) C.meanAbs = 0;
        for (let b = 0; b < 3; b++) {
          const B = C.band[b];
          const pos = 0.5 + p.depth[b] * (C.lfoVal - 0.5);
          const cutoff = p.cutoffLo * ktMul * Math.pow(p.cutoffHi / p.cutoffLo, pos);
          if (c === 0) this.cutoffStat[b] = clampd(cutoff, 10, 0.45 * sr);
          const over = Math.max(0, q0 * B.env.env / 0.5 - 1);
          const q = q0 / (1 + p.resoLimit * over);
          B.g.set(svfG(cutoff, sr));
          B.k.set(1 / q);
          for (let f = 0; f < 3; f++) {
            formantAt(pos, f);
            B.fg[f].set(svfG(formantOut.hz * ktMul, sr));
            B.famp[f].set(formantOut.amp);
          }
          B.fk = fk;
          const pre = drivePreGain(p.drive[b]);
          B.pre.set(pre);
          B.makeup.set(driveMakeup(pre));
          B.level.set(p.level[b]);
          const toneHz = 800 * Math.pow(25, p.tone[b]);
          B.toneCoef = p.tone[b] >= 0.999 ? 1 : onePoleCoef(Math.min(toneHz, 0.45 * sr), sr);
          for (const s of B.svf) WubEngine.flushSvf(s);
          if (Math.abs(B.toneState) < 1e-30) B.toneState = 0;
          if (B.env.env < 1e-30) B.env.env = 0;
        }
      }
      WubEngine.flushLr4(this.monoSplit);
      if (this.harmEnv.env < 1e-30) this.harmEnv.env = 0;
    }

    // inL / inR: Float32Array (inR may be null); outL / outR: Float32Array (outR may be null); off: offset into all four
    render(inL, inR, outL, outR, off, n) {
      const p = this.params;
      const lat = WubEngine.latencySamples(p.os);
      const xin = this.xin, o = this.o, bandIn = this.bandIn, fg = this.fg, fa = this.fa;
      for (let i = 0; i < n; i++) {
        if (this.chunkLeft === 0) { this.control(); this.chunkLeft = kChunk; }
        this.chunkLeft--;

        const iL = inL[off + i], iR = inR !== null ? inR[off + i] : iL;
        if (p.stereoMode === 1) { xin[0] = xin[1] = 0.5 * (iL + iR); }
        else if (p.stereoMode === 2) { xin[0] = 0.5 * (iL + iR); xin[1] = 0.5 * (iL - iR); }
        else { xin[0] = iL; xin[1] = iR; }

        const gIn = this.inGainRamp.next();
        const mixv = this.mixRamp.next();
        const gOut = this.outGainRamp.next();
        const harmB = this.harmRamp.next();
        const subB = this.subRamp.next();
        const gAuto = this.autoOutRamp.next();

        const xm = this.dcMono.process(0.5 * (iL + iR)) * gIn;
        this.tracker.push(xm);
        this.features.push(xm);
        const env = this.harmEnv.process(xm);
        this.osc.process(this.tracker.lastLp);
        const gen = p.harmShape === 1 ? this.osc.saw : (p.harmShape === 2 ? this.osc.square : 0);

        const channels = p.stereoMode === 1 ? 1 : 2;
        o[0] = 0; o[1] = 0;
        for (let c = 0; c < channels; c++) {
          const C = this.ch[c];
          const raw = C.dc.process(xin[c]);
          this.rms.push(raw);
          let x = raw * gIn;
          C.meanAbs += this.aMeanAbs * (Math.abs(x) - C.meanAbs);
          if (harmB > 0) x += harmB * (p.harmShape === 0 ? shaperResidue(x, C.meanAbs) : gen * env);
          if (subB > 0) x += subB * this.osc.sub * env;
          const ax = Math.abs(x);
          if (ax > this.peakIn) this.peakIn = ax;
          C.slowEnv += C.aSlow * (C.fastEnv.process(x) - C.slowEnv);

          bandIn[0] = 0; bandIn[1] = 0; bandIn[2] = 0;
          if (p.bands === 1) {
            C.xa.split(x);
            const lo = C.xa.lo, rest = C.xa.hi;
            C.xb.split(rest);
            bandIn[0] = C.apb.allpass(lo);
            bandIn[1] = C.xb.lo;
            bandIn[2] = C.xb.hi;
          } else bandIn[1] = x;

          let sum = 0;
          for (let b = 0; b < 3; b++) {
            const B = C.band[b];
            const g = B.g.next(), k = B.k.next(), pre = B.pre.next(), mk = B.makeup.next(), lv = B.level.next();
            for (let f = 0; f < 3; f++) { fg[f] = B.fg[f].next(); fa[f] = B.famp[f].next(); }
            if (!this.bandActive(b)) continue;
            let y = bandIn[b];
            B.env.process(y);
            if (p.drivePos === 0) y = this.drive(B, y, pre, mk);
            if (p.wob[b]) y = this.filter(B, y, g, k, fg, fa);
            if (p.drivePos === 1) y = this.drive(B, y, pre, mk);
            sum += y * lv;
          }
          const dry = C.dry.process(x, lat);
          const tg = C.transGain.next();
          const wetPre = sum + tg * (dry - sum);
          this.autoOut.inRms.push(x);
          this.autoOut.wetRms.push(wetPre);
          const wet = wetPre * gAuto * C.duckGain.next();
          o[c] = dry + (wet - dry) * mixv;
        }
        if (channels === 1) o[1] = o[0];

        let L, R;
        if (p.stereoMode === 2) { L = o[0] + o[1]; R = o[0] - o[1]; }
        else { L = o[0]; R = o[1]; }
        if (p.monoBelow >= 1) {
          const m = 0.5 * (L + R);
          this.monoSplit.split(0.5 * (L - R));
          const hi = this.monoSplit.hi;
          L = m + hi; R = m - hi;
        }
        L *= gOut;
        R *= gOut;
        if (p.limiter) { this.limiter.process(L, R); L = this.limiter.L; R = this.limiter.R; }
        L = safetyClip(L);
        R = safetyClip(R);
        const aL = Math.abs(L), aR = Math.abs(R);
        if (aL > this.peakOut) this.peakOut = aL;
        if (aR > this.peakOut) this.peakOut = aR;
        outL[off + i] = L;
        if (outR !== null) outR[off + i] = R;
      }
    }
  }

  // ========================================================== processor ===
  // Messages in (from web-host.js, or the parity runner):
  //   param {id,value} · params {values} · midi {data:[status,d1,d2]} · noteOn {note,vel} · noteOff {note} · retrig ·
  //   transport {bpm,ppq,playing} · tempo {bpm} · source {gen,freq,level} (a built-in test source replaces the
  //   input; gen -1 = use the input bus)
  // Messages out: stats (the fields the plugin editor sends)
  class WubProcessor extends AudioWorkletProcessor {
    constructor(options) {
      super();
      const opts = (options && options.processorOptions) || {};
      this.values = Object.assign({}, opts.params || {});
      this.engine = new WubEngine();
      this.P = newEngineParams();
      this.dirty = true;
      this.applyParams();
      this.engine.prepare(sampleRate);
      this.engine.setTransport(opts.bpm || 120, 0, true);
      this.blockCount = 0;
      this.src = new TestSource();
      this.srcType = -1;
      if (opts.source) this.setSource(opts.source);
      this.genL = new Float32Array(128); this.genR = new Float32Array(128);
      this.port.onmessage = (e) => this.onMessage(e.data);
    }

    // { gen: TestSource type (-1 = use the input bus), freq, level }
    setSource(s) {
      this.srcType = (s.gen === undefined || s.gen === null ? -1 : s.gen) | 0;
      if (this.srcType >= 0) this.src.prepare(this.srcType, +s.freq || 55, +s.level || 0.5, sampleRate);
    }

    onMessage(m) {
      const e = this.engine;
      switch (m.type) {
        case 'param': this.values[m.id] = m.value; this.dirty = true; break;
        case 'params': Object.assign(this.values, m.values); this.dirty = true; break;
        case 'midi': this.applyParams(); this.midi(m.data); break;
        case 'noteOn': this.applyParams(); e.noteOn(m.note | 0); break;
        case 'noteOff': this.applyParams(); e.noteOff(m.note | 0); break;
        case 'retrig': e.retrigger(); break;
        case 'transport': e.setTransport(+m.bpm, +m.ppq || 0, !!m.playing); break;
        case 'tempo': e.setTempo(+m.bpm); break;
        case 'source': this.setSource(m); break;
      }
    }

    midi(d) {
      const e = this.engine, st = d[0];
      if (st >= 0xf0 || st < 0x80) return;
      const cmd = st & 0xf0;
      if (cmd === 0x90 && d[2] > 0) e.noteOn(d[1]);
      else if (cmd === 0x80 || cmd === 0x90) e.noteOff(d[1]);
      else if (cmd === 0xb0 && (d[1] === 120 || d[1] === 123)) e.noteOff(e.heldNote());
    }

    applyParams() {
      if (!this.dirty) return;
      this.engine.setParams(makeEngineParams(this.values, this.P));
      this.dirty = false;
    }

    process(inputs, outputs) {
      const out = outputs[0];
      const L = out && out[0];
      if (!L) return true;
      const R = out.length > 1 ? out[1] : null;
      const n = L.length;
      this.applyParams();
      let inL, inR;
      if (this.srcType >= 0) {
        if (this.genL.length < n) { this.genL = new Float32Array(n); this.genR = new Float32Array(n); }
        for (let i = 0; i < n; i++) { this.src.next(); this.genL[i] = this.src.L; this.genR[i] = this.src.R; }
        inL = this.genL; inR = this.genR;
      } else {
        const inp = inputs[0];
        if (inp && inp.length > 0 && inp[0].length === n) { inL = inp[0]; inR = inp.length > 1 ? inp[1] : null; }
        else { this.genL.fill(0); inL = this.genL; inR = null; }
      }
      this.engine.render(inL, inR, L, R, 0, n);

      if (++this.blockCount >= 12) {   // ~31 Hz at 128 samples / 48 kHz, like the plugin editor's timer
        const e = this.engine;
        this.port.postMessage({
          type: 'stats', peakIn: e.takePeakIn(), peakOut: e.takePeakOut(), lfo: e.lfoValue(0), lfoR: e.lfoValue(1),
          cutoff: [e.cutoffHz(0), e.cutoffHz(1), e.cutoffHz(2)], autoGain: e.autoGainDb(), detected: e.detectedSource(),
          bpm: e.bpm, playing: e.playing, note: e.heldNote(), pitch: e.pitchHz(), pitchConf: e.pitchConfidence(), harm: e.harmonicsBlend(),
          autoOut: e.autoOutputDb(), limiter: e.takeLimiterReductionDb(),
        });
        this.blockCount = 0;
      }
      return true;
    }
  }

  registerProcessor('i-wub-you', WubProcessor);
}
