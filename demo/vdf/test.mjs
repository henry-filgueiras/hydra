// demo/vdf/test.mjs — node test suite for the Wesolowski VDF demo core.
//
// Oracles: a pure-BigInt square-and-multiply powmod for tiny cases, the
// one-shot powMod(x, 2^T, N) identity for evaluate, and the algebraic
// identity q·l + r == 2^T for prove.  Deterministic inputs so failures
// reproduce.
//
// Run: node demo/vdf/test.mjs   (build the pkg first: scripts/wasm_pkg.sh)

import assert from 'node:assert/strict';
import {
  init, powMod, nextPrime, gcd, isProbablePrime,
} from '../../pkg/index.mjs';
import { createVdf, bitLength } from './vdf.mjs';

let checks = 0;
function ok(cond, msg) { assert.ok(cond, msg); checks++; }
function eq(a, b, msg) { assert.strictEqual(a, b, msg); checks++; }

await init();
const vdf = createVdf({ powMod, nextPrime, gcd });

function powModJS(b, e, m) {
  b %= m; let r = 1n;
  while (e > 0n) {
    if (e & 1n) r = (r * b) % m;
    b = (b * b) % m;
    e >>= 1n;
  }
  return r;
}

// Deterministic 512-bit modulus: two fixed 256-bit primes.
const P = nextPrime((1n << 255n) | 0x1234_5678_9abc_def0n);
const Q = nextPrime((1n << 255n) | 0x0fed_cba9_8765_4321n);
const N = P * Q;

// ── bitLength ─────────────────────────────────────────────────────────
eq(bitLength(0n), 0, 'bitLength(0)');
eq(bitLength(1n), 1, 'bitLength(1)');
eq(bitLength(255n), 8, 'bitLength(255)');
eq(bitLength(256n), 9, 'bitLength(256)');
eq(bitLength(1n << 4095n), 4096, 'bitLength(2^4095)');

// ── deriveInput ───────────────────────────────────────────────────────
{
  const x1 = await vdf.deriveInput(N, 'hello, hydra');
  const x2 = await vdf.deriveInput(N, 'hello, hydra');
  const x3 = await vdf.deriveInput(N, 'hello, hydrb');
  eq(x1, x2, 'deriveInput is deterministic');
  ok(x1 !== x3, 'deriveInput depends on the seed');
  ok(x1 >= 2n && x1 < N, 'deriveInput lands in [2, N)');
  eq(gcd(x1, N), 1n, 'deriveInput is coprime to N');
}

// ── deriveChallenge ───────────────────────────────────────────────────
{
  const l1 = await vdf.deriveChallenge(N, 3n, 5n, 1024);
  const l2 = await vdf.deriveChallenge(N, 3n, 5n, 1024);
  eq(l1, l2, 'challenge is deterministic');
  ok(isProbablePrime(l1), 'challenge is prime');
  ok(bitLength(l1) >= 256, 'challenge has its top bit pinned (~256-bit)');
  ok(l1 !== await vdf.deriveChallenge(N, 4n, 5n, 1024), 'challenge binds x');
  ok(l1 !== await vdf.deriveChallenge(N, 3n, 6n, 1024), 'challenge binds y');
  ok(l1 !== await vdf.deriveChallenge(N, 3n, 5n, 1025), 'challenge binds T');
  // Length-prefixed transcript: (x=0x0102, y=0x03) vs (x=0x01, y=0x0203)
  // must not collide.
  ok(await vdf.deriveChallenge(N, 0x0102n, 0x03n, 8) !==
     await vdf.deriveChallenge(N, 0x01n, 0x0203n, 8),
     'transcript is length-prefixed (no concatenation ambiguity)');
}

