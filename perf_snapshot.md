# Hydra Benchmark Report

Generated: `2026-07-10`  Machine: Apple M5 Pro (arm64, macOS)  Build: `Release`

> Current state after the **KARATSUBA_MONT_THRESHOLD retirement**
> (2026-07-10): FIOS (dual-row CIOS) now owns the entire Montgomery
> band k = 1..64.  The Karatsuba+REDC tier that previously served
> k ≥ 32 was benchmark-retired — FIOS beats it at every k in 31..64
> with no crossover (−22.6 % at k=32, −28.0 % at k=64 kernel-level),
> worth **−26 % at 2048-bit and −24 % at 4096-bit** end-to-end.  The
> threshold had been derived against fused CIOS (pre-FIOS) and was
> never re-measured after FIOS landed.  `montgomery_mul_karatsuba`,
> canonical fused CIOS, separate schoolbook+REDC, and SOS all stay
> in-tree as correctness references, not reachable via dispatch.
> `bench_pow_mod` gained `--runs N` (min-of-medians + cross-run CV),
> institutionalizing the manual 6-run A/B protocol.
> See `DIRECTORS_NOTES.md` for hypothesis / design / rationale history.

---

### pow_mod — Modular Exponentiation

_`bench_pow_mod --runs 6` (min-of-6-medians, 50 samples/run); GMP and
OpenSSL columns carried from the 2026-04-18 report (those backends
were not rebuilt this pass; their code is unchanged)._

| Width | Hydra (now)  | Hydra (prior)| Δ vs prior | GMP       | OpenSSL   | Hydra / GMP | Hydra / OpenSSL |
|------:|-------------:|-------------:|-----------:|----------:|----------:|------------:|----------------:|
|   256 |    7.42 µs   |    7.29 µs   |     ~0 %   |  7.21 µs  |  5.29 µs  |       1.03× |           1.40× |
|   512 |   36.33 µs   |   36.42 µs   |     ~0 %   | 27.58 µs  | 19.00 µs  |       1.32× |           1.91× |
|  1024 |  229.21 µs   |  234.56 µs   |      −2 %  | 152.63 µs | 109.75 µs |       1.50× |           2.09× |
|  1536 |  766.77 µs   |  781.12 µs   |      −2 %  | 461.67 µs | 336.75 µs |       1.66× |           2.28× |
|  1984 |    1.78 ms   |    1.78 ms   |       0 %  |   1.03 ms |   1.65 ms |       1.73× |           1.08× |
|  2048 |    1.94 ms   |    2.59 ms   |   **−25 %**|   1.09 ms |  782.9 µs |       1.78× |           2.48× |
|  4096 |   15.35 ms   |   20.13 ms   |   **−24 %**|   7.47 ms |   5.82 ms |       2.05× |           2.64× |

_2048/4096-bit moved from Karatsuba+REDC to FIOS.  The 1984→2048-bit
cliff (1.78 → 2.59 ms, +45 % for a 3 % width step) is gone — the curve
is now smooth across the old backend boundary (1.78 → 1.94 ms).
Cross-run CV at these widths: 1.6 % / 0.4 %._

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
CIOS at small widths.  After the 2026-04-18 threshold cleanup,
`FUSED_THRESHOLD = 1` so dispatch at these k's now routes through
FIOS — the kernel win translates directly to the 256-bit end-to-end
number (see the `pow_mod` table above).  The follow-up `probe_fios_small_k`
sweep (k = 1..7, FIOS vs `montgomery_mul`/`montgomery_sqr`) showed
FIOS wins of −17 % to −33 % end-to-end; full table in DIRECTORS_NOTES.md._

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

### Hot-path hotspots after the threshold retirement

1. **FIOS squaring has no cross-term halving** — pow_mod is ~5:1
   sqr:mul and `montgomery_sqr_fios` forwards to the full mul.  A
   dual-chain squaring that exploits `a[i]·a[j] == a[j]·a[i]` is the
   largest single line item left, at every width (the whole k = 1..64
   band now runs FIOS).  Non-trivial correctness story; see the
   2026-07-10 dragon's next-sprint ranking.
2. **GMP/OpenSSL gap at 2048/4096-bit** — narrowed to 1.78×/2.05×
   (GMP) and 2.48×/2.64× (OpenSSL) after the retirement, from
   2.38×/2.69× and 3.31×/3.46×.  What remains is squaring
   specialization (they have it, Hydra doesn't) plus hand-tuned asm
   (exploration closed after two null results — portable levers only).
3. **Schoolbook leaf at k=16** — the dual-row leaf kernel at n=16
   shows only a −3 % delta vs. the old scalar (whereas k=32 / k=64
   are −40 %).  Compiler auto-vectorization of the baseline narrows
   the gap.  Only affects raw `Hydra * Hydra`; pow_mod no longer
   calls the schoolbook leaves at all (Karatsuba tier retired).
4. **`mul_general` dispatch overhead at k=32** — Karatsuba path is
   5 % slower than raw schoolbook because the padding glue isn't
   free.  Only affects public `operator*`; `pow_mod_montgomery` has
   its own stack-buffered padding so this doesn't leak into the hot
   pow_mod path.

---

_Update by running `bench/run.sh` + `bench_pow_mod` and regenerating
numbers.  Commit intent: reflect the present state, not the history —
history belongs in `DIRECTORS_NOTES.md`._
