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

## 2026-04-15 — Benchmark comparison tool (Claude Sonnet 4.6)

### New files

| File | Role |
|------|------|
| `bench/compare.py` | Python comparison script — terminal + Markdown output |
| `bench/run.sh` | One-shot shell wrapper: build → run → compare |

### CMake targets added

| Target | What it does |
|--------|-------------|
| `bench` | Run raw benchmark output to terminal |
| `bench_json` | Run benchmarks → `build-rel/bench_results.json` |
| `bench_compare` | `bench_json` + terminal comparison report |
| `bench_compare_md` | `bench_json` + Markdown report → `build-rel/bench_report.md` |

### Benchmark workflow

```bash
# Quickest path — build + run + compare in one shot:
./bench/run.sh

# With Boost comparison (requires Boost headers):
./bench/run.sh --boost

# Markdown output for README snippets:
./bench/run.sh --markdown

# CMake equivalents:
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel --target bench_compare -j
cmake --build build-rel --target bench_compare_md -j    # → build-rel/bench_report.md

# Manual JSON export + separate compare pass:
./build-rel/hydra_bench \
    --benchmark_format=json \
    --benchmark_out=results.json \
    --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true
python3 bench/compare.py results.json
python3 bench/compare.py results.json --markdown --output bench_report.md
python3 bench/compare.py results.json --json-out deltas.json    # for CI
```

### compare.py design

**Primary metrics:**

- *Small ops vs. native*: `hydra/small_add` compared against `baseline/u64_add`.
  Goal is < 2× native; anything > 200% is flagged with ⚠.

- *Medium / Large vs. Boost*: `hydra/*` compared against `boost/*` (only
  populated when `-DHYDRA_BENCH_BOOST=ON`). Negative delta = Hydra is faster.

**Sections:**
1. Comparison table: small ops vs. native
2. Comparison table: medium/large ops vs. Boost (skipped if Boost absent)
3. Standalone cost table: `alloc/*` parameterised by limb count
4. Standalone cost table: `copy/*`
5. Standalone cost table: `chain/*`

**Output modes:**
- Terminal (default): ANSI colour, auto-stripped when piped
- `--markdown`: GitHub-flavoured tables for README / PR comments
- `--json-out`: Structured delta JSON for CI regression detection

**Aggregate handling:** The script skips `_mean` / `_median` / `_stddev` /
`_cv` rows emitted by `--benchmark_repetitions`, using only the per-run rows.
`--benchmark_report_aggregates_only` in the CMake targets changes this — if
you manually run with that flag, the script falls back gracefully because only
aggregate rows will be present; it skips all of them and reports "no
benchmarks found". Remove `--benchmark_report_aggregates_only` if you want
per-repetition granularity in the JSON.

Actually, `--benchmark_report_aggregates_only` suppresses the raw per-run
rows and only emits the aggregates. The script currently skips aggregates.
**TODO**: detect aggregate-only JSON and switch to using `_mean` rows.
This is filed as a known limitation.

### Known limitation: aggregate-only JSON

When `--benchmark_report_aggregates_only=true` is passed to `hydra_bench`,
the JSON only contains `_mean` / `_median` / `_stddev` rows, which the
script currently skips. The CMake targets use this flag for cleaner output;
when running manually for the comparison script, omit it:

```bash
./build-rel/hydra_bench \
    --benchmark_format=json \
    --benchmark_out=results.json \
    --benchmark_repetitions=3
# (no --benchmark_report_aggregates_only)
python3 bench/compare.py results.json
```

Or just use `./bench/run.sh` which handles this correctly.

---

_Append future entries below this line._

---

## 2026-04-15 — `add_general` scratch-buffer elimination (Claude Sonnet 4.6)

### Profiler finding

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

### Fix

Allocate the final `LargeRep` first (wrapped in `LargeGuard` for
exception-safety), pass `rep->limbs()` directly to `add_limbs`, set
`rep->used`, then commit to a `Hydra` and call `normalize()`.

The `normalize()` call handles trimming and demotion to Medium/Small if the
upper limbs are zero — exactly the same correctness behaviour as before.
Result: **one allocation, zero extra copies** on the heap path.

The stack path (≤ 4 limbs) is unchanged.

### Correctness verification

31 ASan+UBSan test cases were run covering:
- Large+Large stays Large (carry propagation, 8-limb operands)
- Large result demotes to Medium via normalize()
- Large result demotes to Small via normalize()
- Commutativity and associativity for 5-limb and 6-limb operands
- Specific limb-value checks at boundaries

All passed.

---

## 2026-04-15 — In-place `operator+=` with capacity reuse (Claude Opus 4.6)

### Profiler finding

After the scratch-buffer elimination, Time Profiler showed:

- `hydra::detail::add_limbs(...)` as primary hotspot (~57%)
- `hydra::Hydra::operator=(const Hydra&)` still visible
- `hydra::LargeRep::create(...)` still visible

