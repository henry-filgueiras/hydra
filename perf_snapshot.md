# Hydra Benchmark Report

Generated: `2026-07-10`  Machine: Apple M5 Pro (arm64, macOS)  Build: `Release`

> Current state after the 2026-07-10 arc: **KARATSUBA_MONT_THRESHOLD
> retirement** (FIOS owns the entire k = 1..64 Montgomery band; −26 %
> at 2048-bit, −24 % at 4096-bit) followed by the **halved FIOS
> squaring** landing (`montgomery_sqr_fios_halved`, ~1.5k² MACs vs
> 2k², dispatched for k ≤ 32; a further −2 % to −10 % at 256…2048-bit;
> 4096-bit stays on sqr-as-mul — the halved kernel regressed there).
> A `__restrict` probe on the FIOS pointers was a null result.
> Retired-but-callable references in `detail::`: Karatsuba Montgomery,
> canonical fused CIOS, separate schoolbook+REDC, SOS.
> `bench_pow_mod` gained `--runs N` (min-of-medians + cross-run CV),
> institutionalizing the manual 6-run A/B protocol.
> See `DIRECTORS_NOTES.md` for hypothesis / design / rationale history.

---

### pow_mod — Modular Exponentiation

_`bench_pow_mod --runs N` (min-of-medians, 50 samples/run); "prior" =
2026-04-18 report (pre-retirement, pre-halved-sqr).  GMP and OpenSSL
columns carried from the 2026-04-18 report (those backends were not
rebuilt this pass; their code is unchanged)._

| Width | Hydra (now)  | Hydra (prior)| Δ vs prior | GMP       | OpenSSL   | Hydra / GMP | Hydra / OpenSSL |
|------:|-------------:|-------------:|-----------:|----------:|----------:|------------:|----------------:|
|   256 |    7.08 µs   |    7.29 µs   |      −3 %  |  7.21 µs  |  5.29 µs  |   **0.98×** |           1.34× |
|   512 |   32.71 µs   |   36.42 µs   |     −10 %  | 27.58 µs  | 19.00 µs  |       1.19× |           1.72× |
|  1024 |  221.90 µs   |  234.56 µs   |      −5 %  | 152.63 µs | 109.75 µs |       1.45× |           2.02× |
|  1536 |  755.96 µs   |  781.12 µs   |      −3 %  | 461.67 µs | 336.75 µs |       1.64× |           2.24× |
|  1984 |    1.74 ms   |    1.78 ms   |      −2 %  |   1.03 ms |   1.65 ms |       1.69× |           1.05× |
|  2048 |    1.88 ms   |    2.59 ms   |   **−27 %**|   1.09 ms |  782.9 µs |       1.72× |           2.40× |
|  4096 |   15.35 ms   |   20.13 ms   |   **−24 %**|   7.47 ms |   5.82 ms |       2.05× |           2.64× |

_256-bit now **beats GMP** (0.98×).  2048/4096-bit moved from
Karatsuba+REDC to FIOS; the 1984→2048-bit cliff (+45 % for a 3 % width
step) is gone (1.74 → 1.88 ms).  256…2048-bit additionally carry the
halved-squaring win.  Cross-run CV at 2048/4096: 1.2 % / 0.3 %._

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

### Hot-path hotspots after the threshold retirement + halved squaring

1. **Halved squaring stops at k = 32** — `montgomery_sqr_fios_halved`
   wins −2 % to −10 % e2e at 256…2048-bit but regressed +2.3 % at
   4096-bit: the shrinking product chain leaves the reduce chain
   serial-carry-bound for most of each row at large k.  Recovering
   the k > 32 band needs cross-row software pipelining (pair row i's
   reduce-only phase with row i−1's product tail) — bounded idea,
   unproven.  The k = 33..63 band is e2e-unbenched either way.
2. **GMP/OpenSSL gap at 2048/4096-bit** — narrowed to 1.72×/2.05×
   (GMP) and 2.40×/2.64× (OpenSSL), from 2.38×/2.69× and 3.31×/3.46×
   pre-retirement.  256-bit now beats GMP outright.  What remains at
   the top widths is squaring specialization at k = 64 (see item 1)
   plus hand-tuned asm (exploration closed after two null results —
   portable levers only).
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
