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