The `operator+=(const Hydra&)` implementation was `*this = *this + rhs`,
which even with move semantics still required:

1. `LargeRep::create` — new allocation for the temporary result
2. `operator=(Hydra&&)` — move-assign, destroying the old `LargeRep`
3. Destructor of the temporary (no-op after move, but still a branch)

In chained arithmetic (`acc += step` in a loop), this produced one
alloc + one dealloc per operation even when the accumulator already had
enough room.

### Fix

`operator+=` now has a fast path when `this->is_large()` and the
existing `LargeRep::capacity >= max(lhs_limbs, rhs_limbs) + 1`:

- Captures both `limb_view`s (pointer + count) before mutation
- Calls `add_limbs` directly into `payload.large->limbs()`
- Updates `payload.large->used` with the returned count
- Calls `normalize()` for demotion invariants

If capacity is insufficient, falls back to the original
`*this = *this + rhs` (allocating) path.

### In-place aliasing safety argument

`add_limbs` processes limbs in ascending index order (`i = 0, 1, 2…`).
Each iteration reads `a[i]` (and `b[i]`) before writing `out[i]`.
When `out` aliases the left operand's buffer, each limb is consumed
before overwrite.  The internal `na < nb` swap reorders `a`/`b` pointers
only — `out` is never swapped — so the aliasing invariant holds
regardless of which operand is longer.  Self-addition (`a += a`) is safe
because both reads occur before the write in `s = a[i] + b[i] + carry`.

### Expected benchmark impact

- **`chain/large_add/*`** (new benchmark): major speedup — zero
  allocations in steady state
- **`chain/factorial/*`**: no direct effect (uses `*=`, not `+=`), but
  the same pattern can be applied to `operator*=` in a future pass
- **Small / Medium paths**: unchanged (fast path only triggers for Large)

### Correctness verification

16 ASan+UBSan test cases in `hydra_test.cpp` covering:

- Large += Large basic correctness
- Carry propagation through all limbs
- Self-addition (`a += a`)
- Asymmetric sizes (16-limb += 4-limb and vice versa)
- Chained accumulation (a += b three times)
- Capacity-insufficient fallback path
- Normalization / demotion after in-place add (Large → Small)
- Commutativity and associativity
- Medium and Small path non-regression

All passed under both `-O0` and `-O2`.

### New files / changes

| File | Change |
|------|--------|
| `hydra.hpp` | `operator+=` fast path (capacity reuse) |
| `hydra_test.cpp` | 16 correctness tests replacing empty stub |
| `bench/bench_hydra.cpp` | `chain/large_add` benchmark (8/16/64 limbs) |

---

## 2026-04-15 — Boost chain_large_add benchmark & profiling script (Claude Opus 4.6)

### What was added

Boost.Multiprecision comparison benchmark for chained in-place accumulation,
mirroring the existing `chain/large_add` Hydra benchmark.

### New benchmark: `boost/chain_large_add`

Uses `boost::multiprecision::cpp_int` with `operator+=` in the same 10×
accumulation pattern as the Hydra benchmark.  A helper `make_boost_large(n, seed)`
constructs a `cpp_int` of exactly `n` 64-bit limbs using the same XorShift64
PRNG and seeding as `make_large()`, ensuring the initial values have the same
bit width and magnitude distribution.

Parameterised identically: `->Arg(8)->Arg(16)->Arg(64)`.

### New comparison pairs in compare.py

Three pairs added to `BOOST_PAIRS`:

- `chain/large_add/8`  vs `boost/chain_large_add/8`
- `chain/large_add/16` vs `boost/chain_large_add/16`
- `chain/large_add/64` vs `boost/chain_large_add/64`

### New script: `scripts/profile_chain_large_add.sh`

xctrace profiling script for the chained accumulation scenario.  Mirrors
`profile_large_add.sh` but targets `chain/large_add` and `boost/chain_large_add`.

Features:
- Records 4 traces: Time Profiler + Allocations for both Hydra and Boost
- Uses `--time-limit 5s` and `xctrace record`
- Outputs to timestamped `traces/YYYYMMDD_HHMMSS/` folder
- Configurable via env vars: `LIMBS` (default 16), `TIME_LIMIT`, `MIN_TIME`, `BENCH`

### Files changed / added

| File | Change |
|------|--------|
| `bench/bench_hydra.cpp` | `boost/chain_large_add` benchmark + `make_boost_large()` helper |
| `bench/compare.py` | 3 chain large-add comparison pairs in `BOOST_PAIRS` |
| `scripts/profile_chain_large_add.sh` | New profiling script |

---

## 2026-04-15 — README infographic SVG (Claude Opus 4.6)

### New file

| File | Role |
|------|------|
| `assets/hydra_perf_story.svg` | Production-ready 16:9 infographic for README embedding |

### Design

Three-panel layout (architecture / profiler story / benchmarks) with a dark
graphite/neon-blue palette that renders cleanly on both GitHub light and dark
themes.  Fully standalone SVG — no external assets except an optional Google
Fonts `@import` for Inter (falls back to system sans-serif).

