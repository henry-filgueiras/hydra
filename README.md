# 🐍 Hydra

![Hydra Sigil](assets/hydra_sigil.svg)

[![CI](https://github.com/henry-filgueiras/hydra/actions/workflows/ci.yml/badge.svg)](https://github.com/henry-filgueiras/hydra/actions/workflows/ci.yml)

**A high-performance arbitrary-precision integer library for modern C++**
*competitive modular arithmetic · clean systems-first architecture · small values move at native speed*

Hydra is an experimental **multi-representation integer runtime** designed to preserve the speed of native 64-bit arithmetic while scaling seamlessly into arbitrary precision — with competitive performance on modular exponentiation workloads.

The core idea is simple:

* **small values** stay in the fast machine-word path
* **medium values** use inline fixed limbs
* **large values** spill into a tail-allocated heap representation
* **results normalize downward** into the smallest valid storage class

In other words:

> *pay for complexity only when the value actually needs it*

---

## Quick Example

Toy RSA in six lines — encrypt, decrypt, verify:

```cpp
#include <iostream>
#include "hydra.hpp"
using namespace hydra;

int main() {
    Hydra n("3233");
    Hydra e(17);
    Hydra d(2753);

    Hydra message(65);

    Hydra ciphertext = pow_mod(message, e, n);
    Hydra recovered  = pow_mod(ciphertext, d, n);

    std::cout << "message:    " << message    << "\n";
    std::cout << "ciphertext: " << ciphertext << "\n";
    std::cout << "recovered:  " << recovered  << "\n";
}
```

```text
message:    65
ciphertext: 2790
recovered:  65
```

**Getting Hydra** — it's one header.  Copy `hydra.hpp` into your
project, or via CMake:

```cmake
include(FetchContent)
FetchContent_Declare(hydra
  GIT_REPOSITORY https://github.com/henry-filgueiras/hydra.git
  GIT_TAG        main)
FetchContent_MakeAvailable(hydra)          # exports just hydra_core —
target_link_libraries(app PRIVATE hydra_core)  # no test/bench deps pulled
```

Arbitrary-precision arithmetic just works:

```cpp
Hydra a("123456789012345678901234567890");
Hydra b("-18446744073709551616");

std::cout << a + b << "\n";   // 123456788993898934827525016274
std::cout << gcd(a, b) << "\n";  // 2
```

Primality testing built on the same engine (Baillie–PSW — no known
counterexample, exact below 2⁶⁴):

```cpp
Hydra p = (Hydra(1) << 255) - 19;          // curve25519 field prime
is_probable_prime(p);                      // true, ~0.3 ms
next_prime(Hydra(1) << 256);               // 2^256 + 297, ~0.4 ms
```

---

## ⚡ Performance Snapshot — Modular Exponentiation

<p align="center">
  <img src="assets/hydra_powmod_bench.svg" alt="pow_mod benchmark: Hydra vs Boost, GMP, OpenSSL" width="720">
</p>

Median latency for `pow_mod(base, exp, mod)` across bit widths — single core:

| Width | Hydra | GMP | OpenSSL | Hydra / GMP |
|------:|:-----:|:---:|:-------:|:-----------:|
|  256  | **7.4 µs**  | 7.2 µs  | 5.3 µs  | 1.03× |
| 1024  | **218 µs**  | 153 µs  | 110 µs  | 1.43× |
| 2048  | **1.80 ms** | 1.09 ms | 783 µs  | 1.65× |
| 4096  | **14.5 ms** | 7.5 ms  | 5.8 ms  | 1.94× |

_Provenance: Hydra column measured 2026-07-10 on Apple M5 Pro (arm64,
macOS), Apple clang, Release `-O2`, via `build-rel/bench_pow_mod
--runs 6` — min of per-run medians, 50 samples/run, random top-bit-set
operands with odd moduli; cross-run CV ≈ 1 % at 2048-bit, 0.3 % at
4096-bit.  The GMP and OpenSSL columns are **carried forward from the
2026-04-18 run** of the same benchmark on the same machine (Homebrew
builds; those backends were not rebuilt in the 2026-07-10 pass), so
treat the cross-library ratios as approximate rather than same-run.
Boost.Multiprecision `cpp_int`, from that same 2026-04-18 run, is
3–4.5× slower than current Hydra across 256–4096-bit.  Full tables and
history: [perf_snapshot.md](perf_snapshot.md)._

Hydra currently delivers:

- **3–4.5× faster than Boost.Multiprecision `cpp_int`** across 256–4096-bit widths
- **parity with GMP at 256-bit**, within **2× of GMP at 4096-bit** (hand-tuned C/asm, decades of optimization)
- within **1.4×–2.5× of OpenSSL** (assembly-optimized big-number core)

Achieved via Montgomery reduction (FIOS with halved squaring), Karatsuba multiplication dispatch, and an adaptive sliding window — all in portable C++20 with zero assembly.

<details>
<summary>Full micro-benchmark table (add, mul, shift, div) — older snapshot (2026-04), retained for the small/medium-tier picture; pow_mod numbers above are current</summary>

| Operation             | Hydra      | Reference                      | Δ vs reference |
| --------------------- | ---------- | ------------------------------ | -------------- |
| small add             | 3.20 ns    | `uint64_t` 2.49 ns             | +28.1%         |
| small mul             | 4.25 ns    | `uint64_t` 3.55 ns             | +19.9%         |
| widening add          | 3.17 ns    | Boost 11.49 ns                 | −72.4%         |
| widening mul 128-bit  | 0.78 ns    | Boost 9.31 ns                  | −91.6%         |
| medium add            | 5.99 ns    | Boost 13.10 ns                 | −54.3%         |
| medium mul            | 15.30 ns   | Boost 15.55 ns                 | −1.6%          |
| large add 128-bit     | 5.50 ns    | Boost 13.10 ns                 | −58.0%         |
| large add 256-bit     | 13.44 ns   | Boost 13.01 ns                 | +3.3%          |
| large add 512-bit     | 13.55 ns   | Boost 23.98 ns                 | −43.5%         |
| large mul 128-bit     | 15.39 ns   | Boost 15.55 ns                 | −1.0%          |
| large mul 256-bit     | 19.69 ns   | Boost 19.28 ns                 | +2.1%          |
| large mul 512-bit     | 37.13 ns   | Boost 31.43 ns                 | +18.1%         |
| chain large add 64-limb | 394.5 ns | Boost 426.5 ns                 | −7.5%          |

</details>

### 🌐 WebAssembly — fastest bignum you can ship in a browser

**[▶ Live demo](https://henry-filgueiras.github.io/hydra/)** — run the
shootout on your own device, generate primes, round-trip a message
through toy RSA.  Deployed automatically from `main` by CI.

Native GMP's speed comes from per-architecture assembly, which doesn't
exist for wasm32 — compile GMP-dependent code to WebAssembly and you
get **mini-gmp**, its portable fallback.  Hydra's performance story is
pure portable C++, so it carries to wasm nearly intact.  Head-to-head
under node (`pow_mod`, median latency, `scripts/wasm_bench.sh` —
LLVM `-O2`, identical pipeline for all three backends; emcc's default
post-link `wasm-opt` pass is skipped, it costs Hydra up to 60 %):

| Width | **Hydra (wasm)** | Boost cpp_int (wasm) | mini-gmp (wasm) | vs Boost | vs mini-gmp |
|------:|-----------------:|---------------------:|----------------:|---------:|------------:|
|  256  |    **18.4 µs**   |        72.5 µs       |     93.0 µs     |   3.9×   |    5.0×     |
| 1024  |    **722 µs**    |        2.46 ms       |     5.09 ms     |   3.4×   |    7.0×     |
| 2048  |    **5.61 ms**   |        17.3 ms       |     41.3 ms     |   3.1×   |    7.4×     |
| 4096  |    **48.2 ms**   |        126 ms        |      323 ms     |   2.6×   |    6.7×     |

The full 989-test suite passes under wasm with zero source changes
(`scripts/wasm_bootstrap.sh --full`; CI runs it on every push).
Try it yourself: `scripts/wasm_demo.sh --serve` builds a
self-contained page (`demo/`) that runs this exact shootout live in
your browser — three backends, cross-checked for agreement before
timing, ~150 KB total.  (The demo still builds with emcc's default
pipeline, so its on-screen Hydra numbers — and this screenshot — are
conservative relative to the table above until it's migrated):

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/demo_reference_dark.png">
  <img src="assets/demo_reference_light.png"
       alt="In-browser benchmark demo: grouped bars per width showing Hydra 2.6–3.5× faster than Boost cpp_int and 4.2–6.3× faster than mini-gmp from 256-bit to 4096-bit"
       width="760">
</picture>

### 📦 `hydra-bignum` — JavaScript's missing `powMod`, as an npm package

JavaScript `BigInt` ships no modular exponentiation, no `modInverse`,
no primality test.  [`pkg/`](pkg/) wraps Hydra's wasm build in a pure
`bigint → bigint` API (nothing to allocate or free) and beats the
square-and-multiply loop you'd write over native `BigInt` — V8's own
optimized bignum — by **2.6× at 256-bit through 1.3× at 2048-bit**
(at 4096-bit V8's subquadratic multiply wins; table and caveats in
[pkg/README.md](pkg/README.md)):

```js
import { init, powMod, isProbablePrime, nextPrime } from 'hydra-bignum';
await init();
powMod(2n, 2n ** 127n - 2n, 2n ** 127n - 1n);  // 1n — Fermat on M127
nextPrime(2n ** 64n);                          // 18446744073709551629n
```

Build locally with `scripts/wasm_pkg.sh --test` (~96 KB shipped, MIT,
no GMP inside).

---

## Visual Hydra Performance Story

<p align="center">
  <img src="assets/hydra_perf_story.svg" alt="Hydra performance story" width="100%">
</p>

---

## ✨ Design Goals

Hydra is built around five principles:

### 1) Fast-path sanctity ⚡

Operations on values that fit in 64 bits should compile down to **native arithmetic instructions whenever possible**.

The hot path should look and feel like:

```cpp
Hydra a = 42;
Hydra b = 1337;
Hydra c = a + b;
```

with performance close to:

```cpp
uint64_t c = a + b;
```

---

### 2) Tiered storage heads 🐉

Hydra uses multiple internal representations:

| Head       | Storage              | Use case                                     |
| ---------- | -------------------- | -------------------------------------------- |
| **Small**  | inline 64-bit        | counters, IDs, most arithmetic               |
| **Medium** | inline limbs         | overflow products, fixed-width intermediates |
| **Large**  | tail-allocated limbs | arbitrary precision                          |

This avoids the performance cliff between:

```text
u64 → heap bigint
```

and instead creates a smoother ladder:

```text
u64 → inline limbs → heap bigint
```

---

### 3) Canonical normalization 🧬

Every value is always stored in the **smallest valid representation**.

Examples:

* large result shrinks back to medium
* medium result shrinks back to small
* zero has exactly one canonical form

This keeps equality, hashing, and serialization sane.

---

### 4) Ownership safety 🛡️

Large representations use **tail allocation**:

```text
[ header | limbs... ]
```

to avoid double heap allocations and improve locality.

Temporary heap ownership uses RAII guards internally to remain exception-safe.

---

### 5) Explicit kernel dispatch 🎯

Binary operations dispatch by representation pair:

```text
Small + Small
Small + Medium
Medium + Large
...
```

allowing specialized arithmetic kernels for each case.

Conceptually:

```cpp
add(lhs_kind, rhs_kind)
```

routes into a 2D dispatch matrix.

---

## 🧠 Why Hydra?

Most bigint implementations make a tradeoff:

* either excellent arbitrary precision
* or excellent machine-word performance

Hydra tries to preserve both.

The goal is to make common arithmetic boringly fast while still allowing:

```cpp
Hydra x = factorial(1000);
```

without changing types.

---

## 🔥 Current Architecture

```text
Hydra
├── metadata word (kind / flags / reserved)
├── Small   → inline 64-bit
├── Medium  → inline limb array
└── Large   → pointer to tail-allocated LargeRep
```

Large head layout:

```text
[ used | capacity | limbs... ]
```

---

---

## 🚧 Status

Active development — core arithmetic is implemented and benchmarked.

Completed:

* [x] representation contract (three-tier Small / Medium / Large)
* [x] move / copy correctness
* [x] normalization rules
* [x] addition / subtraction kernels
* [x] multiplication (widening, hand-unrolled 256-bit and 512-bit kernels)
* [x] bit-shift operators (`<<` / `>>`)
* [x] full Hydra÷Hydra division via Knuth Algorithm D (`divmod` / `div` / `mod`)
* [x] signed arithmetic (sign-magnitude representation)
* [x] native interop (implicit conversion from all integral types)
* [x] string parse / format with chunked base-10¹⁸ extraction
* [x] Karatsuba multiplication (production-dispatched at ≥32 limbs)
* [x] number theory primitives (`gcd`, `extended_gcd`, `pow_mod`)
* [x] primality: `is_probable_prime` (Baillie–PSW), `next_prime`, `isqrt`
* [x] benchmarking vs `boost::multiprecision::cpp_int`

* [x] WebAssembly: full test suite + pow_mod shootout vs mini-gmp/Boost (`scripts/wasm_bootstrap.sh`, `scripts/wasm_bench.sh`)
* [x] wasm CI job (full suite under node + npm-package build/test + packed-tarball install test + demo cross-check, every push)
* [x] npm package `hydra-bignum` (`pkg/` — bigint→bigint powMod/modInverse/primality; publish itself is a human step)
* [x] batched modular exponentiation: `pow_mod_batch` fused 2-lane ladder (1.33×@2048b / 1.38×@4096b e2e throughput)
* [x] `llms.txt` — compact machine-readable project digest (API, internals map, perf identity) for AI agents and quick onboarding

Active roadmap:

* [ ] Toom-Cook multiplication for ≥128-limb operands
* [ ] arena-backed Karatsuba scratch (may lower threshold to 16 limbs)
* [ ] `std::hash<Hydra>` specialisation
* [ ] PMR-style allocator hook

> **Not constant-time.** Hydra is variable-time by design and assumes
> public inputs.  It is well-suited to verification workloads (RSA
> signature verification, VDFs, accumulators), primality testing, and
> general bignum arithmetic — not to secret-key operations.

---

## 💭 Philosophy

Hydra is equal parts systems engineering and monster mythology.

The name fits:

> one interface
> many internal heads
> cut one path off and another grows

---

## 🤝 Contributions / design discussion

This project is intentionally exploratory.

Performance discussions, ownership critiques, allocator experiments, and kernel-design ideas are all welcome.

Especially interested in:

* fixed-limb arithmetic
* SIMD experiments
* allocator strategies
* dispatch design
* compiler-visible fast paths

---

*Hydra is a systems toy, a numeric engine, and a love letter to over-engineered elegance.* 🐍

