// Type definitions for hydra-bignum.
// All numeric arguments and results are native `bigint`.

/**
 * Instantiate the WebAssembly module.  Must be awaited once before any
 * other call.  Idempotent and concurrency-safe: simultaneous first
 * calls share one instantiation (the first caller's `moduleOptions`
 * wins).  If instantiation fails, init() can be called again to retry.
 * `moduleOptions` is passed through to the Emscripten module factory
 * (rarely needed).
 */
export function init(moduleOptions?: Record<string, unknown>): Promise<void>;

/**
 * (base ** exp) mod mod, computed without materializing base ** exp
 * (Montgomery ladder for odd moduli).  Result is in [0, mod).
 *
 * @throws RangeError if mod <= 0n or exp < 0n.
 */
export function powMod(base: bigint, exp: bigint, mod: bigint): bigint;

/**
 * Multiplicative inverse of a modulo m: the x in [0, m) with
 * (a * x) % m === 1n.  Returns null when gcd(a, m) !== 1n.
 *
 * @throws RangeError if m <= 0n.
 */
export function modInverse(a: bigint, m: bigint): bigint | null;

/** Greatest common divisor (always >= 0n; gcd(0n, 0n) === 0n). */
export function gcd(a: bigint, b: bigint): bigint;

/**
 * Baillie–PSW primality test — exact below 2^64, no known
 * counterexample above — plus optional extra Miller–Rabin rounds.
 * n < 2n (including negatives) returns false.
 *
 * `extraMillerRabinRounds` must be an integer in [0, 64] (each round
 * multiplies the false-positive bound by 1/4, so 64 already gives
 * < 2^-128; larger values only burn CPU).
 *
 * @throws RangeError if extraMillerRabinRounds is fractional,
 * negative, greater than 64, or not a safe integer — never clamped.
 */
export function isProbablePrime(
  n: bigint,
  extraMillerRabinRounds?: number,
): boolean;

/** Smallest prime strictly greater than n. */
export function nextPrime(n: bigint): bigint;

/** Floor square root.  @throws RangeError if n < 0n. */
export function isqrt(n: bigint): bigint;

/** True iff n is a perfect square (negatives are not). */
export function isPerfectSquare(n: bigint): boolean;
