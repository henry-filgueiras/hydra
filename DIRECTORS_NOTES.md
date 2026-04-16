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

### Division Foundation Layer (Phase 1 extension)

_Implemented 2026-04-15 — Claude Sonnet 4.6_

Three new kernels in `hydra::detail` and four new public methods on `Hydra`:

#### detail kernels

| Kernel              | Purpose                                             |
|---------------------|-----------------------------------------------------|
| `shl_limbs`         | Left-shift a limb array by arbitrary bit count      |
| `shr_limbs`         | Right-shift a limb array by arbitrary bit count     |
| `divmod_u64_limbs`  | Short division of limb array by scalar uint64_t     |

**`shl_limbs`** decomposes `shift` into `whole = shift/64` (whole-limb offset,
handled via `memcpy` or loop offset) and `bits = shift%64` (intra-limb shift,
handled with a two-register sliding window).  The `bits==0` branch avoids the
carry loop entirely.  Output buffer size = `na + whole + 1` limbs.

**`shr_limbs`** mirrors `shl_limbs`; the stitching step is
`out[i] = (a[i+whole] >> bits) | (a[i+whole+1] << (64-bits))`.
`64-bits` is safe because the `bits==0` branch is handled separately.

**`divmod_u64_limbs`** processes limbs MSL→LSL via `unsigned __int128`:
`(rem*2^64 + limbs[i]) ÷ d` — maps to a single `divq` on x86-64,
`udiv`/`msub` on aarch64.  This is the exact primitive used in Knuth D's
inner loop for computing trial quotient digits.

#### Public API additions

| Method                            | Tier support | Heap activity           |
|-----------------------------------|--------------|-------------------------|
| `Hydra operator<<(unsigned)`      | All 3        | None ≤256-bit, one alloc above |
| `Hydra operator>>(unsigned)`      | All 3        | None ≤256-bit, one alloc above |
| `Hydra& operator<<=`              | All 3        | delegates to `<<`       |
| `Hydra& operator>>=`              | All 3        | delegates to `>>`       |
| `Hydra div_u64(uint64_t)`         | All 3        | None ≤256-bit, one alloc above |
| `uint64_t mod_u64(uint64_t)`      | All 3        | **Zero always**         |

`mod_u64` is completely heap-free at all sizes — it computes the remainder
in a single pass without storing the quotient.

#### Small fast paths

`operator<<` has a dedicated `is_small() && shift < 64` fast path that
computes the 128-bit result with two arithmetic instructions and returns
without touching the general kernel.  `operator>>` likewise avoids the
kernel for Small (whole must be 0 after the early-exit guard, so `>> shift`
is a single `payload.small >> shift`).

#### Normalization invariant

Both shift operators and `div_u64` call `normalize()` after construction,
so the result always occupies the smallest valid tier.  `shr_limbs` trims
trailing zeros internally; `shl_limbs` simply never writes leading zeros
(the carry slot is only filled when carry != 0).

#### `to_string()` refactor

The medium/large path of `to_string()` previously allocated a `std::vector`
per decimal digit.  It now calls `mod_u64(10)` (zero heap) and `div_u64(10)`
(one LargeRep alloc on the heap path), reducing temporary allocation by one
per digit and making the intent explicit.

#### Groundwork for Knuth Algorithm D (large ÷ large)

Knuth D operates in three phases:

1. **Normalize**: left-shift divisor until its leading limb has bit 63 set
   (`shift = clz(v[m-1])`).  Apply same shift to dividend.
   → **Uses `operator<<`**.
2. **Main loop**: for each quotient digit `q_hat`, estimate via
   `(u[j+m]*2^64 + u[j+m-1]) / v[m-1]` — a 2-limb ÷ 1-limb step,
   exactly what `divmod_u64_limbs` already does on a 2-element slice.
   → **Uses `divmod_u64_limbs` directly**.
3. **De-normalize**: right-shift remainder by the normalization shift.
   → **Uses `operator>>`**.

`div_u64` / `mod_u64` are also the direct single-limb-divisor base case
of Knuth D when `m == 1`.  Everything needed for the algorithm's outer
skeleton is now in place; the next phase adds the trial-quotient loop and
the multi-limb multiply-subtract step.

---

### Shift Benchmark Suite + Audit Findings

_Added 2026-04-16 — Claude Opus 4.6_

