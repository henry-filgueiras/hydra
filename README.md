# 🐍 Hydra

![Hydra Sigil](assets/hydra_sigil.svg)

**A tiered arbitrary-precision integer for modern C++**
*small values move like native machine integers; large values grow heads.*

Hydra is an experimental **multi-representation integer runtime** designed to preserve the speed of native 64-bit arithmetic while scaling seamlessly into arbitrary precision.

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

Arbitrary-precision arithmetic just works:

```cpp
Hydra a("123456789012345678901234567890");
Hydra b("-18446744073709551616");

std::cout << a + b << "\n";   // 123456788993898934827525016274
std::cout << gcd(a, b) << "\n";  // 2
```

---

## Visual Hydra Performance Story

<p align="center">
  <img src="assets/hydra_perf_story.svg" alt="Hydra performance story" width="100%">
</p>

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

## 📊 Performance Snapshot

Benchmarks run with Google Benchmark on a single core (Apple Silicon, clang release build).
Numbers are wall-time per operation; lower is better.
This is a living benchmark diary — figures will shift as kernels mature.

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

The small-path overhead (~20–28%) reflects the kind-check dispatch in front of native arithmetic; that cost is expected to shrink as the compiler gets more visibility into the inline representation.

The widening and medium paths are the headline results: widening add and widening mul are 72% and 92% ahead of Boost respectively, and medium add beats Boost by 54%. Medium mul sits at rough parity (−2%).

Large-width arithmetic is mixed but largely competitive. Large add wins decisively at 128-bit (−58%) and 512-bit (−44%), with 256-bit at near-parity (+3%). Large mul is at parity for 128- and 256-bit widths (within ±2%), while 512-bit is currently 18% behind Boost. Chained large addition shows a similar mixed profile: near-parity at 8 limbs, behind at 16 limbs (+27%), ahead at 64 limbs (−8%). No severe regressions remain in any measured path.

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
* [x] benchmarking vs `boost::multiprecision::cpp_int`

Active roadmap:

* [ ] Toom-Cook multiplication for ≥128-limb operands
* [ ] arena-backed Karatsuba scratch (may lower threshold to 16 limbs)
* [ ] `std::hash<Hydra>` specialisation
* [ ] PMR-style allocator hook

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

