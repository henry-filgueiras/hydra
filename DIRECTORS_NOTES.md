# DIRECTORS_NOTES.md
_Living design document — updated by agents and humans alike._
_Two sections: **Current Canon** (present-state truth) and **Resolved Dragons** (historical archaeology)._
_Append new entries to the appropriate section; don't overwrite history._

---

## Current Canon

> **Source of truth for the present implementation.**
> Read this section before modifying architecture, dispatch logic, kernels, or benchmarks.

---

### Architecture

_Established 2026-04-15 — Claude Sonnet 4.6 (Phase 1)_

#### Header-Only

All implementation lives in `hydra.hpp`. The existing `hydra.cpp` remains as
a translation-unit stub (empty `#include "hydra.hpp"`). This lets the compiler
inline across call sites without LTO and keeps the repo simple while the
design is still changing fast.

The old `hydra.h` is superseded by `hydra.hpp`. The Makefile remains for
quick iterative builds without CMake.

#### Three-Tier Runtime: Small / Medium / Large

The core abstraction is a tiered integer that selects its representation
at runtime based on value magnitude. Small (≤64-bit), Medium (65–192-bit),
and Large (193-bit+) share a unified API but pick the cheapest available
storage at all times.

#### LimbView: Zero-Copy Read Span

Instead of materialising a limb array for every operation, arithmetic reads
through a `LimbView { const uint64_t* ptr; uint32_t count; }` that points
directly into the payload. No allocation, no copy for the common path.

#### Zero Representation

Zero is `Kind::Small`, `payload.small = 0`, `limb_count() = 0`. The limb
view of zero returns count=0. This makes the general add/sub/mul kernels
handle zero "for free" (adding 0 limbs to anything returns the other side).

#### Normalization Invariant

`normalize()` scans from the most-significant limb downward. It is called
once, after the result is assembled, never recursively. The Large → Medium
path is careful to copy limbs into a local tmp before calling
`LargeRep::destroy`, avoiding use-after-free. Every public-facing
arithmetic result is normalized before it is returned.

#### Overflow Detection in the Small Path

`add_small_small` uses `__builtin_add_overflow` which maps to a single
`adds` / `addco` instruction on aarch64 / x86-64 respectively. No branches
on flags; the carry is just a bool. The compiler can see this and fold it
into branchless code with a conditional medium construction.

#### `__uint128_t` for Multiply

`mul_small_small` and all multi-limb kernels use `unsigned __int128` for
intermediate 128-bit products. This compiles to a single `mul`/`umulh` pair
on aarch64 and `mulq` on x86-64 — no emulation needed.

#### 2D Dispatch

Operations dispatch via a simple `if (both small) [[likely]] fast-path` at
the top of each operator, falling through to a general limb-array kernel for
all other Kind pairs. This keeps the hot path visible to the inliner while
avoiding a full 3×3 switch matrix that would obscure the common case.

The general kernels (`add_general`, `sub_general`, `mul_general`) use
stack-allocated buffers for result sizes ≤ 4 (add) or ≤ 6 (mul) limbs,
avoiding heap allocation for everything up to 256- and 384-bit results
respectively.

#### Subtraction Saturates at Zero (Phase 1)

Unsigned saturating subtraction: `a - b` where `b > a` returns 0. Signed
arithmetic is reserved for phase 2 (the sign bit in metadata is already
allocated at bit 2).

---

### In-Place `operator+=` with Capacity Reuse

_Implemented 2026-04-15 — Claude Opus 4.6_

`operator+=` has a fast path when `this->is_large()` and the existing
`LargeRep::capacity >= max(lhs_limbs, rhs_limbs) + 1`:

- Captures both `limb_view`s (pointer + count) before mutation
- Calls `add_limbs` directly into `payload.large->limbs()`
- Updates `payload.large->used` with the returned count
- Calls `normalize()` for demotion invariants

If capacity is insufficient, falls back to the `*this = *this + rhs`
(allocating) path.

**Aliasing safety:** `add_limbs` processes limbs in ascending index order
(`i = 0, 1, 2…`). Each iteration reads `a[i]` (and `b[i]`) before writing
`out[i]`. When `out` aliases the left operand's buffer, each limb is consumed
before overwrite. Self-addition (`a += a`) is safe because both reads occur
before the write in `s = a[i] + b[i] + carry`.

**Impact:** Zero allocations in steady-state chained accumulation
(`chain/large_add/*` benchmarks).

---

### Specialized Multiplication Kernels (Current Dispatch)

_Implemented 2026-04-15 — Claude Opus 4.6_

Three hand-unrolled kernels in `hydra::detail` replace the generic schoolbook
loop for common fixed-width cases:

