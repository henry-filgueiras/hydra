// bench/bench_hydra.cpp  — revision 2
//
// ═══════════════════════════════════════════════════════════════════════════
// BENCHMARK VALIDITY AUDIT  (why v1 numbers were suspicious)
// ═══════════════════════════════════════════════════════════════════════════
//
// The v1 numbers — especially small_add and small_mul — were almost certainly
// optimistic artefacts.  Three problems:
//
//  1. CONSTANT INPUTS  ─────────────────────────────────────────────────────
//     `a` was constructed once, outside the loop, and never written to
//     again.  Even though `b` changed via rng.next(), the compiler can
//     propagate `a` as a compile-time constant through the inlined operator+.
//     DoNotOptimize(c) only blocks dead-store elimination of the *result*;
//     it does NOT force re-computation of the *inputs* each iteration.
//     A sufficiently smart compiler can reduce `constant_a + b` to a single
//     ADD instruction that never goes through any Hydra dispatch at all.
//
//  2. PRNG OVERHEAD IN THE TIMED REGION  ───────────────────────────────────
//     rng.next() (an XorShift64) costs ~1–2 ns.  For a Hydra Small add
//     that might genuinely be 1–3 ns, this is 33–200% noise in the raw
//     number.  More critically, it makes inputs *look* data-dependent
//     without actually preventing the compiler from hoisting the Hydra
//     operation out of the loop (the PRNG's output is still deterministic
//     and the compiler knows it).
//
//  3. MISSING INPUT BARRIERS  ──────────────────────────────────────────────
//     DoNotOptimize should be applied to *inputs* before the operation, not
//     only to the output after.  Without this, the compiler sees through
//     `a` and `b` as if they were `constexpr`.
//
// Mitigations in v2
// ─────────────────
//   • State-evolving / Fibonacci fold-back: each result feeds the next
//     iteration's inputs.  The compiler cannot constant-fold because it
//     cannot predict the value of the previous result.
//   • DoNotOptimize on INPUTS before every operation.
//   • ClobberMemory() after every loop body (especially important for Large,
//     which allocates; this prevents the compiler from proving no side
//     effects occurred and hoisting the body out entirely).
//   • Pre-seeded input tables for heavyweight benchmarks: PRNG cost is
//     amortised over table construction, not charged per iteration.
//   • Ping-pong pattern for move benchmarks (self-resetting, measures
//     two moves per iteration; divide times by 2 mentally or use counters).
//
// Expected impact on timings
// ──────────────────────────
//   • small_add and small_mul will likely be SLOWER than v1 numbers.
//     If they stay the same, it means the fold-back overhead dominates —
//     which is itself diagnostic.
//   • Large allocation benchmarks are new; expect 10–50 ns for alloc/free.
//   • Move should be near-zero overhead vs. copy (pointer swap vs. clone).
// ═══════════════════════════════════════════════════════════════════════════

#include <benchmark/benchmark.h>

#include "../hydra.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#ifdef HYDRA_BENCH_HAS_BOOST
#  include <boost/multiprecision/cpp_int.hpp>
namespace bmp = boost::multiprecision;
#endif

using namespace hydra;
using namespace hydra::literals;

// ─────────────────────────────────────────────────────────────────────────────
// § 0  Shared helpers
// ─────────────────────────────────────────────────────────────────────────────

struct XorShift64 {
    uint64_t s;
    explicit XorShift64(uint64_t seed = 0xDEAD'BEEF'CAFE'BABEull) : s(seed) {}
    uint64_t next() noexcept {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
    }
};

// Build a Large Hydra from `n` random non-zero limbs (n >= 4 guaranteed Large).
static Hydra make_large(uint32_t n, uint64_t seed = 0xBEEF'CAFEull) {
    std::vector<uint64_t> limbs(n);
    XorShift64 rng{seed};
    for (auto& l : limbs) l = rng.next() | 1u;   // force non-zero
    limbs.back() |= (1ull << 63);                  // force MSL non-zero → stays Large
    return Hydra::from_limbs(limbs.data(), n);
}

// Pre-seeded input table: build N entries outside timed region so PRNG cost
// is never charged per iteration.
template<size_t N = 64>
struct InputTable {
    Hydra vals[N];
    InputTable(uint64_t seed, uint64_t mask = ~0ull) {
        XorShift64 rng{seed};
        for (auto& v : vals)
            v = Hydra{rng.next() & mask};
    }
    const Hydra& operator[](size_t i) const { return vals[i % N]; }
};

// ─────────────────────────────────────────────────────────────────────────────
// § 1  Baselines — native uint64_t  (stateful, fold-back)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_baseline_u64_add(benchmark::State& state) {
    uint64_t a = 0xCAFE'BABE'0000'0001ull;
    uint64_t b = 0xDEAD'BEEF'0000'0001ull;
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        uint64_t c = a + b;
        benchmark::DoNotOptimize(c);
        a = b;
        b = c;
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_baseline_u64_add)->Name("baseline/u64_add");

