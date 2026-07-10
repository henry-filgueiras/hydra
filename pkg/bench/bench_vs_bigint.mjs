// pkg/bench/bench_vs_bigint.mjs — hydra-bignum powMod vs native BigInt.
//
// The comparison the package exists for: JavaScript has no built-in
// modular exponentiation, so the baseline is what everyone writes —
// left-to-right square-and-multiply over native BigInt with a `% mod`
// after every step (V8's BigInt mul/div are themselves optimized
// C++, so this measures Hydra's Montgomery engine against the
// engine's own bignum, not against naive JS).
//
// Protocol (repo rule: min-of-medians, never single-run deltas):
// per run, sample N calibrated batches (>= 25 ms each) and take the
// median ns/op; across --runs R (default 5) take the min of medians;
// report cross-run CV.
//
// Run: node pkg/bench/bench_vs_bigint.mjs [--runs N] [--md]

import { init, powMod } from '../index.mjs';

const argv = process.argv.slice(2);
const RUNS = argv.includes('--runs') ? Number(argv[argv.indexOf('--runs') + 1]) : 5;
const MD = argv.includes('--md');
const WIDTHS = [256, 512, 1024, 2048, 4096];
const TARGET_BATCH_MS = 25;

// deterministic PRNG (mulberry32), same shape as the test suite
let seed = 0xbe7c4;
function rnd32() {
  seed |= 0; seed = (seed + 0x6d2b79f5) | 0;
  let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
  t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
  return ((t ^ (t >>> 14)) >>> 0);
}
function rndBig(bits) {
  let v = 1n;
  for (let b = 1; b < bits; b += 32) {
    const take = Math.min(32, bits - b);
    v = (v << BigInt(take)) | BigInt(rnd32() >>> (32 - take));
  }
  return v;
}

function powModJS(b, e, m) {
  b %= m;
  let r = 1n;
  while (e > 0n) {
    if (e & 1n) r = (r * b) % m;
    b = (b * b) % m;
    e >>= 1n;
  }
  return r;
}

function timeNsPerOp(fn, batch) {
  const t0 = process.hrtime.bigint();
  for (let i = 0; i < batch; i++) fn();
  return Number(process.hrtime.bigint() - t0) / batch;
}

// One run = median of `samples` calibrated batches.
function medianNsPerOp(fn) {
  const est = timeNsPerOp(fn, 1);                       // warmup + estimate
  const batch = Math.max(1, Math.ceil((TARGET_BATCH_MS * 1e6) / est));
  const samples = est > 30e6 ? 3 : 5;                   // spare the slow widths
  const xs = [];
  for (let s = 0; s < samples; s++) xs.push(timeNsPerOp(fn, batch));
  xs.sort((a, b) => a - b);
  return xs[Math.floor(xs.length / 2)];
}

function minOfMedians(fn) {
  const meds = [];
  for (let r = 0; r < RUNS; r++) meds.push(medianNsPerOp(fn));
  const min = Math.min(...meds);
  const mean = meds.reduce((a, b) => a + b) / meds.length;
  const cv = Math.sqrt(meds.reduce((a, b) => a + (b - mean) ** 2, 0) / meds.length) / mean;
  return { min, cv };
}

function fmt(ns) {
  if (ns < 1e3) return `${ns.toFixed(0)} ns`;
  if (ns < 1e6) return `${(ns / 1e3).toFixed(1)} µs`;
  return `${(ns / 1e6).toFixed(2)} ms`;
}

await init();

const rows = [];
for (const bits of WIDTHS) {
  const m = rndBig(bits) | 1n;
  const b = rndBig(bits - 1);
  const e = rndBig(bits);

  const expect = powModJS(b, e, m);                     // cross-check before timing
  if (powMod(b, e, m) !== expect) {
    throw new Error(`disagreement at ${bits}-bit — refusing to bench a wrong answer`);
  }

  const js = minOfMedians(() => powModJS(b, e, m));
  const hy = minOfMedians(() => powMod(b, e, m));
  rows.push({ bits, js, hy, speedup: js.min / hy.min });
  process.stderr.write(
    `${String(bits).padStart(4)}-bit  BigInt ${fmt(js.min).padStart(9)}  ` +
    `hydra ${fmt(hy.min).padStart(9)}  ×${(js.min / hy.min).toFixed(2)}  ` +
    `(cv ${(js.cv * 100).toFixed(1)}%/${(hy.cv * 100).toFixed(1)}%)\n`);
}

if (MD) {
  console.log('| Width | native `BigInt` | hydra-bignum (wasm) | speedup |');
  console.log('|------:|----------------:|--------------------:|--------:|');
  for (const r of rows) {
    console.log(`| ${r.bits}-bit | ${fmt(r.js.min)} | ${fmt(r.hy.min)} | **${r.speedup.toFixed(1)}×** |`);
  }
} else {
  console.log(`\nnode ${process.version}, runs=${RUNS}, min-of-medians`);
}
