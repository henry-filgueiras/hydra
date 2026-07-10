// hydra-bignum — fast number theory for JavaScript BigInt, powered by
// the Hydra C++ bignum library compiled to WebAssembly.
//
// All functions take and return native `bigint` values; there are no
// wrapper objects and nothing to free.  Call `await init()` once
// before anything else.
//
//   import { init, powMod, isProbablePrime } from 'hydra-bignum';
//   await init();
//   powMod(2n, 2n ** 127n - 2n, 2n ** 127n - 1n);   // Fermat: 1n
//
// Interop shape: numbers cross into wasm as LSB-first u64 limb arrays
// (Hydra's native representation) on the wasm stack, via the
// BigUint64Array heap view.  Sign handling and precondition checks
// live here so the wasm module never throws (it is built EH-free for
// speed — see pkg/hydra_binding.cpp).

import createHydraModule from './dist/hydra_core.mjs';

let M = null;

/** Instantiate the wasm module.  Idempotent. */
export async function init(moduleOptions) {
  if (!M) M = await createHydraModule(moduleOptions);
}

function core() {
  if (!M) throw new Error('hydra-bignum: call `await init()` before use');
  return M;
}

function requireBigInt(name, v) {
  if (typeof v !== 'bigint') {
    throw new TypeError(`hydra-bignum: ${name} must be a bigint, got ${typeof v}`);
  }
}

// ── limb interop ─────────────────────────────────────────────────────
// Buffers live on the wasm stack for the duration of a single call
// (stackSave/stackRestore).  The stack is 8 MiB; cap interop at 4 MiB
// per number, far beyond any workload these routines finish in
// reasonable time anyway.

const WORD = 8;
const LIMB_BITS = 64n;
const LIMB_MASK = (1n << 64n) - 1n;
const MAX_LIMBS = (4 << 20) / WORD;

function limbCountOf(x) {           // x >= 0n
  if (x === 0n) return 0;
  const count = Math.ceil(x.toString(16).length / 16);
  if (count > MAX_LIMBS) {
    throw new RangeError(`hydra-bignum: operand exceeds ${MAX_LIMBS * 64} bits`);
  }
  return count;
}

// Reserve stack space for `count` limbs (>= 1 word so the pointer is
// always valid) and fill it with x's limbs.  Returns the pointer.
function pushBig(m, x, count) {
  const ptr = m.stackAlloc(Math.max(count, 1) * WORD);
  const heap = m.HEAPU64;           // re-fetch every call: memory growth invalidates views
  const base = ptr >>> 3;
  for (let i = 0; i < count; i++) {
    heap[base + i] = x & LIMB_MASK;
    x >>= LIMB_BITS;
  }
  return ptr;
}

function readBig(m, ptr, count) {
  const heap = m.HEAPU64;
  const base = ptr >>> 3;
  let v = 0n;
  for (let i = count - 1; i >= 0; i--) v = (v << LIMB_BITS) | heap[base + i];
  return v;
}

// Run fn inside a wasm-stack frame.
function withStack(fn) {
  const m = core();
  const sp = m.stackSave();
  try {
    return fn(m);
  } finally {
    m.stackRestore(sp);
  }
}

// ── public API ───────────────────────────────────────────────────────

/**
 * (base ** exp) mod mod, without materializing base ** exp.
 * Result is the canonical representative in [0, mod).
 * Throws RangeError on mod <= 0 or exp < 0.
 */
export function powMod(base, exp, mod) {
  requireBigInt('base', base);
  requireBigInt('exp', exp);
  requireBigInt('mod', mod);
  if (mod <= 0n) throw new RangeError('hydra-bignum: modulus must be positive');
  if (exp < 0n) {
    throw new RangeError('hydra-bignum: negative exponent — compose modInverse() with powMod()');
  }
  if (mod === 1n) return 0n;
  base %= mod;
  if (base < 0n) base += mod;
  return withStack((m) => {
    const bc = limbCountOf(base), ec = limbCountOf(exp), mc = limbCountOf(mod);
    const bp = pushBig(m, base, bc);
    const ep = pushBig(m, exp, ec);
    const mp = pushBig(m, mod, mc);
    const op = m.stackAlloc(mc * WORD);
    const n = m._hydra_pow_mod(bp, bc, ep, ec, mp, mc, op) >>> 0;
    return readBig(m, op, n);
  });
}

