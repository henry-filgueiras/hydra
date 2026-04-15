# Hydra Benchmark Report

Generated: `2026-04-15 14:17`  Machine: `Henrys-MacBook-Pro.local`  CPUs: `18 × 24 MHz`  Build: `release`

> Source: `bench_results.json`  Metric: `cpu_time`

### Small operations vs. native uint64_t
*Lower Δ is better. Goal: < 2× native for small ops. · metric: `cpu_time`*

| Operation | Subject (ns) | Reference (ns) | Ratio | Δ |
|-----------|-------------|----------------|-------|---|
| small add | 2.91 ns | 2.46 ns `baseline/u64_add` | 1.19× | +18.5% |
| small mul | 3.99 ns | 3.52 ns `baseline/u64_mul` | 1.13× | +13.3% |
| small sub (vs add) | 1.10 ns | 2.46 ns `baseline/u64_add` | 0.45× | -55.4% |
| widening add (vs native) | 3.14 ns | 2.46 ns `baseline/u64_add` | 1.28× | +27.8% |
| widening mul 128 (vs native) | 813.3 ps | 3.52 ns `baseline/u64_mul` | 0.23× | -76.9% |

### Medium / Large operations vs. Boost.Multiprecision
*Negative Δ means Hydra is faster than Boost. · metric: `cpu_time`*

| Operation | Subject (ns) | Reference (ns) | Ratio | Δ |
|-----------|-------------|----------------|-------|---|
| widening add | 3.14 ns | 11.51 ns `boost/widening_add` | 0.27× | -72.7% |
| widening mul 128 | 813.3 ps | 9.31 ns `boost/widening_mul` | 0.09× | -91.3% |
| medium add (vs large/128) | 6.32 ns | 12.49 ns `boost/large_add/128` | 0.51× | -49.4% |
| medium mul (vs large/128) | 18.30 ns | 15.98 ns `boost/large_mul/128` | 1.15× | +14.5% |
| small add | 2.91 ns | 6.38 ns `boost/small_add` | 0.46× | -54.3% |
| small mul | 3.99 ns | 7.38 ns `boost/small_mul` | 0.54× | -46.0% |
| large add 128-bit | 5.46 ns | 12.49 ns `boost/large_add/128` | 0.44× | -56.3% |
| large add 256-bit | 13.35 ns | 13.07 ns `boost/large_add/256` | 1.02× | +2.1% |
| large add 512-bit | 13.30 ns | 24.29 ns `boost/large_add/512` | 0.55× | -45.3% |
| large mul 128-bit | 18.10 ns | 15.98 ns `boost/large_mul/128` | 1.13× | +13.3% |
| large mul 256-bit | 19.71 ns | 20.54 ns `boost/large_mul/256` | 0.96× | -4.1% |
| large mul 512-bit | 36.07 ns | 30.87 ns `boost/large_mul/512` | 1.17× | +16.9% |
| chain large add 8-limb | 50.12 ns | 53.31 ns `boost/chain_large_add/8` | 0.94× | -6.0% |
| chain large add 16-limb | 101.81 ns | 100.95 ns `boost/chain_large_add/16` | 1.01× | +0.9% |
| chain large add 64-limb | 398.10 ns | 421.54 ns `boost/chain_large_add/64` | 0.94× | -5.6% |

### Allocation costs

| Benchmark | 4 limbs | 8 limbs | 16 limbs | 64 limbs | 256 limbs |
|-----------|------------|------------|------------|------------|------------|
| `alloc/from_limbs` | 11.58 ns | 9.44 ns | 10.08 ns | 22.11 ns | 28.43 ns |
| `alloc/largerep_clone` | 11.57 ns | — | 9.74 ns | 19.82 ns | 28.01 ns |
| `alloc/largerep_create_destroy` | 9.56 ns | — | 8.18 ns | 15.05 ns | 12.62 ns |

| Benchmark | Time |
|-----------|------|
| `alloc/normalize_large_to_medium` | 13.19 ns |
| `alloc/normalize_medium_to_small` | 1.54 ns |

### Copy / Move costs

| Benchmark | 4 limbs | 16 limbs | 64 limbs | 256 limbs |
|-----------|------------|------------|------------|------------|
| `copy/large` | 11.86 ns | 9.98 ns | 20.41 ns | 28.49 ns |
| `copy/move_large` | 1.25 ns | 1.25 ns | 1.25 ns | 1.25 ns |

| Benchmark | Time |
|-----------|------|
| `copy/medium` | 440.3 ps |
| `copy/move_medium` | 2.24 ns |
| `copy/small` | 369.0 ps |

### Arithmetic chain throughput

| Benchmark | 8 limbs | 10 limbs | 16 limbs | 20 limbs | 30 limbs | 50 limbs | 64 limbs |
|-----------|------------|------------|------------|------------|------------|------------|------------|
| `chain/factorial` | — | 14.62 ns | — | 68.73 ns | 121.38 ns | 385.85 ns | — |
| `chain/large_add` | 50.12 ns | — | 101.81 ns | — | — | — | 398.10 ns |

| Benchmark | Time |
|-----------|------|
| `chain/small_add_10` | 31.50 ns |