The shift substrate (Phase 1) landed 2026-04-15 (commit `5646372`). A
follow-up audit confirmed the implementation meets every design constraint:

- **Tier coverage** — Small fast-path, Medium via 4-limb stack buffer
  (covers Medium at 3 limbs plus a 4-limb Large bonus), heap path otherwise.
- **Allocation discipline** — Shift results that fit in ≤ 4 limbs never
  allocate; heap path writes directly into `LargeRep::limbs()` with no
  scratch buffer, mirroring the `add_general` pattern.
- **Normalization** — `from_limbs()` and `normalize()` cover every exit so
  Large→Medium→Small demotion happens in a single shift when possible
  (`test_shr_large_demotes_to_small` exercises a 4-limb → Small collapse).
- **Primitive separation** — `detail::shl_limbs` and `detail::shr_limbs`
  each split `shift` into `whole = shift/64` and `bits = shift%64` and
  branch on `bits == 0` to avoid the `64 - bits` UB edge.

#### New benchmark coverage

Added in `bench/bench_hydra.cpp` (§ 7b):

| Name                     | Inputs                              | What it isolates |
|--------------------------|-------------------------------------|------------------|
| `shift/left_small`       | `Hydra{u64}`, shift ∈ {1, 63}       | Small fast path |
| `shift/left_medium`      | 3-limb Medium, shift ∈ {1, 63, 64, 65, 127} | Stack vs heap path |
| `shift/left_large`       | 8-limb Large, shift ∈ {1, 63, 64, 65, 127}  | Pure heap path |
| `shift/right_medium`     | 3-limb Medium, shift ∈ {1, 63, 64, 65, 127} | Stitch + demote |
| `shift/right_large`      | 8-limb Large, shift ∈ {1, 63, 64, 65, 127}  | Multi-limb stitch |
| `chain/shift_small_10`   | 5× `(<<1, >>1)` loop                 | Steady-state Small |
| `chain/shift_large_10`   | 5× `(<<1, >>1)` on 4-limb Large      | Steady-state heap |

Shift magnitudes are chosen at the limb-geometry cliffs: `1` (trivial carry),
`63` (max intra-limb), `64` (pure whole-limb, `bits==0` memcpy path), `65`
(first shift where both whole and bits are non-zero), `127` (full stitch on
a 2-limb window). All five branches of `shl_limbs`/`shr_limbs` are covered.

#### Representative numbers (Linux g++ -O3, sandbox, single-core 48 MHz VM)

These absolute times are not portable to the host Mac, but the *relative*
structure is meaningful:

```
shift/left_small/1        0.67 ns    (Small fast path)
shift/left_medium/1       3.85 ns    (Medium, stays Medium, stack path)
shift/left_medium/63      8.28 ns    (promotes to Large, heap path)
shift/left_large/1        8.11 ns    (8-limb Large, heap path)
shift/left_large/65       8.94 ns    (slightly more work, stitching)
shift/right_medium/1      3.68 ns    (stays Medium, stack path)
shift/right_large/1       9.31 ns    (8-limb right-shift, heap path)
chain/shift_small_10      1.76 ns/op (5x shift round-trip, stays Small)
chain/shift_large_10     10.1 ns/op  (5x shift round-trip on Large)
```

The Small fast path at 0.67 ns — an order of magnitude under any kernel
call — validates the "pay for complexity only when the value actually
needs it" thesis at the shift layer.

#### Stale bench comments fixed

Two comments in `BM_boost_large_add` / `BM_hydra_large_add_for_boost_cmp`
claimed "Hydra has no bit-shift operator" as rationale for the fixed-operand
fold. Updated to reflect the post-Phase-1 reality: shifts exist, but fixed
operands still isolate the add measurement from shift cost.

---

### Full Hydra÷Hydra Division (Knuth Algorithm D)

_Implemented 2026-04-16 — Claude Opus 4.6_

Phase 2's headline feature: `Hydra::divmod` (plus `div`/`mod` delegates)
produces quotient + remainder in a single pass for any two positive Hydras.

#### Algorithm choice: Knuth D over Burnikel-Ziegler

BZ was considered and deferred. It only amortises above ~128 limbs and its
inner multiply must be sub-quadratic for the asymptotic win to materialise.
Hydra's current multiply ceiling is schoolbook O(n²) (8×8 kernel); wiring
BZ on top would produce exactly the same asymptotic cost as Knuth D with
more complexity. Knuth D is the right fit until Karatsuba lands.

