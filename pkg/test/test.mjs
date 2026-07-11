// pkg/test/test.mjs — node test suite for the hydra-bignum wrapper.
//
// Oracles are independent pure-BigInt implementations (square-and-
// multiply powmod, Euclid gcd), plus literature landmarks for the
// primality layer.  Deterministic PRNG so failures reproduce.
//
// Run: node pkg/test/test.mjs   (build first: scripts/wasm_pkg.sh)

import assert from 'node:assert/strict';
import {
  init, powMod, modInverse, gcd,
  isProbablePrime, nextPrime, isqrt, isPerfectSquare,
} from '../index.mjs';

let checks = 0;
function ok(cond, msg) { assert.ok(cond, msg); checks++; }
function eq(a, b, msg) { assert.strictEqual(a, b, msg); checks++; }

// ── deterministic PRNG (mulberry32) → random bigints ────────────────
let seed = 0x5eed;
function rnd32() {
  seed |= 0; seed = (seed + 0x6d2b79f5) | 0;
  let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
  t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
  return ((t ^ (t >>> 14)) >>> 0);
}
function rndBig(bits) {           // exactly `bits` bits (top bit set)
  let v = 1n;
  for (let b = 1; b < bits; b += 32) {
    const take = Math.min(32, bits - b);
    v = (v << BigInt(take)) | BigInt(rnd32() >>> (32 - take));
  }
  return v;
}

// ── oracles ──────────────────────────────────────────────────────────
function powModJS(b, e, m) {
  if (m === 1n) return 0n;
  b %= m; if (b < 0n) b += m;
  let r = 1n;
  while (e > 0n) {
    if (e & 1n) r = (r * b) % m;
    b = (b * b) % m;
    e >>= 1n;
  }
  return r;
}
function gcdJS(a, b) {
  a = a < 0n ? -a : a; b = b < 0n ? -b : b;
  while (b) { [a, b] = [b, a % b]; }
  return a;
}

// ── init() concurrency ───────────────────────────────────────────────
// Simultaneous first calls must share one instantiation (the wrapper
// caches the promise, not the resolved module).
{
  const p1 = init();
  const p2 = init();
  ok(p1 === p2, 'concurrent init() calls share the same promise');
  await Promise.all([p1, p2]);
  ok(init() === p1, 'init() after resolution returns the cached promise');
}

await init();

// ── per-operation wasm-stack budget ──────────────────────────────────
// Operands individually under the 4 MiB per-number cap, but whose
// combined interop buffers exceed the 6 MiB per-operation budget, must
// throw instead of silently overflowing the 8 MiB wasm stack.
{
  const big = 1n << (21n << 20n);         // ~2.6 MiB of limbs
  assert.throws(() => powMod(2n, big, big + 1n), RangeError,
                'powMod over per-operation stack budget throws');
  checks++;
  assert.throws(() => isProbablePrime(1n << (33n << 20n)), RangeError,
                'operand over per-number cap throws');
  checks++;
  // Zero-valued operands reserve one stack word each (pushBig allocates
  // max(count, 1)); the budget must count the same.  393216-limb mod:
  // 2 * 393216 raw limbs sat exactly at the budget under the old
  // accounting, but the two 1-word reservations for base = exp = 0n
  // push the real allocation over it.
  const mod = 1n << (64n * 393216n - 1n);
  assert.throws(() => powMod(0n, 0n, mod), RangeError,
                'zero-valued operands count toward the stack budget');
  checks++;
}

// ── zero operands cross the boundary correctly ───────────────────────
eq(powMod(0n, 0n, 7n), 1n, '0^0 mod 7 == 1 (pow_mod convention)');
eq(modInverse(0n, 1n), 0n, 'modInverse(0, 1) == 0 (everything is 0 mod 1)');

// ── powMod ───────────────────────────────────────────────────────────
for (const bits of [8, 64, 65, 128, 192, 256, 521, 1024, 2048]) {
  for (let i = 0; i < 4; i++) {
    const m = rndBig(bits) | (i % 2 === 0 ? 1n : 0n);  // odd and even moduli
    const b = rndBig(Math.max(2, bits - 3));
    const e = rndBig(bits);
    const got = powMod(b, e, m);
    eq(got, powModJS(b, e, m), `powMod @${bits}b sample ${i}`);
    ok(got >= 0n && got < m, 'powMod result in [0, mod)');
  }
}
eq(powMod(0n, 12345n, 97n), 0n, 'zero base');
eq(powMod(5n, 0n, 97n), 1n, 'zero exponent -> 1');
eq(powMod(5n, 0n, 1n), 0n, 'mod 1 -> 0');
eq(powMod(-2n, 3n, 97n), powModJS(-2n, 3n, 97n), 'negative base normalized');
eq(powMod(2n, 2n ** 127n - 2n, 2n ** 127n - 1n), 1n, 'Fermat on M127');
assert.throws(() => powMod(2n, -1n, 7n), RangeError, 'negative exponent throws');
assert.throws(() => powMod(2n, 3n, 0n), RangeError, 'zero modulus throws');
assert.throws(() => powMod(2, 3n, 7n), TypeError, 'non-bigint throws');
checks += 3;