Embed in README with:
```markdown
![Hydra Performance Story](assets/hydra_perf_story.svg)
```

---

## 2026-04-15 — Benchmark comparison audit & fixes (Claude Opus 4.6)

Systematic audit of every `hydra/*` vs `boost/*` paired benchmark revealed four
bugs that caused the comparison to measure different operations on the two sides.

### Bug 1 — `boost/widening_add` did not exist

`compare.py` registered `("hydra/widening_add", "boost/widening_add", ...)` but
no such Boost benchmark was defined.  Added `BM_boost_widening_add` mirroring
Hydra's exact fold: `a` stays fixed near UINT64_MAX; `b` is refreshed each
iteration as `(low-64-bits-of-sum | base)`, keeping it near UINT64_MAX so every
addition widens to 128 bits.

### Bug 2 — `hydra/widening_mul_128` fold collapsed b to a small value

Hydra used `b = Hydra{lv.ptr[0] | 1u}` (the **low** 64-bit limb of the 128-bit
product).  For inputs near UINT64_MAX the low limb of the product is ~8, so from
iteration 2 onward Hydra was measuring a tiny×large multiply rather than a
widening multiply.  Boost used `b = (c >> 64) | 1` (the **high** 64-bit limb),
which stays near UINT64_MAX.

Fix: change to `lv.ptr[1] | 1u` (the high limb), and also add `a = b` before
updating b so the Fibonacci-style fold matches Boost's structure exactly.

### Bug 3 — `hydra/large_add_cmp` fold grew b to full-width while Boost kept b at half-width

Hydra's fold was `a = b; b = c.is_large() ? c : make_large(n/2)`.  Since the
addition result is always Large, `b = c` (full-width) every iteration.  Boost's
fold was `a = b; b = c >> 1`, which is a **stabilising** fold (the shift keeps b
bounded at ~n_bits − 1 wide).  Hydra has no bit-shift operator, so there is no
equivalent stabilising fold, making the two sides permanently structurally
asymmetric.

Fix: drop the fold-back on **both** sides.  Use fixed operands (a = full-width,
b = half-width, seeded once before the loop) and rely on `DoNotOptimize` on both
to prevent constant-folding.  This is the only approach that is simultaneously
provably equivalent and allocation-free in the timed loop body.

### Bug 4 — `hydra/large_mul_cmp` had a dead branch adding a per-iteration allocation

```cpp
// broken — both arms identical, b never depends on c
b = c.is_large() ? make_large(std::max(n / 2, 2u))
                 : make_large(std::max(n / 2, 2u));
```

Every iteration allocated a fresh Large value on Hydra's side, while Boost's
`b = c >> n_bits` fold was pure arithmetic with no allocation.  Also, because
both arms were the same, the compiler could in principle prove b was independent
of c and hoist the make_large call out of the loop.

Fix: same fixed-operand approach as large_add (see above).

### Files changed

| File | Change |
|------|--------|
| `bench/bench_hydra.cpp` | All four bug fixes + `BM_boost_widening_add` |


## 2026-04-15 - Design Fork: 32-byte aligned vs 24-byte packed Hydra (ChatGPT and Gemini)

A recurring design question is whether Hydra should remain a **fixed 32-byte object footprint** or offer an optional **24-byte packed representation**.

### Current recommendation (default)

Keep the **32-byte aligned layout as the canonical default**.

Reasons:

* clean cache-line behavior

  * 2 objects per 64-byte line
* future SIMD friendliness

  * natural fit for 256-bit register-oriented loads
* simpler field alignment and code generation
* fewer layout edge-cases
* easier reasoning about payload invariants
* benchmark continuity and reproducibility

This is the current benchmarked and publicly communicated artifact.

---

### Exploratory variant path

Potential future design:

```cpp
template <typename Policy = aligned_32_policy>
class Hydra;
```

or

```cpp
template <std::size_t InlineBytes = 32>
class Hydra;
```

Candidate policies:

* `aligned_32_policy`

  * performance-first
  * benchmark default
  * SIMD-ready
  * simpler codegen

* `packed_24_policy`

  * memory-density-first
  * higher container density
  * potentially 8-byte savings per object
  * may improve some memory-bound workloads

This would allow developers to explicitly choose:

> **throughput-optimized**
> vs
> **density-optimized**

---

### Important caution

Do **not** introduce policy templating until the current canonical implementation stabilizes.

Premature policy abstraction risks:

* benchmark fragmentation
* code duplication
* optimizer divergence
* more difficult perf attribution
* documentation complexity

The benchmark narrative is currently built around the 32-byte aligned design.

That story should remain stable until v1 architecture is considered mature.

---

### Suggested roadmap gate

Only explore policy-based layouts after:

* multiplication kernels stabilize
* shift / division support lands
* signed representation strategy is chosen
* benchmark suite remains green and apples-to-apples

Treat this as a **Phase 2 architectural fork**, not an immediate work item.
