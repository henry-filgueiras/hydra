# hydra-bignum

Fast number theory for JavaScript `BigInt` — the missing `powMod`,
plus modular inverse, Baillie–PSW primality, next-prime, and integer
square root.  Powered by [Hydra](https://github.com/henry-filgueiras/hydra),
a C++20 bignum library, compiled to WebAssembly (~96 KB total, no
dependencies).

JavaScript's `BigInt` has no modular exponentiation: `(b ** e) % m`
materializes `b ** e` (astronomically large), and the hand-rolled
square-and-multiply loop everyone writes instead leaves 1.3–2.6×
on the table (below).  There is also no built-in `modInverse`,
primality test, or `isqrt`.  This package fills those gaps with one
`await init()` and plain `bigint → bigint` functions — no wrapper
objects, nothing to free.

```js
import { init, powMod, modInverse, isProbablePrime, nextPrime } from 'hydra-bignum';
await init();

powMod(2n, 2n ** 127n - 2n, 2n ** 127n - 1n);  // 1n  (Fermat on M127)
modInverse(17n, 2n ** 255n - 19n);             // 60992...n  (RSA/ECC-style inverse)
isProbablePrime(2n ** 521n - 1n);              // true (Baillie–PSW)
nextPrime(2n ** 64n);                          // 18446744073709551629n
```

## API

All numeric arguments and results are native `bigint`.

| Function | Semantics |
|---|---|
| `init(opts?)` | instantiate the wasm module (await once, idempotent) |
| `powMod(b, e, m)` | `(b ** e) mod m`, result in `[0, m)`; throws on `m <= 0` or `e < 0` |
| `modInverse(a, m)` | `x` with `(a * x) % m === 1n`, or `null` if `gcd(a, m) !== 1n` |
| `gcd(a, b)` | greatest common divisor, always `>= 0n` |
| `isProbablePrime(n, extraRounds?)` | Baillie–PSW (exact < 2^64, no known counterexample) + optional extra Miller–Rabin rounds — integer in `[0, 64]`, else `RangeError` (64 rounds already bound the error below 2⁻¹²⁸) |
| `nextPrime(n)` | smallest prime `> n` |
| `isqrt(n)` | floor square root; throws on negative |
| `isPerfectSquare(n)` | `true` iff `n` is a perfect square |

## Performance vs native BigInt

`powMod` against the standard square-and-multiply loop over native
`BigInt` (V8's own optimized bignum), node 26, Apple M5 Pro,
min-of-medians over 6 runs (`pkg/bench/bench_vs_bigint.mjs`):

| Width | native `BigInt` | hydra-bignum (wasm) | speedup |
|------:|----------------:|--------------------:|--------:|
| 256-bit | 48.3 µs | 18.3 µs | **2.6×** |
| 512-bit | 204.9 µs | 102.3 µs | **2.0×** |
| 1024-bit | 1.13 ms | 732.9 µs | **1.5×** |
| 2048-bit | 7.24 ms | 5.57 ms | **1.3×** |
| 4096-bit | 37.38 ms | 48.66 ms | 0.8× |

Honest numbers: Hydra's Montgomery engine wins up to 2048-bit — the
RSA/DH/VDF verification band.  At 4096-bit V8's subquadratic
multiplication overtakes Hydra's O(k²) kernels under wasm; if your
workload lives there, native `BigInt` is currently faster (Toom-Cook
is on Hydra's roadmap).  `isProbablePrime`/`nextPrime` latency is
essentially `powMod` latency, so the same wins carry over.

## Scope & caveats

- **Variable-time by design.**  Hydra is built for *public-input*
  workloads — verification, VDFs, primality, accumulators.  Do not
  use it on secret keys or other secret data; timing reveals operand
  values.  (Native `BigInt` is not constant-time either.)
- Requires `BigUint64Array` and wasm BigInt integration: node ≥ 18
  and all evergreen browsers.
- Works in workers.  The `.wasm` file ships alongside the ES-module
  glue and is resolved via `import.meta.url` (bundlers handle this).

## Building from source

```sh
git clone https://github.com/henry-filgueiras/hydra && cd hydra
scripts/wasm_bootstrap.sh     # emscripten toolchain (macOS/Homebrew)
scripts/wasm_pkg.sh --test    # build pkg/dist + run the test suite
node pkg/bench/bench_vs_bigint.mjs --runs 6 --md
```

MIT.  The wasm module contains only Hydra code — no GMP, no LGPL
components.