| Kernel   | Width              | MACs | Target path           |
|----------|--------------------|------|-----------------------|
| `mul_3x3`| ≤3 × ≤3 (≤192-bit) | 9    | `medium_mul`          |
| `mul_4x4`| 4 × 4 (256-bit)    | 16   | `large_mul_256`       |
| `mul_8x8`| 8 × 8 (512-bit)    | 64   | `large_mul_512`       |

All three use **row-based unrolling** with a single `unsigned __int128`
accumulator (`HYDRA_ROW_MAC` macro). Each MAC computes
`acc = a[i]*b[j] + out[i+j] + carry` — this provably never overflows
`__int128` because `max(out[k]) + max(carry) + max(product) = 2^128 − 1`.

Dispatch in `mul_general`:

```
max_limbs ≤ 3        → mul_3x3 (covers Medium×Medium, Small×Medium, etc.)
lv==4 && rv==4       → mul_4x4
lv==8 && rv==8       → mul_8x8
otherwise            → generic mul_limbs (unchanged)
```

**Current benchmark profile (x86-64, g++ -O3):**

| Path           | Hydra    | Boost    | Delta     |
|----------------|----------|----------|-----------|
| `medium_mul`   | 9.77 ns  | 15.98 ns | −39%      |
| `large_mul_256`| 10.41 ns | 20.54 ns | −49%      |
| `large_mul_512`| 26.85 ns | 30.87 ns | −13%      |

All three paths beat Boost, exceeding the ±5% target.

---

### 32-Byte Aligned Default Footprint

_Design decision recorded 2026-04-15 — ChatGPT and Gemini_

**Keep the 32-byte aligned layout as the canonical default.** Reasons:

- Clean cache-line behaviour: 2 objects per 64-byte line
- Future SIMD friendliness: natural fit for 256-bit register-oriented loads
- Simpler field alignment and code generation
- Fewer layout edge-cases
- Easier reasoning about payload invariants
- Benchmark continuity and reproducibility

This is the current benchmarked and publicly communicated artifact.

---

### Future Design Fork: Policy-Based Layout (Phase 2+)

_Discussed 2026-04-15 — ChatGPT and Gemini_

A potential future design once the canonical implementation stabilises:

```cpp
template <typename Policy = aligned_32_policy>
class Hydra;
```

Candidate policies:

- `aligned_32_policy` — performance-first, benchmark default, SIMD-ready, simpler codegen
- `packed_24_policy` — memory-density-first, 8-byte savings per object, potentially better
  memory-bound workloads

**Do not introduce policy templating until after:**
- Multiplication kernels stabilise
- Shift / division support lands
- Signed representation strategy is chosen
- Benchmark suite remains green and apples-to-apples

Premature policy abstraction risks benchmark fragmentation, code duplication,
optimizer divergence, harder perf attribution, and documentation complexity.
Treat this as a **Phase 2 architectural fork**, not an immediate work item.

---

### Phase 2 Roadmap (Active TODOs)

_Catalogued 2026-04-15 — Claude Sonnet 4.6_

- **No signed arithmetic.** Sign bit (meta bit 2) is allocated but ignored.
- **No division or modulo.** Planned using Knuth Algorithm D.
- **`to_string()` for Medium/Large is very slow.** Uses repeated division by 10
  via the limb kernel. Replace with base-10^9 extraction.
- **Schoolbook O(n²) multiplication.** Fine up to ~200 limbs; add Karatsuba
  at n ≥ 32.
- **No allocator customisation.** `LargeRep` uses `::operator new` directly.
  A PMR-style allocator hook is a natural extension.
- **No bit-shift operators.** `<<` and `>>` are needed for many algorithms.
- **No hash specialisation.** `std::hash<Hydra>` should be added for use in
  unordered containers.
- **Stack buffer size is intuition-based.** The 4-limb / 6-limb cutoffs in
  `add_general` / `mul_general` were chosen by intuition; profile-guided
  tuning is warranted.
- **`operator*=` capacity reuse.** Same pattern as `operator+=` fast path;
  apply to `operator*=` in a future pass.
- **Karatsuba / Toom-Cook.** `mul_8x8` is the current ceiling; add divide-
  and-conquer for very large operands.

---

### Benchmark & Tooling Infrastructure

_Established 2026-04-15 — Claude Sonnet 4.6; extended by Claude Opus 4.6_

#### Files

| File                                    | Role                                                         |
|-----------------------------------------|--------------------------------------------------------------|
| `bench/bench_hydra.cpp`                 | Google Benchmark suite (Hydra + Boost comparison)            |
| `bench/compare.py`                      | Python comparison script — terminal + Markdown output        |
| `bench/run.sh`                          | One-shot shell wrapper: build → run → compare                |
| `scripts/profile_chain_large_add.sh`    | xctrace profiler for chained accumulation (Hydra vs Boost)   |
| `assets/hydra_perf_story.svg`           | Production-ready 16:9 infographic for README embedding        |

