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
let initPromise = null;

/**
 * Instantiate the wasm module.  Idempotent and concurrency-safe:
 * simultaneous first calls share a single instantiation (the first
 * caller's `moduleOptions` wins; later options are ignored).  If
 * instantiation fails the cached promise is cleared so init() can be
 * retried.
 */
export function init(moduleOptions) {
  if (!initPromise) {
    initPromise = createHydraModule(moduleOptions).then(
      (m) => { M = m; },
      (err) => { initPromise = null; throw err; },
    );
  }
  return initPromise;
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
// (stackSave/stackRestore).  The stack is 8 MiB (-sSTACK_SIZE in
// scripts/wasm_pkg.sh).  Two limits guard it:
//   - per operand: 4 MiB (MAX_LIMBS), so one number can't eat the stack;
//   - per operation: 6 MiB (OP_BUDGET_LIMBS) summed over ALL buffers a
//     call pushes (operands + result), because several 4 MiB operands
//     could otherwise coexist in one frame and overflow silently.
// The budget sums stackLimbCount() of every buffer — the same rule the
// reservations use — so a zero-valued operand (0 limbs mathematically,
// 1 word reserved) is accounted and the two arithmetics can't drift.
// The remaining ≥2 MiB is headroom for the wasm callee frames, which
// are small (the C++ side heap-allocates its working state; the
// binding's buffers are all caller-provided — see pkg/hydra_binding.cpp).

const WORD = 8;
const LIMB_BITS = 64n;
const LIMB_MASK = (1n << 64n) - 1n;
const MAX_LIMBS = (4 << 20) / WORD;
const OP_BUDGET_LIMBS = (6 << 20) / WORD;

// Stack words a `count`-limb buffer actually reserves: at least one,
// so the pointer is always valid even for zero-valued operands.  Every
// reservation (pushBig, result buffers) AND every checkOpBudget() term
// must go through this helper — that is the whole invariant.
function stackLimbCount(count) {
  return Math.max(count, 1);
}

function limbCountOf(x) {           // x >= 0n
  if (x === 0n) return 0;
  const count = Math.ceil(x.toString(16).length / 16);
  if (count > MAX_LIMBS) {
    throw new RangeError(`hydra-bignum: operand exceeds ${MAX_LIMBS * 64} bits`);
  }
  return count;
}

// Total stack limbs one call will push (operands + result buffer).
function checkOpBudget(totalLimbs) {
  if (totalLimbs > OP_BUDGET_LIMBS) {
    throw new RangeError(
      `hydra-bignum: operation needs ${totalLimbs * WORD} bytes of wasm-stack ` +
      `interop buffers, over the ${OP_BUDGET_LIMBS * WORD}-byte per-operation budget`);
  }
}

// Reserve stack space for `count` limbs (stackLimbCount words) and
// fill it with x's limbs.  Returns the pointer.
function pushBig(m, x, count) {
  const ptr = m.stackAlloc(stackLimbCount(count) * WORD);
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
    const oc = stackLimbCount(mc);               // result buffer (result < mod)
    checkOpBudget(stackLimbCount(bc) + stackLimbCount(ec) + stackLimbCount(mc) + oc);
    const bp = pushBig(m, base, bc);
    const ep = pushBig(m, exp, ec);
    const mp = pushBig(m, mod, mc);
    const op = m.stackAlloc(oc * WORD);
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
    const oc = stackLimbCount(mc);               // result buffer (result < m)
    checkOpBudget(stackLimbCount(ac) + stackLimbCount(mc) + oc);
    const ap = pushBig(mod_, a, ac);
    const mp = pushBig(mod_, m, mc);
    const op = mod_.stackAlloc(oc * WORD);
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
    const oc = stackLimbCount(Math.max(ac, bc)); // result buffer
    checkOpBudget(stackLimbCount(ac) + stackLimbCount(bc) + oc);
    const ap = pushBig(m, a, ac);
    const bp = pushBig(m, b, bc);
    const op = m.stackAlloc(oc * WORD);
    const n = m._hydra_gcd(ap, ac, bp, bc, op) >>> 0;
    return readBig(m, op, n);
  });
}

// Operational cap on extra Miller–Rabin rounds — deliberately far
// below the uint32 ABI limit.  Each round multiplies the false-positive
// bound by <= 1/4, so 64 rounds on top of Baillie–PSW is already
// < 4^-64 = 2^-128, past any cryptographic requirement; more rounds buy
// nothing while each costs a full powMod (~50 ms at 4096 bits under
// wasm — UINT32_MAX rounds is a multi-year DoS footgun).  The native
// signature is also `int`, so values above 2^31 - 1 would go negative
// after the uint32_t ABI parameter narrows.
const MAX_EXTRA_MR_ROUNDS = 64;

/**
 * Baillie–PSW primality test (exact below 2^64; no known
 * counterexample above), plus `extraMillerRabinRounds` additional
 * Miller–Rabin rounds for the extra-paranoid.  n < 2 (including
 * negatives) is composite by convention.
 *
 * `extraMillerRabinRounds` must be an integer in [0, 64]; anything
 * else (fractional, negative, > 64, non-finite, or an unsafe integer)
 * throws RangeError — never clamped, never wrapped.
 */
export function isProbablePrime(n, extraMillerRabinRounds = 0) {
  requireBigInt('n', n);
  if (!Number.isSafeInteger(extraMillerRabinRounds) ||
      extraMillerRabinRounds < 0 || extraMillerRabinRounds > MAX_EXTRA_MR_ROUNDS) {
    throw new RangeError(
      `hydra-bignum: extraMillerRabinRounds must be an integer in [0, ${MAX_EXTRA_MR_ROUNDS}]`);
  }
  if (n < 2n) return false;
  return withStack((m) => {
    const nc = limbCountOf(n);
    checkOpBudget(stackLimbCount(nc));
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
    const oc = nc + 1;                           // Bertrand: result < 2n
    checkOpBudget(stackLimbCount(nc) + oc);
    const np = pushBig(m, n, nc);
    const op = m.stackAlloc(oc * WORD);
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
    const oc = ((nc + 1) >> 1) + 1;              // result buffer
    checkOpBudget(stackLimbCount(nc) + oc);
    const np = pushBig(m, n, nc);
    const op = m.stackAlloc(oc * WORD);
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
    checkOpBudget(stackLimbCount(nc));
    const np = pushBig(m, n, nc);
    return m._hydra_is_perfect_square(np, nc) === 1;
  });
}
