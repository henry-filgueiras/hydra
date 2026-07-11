// pkg/bench/bench_view_probe.mjs — JS-visible A/B for the borrowed-view
// import probe (see bench/probe_view_import.cpp for the native isolation
// and DIRECTORS_NOTES "borrowed-view" devlog entry for the verdict).
//
// Compares the full synchronous wasm-call pipeline for isPerfectSquare:
//   owned : extract → _hydra_is_perfect_square      (from_limbs inside)
//   view  : extract → _hydra_is_perfect_square_view (reads limbs in place)
// plus T_extract alone (BigInt → HEAPU64, common to both paths) so the
// wasm-side difference can be read against the JS marshalling floor.
// The predicate returns a bool, so T_emit/T_rebuild are nil by shape.
//
// Marshalling mirrors pkg/index.mjs verbatim (pushBig / stack frame);
// the module is instantiated directly so the experimental export stays
// out of the public wrapper.
//
// Protocol (house rules): correctness cross-check before timing;
// operands rotate (8 per width); min-of-medians over --runs (default 6),
// 5 samples/run, batches sized ≥ ~15 ms; cross-run CV printed.
//
// Run: node pkg/bench/bench_view_probe.mjs --runs 6 --md

import { performance } from 'node:perf_hooks';
import os from 'node:os';
import createHydraModule from '../dist/hydra_core.mjs';

const args = process.argv.slice(2);
const RUNS = args.includes('--runs') ? Number(args[args.indexOf('--runs') + 1]) : 6;
const MD = args.includes('--md');
const SAMPLES = 5;
const NB = 8;                        // rotating operands per width
const TARGET_MS = 15;                // min batch duration per sample

const M = await createHydraModule();

const WORD = 8;
const LIMB_BITS = 64n;
const LIMB_MASK = (1n << 64n) - 1n;

function limbCountOf(x) {
  return x === 0n ? 0 : Math.ceil(x.toString(16).length / 16);
}
function pushBig(x, count) {          // mirrors pkg/index.mjs
  const ptr = M.stackAlloc(Math.max(count, 1) * WORD);
  const heap = M.HEAPU64;
  const base = ptr >>> 3;
  for (let i = 0; i < count; i++) {
    heap[base + i] = x & LIMB_MASK;
    x >>= LIMB_BITS;
  }
  return ptr;
}

// deterministic PRNG (mulberry32), as in pkg/test/test.mjs
let seed = 0x5eed;
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

function callOwned(n, nc) {
  const sp = M.stackSave();
  const np = pushBig(n, nc);
  const r = M._hydra_is_perfect_square(np, nc);
  M.stackRestore(sp);
  return r;
}
function callView(n, nc) {
  const sp = M.stackSave();
  const np = pushBig(n, nc);
  const r = M._hydra_is_perfect_square_view(np, nc);
  M.stackRestore(sp);
  return r;
}
function extractOnly(n, nc) {
  const sp = M.stackSave();
  pushBig(n, nc);
  M.stackRestore(sp);
}

// ── correctness gate ─────────────────────────────────────────────────
for (const bits of [128, 256, 512, 1024, 2048, 4096]) {
  for (let t = 0; t < 6; t++) {
    const r = rndBig(bits / 2);
    for (const n of [r * r, r * r + 1n, r * r - 1n, rndBig(bits)]) {
      const nc = limbCountOf(n);
      const a = callOwned(n, nc), b = callView(n, nc);
      if (a !== b) {
        console.error(`MISMATCH at ${bits}b: owned=${a} view=${b}`);
        process.exit(1);
      }
    }
  }
}
console.log('correctness gate: owned == view on squares/±1/random at all widths\n');

// ── measurement ──────────────────────────────────────────────────────
function median(v) { const s = [...v].sort((a, b) => a - b); return s[s.length >> 1]; }

function measureNs(fn) {
  let reps = 1;                       // calibrate to ≥ TARGET_MS per sample
  for (;;) {
    const t0 = performance.now();
    for (let i = 0; i < reps; i++) fn(i);
    const ms = performance.now() - t0;
    if (ms >= TARGET_MS) { reps = Math.max(1, Math.ceil(reps * (TARGET_MS * 1.2) / ms)); break; }
    reps *= 4;
  }
  const medians = [];
  for (let r = 0; r < RUNS; r++) {
    const s = [];
    for (let i = 0; i < SAMPLES; i++) {
      const t0 = performance.now();
      for (let j = 0; j < reps; j++) fn(j);
      s.push((performance.now() - t0) * 1e6 / reps);
    }
    medians.push(median(s));
  }
  const mn = Math.min(...medians);
  const mean = medians.reduce((a, b) => a + b) / medians.length;
  const cv = Math.sqrt(medians.reduce((a, m) => a + (m - mean) ** 2, 0) / (medians.length - 1)) / mean;
  return { ns: mn, cv };
}

const rows = [];
for (const bits of [128, 256, 512, 1024, 2048, 4096]) {
  const ops = Array.from({ length: NB }, () => rndBig(bits));
  const cnts = ops.map(limbCountOf);

  const ext = measureNs((i) => extractOnly(ops[i % NB], cnts[i % NB]));
  const own = measureNs((i) => callOwned(ops[i % NB], cnts[i % NB]));
  const view = measureNs((i) => callView(ops[i % NB], cnts[i % NB]));
  rows.push({ bits, ext, own, view });
}

const fmt = (ns) => ns >= 10000 ? `${(ns / 1000).toFixed(1)} µs` : `${ns.toFixed(0)} ns`;
if (MD) {
  console.log('| bits | T_extract | e2e owned | e2e view | Δe2e | worst CV |');
  console.log('|-----:|----------:|----------:|---------:|-----:|---------:|');
}
for (const { bits, ext, own, view } of rows) {
  const d = (own.ns - view.ns) / own.ns * 100;
  const cv = Math.max(ext.cv, own.cv, view.cv) * 100;
  console.log(MD
    ? `| ${bits} | ${fmt(ext.ns)} | ${fmt(own.ns)} | ${fmt(view.ns)} | ${d >= 0 ? '+' : ''}${d.toFixed(1)}% | ${cv.toFixed(0)}% |`
    : `${String(bits).padStart(4)}b  extract ${fmt(ext.ns).padStart(9)}  owned ${fmt(own.ns).padStart(9)}  view ${fmt(view.ns).padStart(9)}  Δ ${d >= 0 ? '+' : ''}${d.toFixed(1)}%  (CV ${cv.toFixed(0)}%)`);
}
console.log(`\nnode ${process.version} | ${os.cpus()[0].model} | runs=${RUNS}, ${SAMPLES} samples/run, min-of-medians, ≥${TARGET_MS} ms batches`);