#### CMake Targets

| Target              | What it does                                               |
|---------------------|------------------------------------------------------------|
| `bench`             | Run raw benchmark output to terminal                       |
| `bench_json`        | Run benchmarks → `build-rel/bench_results.json`            |
| `bench_compare`     | `bench_json` + terminal comparison report                  |
| `bench_compare_md`  | `bench_json` + Markdown report → `build-rel/bench_report.md`|

#### compare.py Design

**Primary metrics:**

- *Small ops vs. native*: `hydra/small_add` compared against `baseline/u64_add`.
  Goal is < 2× native; anything > 200% is flagged with ⚠.
- *Medium / Large vs. Boost*: `hydra/*` compared against `boost/*` (only
  populated when `-DHYDRA_BENCH_BOOST=ON`). Negative delta = Hydra is faster.

**Sections:** comparison table (small vs native), comparison table (medium/large
vs Boost), standalone cost tables for `alloc/*`, `copy/*`, `chain/*`.

**Output modes:** Terminal (ANSI colour, auto-stripped when piped),
`--markdown` (GitHub-flavoured tables), `--json-out` (structured delta JSON
for CI regression detection).

**Known limitation — aggregate-only JSON:** When
`--benchmark_report_aggregates_only=true` is passed to `hydra_bench`, the
JSON only contains `_mean` / `_median` / `_stddev` rows, which the script
currently skips. Use `./bench/run.sh` which handles this correctly, or omit
the flag when running manually:

```bash
./build-rel/hydra_bench \
    --benchmark_format=json \
    --benchmark_out=results.json \
    --benchmark_repetitions=3
python3 bench/compare.py results.json
```

**TODO:** detect aggregate-only JSON and switch to `_mean` rows automatically.

#### Build Quick-Start

```bash
# Debug build (ASan + UBSan)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# Release benchmark
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel --target hydra_bench -j
./build-rel/hydra_bench

# Quickest comparison path:
./bench/run.sh

# With Boost comparison:
./bench/run.sh --boost

# Markdown output for README snippets:
./bench/run.sh --markdown
```

---

---

## Resolved Dragons (Historical Performance Archaeology)

> **These findings were accurate when written. The bottlenecks described here
> no longer exist in the current codebase.**
> Preserved for regression awareness and institutional memory.
> Do not treat these as current-state architecture.

---

### Dragon 1 — Double Allocation in `add_general` (Heap Path)

_Found and resolved 2026-04-15 — Claude Sonnet 4.6_
_Status: **RESOLVED** — current `add_general` does one allocation, zero extra copies._

#### Profiler finding

Time Profiler identified the dominant stack in `add_general` (heap path) as:

```
std::vector scratch allocation
→ Hydra::from_limbs
  → LargeRep::create
  → memmove
```

The arithmetic kernel (`add_limbs`) was **not** the bottleneck. The waste was
two allocations and a full memcpy of the limb array:

1. `std::vector<uint64_t> out(max_limbs)` — scratch buffer
2. `from_limbs(out.data(), used)` → `LargeRep::create(count)` — final rep
3. `std::memcpy(rep->limbs(), limbs, count * sizeof(uint64_t))` — copy scratch → rep

#### Fix applied

Allocated the final `LargeRep` first (wrapped in `LargeGuard` for
exception-safety), passed `rep->limbs()` directly to `add_limbs`, set
`rep->used`, then committed to a `Hydra` and called `normalize()`.

`normalize()` handles trimming and demotion to Medium/Small if the upper limbs
are zero — exactly the same correctness behaviour as before.

#### Verification

31 ASan+UBSan test cases covering carry propagation, Large→Medium and
Large→Small demotion, commutativity, associativity, 5- and 6-limb operands.
All passed.

---

### Dragon 2 — Missing Capacity Reuse in `operator+=` (Per-Iteration Alloc+Dealloc)

_Found and resolved 2026-04-15 — Claude Opus 4.6_
_Status: **RESOLVED** — `operator+=` now has a capacity-reuse fast path (see Current Canon)._

#### Profiler finding

After the Dragon 1 fix, Time Profiler showed:

- `hydra::detail::add_limbs(...)` as primary hotspot (~57%)
- `hydra::Hydra::operator=(const Hydra&)` still visible
- `hydra::LargeRep::create(...)` still visible

The old `operator+=(const Hydra&)` implementation was `*this = *this + rhs`,
which even with move semantics still required:

1. `LargeRep::create` — new allocation for the temporary result
2. `operator=(Hydra&&)` — move-assign, destroying the old `LargeRep`
3. Destructor of the temporary (no-op after move, but still a branch)

In chained arithmetic (`acc += step` in a loop), this produced one
alloc + one dealloc per operation even when the accumulator already had
enough room.

#### Fix applied

