# Hydra Benchmark Report

Generated: `2026-04-18`  Machine: Apple M5 Pro (arm64, macOS)  Build: `Release`

> Current state after the **scratch-workspace** (pow_mod allocator removal),
> **dual-row schoolbook leaf kernel**, **SOS Montgomery null-result**, and
> **dual-row CIOS / FIOS** sprints.  `pow_mod` at every width in the
> fused-CIOS band (k=8..31, i.e. 512-bit through 1984-bit) moved
> materially in the FIOS sprint; the canonical fused CIOS
> (`montgomery_mul_fused`) now stays in-tree as a reference and is
> called as the benchmark A-side for future experiments, but dispatch
> routes through `montgomery_mul_fios` instead.  Karatsuba-backed
> widths (k ≥ 32, 2048-bit and 4096-bit) and the sub-fused band
> (k < 8, 256-bit) were not touched and are unchanged.
> See `DIRECTORS_NOTES.md` for hypothesis / design / rationale history.

---

### pow_mod — Modular Exponentiation

_Min-of-6 runs, 50-sample median per run; `bench_pow_mod --markdown`_

| Width | Hydra (FIOS) | Hydra (was)  | Δ vs prior | GMP       | OpenSSL   | Hydra / GMP | Hydra / OpenSSL |
|------:|-------------:|-------------:|-----------:|----------:|----------:|------------:|----------------:|
|   256 |    9.60 µs   |    9.67 µs   |      −1 %  |  7.21 µs  |  5.29 µs  |       1.33× |           1.82× |
|   512 |   35.35 µs   |   52.06 µs   |     −32 %  | 27.58 µs  | 19.00 µs  |       1.28× |           1.86× |
|  1024 |  232.33 µs   |  309.23 µs   |     −25 %  | 152.63 µs | 109.75 µs |       1.52× |           2.12× |
|  1536 |  775.35 µs   |    1.14 ms   |     −32 %  | 461.67 µs | 336.75 µs |       1.68× |           2.30× |
|  1984 |    1.78 ms   |    2.51 ms   |     −29 %  |   1.03 ms |   1.65 ms |       1.73× |           1.08× |
|  2048 |    2.59 ms   |    2.59 ms   |       0 %  |   1.09 ms |  782.9 µs |       2.38× |           3.31× |
|  4096 |   20.08 ms   |   19.93 ms   |      +1 %  |   7.47 ms |   5.82 ms |       2.69× |           3.45× |

_1024-bit moved from −1.96× to −1.52× vs GMP (from +96 % to +52 % gap); 1536
and 1984 gained similarly.  Karatsuba-backed widths (2048/4096) unchanged.
256-bit is below `FUSED_THRESHOLD = 8` and stays on the separate
schoolbook + REDC path, untouched by this sprint._

---

### Montgomery kernel A/B (fixed operands, no pow_mod rotation)

_`build-rel/probe_mont_fios`, median of warmup+hot reps at each k._

| k  | `montgomery_mul_fused` | `montgomery_mul_fios` | Δ fios / fused |
|---:|-----------------------:|----------------------:|---------------:|
|  4 |               44.6 ns  |              21.4 ns  |        −52 %   |
|  6 |               60.6 ns  |              36.0 ns  |        −41 %   |
|  8 |               83.3 ns  |              54.4 ns  |        −35 %   |
| 12 |              134.4 ns  |             103.0 ns  |        −23 %   |
| 16 |              235.9 ns  |             181.6 ns  |        −23 %   |
| 24 |              591.3 ns  |             425.2 ns  |        −28 %   |
| 31 |             1049.3 ns  |             826.5 ns  |        −21 %   |
| 32 |             1111.0 ns  |             869.1 ns  |        −22 %   |

_The k=4 and k=6 entries show FIOS's structural advantage over fused
CIOS at small widths — but dispatch at those k's routes through
`montgomery_mul` (separate schoolbook + REDC), not fused, so the
kernel win does not translate to the end-to-end 256-bit column.
Whether to lower `FUSED_THRESHOLD` below 8 is a potential follow-up._

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

### Hot-path hotspots after the FIOS sprint

1. **Sub-fused band (k < 8, 256-bit) separate schoolbook + REDC** —
   still the dominant cost at 256-bit.  The FIOS kernel probe shows
   the dual-row structure would also help at k=4,6 (−52 % / −41 %
   vs fused CIOS), but the current dispatch routes k < 8 through
   `montgomery_mul` (separate schoolbook + REDC), not FIOS.  Whether
   to lower `FUSED_THRESHOLD` below 8 depends on FIOS also beating
   the separate path at k=4,6 — not yet measured.
2. **Karatsuba-backed Montgomery REDC (k ≥ 32, 2048+)** — after FIOS
   moved the fused band, the 2048/4096-bit columns are unchanged
   and GMP/OpenSSL gaps there remain the largest deltas
   (Hydra/OpenSSL ≈ 3.3–3.5×).  The REDC phase of Karatsuba-backed
   Montgomery still uses the sequential `montgomery_redc` word-by-
   word reduce.  A "dual-chain REDC" analogous to FIOS's reduce half
   is a candidate follow-up.
3. **Schoolbook leaf at k=16** — the dual-row leaf kernel at n=16
   shows only a −3 % delta vs. the old scalar (whereas k=32 / k=64
   are −40 %).  Compiler auto-vectorization of the baseline narrows
   the gap.  Largely superseded as a pow_mod bottleneck by the FIOS
   win, but still marginal interest for raw `Hydra * Hydra` at 1024-bit.
4. **`mul_general` dispatch overhead at k=32** — Karatsuba path is
   5 % slower than raw schoolbook because the padding glue isn't
   free.  Only affects public `operator*`; `pow_mod_montgomery` has
   its own stack-buffered padding so this doesn't leak into the hot
   pow_mod path.

---

_Update by running `bench/run.sh` + `bench_pow_mod` and regenerating
numbers.  Commit intent: reflect the present state, not the history —
history belongs in `DIRECTORS_NOTES.md`._