/**
 * Multiplicative inverse of a modulo m: the x in [0, m) with
 * (a * x) mod m === 1.  Returns null when gcd(a, m) !== 1.
 */
export function modInverse(a, m) {
  requireBigInt('a', a);
  requireBigInt('m', m);
  if (m <= 0n) throw new RangeError('hydra-bignum: modulus must be positive');
  a %= m;
  if (a < 0n) a += m;
  return withStack((mod_) => {
    const ac = limbCountOf(a), mc = limbCountOf(m);
    const ap = pushBig(mod_, a, ac);
    const mp = pushBig(mod_, m, mc);
    const op = mod_.stackAlloc(mc * WORD);
    const n = mod_._hydra_mod_inverse(ap, ac, mp, mc, op) >>> 0;
    if (n === 0xffffffff) return null;
    return readBig(mod_, op, n);
  });
}

/** Greatest common divisor (always >= 0; gcd(0, 0) === 0). */
export function gcd(a, b) {
  requireBigInt('a', a);
  requireBigInt('b', b);
  if (a < 0n) a = -a;
  if (b < 0n) b = -b;
  return withStack((m) => {
    const ac = limbCountOf(a), bc = limbCountOf(b);
    const ap = pushBig(m, a, ac);
    const bp = pushBig(m, b, bc);
    const op = m.stackAlloc(Math.max(ac, bc, 1) * WORD);
    const n = m._hydra_gcd(ap, ac, bp, bc, op) >>> 0;
    return readBig(m, op, n);
  });
}

/**
 * Baillie–PSW primality test (exact below 2^64; no known
 * counterexample above), plus `extraMillerRabinRounds` additional
 * Miller–Rabin rounds for the extra-paranoid.  n < 2 (including
 * negatives) is composite by convention.
 */
export function isProbablePrime(n, extraMillerRabinRounds = 0) {
  requireBigInt('n', n);
  if (!Number.isInteger(extraMillerRabinRounds) || extraMillerRabinRounds < 0) {
    throw new RangeError('hydra-bignum: extraMillerRabinRounds must be a non-negative integer');
  }
  if (n < 2n) return false;
  return withStack((m) => {
    const nc = limbCountOf(n);
    const np = pushBig(m, n, nc);
    return m._hydra_is_probable_prime(np, nc, extraMillerRabinRounds) === 1;
  });
}

/** Smallest prime strictly greater than n. */
export function nextPrime(n) {
  requireBigInt('n', n);
  if (n < 2n) return 2n;
  return withStack((m) => {
    const nc = limbCountOf(n);
    const np = pushBig(m, n, nc);
    const op = m.stackAlloc((nc + 1) * WORD);   // Bertrand: result < 2n
    const c = m._hydra_next_prime(np, nc, op) >>> 0;
    return readBig(m, op, c);
  });
}

/** Floor square root.  Throws RangeError on negative n. */
export function isqrt(n) {
  requireBigInt('n', n);
  if (n < 0n) throw new RangeError('hydra-bignum: isqrt of a negative number');
  return withStack((m) => {
    const nc = limbCountOf(n);
    const np = pushBig(m, n, nc);
    const op = m.stackAlloc((((nc + 1) >> 1) + 1) * WORD);
    const c = m._hydra_isqrt(np, nc, op) >>> 0;
    return readBig(m, op, c);
  });
}

/** True iff n is a perfect square (negatives are not). */
export function isPerfectSquare(n) {
  requireBigInt('n', n);
  if (n < 0n) return false;
  return withStack((m) => {
    const nc = limbCountOf(n);
    const np = pushBig(m, n, nc);
    return m._hydra_is_perfect_square(np, nc) === 1;
  });
}