#### Public API shape

```cpp
struct Hydra::DivModResult { Hydra quotient; Hydra remainder; };

Hydra::DivModResult Hydra::divmod(const Hydra& divisor) const;
Hydra               Hydra::div   (const Hydra& divisor) const;   // delegates
Hydra               Hydra::mod   (const Hydra& divisor) const;   // delegates
```

`DivModResult` is declared as a nested struct, but defined **out-of-line**
at namespace scope because its fields have type `Hydra` — which is
incomplete at the point of the forward declaration inside the class body.
The three methods are forward-declared inside the class and defined after
`Hydra` closes. This is invisible to callers.

Division by zero throws `std::domain_error`. Negative operands are
undefined (signed arithmetic is still Phase 2 roadmap). Zero dividend,
`divisor > dividend`, and `divisor == dividend` are all handled without
entering the Knuth D kernel.

#### Scratch buffer policy: stack ≤32 limbs, heap above

```cpp
constexpr uint32_t STACK_LIMIT = 32;  // 2048-bit operands
uint64_t q_stack[STACK_LIMIT + 1];          // 33 × 8 = 264 B
uint64_t r_stack[STACK_LIMIT];              //       256 B
uint64_t work_stack[(STACK_LIMIT + 1) + STACK_LIMIT]; // 520 B
```

Total stack frame for the divmod call ≈ 1 KiB — well within any reasonable
budget. Operands larger than 2048 bits fall through to `std::vector` scratch.
This matches the same threshold we use for `mul_general` sizing.

#### Kernel: `detail::divmod_knuth_limbs`

Located in `hydra.hpp` adjacent to `divmod_u64_limbs`. Structure:

1. **Normalize.** `shift = __builtin_clzll(v[nv-1])` so the divisor's
   most-significant bit is set. Apply to both `u` (with carry out, hence
   the `nu+1` slot) and `v` via the same sliding-window pattern used by
   `shl_limbs`. Zero-shift path is a `memcpy`.

2. **Main loop.** For `jj` from `nu-nv` down to `0`:
   - Estimate `q_hat` from the top two dividend limbs divided by the
     top divisor limb. Clamp to `~0ull` when the top dividend limb
     equals the top divisor limb (the edge case Knuth explicitly calls out).
   - Refine via the three-limb check
     `q_hat*v[nv-2] > (r_hat<<64 | u[j+nv-2])`. An `r_hat_overflowed`
     boolean short-circuits the refinement when `r_hat + v[nv-1]`
     exceeds 64 bits — cleaner than a `goto` out of the loop.
   - Multiply-subtract `q_hat * v` from `u[j..j+nv]`. Borrow propagation
     uses the two-step unsigned pattern
     `d1 = ui - p_lo; b1 = (d1 > ui); d2 = d1 - borrow; b2 = (d2 > d1);`
     to avoid signed-integer UB entirely.
   - Add-back step (rare: fires only when `q_hat` was over by 1 after
     refinement): decrement `q_hat`, add `v` back into `u[j..j+nv]`,
     discard the final carry (it cancels the earlier underflow).
   - Store `q_hat` at `q[jj]`.

3. **De-normalize.** Right-shift the remainder in `u[0..nv]` by `shift`.
   Zero-shift path is a `memcpy`.

Aliasing safety is guaranteed by the two-step borrow pattern: every read
of `u[j+i]` completes before `u[j+i]` is written.

#### Correctness coverage

`hydra_test.cpp` adds **18** divmod tests covering the shape of the
algorithm, not just random sweeps:

- Zero dividend, divisor > dividend, equal values, divisor == 0 (throws)
- Exact divisibility round-trip (`(q*b)/b == q`)
- Power-of-two divisors cross-checked against `>>`
- Single-limb delegation (`nv == 1` falls back to `div_u64`/`mod_u64`)
- Pre-normalized divisor (top bit already set → `shift == 0` path)
- Worst-case `q_hat` overestimate
  (`v = [FFFF…, FFFF…, 8000…01]`) — exercises the refinement loop
- 200 random pairs targeting add-back frequency
- Stack-boundary (32/16 limbs) and heap path (40/20 limbs)
- 128/64, 192/128, 512/256, 1024/512 (match the benchmark shapes)
- Delegate consistency (`div == divmod.quotient`, `mod == divmod.remainder`)