Added a fast path when `this->is_large()` and
`LargeRep::capacity >= max(lhs_limbs, rhs_limbs) + 1`. See Current Canon for
the full aliasing safety argument and implementation details.

#### Verification

16 ASan+UBSan test cases (Large+=Large, carry propagation, self-addition,
asymmetric sizes, chained accumulation, fallback path, normalization/demotion,
commutativity/associativity, Medium and Small non-regression). All passed at
`-O0` and `-O2`.

---

### Dragon 3 — mul Kernel Design Iteration (Column-Sum → 3-Word → Row-Based)

_Explored and resolved 2026-04-15 — Claude Opus 4.6_
_Status: **RESOLVED** — current kernels use row-based unrolling (see Current Canon)._

The final row-based design was reached after two failed intermediate attempts:

**Attempt 1 — Column-sum with single `__int128` accumulator**
Summed all `a[i]*b[j]` where `i+j=k` into a single `unsigned __int128`. This
overflows when a column has 2+ products (max ≈ 2^129 for 2 terms). Incorrect.

**Attempt 2 — 3-word (192-bit) accumulator (`mac3` with c0/c1/c2 triple)**
Correct, but the extra `__int128` additions per MAC added ~4 instructions of
overhead — the 8×8 kernel was 21% *slower* than the generic schoolbook.

**Final — Row-based unrolling with single `__int128` accumulator**
Same memory access pattern as generic schoolbook, but with all loops fully
unrolled and branches removed. The `__int128` accumulator handles exactly one
product + one prior output + one carry per step, which never overflows.

Raw kernel speedups over generic `mul_limbs` (baseline before specialisation):

| Kernel | Specialized | Generic  | Speedup |
|--------|-------------|----------|---------|
| 4×4    | 3.40 ns     | 9.32 ns  | 2.7×    |
| 8×8    | 15.88 ns    | 23.87 ns | 1.5×    |

---

### Dragon 4 — Four Benchmark Comparison Bugs (Apples-to-Oranges Pairs)

_Found and resolved 2026-04-15 — Claude Opus 4.6_
_Status: **RESOLVED** — benchmark pairs are now structurally equivalent._

A systematic audit of every `hydra/*` vs `boost/*` paired benchmark revealed
four bugs that caused the comparison to measure different operations on the
two sides.

#### Bug 1 — `boost/widening_add` did not exist

`compare.py` registered the pair but no such Boost benchmark was defined.
Added `BM_boost_widening_add` mirroring Hydra's fold: `a` stays fixed near
UINT64_MAX; `b` is refreshed each iteration as `(low-64-bits-of-sum | base)`,
keeping it near UINT64_MAX so every addition widens to 128 bits.

#### Bug 2 — `hydra/widening_mul_128` fold collapsed `b` to a small value

Hydra used `b = Hydra{lv.ptr[0] | 1u}` (the **low** 64-bit limb of the
128-bit product). For inputs near UINT64_MAX the low limb of the product is
~8, so from iteration 2 onward Hydra was measuring a tiny×large multiply.
Boost used `b = (c >> 64) | 1` (the **high** 64-bit limb), which stays near
UINT64_MAX.

Fix: change to `lv.ptr[1] | 1u` (the high limb) and add `a = b` before
updating `b` so the Fibonacci-style fold matches Boost's structure exactly.

#### Bug 3 — `hydra/large_add_cmp` fold grew `b` to full-width while Boost kept `b` at half-width

Hydra's fold was `a = b; b = c.is_large() ? c : make_large(n/2)`. Since the
addition result is always Large, `b = c` (full-width) every iteration.
Boost's fold was `a = b; b = c >> 1`, a **stabilising** fold keeping `b`
bounded at ~n_bits − 1 wide. Hydra has no bit-shift operator, so there is no
equivalent stabilising fold — the two sides were permanently structurally
asymmetric.

Fix: drop the fold-back on both sides. Use fixed operands (a = full-width,
b = half-width, seeded once before the loop) and rely on `DoNotOptimize` on
both to prevent constant-folding. This is the only approach that is
simultaneously provably equivalent and allocation-free in the timed loop body.

#### Bug 4 — `hydra/large_mul_cmp` had a dead branch adding a per-iteration allocation

```cpp
// broken — both arms identical, b never depends on c
b = c.is_large() ? make_large(std::max(n / 2, 2u))
                 : make_large(std::max(n / 2, 2u));
```

Every iteration allocated a fresh Large value on Hydra's side while Boost's
`b = c >> n_bits` fold was pure arithmetic. The compiler could also in
principle prove `b` was independent of `c` and hoist the `make_large` call
out of the loop entirely.

Fix: same fixed-operand approach as `large_add_cmp` (see Bug 3).

---

_Append new entries to **Current Canon** or **Resolved Dragons** as appropriate._
