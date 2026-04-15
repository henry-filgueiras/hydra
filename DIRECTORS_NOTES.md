# DIRECTORS_NOTES.md
_Living design document — updated by agents and humans alike._
_Append new entries at the bottom; don't overwrite history._

---

## 2026-04-15 — Phase 1 implementation (Claude Sonnet 4.6)

### What was built

First serious implementation of the Hydra tiered integer runtime.
Three output files:

| File | Role |
|------|------|
| `hydra.hpp` | Complete header-only implementation |
| `bench/bench_hydra.cpp` | Google Benchmark suite |
| `CMakeLists.txt` | Modern CMake build; FetchContent for benchmark |

The old `hydra.h` is superseded by `hydra.hpp`. The Makefile remains for
quick iterative builds without CMake.

---

### Architecture decisions

#### Header-only

All implementation lives in `hydra.hpp`. The existing `hydra.cpp` remains as
a translation-unit stub (empty `#include "hydra.hpp"`). This lets the compiler
inline across call sites without LTO and keeps the repo simple while the
design is still changing fast.

#### LimbView: zero-copy read span

Instead of materialising a limb array for every operation, arithmetic reads
through a `LimbView { const uint64_t* ptr; uint32_t count; }` that points
directly into the payload. No allocation, no copy for the common path.

#### Zero representation

Zero is `Kind::Small`, `payload.small = 0`, `limb_count() = 0`. The limb
view of zero returns count=0. This makes the general add/sub/mul kernels
handle zero "for free" (adding 0 limbs to anything returns the other side).

#### normalize() design

Normalize scans from the most-significant limb downward. It is called once,
after the result is assembled, never recursively. The Large → Medium path is
careful to copy limbs into a local tmp before calling `LargeRep::destroy`,
avoiding use-after-free.

#### Overflow detection in the Small path

`add_small_small` uses `__builtin_add_overflow` which maps to a single
`adds` / `addco` instruction on aarch64 / x86-64 respectively. No branches
on flags; the carry is just a bool. The compiler can see this and fold it
into branchless code with a conditional medium construction.

#### `__uint128_t` for multiply

`mul_small_small` and all multi-limb kernels use `unsigned __int128` for
intermediate 128-bit products. This compiles to a single `mul`/`umulh` pair
on aarch64 and `mulq` on x86-64 — no emulation needed.

#### 2D dispatch

Operations dispatch via a simple `if (both small) [[likely]] fast-path` at
the top of each operator, falling through to a general limb-array kernel for
all other Kind pairs. This keeps the hot path visible to the inliner while
avoiding a full 3×3 switch matrix that would obscure the common case.

The general kernels (`add_general`, `sub_general`, `mul_general`) use
stack-allocated buffers for result sizes ≤ 4 (add) or ≤ 6 (mul) limbs,
avoiding heap allocation for everything up to 256- and 384-bit results
respectively.

#### Subtraction saturates at zero (phase 1)

Unsigned saturating subtraction: `a - b` where `b > a` returns 0. Signed
arithmetic is reserved for phase 2 (the sign bit in metadata is already
allocated at bit 2).

---

### Known limitations / phase 2 TODO

- **No signed arithmetic.** Sign bit (meta bit 2) is allocated but ignored.
- **No division or modulo.** Planned for phase 2 using Knuth Algorithm D.
- **to_string() for Medium/Large is very slow.** Uses repeated division by 10
  via the limb kernel. Replace with base-10^9 extraction in phase 2.
- **Schoolbook O(n²) multiplication.** Fine up to ~200 limbs; add Karatsuba
  at n ≥ 32 in phase 2.
- **No allocator customisation.** LargeRep uses `::operator new` directly.
  A PMR-style allocator hook is a natural extension.
- **No bit-shift operators.** `<<` and `>>` are needed for many algorithms.
- **No hash specialisation.** `std::hash<Hydra>` should be added for use in
  unordered containers.
- **Stack buffer size is arbitrary.** The 4-limb / 6-limb cutoffs were chosen
  by intuition; profile-guided tuning is warranted.

---

### Build quick-start

```bash
# Debug build (ASan + UBSan)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# Release benchmark
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel --target hydra_bench -j
./build-rel/hydra_bench
```

---

_Append future entries below this line._

