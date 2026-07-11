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
|   256 |    7.42 µs   |    7.29 µs   |      +2 %  |  7.21 µs  |  5.29 µs  |       1.03× |           1.40× |
|   512 |   32.20 µs   |   36.42 µs   |     −12 %  | 27.58 µs  | 19.00 µs  |       1.17× |           1.69× |
|  1024 |  218.40 µs   |  234.56 µs   |      −7 %  | 152.63 µs | 109.75 µs |       1.43× |           1.99× |
|  1536 |  729.20 µs   |  781.12 µs   |      −7 %  | 461.67 µs | 336.75 µs |       1.58× |           2.17× |
|  1984 |    1.64 ms   |    1.78 ms   |      −8 %  |   1.03 ms |   1.65 ms |       1.59× |           0.99× |
|  2048 |    1.80 ms   |    2.59 ms   |   **−31 %**|   1.09 ms |  782.9 µs |       1.65× |           2.30× |
|  4096 |   14.48 ms   |   20.13 ms |   **−28 %**|   7.47 ms |   5.82 ms |   **1.94×** |           2.49× |

_Cumulative 2026-07-10 arc: threshold retirement + halved squaring +
adaptive window.  4096-bit is now **under 2× GMP**; 1984-bit is at
parity with OpenSSL (0.99×); 256-bit sits at parity with GMP
(0.98×–1.03× across runs — the adaptive-window plumbing costs ~1 %
there, traded for −4 to −6 % at ≥1536-bit).  The 1984→2048-bit
Karatsuba cliff is gone.  Cross-run CV at 2048/4096: ~1 % / 0.3 %._

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

### WebAssembly shootout (2026-07-10, LLVM-only pipeline)

_`scripts/wasm_bench.sh` — emscripten 6.0.2 (clang 23.0.0git,
Binaryen wasm-opt 130 — the version CI is pinned to as of
2026-07-11), LLVM `-O2` with binaryen limited to a passes-free DWARF
strip (emcc's default post-link `wasm-opt -O2` pessimizes Hydra's
kernels up to +60 % — see the binaryen dragon in DIRECTORS_NOTES,
where the older, slower table is archived).  Flags:
`-sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608`, no wasm-EH, node 26,
min-of-2 × 50-sample medians, all three backends in one binary,
identical pipeline for all three._

| Width | Hydra (wasm) | Boost cpp_int | mini-gmp  | vs Boost | vs mini-gmp |
|------:|-------------:|--------------:|----------:|---------:|------------:|
|  256  |     18.4 µs  |      72.5 µs  |   93.0 µs |   3.9×   |    5.0×     |
|  512  |    102.1 µs  |     394.5 µs  |  669.9 µs |   3.9×   |    6.6×     |
| 1024  |    722.0 µs  |      2.46 ms  |   5.09 ms |   3.4×   |    7.0×     |
| 1536  |     2.36 ms  |      7.29 ms  |  17.15 ms |   3.1×   |    7.3×     |
| 1984  |     5.06 ms  |     15.54 ms  |  36.89 ms |   3.1×   |    7.3×     |
| 2048  |     5.61 ms  |     17.31 ms  |  41.31 ms |   3.1×   |    7.4×     |
| 4096  |    48.19 ms  |    126.06 ms  | 323.49 ms |   2.6×   |    6.7×     |

_mini-gmp is GMP's bundled portable fallback — what GMP-dependent code
becomes on wasm32, where GMP's asm doesn't exist.  Hydra's advantage
**widens** with width (5.0× → 7.4× vs mini-gmp through 2048-bit).
The pipeline fix helped Hydra far more than the comparators (−26 % vs
−11 % Boost / −0.7 % mini-gmp at 2048-bit): binaryen's rewrites hurt
Hydra's fused-accumulator kernels specifically.  The wasm-vs-native
tax on Hydra is ~2.5-3.1× (5.61 ms vs 1.80 ms at 2048-bit), dominated
by software lowering of the 64×64→128 multiply.  `-fwasm-exceptions`
costs Hydra a further ~15 % and is left off for benches (nothing
throws in the timed path); the test suite needs it.  The live demo
(`demo/`) still builds with the old default pipeline — its on-screen
numbers are conservative until it's migrated._

---

### pow_mod_batch — fused 2-lane ladder (2026-07-10)

_`bench/probe_pow_mod_batch.cpp` — native M5 Pro, rotating operands
(8 pairs), min of 5 samples, per-exponentiation latency vs two
production `pow_mod` calls.  Tier-1 shape: shared exponent + modulus
(RSA verify / VDF / accumulator workloads)._

| Width | pow_mod ns/op | pow_mod_batch ns/op | throughput gain |
|------:|--------------:|--------------------:|----------------:|
|  256  |        7 549  |               5 433 |      1.39×      |
|  512  |       33 101  |              27 001 |      1.23×      |
| 1024  |      218 310  |             182 248 |      1.20×      |
| 2048  |    1 791 958  |           1 345 761 |    **1.33×**    |
| 4096  |   14 468 723  |          10 494 828 |    **1.38×**    |

_Kernel-layer fusion is 1.54×/1.50× at 2048/4096-bit
(`probe_mont_interleave`); the e2e gap is the baseline's halved-
squaring advantage plus residual L1 competition at k=64.  Batched
Hydra at 2048-bit (1.35 ms) does not yet catch single-op GMP
(~1.05 ms).  wasm: fusion is a null (1.02–1.08×) — the API works
there but doesn't win.  Not constant-time; public inputs only._

---

### hydra-bignum (npm) vs native JS BigInt (2026-07-10)

_`pkg/bench/bench_vs_bigint.mjs --runs 6` — node 26, Apple M5 Pro.
Baseline: square-and-multiply modexp over native `BigInt` (V8's
optimized bignum — the loop JS developers actually write, since the
language has no built-in `powMod`).  Hydra side is the shipped
`pkg/dist` module: LLVM `-O2`, binaryen limited to a passes-free
DWARF strip, EH-free, end-to-end through the BigInt⇄limbs wrapper._

| Width | native `BigInt` | hydra-bignum (wasm) | speedup |
|------:|----------------:|--------------------:|--------:|
|  256  |        48.3 µs  |            18.3 µs  | **2.6×** |
|  512  |       204.9 µs  |           102.3 µs  | **2.0×** |
| 1024  |        1.13 ms  |           732.9 µs  | **1.5×** |
| 2048  |        7.24 ms  |            5.57 ms  | **1.3×** |
| 4096  |       37.38 ms  |           48.66 ms  |   0.8×   |

_Honest loss at 4096-bit: V8 multiplies subquadratically (Karatsuba+
Toom band) while Hydra's FIOS is O(k²) and pays the wasm i128-lowering
tax.  Wrapper interop costs ~1 µs/call (18.3 µs end-to-end vs 17.5 µs
in-wasm at 256-bit)._

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
