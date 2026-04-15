── Comparison report ───────────────────────────────────────────────────
# Hydra Benchmark Report

Generated: `2026-04-15 08:00`  Machine: `Henrys-MacBook-Pro.local`  CPUs: `18 × 24 MHz`  Build: `release`

> Source: `bench_results.json`  Metric: `cpu_time`

### Small operations vs. native uint64_t
*Lower Δ is better. Goal: < 2× native for small ops. · metric: `cpu_time`*

| Operation | Subject (ns) | Reference (ns) | Ratio | Δ |
|-----------|-------------|----------------|-------|---|
| small add | 3.11 ns | 2.47 ns `baseline/u64_add` | 1.26× | +26.1% |
| small mul | 4.03 ns | 3.40 ns `baseline/u64_mul` | 1.19× | +18.5% |
| small sub (vs add) | 1.09 ns | 2.47 ns `baseline/u64_add` | 0.44× | -55.6% |
| widening add (vs native) | 3.18 ns | 2.47 ns `baseline/u64_add` | 1.29× | +28.8% |
| widening mul 128 (vs native) | 3.79 ns | 3.40 ns `baseline/u64_mul` | 1.12× | +11.7% |

### Medium / Large operations vs. Boost.Multiprecision
*Negative Δ means Hydra is faster than Boost. · metric: `cpu_time`*

| Operation | Subject (ns) | Reference (ns) | Ratio | Δ |
|-----------|-------------|----------------|-------|---|
| widening mul 128 | 3.79 ns | 9.32 ns `boost/widening_mul` | 0.41× | -59.3% |
| medium add (vs large/128) | 6.34 ns | 9.15 ns `boost/large_add/128` | 0.69× | -30.6% |
| medium mul (vs large/128) | 18.22 ns | 25.76 ns `boost/large_mul/128` | 0.71× | -29.3% |
| small add | 3.11 ns | 6.52 ns `boost/small_add` | 0.48× | -52.3% |
| small mul | 4.03 ns | 7.79 ns `boost/small_mul` | 0.52× | -48.3% |
| large add 128-bit | 18.19 ns | 9.15 ns `boost/large_add/128` | 1.99× | +98.9% |
| large add 256-bit | 2.77 µs | 20.74 ns `boost/large_add/256` | 133.53× | +13252.7% ⚠ |
| large add 512-bit | 2.76 µs | 21.45 ns `boost/large_add/512` | 128.79× | +12779.4% ⚠ |
| large mul 128-bit | 30.94 ns | 25.76 ns `boost/large_mul/128` | 1.20× | +20.1% |
| large mul 256-bit | 30.70 ns | 36.36 ns `boost/large_mul/256` | 0.84× | -15.6% |
| large mul 512-bit | 67.41 ns | 41.37 ns `boost/large_mul/512` | 1.63× | +63.0% |

  (skipped 1 pair(s) — benchmarks not in results)

### Allocation costs

| Benchmark | 4 limbs | 8 limbs | 16 limbs | 64 limbs | 256 limbs |
|-----------|------------|------------|------------|------------|------------|
| `alloc/from_limbs` | 12.02 ns | 9.96 ns | 10.24 ns | 20.59 ns | 28.86 ns |
| `alloc/largerep_clone` | 12.13 ns | — | 10.04 ns | 19.29 ns | 27.76 ns |
| `alloc/largerep_create_destroy` | 10.02 ns | — | 8.56 ns | 15.94 ns | 13.16 ns |

| Benchmark | Time |
|-----------|------|
| `alloc/normalize_large_to_medium` | 14.19 ns |
| `alloc/normalize_medium_to_small` | 2.01 ns |

### Copy / Move costs

| Benchmark | 4 limbs | 16 limbs | 64 limbs | 256 limbs |
|-----------|------------|------------|------------|------------|
| `copy/large` | 12.42 ns | 10.53 ns | 21.24 ns | 30.16 ns |
| `copy/move_large` | 1.28 ns | 1.26 ns | 1.26 ns | 1.25 ns |

| Benchmark | Time |
|-----------|------|
| `copy/medium` | 435.5 ps |
| `copy/move_medium` | 2.25 ns |
| `copy/small` | 335.7 ps |

### Arithmetic chain throughput

| Benchmark | 10 limbs | 20 limbs | 30 limbs | 50 limbs |
|-----------|------------|------------|------------|------------|
| `chain/factorial` | 24.94 ns | 69.81 ns | 113.50 ns | 401.95 ns |

| Benchmark | Time |
|-----------|------|
| `chain/small_add_10` | 31.86 ns |

---

**Note:** Some comparison pairs were skipped.
Run with `--benchmark_filter=all` or enable Boost benchmarks.

