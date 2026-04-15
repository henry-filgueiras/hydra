// bench/bench_hydra.cpp
//
// Google Benchmark suite for Hydra.
//
// Build with CMake (see root CMakeLists.txt):
//   cmake -B build -DCMAKE_BUILD_TYPE=Release
//   cmake --build build --target hydra_bench
//   ./build/hydra_bench
//
// Baseline philosophy
// ───────────────────
// Every Hydra benchmark is paired with a native uint64_t baseline so the
// ratio (overhead factor) is immediately visible. The baseline intentionally
// uses DoNotOptimize / ClobberMemory to prevent the compiler from removing
// the work, keeping both measurements on the same footing.

#include <benchmark/benchmark.h>
#include "../hydra.hpp"

#include <cstdint>
#include <vector>

using namespace hydra;
using namespace hydra::literals;

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

// Prevent the compiler from optimising away a value.
template<typename T>
[[gnu::always_inline]] inline void do_not_optimise(T&& v) {
    benchmark::DoNotOptimize(v);
}

// A simple xor-shift PRNG so benchmark inputs vary across iterations.
struct XorShift64 {
    uint64_t state;
    explicit XorShift64(uint64_t seed = 0xDEAD'BEEF'CAFE'BABEull)
        : state(seed) {}
    uint64_t next() noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// § 1  Baselines — native uint64_t
// ─────────────────────────────────────────────────────────────────────────────

static void BM_u64_add(benchmark::State& state) {
    XorShift64 rng;
    uint64_t a = rng.next(), b = rng.next();
    for (auto _ : state) {
        a += b;
        do_not_optimise(a);
        b = rng.next();
    }
}
BENCHMARK(BM_u64_add)->Name("native/u64_add");

static void BM_u64_mul(benchmark::State& state) {
    XorShift64 rng;
    uint64_t a = rng.next(), b = rng.next();
    for (auto _ : state) {
        a *= b;
        do_not_optimise(a);
        b = rng.next();
    }
}
BENCHMARK(BM_u64_mul)->Name("native/u64_mul");

// ─────────────────────────────────────────────────────────────────────────────
// § 2  Small + Small  (the hot path — should approach native speed)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_hydra_small_add(benchmark::State& state) {
    XorShift64 rng;
    // Values chosen to stay in [0, 2^32) to guarantee no overflow into Medium.
    Hydra a{ rng.next() >> 32 };
    Hydra b{ rng.next() >> 32 };
    for (auto _ : state) {
        Hydra c = a + b;
        do_not_optimise(c);
        b = Hydra{ rng.next() >> 32 };
    }
}
BENCHMARK(BM_hydra_small_add)->Name("hydra/small_add");

static void BM_hydra_small_mul(benchmark::State& state) {
    XorShift64 rng;
    // 32-bit inputs: products guaranteed to fit in 64-bit (no widening to Medium).
    Hydra a{ rng.next() >> 32 };
    Hydra b{ rng.next() >> 32 };
    for (auto _ : state) {
        Hydra c = a * b;
        do_not_optimise(c);
        b = Hydra{ rng.next() >> 32 };
    }
}
BENCHMARK(BM_hydra_small_mul)->Name("hydra/small_mul");

// ─────────────────────────────────────────────────────────────────────────────
// § 3  Widening — overflow from Small into Medium
// ─────────────────────────────────────────────────────────────────────────────

static void BM_hydra_widening_add(benchmark::State& state) {
    XorShift64 rng;
    // Both near UINT64_MAX / 2 → sum overflows into Medium.
    const uint64_t half_max = std::numeric_limits<uint64_t>::max() / 2;
    Hydra a{ half_max + rng.next() % (half_max / 2) };
    Hydra b{ half_max + rng.next() % (half_max / 2) };
    for (auto _ : state) {
        Hydra c = a + b;
        do_not_optimise(c);
        b = Hydra{ half_max + rng.next() % (half_max / 2) };
    }
}
BENCHMARK(BM_hydra_widening_add)->Name("hydra/widening_add");

static void BM_hydra_widening_mul(benchmark::State& state) {
    XorShift64 rng;
    // Full 64-bit inputs → 128-bit product stored as Medium(2 limbs).
    Hydra a{ rng.next() | 1u };  // odd, prevents trivial zero
    Hydra b{ rng.next() | 1u };
    for (auto _ : state) {
        Hydra c = a * b;
        do_not_optimise(c);
        b = Hydra{ rng.next() | 1u };
    }
}
BENCHMARK(BM_hydra_widening_mul)->Name("hydra/widening_mul");

// ─────────────────────────────────────────────────────────────────────────────
// § 4  Medium × Medium  (192-bit inputs, result may be up to 384-bit → Large)
// ─────────────────────────────────────────────────────────────────────────────

static Hydra make_medium_value(uint64_t seed) {
    XorShift64 rng{ seed };
    // 3-limb medium value: build via two widening multiplications.
    Hydra base{ rng.next() };
    Hydra mul2{ rng.next() };
    Hydra wide = base * mul2;  // 128-bit
    // Add one more small to reach 3 limbs occasionally.
    return wide + Hydra{ rng.next() >> 1 };
}

static void BM_hydra_medium_add(benchmark::State& state) {
    XorShift64 rng;
    Hydra a = make_medium_value(rng.next());
    Hydra b = make_medium_value(rng.next());
    for (auto _ : state) {
        Hydra c = a + b;
        do_not_optimise(c);
        b = make_medium_value(rng.next());
    }
}
BENCHMARK(BM_hydra_medium_add)->Name("hydra/medium_add");

static void BM_hydra_medium_mul(benchmark::State& state) {
    XorShift64 rng;
    Hydra a = make_medium_value(rng.next());
    Hydra b = make_medium_value(rng.next());
    for (auto _ : state) {
        Hydra c = a * b;
        do_not_optimise(c);
        b = make_medium_value(rng.next());
    }
}
BENCHMARK(BM_hydra_medium_mul)->Name("hydra/medium_mul");

// ─────────────────────────────────────────────────────────────────────────────
// § 5  Copy performance (important for value-type ergonomics)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_hydra_copy_small(benchmark::State& state) {
    Hydra src{ 0xDEAD'BEEF'CAFE'BABEull };
    for (auto _ : state) {
        Hydra copy = src;   // NOLINT
        do_not_optimise(copy);
    }
}
BENCHMARK(BM_hydra_copy_small)->Name("hydra/copy_small");

static void BM_hydra_copy_large(benchmark::State& state) {
    // Build a Large value: factorial-ish accumulation.
    Hydra src{ 1u };
    for (uint64_t i = 2; i <= 20; ++i)
        src *= Hydra{ i };  // 20! ≈ 2.4e18  — might still be Small actually
    // Ensure it's actually large
    for (uint64_t i = 21; i <= 25; ++i)
        src *= Hydra{ i };  // 25! ≈ 1.55e25 → Medium or Large
    for (auto _ : state) {
        Hydra copy = src;
        do_not_optimise(copy);
    }
}
BENCHMARK(BM_hydra_copy_large)->Name("hydra/copy_large");

// ─────────────────────────────────────────────────────────────────────────────
// § 6  normalize() overhead (micro-benchmark)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_hydra_normalize_small(benchmark::State& state) {
    for (auto _ : state) {
        Hydra h{ 42u };
        h.normalize();
        do_not_optimise(h);
    }
}
BENCHMARK(BM_hydra_normalize_small)->Name("hydra/normalize_small");

static void BM_hydra_normalize_medium(benchmark::State& state) {
    for (auto _ : state) {
        Hydra h = Hydra::make_medium(0xFFFF'FFFF'FFFF'FFFFull,
                                     0xFFFF'FFFF'FFFF'FFFFull,
                                     0, 2);
        h.normalize();
        do_not_optimise(h);
    }
}
BENCHMARK(BM_hydra_normalize_medium)->Name("hydra/normalize_medium");

// ─────────────────────────────────────────────────────────────────────────────

BENCHMARK_MAIN();