All 457 tests pass at `-O0` with ASan+UBSan and at `-O2`.

#### Representative numbers (Linux g++ -O3, sandbox VM)

```
div/128_64         17.1 ns    (single-limb divisor → div_u64 delegation)
div/192_128        18.3 ns    (minimum Knuth D shape, nv=2)
div/512_256        65.5 ns    (nv=4, matches large_mul_256 width)
div/1024_512        133 ns    (nv=8, matches large_mul_512 width)
```

The 128/64 case routes through `div_u64` so its 17 ns baseline reflects
one allocation plus the scalar div path. The 192/128 → 1024/512 walk
confirms roughly O(n²) scaling with small constant factors — consistent
with Knuth D's expected cost when the outer loop runs `nu - nv + 1` times
and the inner multiply-subtract is linear in `nv`.

#### What this unlocks

- `to_string` no longer has to loop through `mod_u64(10)` — a future
  refactor can use `divmod(1_000_000_000)` to chunk 9 decimal digits at
  a time.
- Modular reduction for number-theoretic workloads (no longer limited
  to 64-bit moduli).
- GCD via Euclidean reduction directly on Hydras.

---

### Knuth-D Profiler Pass (Phase 2 follow-up)

_Conducted 2026-04-16 — Claude Opus 4.6_

A profiler-guided optimization pass focused exclusively on
`detail::divmod_knuth_limbs`.  The instruction from the director was
explicit: **only land optimizations supported by measured hotspot data —
no speculative churn.**  After instrumentation and A/B testing, the
**final landed change is zero new kernel code paths**; the contribution
is a reusable measurement harness, documented findings, and an
explicitly-curated list of rejected speculative ideas.

#### Tooling landed

| Artifact                          | Purpose                                                              |
|-----------------------------------|----------------------------------------------------------------------|
| `HYDRA_PROFILE_KNUTH=1` macro     | Compile-time opt-in. Enables ten inline counters + per-section timing. |
| `detail::KnuthProfSection`        | RAII region timer (steady_clock); zero cost when undefined.          |
| `bench/profile_knuth.cpp`         | Standalone timing harness (no Google Benchmark dependency).          |
| `HYDRA_KNUTH_PROF_SECTION` macros | Expand to `((void)0)` in production.  Verified by diffing codegen:   |
|                                   | assembly of `divmod_knuth_limbs` is byte-identical with vs without   |
|                                   | the macros (342 lines either way).                                   |

Build and run:
```bash
# Production build (no instrumentation — zero overhead)
g++ -std=c++20 -O3 -DNDEBUG -I. bench/profile_knuth.cpp -o profile_knuth

# Instrumented build (per-section counters live)
g++ -std=c++20 -O3 -DNDEBUG -DHYDRA_PROFILE_KNUTH=1 -I. \
    bench/profile_knuth.cpp -o profile_knuth_inst
```

#### Measured cost distribution

Linux aarch64 sandbox, g++ 11 `-O3 -DNDEBUG`, median of 10 runs, random
operand pairs (non-adversarial, MSB of divisor pre-set so no shift-zero
edge case dominates).  "direct" = kernel called with caller-owned
scratch (skips `from_limbs`/allocate/normalize wrapper).

```
shape        direct ns/op   divmod ns/op   per-step ns   per-inner-limb ns
256/128         25.3           26.2            8.4           4.2
512/256         64.0           71.5           12.8           3.2
1024/512       125.7          128.8           14.0           1.8
2048/1024      311.2          308.0           18.3           1.14
```

Per-step = direct ÷ (nu − nv + 1) outer iterations.
Per-inner-limb = per-step ÷ nv.  The descending per-inner-limb cost is
expected — the outer per-step overhead amortises across more inner limbs.

The `divmod` wrapper adds effectively zero overhead vs `direct` at all
shapes (some runs are even slightly *faster* due to allocator residency
effects — the stack buffers in `Hydra::divmod` are warm on second-pass
repeats).  This validates the "stack ≤ 32 limbs" threshold — nothing
benchmarked here touches `std::vector`-backed scratch.

#### Per-section breakdown (instrumented build)