// ── modInverse ───────────────────────────────────────────────────────
const P256 = 2n ** 255n - 19n;
for (let i = 0; i < 8; i++) {
  const a = rndBig(200 + i);
  const inv = modInverse(a, P256);
  ok(inv !== null && (a * inv) % P256 === 1n, `modInverse vs prime, sample ${i}`);
}
eq(modInverse(6n, 9n), null, 'no inverse when gcd != 1');
eq(modInverse(0n, 5n), null, 'zero has no inverse');
eq(modInverse(1n, 7n), 1n, 'inverse of 1');
eq(modInverse(-3n, 7n), modInverse(4n, 7n), 'negative a normalized');
assert.throws(() => modInverse(3n, -7n), RangeError, 'non-positive modulus throws');
checks++;

// ── gcd ──────────────────────────────────────────────────────────────
for (let i = 0; i < 10; i++) {
  const a = rndBig(64 + 17 * i), b = rndBig(200 - 11 * i);
  eq(gcd(a, b), gcdJS(a, b), `gcd sample ${i}`);
}
eq(gcd(0n, 0n), 0n, 'gcd(0,0)');
eq(gcd(0n, 42n), 42n, 'gcd(0,b)');
eq(gcd(-12n, 18n), 6n, 'gcd of negatives');
const g = 2n ** 89n - 1n;                       // prime
eq(gcd(g * 12n, g * 35n), g, 'large common factor');

// ── isProbablePrime ──────────────────────────────────────────────────
const primes = [2n, 3n, 97n, 2n ** 61n - 1n, 2n ** 127n - 1n, P256, 2n ** 521n - 1n];
for (const p of primes) ok(isProbablePrime(p), `prime: ${p.toString().slice(0, 24)}…`);
const composites = [
  0n, 1n, -7n, 4n, 561n,                        // Carmichael
  3215031751n,                                  // strong pseudoprime bases 2,3,5,7
  2n ** 67n - 1n,                               // composite Mersenne
  (2n ** 61n - 1n) * (2n ** 89n - 1n),          // semiprime of two Mersenne primes
];
for (const c of composites) ok(!isProbablePrime(c), `composite: ${c.toString().slice(0, 24)}…`);
ok(isProbablePrime(2n ** 127n - 1n, 5), 'extra MR rounds, prime verdict stable');
ok(!isProbablePrime(561n, 5), 'extra MR rounds, composite verdict stable');

// extraMillerRabinRounds contract: integer in [0, 64], RangeError otherwise
// (never clamped or wrapped at the uint32 ABI boundary).
ok(isProbablePrime(97n, 0), 'rounds boundary: 0 accepted');
ok(isProbablePrime(97n, 1), 'rounds boundary: 1 accepted');
ok(isProbablePrime(2n ** 61n - 1n, 64), 'rounds boundary: 64 (cap) accepted');
ok(!isProbablePrime(561n, 64), 'composite verdict stable at the cap');
for (const bad of [
  -1, 65,                                 // just outside [0, 64]
  0xffffffff,                             // UINT32_MAX: ABI-valid, operationally rejected
  0x100000000,                            // 2^32: would wrap to 0 in uint32
  Number.MAX_SAFE_INTEGER,                // 2^53 - 1: would wrap to 4294967295
  Infinity, NaN, 0.5, 1.5, -0.5,          // non-integers
]) {
  assert.throws(() => isProbablePrime(7n, bad), RangeError,
                `rounds rejected: ${String(bad)}`);
  checks++;
}

// ── nextPrime ────────────────────────────────────────────────────────
eq(nextPrime(-100n), 2n, 'nextPrime below 2');
eq(nextPrime(2n), 3n, 'nextPrime(2)');
eq(nextPrime(2n ** 64n), 2n ** 64n + 13n, 'landmark 2^64 + 13');
eq(nextPrime(2n ** 256n), 2n ** 256n + 297n, 'landmark 2^256 + 297');

// ── isqrt / isPerfectSquare ──────────────────────────────────────────
eq(isqrt(0n), 0n, 'isqrt(0)');
eq(isqrt(15n), 3n, 'isqrt(15)');
for (let i = 0; i < 8; i++) {
  const n = rndBig(80 + 60 * i);
  const s = isqrt(n);
  ok(s * s <= n && (s + 1n) * (s + 1n) > n, `isqrt floor property, sample ${i}`);
  ok(isPerfectSquare(s * s), 'square of isqrt is a perfect square');
}
ok(!isPerfectSquare(-4n), 'negatives are not squares');
ok(isPerfectSquare(0n) && isPerfectSquare(1n), '0 and 1 are squares');
ok(!isPerfectSquare((2n ** 100n + 3n) ** 2n - 1n), 'off-by-one non-square');
assert.throws(() => isqrt(-1n), RangeError, 'isqrt(-1) throws');
checks++;

// ── uninitialized-use guard (fresh import path not testable here; API guard only)
console.log(`hydra-bignum: all ${checks} checks passed`);
