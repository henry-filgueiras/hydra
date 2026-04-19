# Hydra Benchmark Report

Generated: `2026-04-18`  Machine: Apple M5 Pro (arm64, macOS)  Build: `Release`

> Current state after the **scratch-workspace** (pow_mod allocator removal),
> **dual-row schoolbook leaf kernel**, and **SOS Montgomery null-result**
> sprints.  No `pow_mod` numbers moved in the SOS sprint; the SOS primitives
> ship as callable alternates in `detail::` but the default dispatch still
> uses fused CIOS for k=8..31 because the SOS structure costs more
> end-to-end than fused CIOS in that range.  See `DIRECTORS_NOTES.md` for
> hypothesis / design / rationale history.

---

### pow_mod — Modular Exponentiation

_10-run median; `bench_pow_mod --json`_

| Width | Hydra     | GMP      | OpenSSL  | Hydra / GMP | Hydra / OpenSSL |
|------:|----------:|---------:|---------:|------------:|----------------:|
|   256 |   9.98 µs |  7.33 µs |  5.40 µs |       1.36× |           1.85× |
|   512 |  51.63 µs | 27.58 µs | 20.54 µs |       1.87× |           2.51× |
|  1024 | 317.15 µs | 161.40 µs | 127.90 µs |     1.96× |           2.48× |
|  2048 |  2.71 ms  |  1.11 ms |  0.79 ms |       2.42× |           3.43× |
|  4096 | 20.09 ms  |  7.69 ms |  5.92 ms |       2.62× |           3.40× |

_2048/4096-bit improved by −16.6 % / −15.9 % in the dual-row sprint.
Below k ≥ 32 (2048-bit) Hydra stays on the fused-CIOS path, untouched
by the recent sprints._

---

### Multiplication — kernel microbenchmarks

_Median of 5×0.3 s, `hydra_bench`_

| k (limbs) | `mul_school` | `mul_karatsuba` | `mul_dispatched` |
|----------:|-------------:|----------------:|-----------------:|
|         1 |     2.2 ns   |            —    |             —    |
|         2 |     3.7 ns   |      4.9 ns     |             —    |
|         3 |     5.9 ns   |            —    |             —    |
|         4 |     6.9 ns   |      8.2 ns     |             —    |
|         8 |    22.3 ns   |     22.3 ns     |             —    |
|        16 |   136.8 ns   |     81.1 ns     |          113.5 ns |
|        32 |   345.6 ns   |    313.1 ns     |          361.9 ns |
|        64 |  1391.2 ns   |   1142.4 ns     |         1210.8 ns |
|       128 |       —      |            —    |         4117.6 ns |

_Karatsuba beats schoolbook starting at k=32 (−10 %); at k=64 the
margin is −18 %.  Dispatch overhead (mul_general's operand-padding +
workspace setup) adds ~50 ns vs. raw `mul_karatsuba`._

---

### Small operations vs. native `uint64_t`

_From `hydra_bench` baseline family; M5 Pro scalar._

| Operation                     | Subject   | Reference             | Ratio |
|-------------------------------|-----------|-----------------------|------:|
| small add                     | 3.1 ns    | baseline/u64_add 2.5  | 1.24× |
| small mul                     | 4.0 ns    | baseline/u64_mul 3.5  | 1.14× |
| widening mul 128 (vs native)  | 0.8 ns    | baseline/u64_mul 3.5  | 0.23× |

---

### Allocation costs

| Benchmark                          |  4   |  16  |  64  | 256  |
|------------------------------------|-----:|-----:|-----:|-----:|
| `alloc/from_limbs` (ns)            | 11.6 |  10.0 | 21.0 | 29.8 |
| `alloc/largerep_create_destroy`    |  9.7 |   9.3 | 16.2 | 13.4 |
| `alloc/largerep_clone`             | 12.1 |  10.4 | 20.9 | 29.5 |

`alloc/normalize_large_to_medium ≈ 13 ns` · `normalize_medium_to_small ≈ 1.8 ns`

---

### Hot-path hotspots after the last three sprints

1. **CIOS Montgomery row loop** — still the dominant cost at 1024-bit
   (k=16), where Karatsuba doesn't engage.  The SOS sprint
   (2026-04-18) confirmed that restructuring the multiply phase to
   reuse `mac_row_2` does *not* help — fused CIOS is structurally
   tighter at k=8..31 (smaller working set, single accumulator stays
   in L1).  Remaining options are inline asm on the row loop or a
   dual-row CIOS variant that keeps the fused accumulator.  See the
   "Dragon — SOS Montgomery (Null Result)" entry in
   `DIRECTORS_NOTES.md`.
2. **Schoolbook leaf at k=16** — the dual-row kernel at n=16 shows
   only a −3 % delta vs. the old scalar (whereas k=32 / k=64 are −40 %).
   Compiler auto-vectorization of the baseline narrows the gap.
   Marginal interest — maybe addressable via explicit 4-row unroll.
3. **mul_general dispatch overhead at k=32** — Karatsuba path is
   5 % slower than raw schoolbook because the padding glue isn't
   free.  Only affects public `operator*`; `pow_mod_montgomery` has
   its own stack-buffered padding so this doesn't leak into the hot
   pow_mod path.

---

_Update by running `bench/run.sh` + `bench_pow_mod` and regenerating
numbers.  Commit intent: reflect the present state, not the history —
history belongs in `DIRECTORS_NOTES.md`._
