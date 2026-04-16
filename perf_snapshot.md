# Hydra Benchmark Report

Generated: `2026-04-15 17:25`  Machine: `Henrys-MacBook-Pro.local`  CPUs: `18 × 24 MHz`  Build: `release`

> Source: `bench_results.json`  Metric: `cpu_time`

### Small operations vs. native uint64_t
*Lower Δ is better. Goal: < 2× native for small ops. · metric: `cpu_time`*

| Operation | Subject (ns) | Reference (ns) | Ratio | Δ |
|-----------|-------------|----------------|-------|---|
| small add | 3.20 ns | 2.49 ns `baseline/u64_add` | 1.28× | +28.1% |
| small mul | 4.25 ns | 3.55 ns `baseline/u64_mul` | 1.20× | +19.9% |
| small sub (vs add) | 1.07 ns | 2.49 ns `baseline/u64_add` | 0.43× | -57.2% |
| widening add (vs native) | 3.17 ns | 2.49 ns `baseline/u64_add` | 1.27× | +27.1% |
| widening mul 128 (vs native) | 780.3 ps | 3.55 ns `baseline/u64_mul` | 0.22× | -78.0% |

### Medium / Large operations vs. Boost.Multiprecision
*Negative Δ means Hydra is faster than Boost. · metric: `cpu_time`*

| Operation | Subject (ns) | Reference (ns) | Ratio | Δ |
|-----------|-------------|----------------|-------|---|
| widening add | 3.17 ns | 11.49 ns `boost/widening_add` | 0.28× | -72.4% |
| widening mul 128 | 780.3 ps | 9.31 ns `boost/widening_mul` | 0.08× | -91.6% |
| medium add (vs large/128) | 5.99 ns | 13.10 ns `boost/large_add/128` | 0.46× | -54.3% |
| medium mul (vs large/128) | 15.30 ns | 15.55 ns `boost/large_mul/128` | 0.98× | -1.6% |
| small add | 3.20 ns | 6.53 ns `boost/small_add` | 0.49× | -51.1% |
| small mul | 4.25 ns | 7.90 ns `boost/small_mul` | 0.54× | -46.1% |
| large add 128-bit | 5.50 ns | 13.10 ns `boost/large_add/128` | 0.42× | -58.0% |
| large add 256-bit | 13.44 ns | 13.01 ns `boost/large_add/256` | 1.03× | +3.3% |
| large add 512-bit | 13.55 ns | 23.98 ns `boost/large_add/512` | 0.56× | -43.5% |
| large mul 128-bit | 15.39 ns | 15.55 ns `boost/large_mul/128` | 0.99× | -1.0% |
| large mul 256-bit | 19.69 ns | 19.28 ns `boost/large_mul/256` | 1.02× | +2.1% |
| large mul 512-bit | 37.13 ns | 31.43 ns `boost/large_mul/512` | 1.18× | +18.1% |
| chain large add 8-limb | 54.27 ns | 54.04 ns `boost/chain_large_add/8` | 1.00× | +0.4% |
| chain large add 16-limb | 119.33 ns | 94.02 ns `boost/chain_large_add/16` | 1.27× | +26.9% |
| chain large add 64-limb | 394.48 ns | 426.54 ns `boost/chain_large_add/64` | 0.92× | -7.5% |

### Allocation costs

| Benchmark | 4 limbs | 8 limbs | 16 limbs | 64 limbs | 256 limbs |
|-----------|------------|------------|------------|------------|------------|
| `alloc/from_limbs` | 11.55 ns | 9.58 ns | 9.99 ns | 20.37 ns | 28.82 ns |
| `alloc/largerep_clone` | 11.60 ns | — | 9.76 ns | 19.97 ns | 28.23 ns |
| `alloc/largerep_create_destroy` | 9.67 ns | — | 8.24 ns | 15.46 ns | 12.87 ns |

| Benchmark | Time |
|-----------|------|
| `alloc/normalize_large_to_medium` | 13.29 ns |
| `alloc/normalize_medium_to_small` | 1.81 ns |

### Copy / Move costs

| Benchmark | 4 limbs | 16 limbs | 64 limbs | 256 limbs |
|-----------|------------|------------|------------|------------|
| `copy/large` | 12.00 ns | 10.07 ns | 20.58 ns | 28.78 ns |
| `copy/move_large` | 1.26 ns | 1.25 ns | 1.26 ns | 1.26 ns |

| Benchmark | Time |
|-----------|------|
| `copy/medium` | 445.7 ps |
| `copy/move_medium` | 2.23 ns |
| `copy/small` | 331.5 ps |

### Arithmetic chain throughput

| Benchmark | 8 limbs | 10 limbs | 16 limbs | 20 limbs | 30 limbs | 50 limbs | 64 limbs |
|-----------|------------|------------|------------|------------|------------|------------|------------|
| `chain/factorial` | — | 19.09 ns | — | 68.26 ns | 122.53 ns | 368.39 ns | — |
| `chain/large_add` | 54.27 ns | — | 119.33 ns | — | — | — | 394.48 ns |

| Benchmark | Time |
|-----------|------|
| `chain/small_add_10` | 31.92 ns |