Absolute numbers below are inflated by ~20–30 ns per `steady_clock::now()`
call.  **Compare the relative columns, not absolute numbers** — the goal
is to partition total cost, not to measure section cost precisely.

```
shape          norm       qest     qref    mulsub    adback    denorm     avg refineIter/step
256/128        16 ns      63 ns    43 ns   46 ns     0 ns      16 ns      0.00
512/256        23 ns     104 ns    74 ns   76 ns     0 ns      15 ns      0.60   (but divided across 5 steps = 0.12)
1024/512       17 ns     176 ns   140 ns   152 ns    0 ns      15 ns      0.33
2048/1024      18 ns     373 ns   246 ns   348 ns    0 ns      15 ns      0.06
```

Subtracting the ~15 ns timer overhead per section and comparing totals:

- **Normalize:** small constant (~1–3 ns real work, dominated by a
  single limb-shift pass).  **Under 5% of total at every shape.**
- **Denormalize:** same — small constant.
- **q_hat estimate** (divq / __udivti3 on aarch64): scales with outer
  steps, not with nv.  **At nv ≥ 8 it is the single biggest per-step
  contributor.**
- **q_hat refinement check:** 128-bit multiply + compare per iteration,
  0 or 1 iterations per step.  Measured iteration rate 0.24–0.33/step.
- **Multiply-subtract:** scales as O(steps × nv).  **Dominant total
  cost at nv ≥ 8.**
- **Add-back:** zero hits across 4000 random operand pairs × all shapes.
  The branch is taken 0% of the time on non-adversarial inputs.

#### Branch-predictability survey (1000 random pairs per shape)

```
shape         refineIter / step   addback / step   qhatClamp / step
256/128           0.2377             0.0000            0.0000
512/256           0.2854             0.0000            0.0000
1024/512          0.3094             0.0000            0.0000
2048/1024         0.3251             0.0000            0.0000
```

**Add-back** and **q_hat clamp** (u_top == v_hi): both truly cold.
The branch predictor learns these in a handful of iterations and then
never mispredicts.  **Refinement loop** iterates on roughly 1-in-4 to
1-in-3 steps — not cold, not hot, and already a single-iteration loop
in practice (Knuth §4.3.1 Theorem B bounds it at 2).

#### Stack-vs-heap scratch threshold

All four benchmarked shapes fit within `STACK_LIMIT = 32` (the
`Hydra::divmod` stack budget).  The heap-scratch path (`std::vector`)
is only exercised by `nu > 32` (operands wider than 2048-bit).
No measured perf delta between direct and wrapped calls → heap path
is not a current bottleneck at the shapes the project cares about.

#### A/B tested — NOT LANDED — branch hints

The measured frequency data suggested three possible branch hints:

| Location                                  | Measured hit rate | A/B result |
|-------------------------------------------|-------------------|------------|
| `[[unlikely]]` on add-back path           | 0 hits / step     | Neutral    |
| `[[unlikely]]` on `u_top >= v_hi` clamp   | 0 hits / step     | Regression on 256/128 & 512/256 |
| `[[likely]]`  on `u_top <  v_hi` (inverted) | 100% hit rate    | Regression on 256/128 & 512/256 |

Measurement methodology: 10 interleaved runs per binary, minima
compared, confirmed across 3 separate build cycles.

**Net effect of landing both hints together:**
- 256/128:  25.1 ns → 27.3 ns  (+8.8% regression)
- 512/256:  64.1 ns → 69.2 ns  (+8.0% regression)
- 1024/512: 125.7 ns → 121.5 ns (−3.3% improvement)
- 2048/1024: 310 ns → 312 ns    (flat — noise)

Root cause analysis: modern branch predictors learn these trivial
cold paths in the first ~10 iterations; the `[[unlikely]]` attribute
changes only **code layout**, moving the cold body further from the
hot per-step body.  For small-nv shapes the outer loop is short
enough that the jump to the out-of-line cold block lands in a
less-cache-friendly location (or disrupts the fetch-block alignment
of the hot path).  Larger nv tolerates the layout change better, but
not by enough to offset the per-step regression.

**Decision:** land neither hint.  Leave a comment at each site
pointing at this note, so future agents don't waste time re-proposing
the same optimization.  Rationale noted inline:
```cpp
// Measured 2026-04-16: adding [[unlikely]] here was
// performance-neutral at every shape (compiler + modern branch
// predictor already treat the path as cold).  Kept hint-free to
// avoid the layout perturbation that the paired `[[unlikely]]` on
// the qhat clamp caused.
```