static void BM_baseline_u64_mul(benchmark::State& state) {
    uint64_t a = 0xCAFE'BABEull;
    uint64_t b = 0xDEAD'BEEFull;
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        uint64_t c = a * b;
        benchmark::DoNotOptimize(c);
        a = b;
        b = c | 1u;   // keep non-zero
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_baseline_u64_mul)->Name("baseline/u64_mul");

// ─────────────────────────────────────────────────────────────────────────────
// § 2  Small + Small  (stateful fold-back, guaranteed no overflow)
//
// Inputs are masked to 32 bits so a+b < 2^33 < 2^64.  The result is
// fed back after masking to keep values in range indefinitely.
// DoNotOptimize on INPUTS is the key change from v1.
// ─────────────────────────────────────────────────────────────────────────────

static void BM_small_add(benchmark::State& state) {
    Hydra a{0xCAFEull}, b{0xBABEull};
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        Hydra c = a + b;
        benchmark::DoNotOptimize(c);
        a = b;
        // Mask low 32 bits of result to stay in Small forever.
        b = c.is_small() ? Hydra{c.to_u64() & 0xFFFF'FFFFull}
                         : Hydra{0xBABEull};
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_small_add)->Name("hydra/small_add");

static void BM_small_mul(benchmark::State& state) {
    Hydra a{0xCAFEull}, b{0xBABEull};
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        Hydra c = a * b;
        benchmark::DoNotOptimize(c);
        a = b;
        // Mask to 16 bits; 16-bit × 16-bit < 2^32 → always Small.
        b = c.is_small() ? Hydra{(c.to_u64() & 0xFFFFull) | 1u}
                         : Hydra{0xBABEull};
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_small_mul)->Name("hydra/small_mul");

static void BM_small_sub(benchmark::State& state) {
    // Keep a >= b by construction (a from upper half, b from lower half).
    Hydra a{0xFFFF'0000ull}, b{0x0000'FFFFull};
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        Hydra c = a - b;
        benchmark::DoNotOptimize(c);
        a = Hydra{c.is_small() ? (c.to_u64() | 0x8000'0000ull) : 0xFFFF'0000ull};
        b = Hydra{c.is_small() ? (c.to_u64() & 0x0000'FFFFull) : 0x0000'FFFFull};
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_small_sub)->Name("hydra/small_sub");

// ─────────────────────────────────────────────────────────────────────────────
// § 3  Widening — overflow Small → Medium
//
// Inputs are deliberately near UINT64_MAX so every add overflows,
// and every 64-bit mul produces a 128-bit result.
// ─────────────────────────────────────────────────────────────────────────────

static void BM_widening_add(benchmark::State& state) {
    // Both high values; sum always overflows into Medium.
    const uint64_t base = std::numeric_limits<uint64_t>::max() - 0xFFFFull;
    Hydra a{base}, b{base + 1};
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        Hydra c = a + b;
        benchmark::DoNotOptimize(c);
        // Evolve b from low limb of result (always Medium, 2 limbs).
        if (c.is_medium()) {
            auto lv = c.limb_view();
            b = Hydra{(lv.ptr[0] | base)};   // keep high, stays near UINT64_MAX
        }
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_widening_add)->Name("hydra/widening_add");

static void BM_widening_mul_128(benchmark::State& state) {
    // Full 64-bit × 64-bit → 128-bit result (Medium, 2 limbs).
    Hydra a{0xFFFF'FFFF'FFFF'FFFDull};
    Hydra b{0xFFFF'FFFF'FFFF'FFFBull};
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        Hydra c = a * b;
        benchmark::DoNotOptimize(c);
        // Evolve using low limb of 128-bit product.
        if (c.is_medium()) {
            auto lv = c.limb_view();
            b = Hydra{lv.ptr[0] | 1u};
        }
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_widening_mul_128)->Name("hydra/widening_mul_128");

// ─────────────────────────────────────────────────────────────────────────────
// § 4  Medium arithmetic  (192-bit operands, stateful)
// ─────────────────────────────────────────────────────────────────────────────

// Build a 2-limb Medium value (128-bit) from a seed.
static Hydra make_medium(uint64_t seed) {
    XorShift64 rng{seed};
    uint64_t lo = rng.next(), hi = rng.next() | 1u;
    return Hydra::make_medium(lo, hi, 0, 2);
}

static void BM_medium_add(benchmark::State& state) {
    Hydra a = make_medium(0xAAAA), b = make_medium(0xBBBB);
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        Hydra c = a + b;
        benchmark::DoNotOptimize(c);
        // Feed result back; mask to keep it Medium not Large.
        a = b;
        if (c.is_medium()) {
            auto lv = c.limb_view();
            b = Hydra::make_medium(lv.ptr[0], lv.ptr[1] & 0x7FFF'FFFF'FFFF'FFFFull, 0, 2);
        } else {
            b = make_medium(c.limb_count());
        }
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_medium_add)->Name("hydra/medium_add");

static void BM_medium_mul(benchmark::State& state) {
    Hydra a = make_medium(0xAAAA), b = make_medium(0xBBBB);
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        Hydra c = a * b;   // 128-bit × 128-bit → up to 256-bit (Large)
        benchmark::DoNotOptimize(c);
        // Narrow b back to Medium for next iteration.
        if (c.is_medium()) {
            a = b; b = c;
        } else if (c.is_large()) {
            // Result was Large; reset to keep benchmark in steady state.
            a = make_medium(0xAAAA);
            b = make_medium(0xBBBB);
        }
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_medium_mul)->Name("hydra/medium_mul");

// ─────────────────────────────────────────────────────────────────────────────
// § 5  Allocation benchmarks
//
// Measures the raw cost of the heap path: allocate, clone, destroy.
// Parameterised by limb count so we can see scaling.
// ─────────────────────────────────────────────────────────────────────────────

// Raw LargeRep lifecycle — no Hydra wrapper overhead.
static void BM_alloc_largerep_create_destroy(benchmark::State& state) {
    const auto n = static_cast<uint32_t>(state.range(0));
    for (auto _ : state) {
        auto* rep = LargeRep::create(n);
        benchmark::DoNotOptimize(rep);
        LargeRep::destroy(rep);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_alloc_largerep_create_destroy)
    ->Name("alloc/largerep_create_destroy")
    ->Arg(4)->Arg(16)->Arg(64)->Arg(256);

// Full Hydra::from_limbs — includes normalize() pass.
static void BM_alloc_from_limbs(benchmark::State& state) {
    const auto n = static_cast<uint32_t>(state.range(0));
    std::vector<uint64_t> limbs(n);
    XorShift64 rng{0xFEED};
    for (auto& l : limbs) l = rng.next() | 1u;
    limbs.back() |= (1ull << 63);   // guarantee Large after normalize()

    for (auto _ : state) {
        Hydra h = Hydra::from_limbs(limbs.data(), n);
        benchmark::DoNotOptimize(h);
        benchmark::ClobberMemory();
        // h is destroyed here → measures alloc+free per iteration
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_alloc_from_limbs)
    ->Name("alloc/from_limbs")
    ->Arg(4)->Arg(8)->Arg(16)->Arg(64)->Arg(256);

// LargeRep clone — deep copy of the raw heap block.
static void BM_alloc_largerep_clone(benchmark::State& state) {
    const auto n = static_cast<uint32_t>(state.range(0));
    auto* src = LargeRep::create(n);
    src->used = n;
    XorShift64 rng{0xC0FFEE};
    for (uint32_t i = 0; i < n; ++i) src->limbs()[i] = rng.next();

    for (auto _ : state) {
        auto* dst = LargeRep::clone(src);
        benchmark::DoNotOptimize(dst);
        LargeRep::destroy(dst);
        benchmark::ClobberMemory();
    }
    LargeRep::destroy(src);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_alloc_largerep_clone)
    ->Name("alloc/largerep_clone")
    ->Arg(4)->Arg(16)->Arg(64)->Arg(256);

// Normalize Large→Medium: build a Large with 3 significant limbs
// wrapped in a 4-limb allocation (MSL is zero), then normalize.
// This exercises the demote-to-Medium branch of normalize().
//
// NOTE: from_limbs() already normalizes, so we must construct the
// "over-allocated Large" manually via LargeRep::create.
static void BM_alloc_normalize_large_to_medium(benchmark::State& state) {
    for (auto _ : state) {
        // Build a Large with capacity=4, used=4, but limb[3]=0
        // so normalize() will demote it to Medium(3).
        auto* rep = LargeRep::create(4);
        rep->used = 4;
        rep->limbs()[0] = 0xAAAA'AAAA'AAAA'AAAAull;
        rep->limbs()[1] = 0xBBBB'BBBB'BBBB'BBBBull;
        rep->limbs()[2] = 0xCCCC'CCCC'CCCC'CCCCull;
        rep->limbs()[3] = 0;   // leading zero → will be trimmed

        Hydra h;
        h.meta            = Hydra::make_large_meta();
        h.payload.large   = rep;

        benchmark::DoNotOptimize(h);
        h.normalize();   // should demote: Large(used=4,top=0) → Medium(3)
        benchmark::DoNotOptimize(h);
        benchmark::ClobberMemory();

        // Assert correctness in debug builds.
        assert(h.is_medium());
    }
}
BENCHMARK(BM_alloc_normalize_large_to_medium)
    ->Name("alloc/normalize_large_to_medium");

static void BM_alloc_normalize_medium_to_small(benchmark::State& state) {
    for (auto _ : state) {
        // Medium with used=2 but limb[1]=0 → should demote to Small.
        Hydra h = Hydra::make_medium(0xDEAD'BEEFull, 0, 0, 2);
        benchmark::DoNotOptimize(h);
        h.normalize();
        benchmark::DoNotOptimize(h);
        benchmark::ClobberMemory();
        assert(h.is_small());
    }
}
BENCHMARK(BM_alloc_normalize_medium_to_small)
    ->Name("alloc/normalize_medium_to_small");

// ─────────────────────────────────────────────────────────────────────────────
// § 6  Copy / move audit
//
// Isolates the representation-specific cost of each copy/move path.
// ─────────────────────────────────────────────────────────────────────────────

// Small copy — should be two 64-bit stores, near-zero overhead.
static void BM_copy_small(benchmark::State& state) {
    Hydra src{0xDEAD'BEEF'CAFE'BABEull};
    for (auto _ : state) {
        benchmark::DoNotOptimize(src);
        Hydra dst = src;
        benchmark::DoNotOptimize(dst);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_copy_small)->Name("copy/small");

// Medium copy — 3× 64-bit stores, no allocation.
static void BM_copy_medium(benchmark::State& state) {
    Hydra src = make_medium(0xFACE'FEED);
    for (auto _ : state) {
        benchmark::DoNotOptimize(src);
        Hydra dst = src;
        benchmark::DoNotOptimize(dst);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_copy_medium)->Name("copy/medium");

// Large copy — triggers LargeRep::clone (heap allocation + memcpy).
static void BM_copy_large(benchmark::State& state) {
    Hydra src = make_large(static_cast<uint32_t>(state.range(0)));
    for (auto _ : state) {
        benchmark::DoNotOptimize(src);
        Hydra dst = src;
        benchmark::DoNotOptimize(dst);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(
        state.iterations()
        * static_cast<int64_t>(state.range(0))
        * static_cast<int64_t>(sizeof(uint64_t)));
}
BENCHMARK(BM_copy_large)->Name("copy/large")
    ->Arg(4)->Arg(16)->Arg(64)->Arg(256);

// Large move — ping-pong: 2 pointer-swap moves per iteration.
// Divide the reported time by 2 to get per-move cost.
static void BM_move_large(benchmark::State& state) {
    Hydra a = make_large(static_cast<uint32_t>(state.range(0)));
    Hydra b;
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        b = std::move(a);
        benchmark::DoNotOptimize(b);
        a = std::move(b);
        benchmark::DoNotOptimize(a);
        benchmark::ClobberMemory();
    }
    // Two moves per iteration — report per-move cost via counter.
    // Two moves per iteration; the raw time is per-pair.
    // Divide reported ns/op by 2 to get per-move cost.
    state.counters["moves_per_iter"] = 2.0;
}
BENCHMARK(BM_move_large)->Name("copy/move_large")
    ->Arg(4)->Arg(16)->Arg(64)->Arg(256);

// Medium move — same ping-pong, but no allocation involved.
static void BM_move_medium(benchmark::State& state) {
    Hydra a = make_medium(0xFACE'FEED), b;
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        b = std::move(a);
        benchmark::DoNotOptimize(b);
        a = std::move(b);
        benchmark::DoNotOptimize(a);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_move_medium)->Name("copy/move_medium");

// ─────────────────────────────────────────────────────────────────────────────
// § 7  Arithmetic chain benchmarks
//
// Closer to real workloads: sequences of operations on evolving values.
// Measures amortised per-operation cost including intermediate temporaries.
// ─────────────────────────────────────────────────────────────────────────────

// 10 sequential additions on Small values (tight loop, no overflow).
static void BM_chain_small_add_10(benchmark::State& state) {
    Hydra acc{1u};
    const Hydra step{0xABCDull};
    for (auto _ : state) {
        benchmark::DoNotOptimize(acc);
        acc += step; acc += step; acc += step; acc += step; acc += step;
        acc += step; acc += step; acc += step; acc += step; acc += step;
        // Mask back to Small to prevent accumulation into Medium.
        if (!acc.is_small()) acc = Hydra{1u};
        benchmark::DoNotOptimize(acc);
        benchmark::ClobberMemory();
    }
    state.counters["ops_per_iter"] = 10;
}
BENCHMARK(BM_chain_small_add_10)->Name("chain/small_add_10");

// Factorial-style accumulation: representative of combinatorics workloads.
// Computes n! iteratively, measuring total throughput.
static void BM_chain_factorial(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    for (auto _ : state) {
        Hydra acc{1u};
        for (int i = 2; i <= n; ++i)
            acc *= Hydra{static_cast<uint64_t>(i)};
        benchmark::DoNotOptimize(acc);
        benchmark::ClobberMemory();
    }
    state.counters["ops_per_iter"] = n - 1;
}
BENCHMARK(BM_chain_factorial)->Name("chain/factorial")
    ->Arg(10)->Arg(20)->Arg(30)->Arg(50);

// Chained large additions: exercises the in-place operator+= fast path
// when the accumulator is Large and has sufficient capacity.
// Parameterised by limb count; step is a same-sized Large value.
static void BM_chain_large_add(benchmark::State& state) {
    const auto n = static_cast<uint32_t>(state.range(0));
    Hydra acc  = make_large(n, 0x1111'1111ull);
    Hydra step = make_large(n, 0x2222'2222ull);
    for (auto _ : state) {
        benchmark::DoNotOptimize(acc);
        acc += step; acc += step; acc += step; acc += step; acc += step;
        acc += step; acc += step; acc += step; acc += step; acc += step;
        benchmark::DoNotOptimize(acc);
        benchmark::ClobberMemory();
    }
    state.counters["ops_per_iter"] = 10;
}
BENCHMARK(BM_chain_large_add)->Name("chain/large_add")
    ->Arg(8)->Arg(16)->Arg(64);

// ─────────────────────────────────────────────────────────────────────────────
// § 8  Boost.Multiprecision comparison  (opt-in: -DHYDRA_BENCH_BOOST=ON)
//
// Compile with:
//   cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DHYDRA_BENCH_BOOST=ON
//   cmake --build build-rel --target hydra_bench
//
// These benchmarks use the same stateful fold-back pattern so the
// comparison is apples-to-apples.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef HYDRA_BENCH_HAS_BOOST

static void BM_boost_small_add(benchmark::State& state) {
    bmp::cpp_int a{0xCAFEull}, b{0xBABEull};
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        bmp::cpp_int c = a + b;
        benchmark::DoNotOptimize(c);
        a = b;
        // Keep in small range — Boost doesn't have a "Small" fast path
        // guarantee, so this measures its general small-value behaviour.
        b = c % (1ull << 32);
        if (b == 0) b = 1;
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_boost_small_add)->Name("boost/small_add");

static void BM_boost_small_mul(benchmark::State& state) {
    bmp::cpp_int a{0xCAFEull}, b{0xBABEull};
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        bmp::cpp_int c = a * b;
        benchmark::DoNotOptimize(c);
        a = b;
        b = (c & 0xFFFFull);
        if (b == 0) b = 1;
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_boost_small_mul)->Name("boost/small_mul");

static void BM_boost_widening_mul(benchmark::State& state) {
    bmp::cpp_int a{std::numeric_limits<uint64_t>::max() - 3};
    bmp::cpp_int b{std::numeric_limits<uint64_t>::max() - 5};
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        bmp::cpp_int c = a * b;
        benchmark::DoNotOptimize(c);
        a = b;
        b = (c >> 64) | 1;
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_boost_widening_mul)->Name("boost/widening_mul");

static void BM_boost_large_add(benchmark::State& state) {
    const int n_bits = static_cast<int>(state.range(0));
    // Build a value of approximately n_bits using bit-shifting.
    bmp::cpp_int a = (bmp::cpp_int(1) << n_bits) - 1;
    bmp::cpp_int b = (bmp::cpp_int(1) << (n_bits / 2)) + 0xDEADBEEFull;
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        bmp::cpp_int c = a + b;
        benchmark::DoNotOptimize(c);
        a = b;
        b = c >> 1;
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_boost_large_add)->Name("boost/large_add")
    ->Arg(128)->Arg(256)->Arg(512);

static void BM_boost_large_mul(benchmark::State& state) {
    const int n_bits = static_cast<int>(state.range(0));
    bmp::cpp_int a = (bmp::cpp_int(1) << n_bits) - 1;
    bmp::cpp_int b = (bmp::cpp_int(1) << (n_bits / 2)) + 0xBEEFull;
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        bmp::cpp_int c = a * b;
        benchmark::DoNotOptimize(c);
        a = b;
        b = c >> n_bits;   // keep roughly n_bits wide
        if (b == 0) b = (bmp::cpp_int(1) << (n_bits / 2)) + 1;
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_boost_large_mul)->Name("boost/large_mul")
    ->Arg(128)->Arg(256)->Arg(512);

// ── Hydra mirror benchmarks for apples-to-apples comparison ─────────────────

static void BM_hydra_large_add_for_boost_cmp(benchmark::State& state) {
    const auto n = static_cast<uint32_t>(state.range(0) / 64);
    Hydra a = make_large(std::max(n, 2u));
    Hydra b = make_large(std::max(n / 2, 2u), 0xBEEF'CAFEull);
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        Hydra c = a + b;
        benchmark::DoNotOptimize(c);
        a = b;
        b = c.is_large() ? c : make_large(std::max(n / 2, 2u));
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_hydra_large_add_for_boost_cmp)->Name("hydra/large_add_cmp")
    ->Arg(128)->Arg(256)->Arg(512);

static void BM_hydra_large_mul_for_boost_cmp(benchmark::State& state) {
    const auto n = static_cast<uint32_t>(state.range(0) / 64);
    Hydra a = make_large(std::max(n, 2u));
    Hydra b = make_large(std::max(n / 2, 2u), 0xBEEF'CAFEull);
    for (auto _ : state) {
        benchmark::DoNotOptimize(a);
        benchmark::DoNotOptimize(b);
        Hydra c = a * b;
        benchmark::DoNotOptimize(c);
        a = b;
        // Extract upper half for next b, keep size stable.
        b = c.is_large() ? make_large(std::max(n / 2, 2u))
                         : make_large(std::max(n / 2, 2u));
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_hydra_large_mul_for_boost_cmp)->Name("hydra/large_mul_cmp")
    ->Arg(128)->Arg(256)->Arg(512);

#endif  // HYDRA_BENCH_HAS_BOOST

// ─────────────────────────────────────────────────────────────────────────────

BENCHMARK_MAIN();