// ── evaluate ──────────────────────────────────────────────────────────
{
  // Tiny case against the pure-JS oracle.
  const y = await vdf.evaluate(1000003n, 7n, 20, { chunkBits: 3 });
  eq(y, powModJS(7n, 1n << 20n, 1000003n), 'evaluate matches JS oracle (tiny)');

  // One-shot identity at full width, plus chunk-size invariance.
  const x = await vdf.deriveInput(N, 'eval');
  const T = 4096;
  const y1 = await vdf.evaluate(N, x, T, { chunkBits: 64 });
  const y2 = await vdf.evaluate(N, x, T, { chunkBits: 1000 });   // non-divisor
  const y3 = await vdf.evaluate(N, x, T);
  eq(y1, powMod(x, 1n << BigInt(T), N), 'evaluate == one-shot x^(2^T) mod N');
  eq(y1, y2, 'evaluate is chunk-size invariant (64 vs 1000)');
  eq(y1, y3, 'evaluate is chunk-size invariant (default)');

  // Progress callback: monotonic, awaited, ends at T.
  const seen = [];
  await vdf.evaluate(N, x, 100, {
    chunkBits: 32,
    onProgress: async (done, total) => { seen.push([done, total]); },
  });
  eq(seen.map(([d]) => d).join(','), '32,64,96,100', 'progress is monotonic to T');
  ok(seen.every(([, t]) => t === 100), 'progress total is T');
}

// ── prove + verify round trip ─────────────────────────────────────────
{
  const T = 4096;
  const x = await vdf.deriveInput(N, 'round-trip');
  const y = await vdf.evaluate(N, x, T);
  const proof = await vdf.prove(N, x, y, T);

  // Algebraic identity behind the protocol: q·l + r == 2^T.
  const r = powMod(2n, BigInt(T), proof.l);
  eq(((1n << BigInt(T)) / proof.l) * proof.l + r, 1n << BigInt(T),
     'q·l + r == 2^T');
  eq(proof.l, await vdf.deriveChallenge(N, x, y, T), 'proof carries the FS challenge');

  ok(await vdf.verify(N, x, y, T, proof), 'honest proof verifies');

  // Prover chunk-size invariance (odd chunk, nibble rounding path).
  const p2 = await vdf.prove(N, x, y, T, { chunkBits: 100 });
  const p3 = await vdf.prove(N, x, y, T, { chunkBits: 1 });
  eq(proof.pi, p2.pi, 'prove is chunk-size invariant (100)');
  eq(proof.pi, p3.pi, 'prove is chunk-size invariant (1 → nibble)');

  // Rejections: every tampered transcript or malformed proof fails.
  ok(!await vdf.verify(N, x, y + 1n, T, proof), 'rejects tampered y');
  ok(!await vdf.verify(N, x, y, T, { pi: proof.pi + 1n }), 'rejects tampered π');
  ok(!await vdf.verify(N, x, y, T - 1, proof), 'rejects understated T');
  ok(!await vdf.verify(N, x + 1n, y, T, proof), 'rejects different x');
  ok(!await vdf.verify(N, x, y, T, { pi: 0n }), 'rejects π = 0');
  ok(!await vdf.verify(N, x, y, T, { pi: N }), 'rejects π >= N');
  ok(!await vdf.verify(N, x, y, T, { pi: '7' }), 'rejects non-bigint π');
  ok(!await vdf.verify(N, x, 0n, T, proof), 'rejects y = 0');
}

// ── T < |l| edge: q = 0, π = 1 ────────────────────────────────────────
{
  const T = 16;                       // 2^16 < l (~2^256) → q = 0
  const x = await vdf.deriveInput(N, 'tiny-T');
  const y = await vdf.evaluate(N, x, T);
  const proof = await vdf.prove(N, x, y, T);
  eq(proof.pi, 1n, 'q = 0 ⇒ π = 1');
  ok(await vdf.verify(N, x, y, T, proof), 'q = 0 proof verifies');
}

// ── generateModulus ───────────────────────────────────────────────────
{
  const M = await vdf.generateModulus(512);
  ok(bitLength(M) >= 511 && bitLength(M) <= 512, 'modulus is ~512-bit');
  eq(M & 1n, 1n, 'modulus is odd');
  ok(!isProbablePrime(M), 'modulus is composite (p·q)');
}

// ── realistic width: 2048-bit N, T = 2^14 ─────────────────────────────
{
  const M = P * Q * nextPrime((1n << 511n) | 0xdeadbeefn) *
            nextPrime((1n << 511n) | 0xcafef00dn);   // 2046-bit composite
  const T = 1 << 14;
  const x = await vdf.deriveInput(M, 'flagship');
  const y = await vdf.evaluate(M, x, T);
  const proof = await vdf.prove(M, x, y, T);
  ok(await vdf.verify(M, x, y, T, proof), '2048-bit round trip verifies');
  ok(!await vdf.verify(M, x, y, T + 1, proof), '2048-bit rejects wrong T');
}

console.log(`vdf: all ${checks} checks passed`);