#### Rejected speculative ideas (not data-supported)

Each of these was considered during the pass.  None were implemented
because no measurement justified the change.

1. **Hand-unrolled `nv == 2` specialization (256/128 fast path).**
   Rationale would be: the 256/128 shape spends most of its time in
   per-step fixed overhead (q_hat estimate, refinement check).  A
   hand-written 2-limb kernel could save ~30% on this one shape.
   **Rejected** because: no production workload has been identified
   that is bottlenecked on 256-bit division; adding a second kernel
   path doubles the correctness-testing surface for one shape.
   Revisit when (if) a real consumer shows up.

2. **Barrett/reciprocal-based trial quotient.**
   Reciprocal-of-divisor estimation (Möller–Granlund) can shave the
   `__udivti3` call path.  **Rejected** because: on aarch64, the
   trial quotient compiles to a call into `__udivti3` (128÷64) which
   is a single `udiv` on the CPU — already well-pipelined.  Barrett
   wins only when the divisor is reused across many divisions, which
   is not this kernel's shape.

3. **Reduced refinement iterations.**
   The current while-loop does at most 2 iterations.  Refinement
   fires on ~25–33% of steps, always doing exactly 1 iteration at
   the measured distribution.  **Rejected** — no room to shorten.

4. **Loop unrolling in mul-sub.**
   `nv` varies from 2 to 32 at runtime.  Unrolling would require a
   template dispatch matrix.  Generated assembly at `-O3` already
   issues 1 MAC per ~7 instructions with good pipelining (verified
   by inspecting `knuth_asm_probe.s` — `mul`/`umulh`/`adds`/`cinc`/
   `subs`/`cset`/`sbcs` pattern).  **Rejected** — nothing to gain
   without radically restructuring the kernel.

5. **Avoid scratch zero-initialisation.**
   Audited: the Knuth kernel never depends on scratch being
   zeroed.  `std::memcpy` / shift loops write every limb they read.
   `Hydra::divmod`'s stack buffers are uninitialised arrays (no
   zero-init) and `std::vector::resize` only zero-inits on the heap
   path which is never exercised ≤ 2048-bit.  **Nothing to fix.**

6. **Scratch reuse across outer loop iterations.**
   The kernel already performs all work in-place within the single
   `work[]` buffer (u_norm + v_norm layout).  No per-step allocation.
   **Nothing to fix.**

7. **Sub-quadratic division (Burnikel-Ziegler, Mulders, etc.).**
   Out of scope for this pass — and already noted in the main
   phase-2 roadmap as "not worth it until Karatsuba multiply lands."

#### Running the profiler

Future profiler passes should re-run:
```bash
g++ -std=c++20 -O3 -DNDEBUG -I. bench/profile_knuth.cpp \
    -o /tmp/profile_knuth_plain
g++ -std=c++20 -O3 -DNDEBUG -DHYDRA_PROFILE_KNUTH=1 -I. \
    bench/profile_knuth.cpp -o /tmp/profile_knuth_inst
/tmp/profile_knuth_plain    # clean timing
/tmp/profile_knuth_inst     # per-section breakdown
```

Run each binary 5–10 times and take the minimum or median — the
sandbox VM has ~5 ns of per-run noise at the 25–70 ns scale.

---

### Phase 2 Roadmap (Active TODOs)

_Catalogued 2026-04-15 — Claude Sonnet 4.6; updated 2026-04-16_

- **No signed arithmetic.** Sign bit (meta bit 2) is allocated but ignored.
- ~~**No full Hydra÷Hydra division.**~~ **Landed 2026-04-16** as Knuth
  Algorithm D in `detail::divmod_knuth_limbs` with `Hydra::divmod`/`div`/`mod`
  public API. See "Full Hydra÷Hydra Division" above.
- **`to_string()` still slow.** Now uses `div_u64`/`mod_u64` (one alloc per
  digit); replace with base-10^9 extraction to cut digit-loop count by 9×.
- **Schoolbook O(n²) multiplication.** Fine up to ~200 limbs; add Karatsuba
  at n ≥ 32.
- **No allocator customisation.** `LargeRep` uses `::operator new` directly.
  A PMR-style allocator hook is a natural extension.
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
