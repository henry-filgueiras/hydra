// profile_mul_dispatch.cpp — dispatch-seam timing harness for mul_general
//
// Purpose: measure the effect of wiring the Karatsuba kernel into
// mul_general via `max_limbs >= KARATSUBA_THRESHOLD_LIMBS`.  This is a
// self-contained harness (no Google Benchmark dependency), parallel in
// spirit to bench/profile_knuth.cpp — the Google Benchmark archive in
// build-rel/_deps is pre-built for a foreign arch in this sandbox, so
// bench_hydra.cpp cannot be rebuilt here.
//
// Build and run:
//   g++ -std=c++20 -O3 -DNDEBUG -I. bench/profile_mul_dispatch.cpp \
//       -o profile_mul_dispatch
//   ./profile_mul_dispatch
//
// The harness compares three measurement points per operand width:
//
//   1. kernel/school — raw detail::mul_limbs
//   2. kernel/karat  — raw detail::mul_karatsuba (when n is a pow2 ≥ 2)
//   3. dispatch/op*  — public operator*, i.e. the production dispatch
//                      path post-integration
//
// Widths covered: 16 (below threshold), 32 (threshold), 64, 128.
// Extra pass: dispatch overhead at 5/9/15/24/31 limbs — shapes that
// stay on the schoolbook fallback so we can confirm the new branch
// adds ≤ 1 ns.

#include "hydra.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using hydra::Hydra;
using clock_t_ = std::chrono::steady_clock;

struct XorShift64 {
    uint64_t s;
    explicit XorShift64(uint64_t seed = 0xDEADBEEFCAFEBABEull) : s(seed) {}
    uint64_t next() noexcept {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }
};

static std::vector<uint64_t> random_limbs(uint32_t n, uint64_t seed) {
    XorShift64 rng{seed};
    std::vector<uint64_t> r(n);
    for (auto& l : r) l = rng.next() | 1u;
    r.back() |= (1ull << 63);
    return r;
}

static Hydra make_large(uint32_t n, uint64_t seed) {
    auto lv = random_limbs(n, seed);
    return Hydra::from_limbs(lv.data(), n);
}

// Median of `reps` runs of `fn()` — each run loops `iters` times.
template <typename Fn>
static double median_ns(uint32_t reps, uint64_t iters, Fn&& fn) {
    std::vector<double> samples;
    samples.reserve(reps);
    for (uint32_t r = 0; r < reps; ++r) {
        auto t0 = clock_t_::now();
        for (uint64_t i = 0; i < iters; ++i) fn();
        auto t1 = clock_t_::now();
        auto dns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        samples.push_back(static_cast<double>(dns) / static_cast<double>(iters));
    }
    std::sort(samples.begin(), samples.end());
    return samples[reps / 2];
}

static uint64_t pick_iters(uint32_t n) {
    // Aim for ~50 ms per rep at O3 on a modern CPU.
    if (n <= 16) return 200000;
    if (n <= 32) return 60000;
    if (n <= 64) return 15000;
    return 3000;
}

// ── raw-kernel benches ─────────────────────────────────────────────────

static double bench_school(uint32_t n) {
    auto a = random_limbs(n, 0xA11CE ^ n);
    auto b = random_limbs(n, 0xB0B   ^ n);
    std::vector<uint64_t> out(2 * n);
    asm volatile ("" ::: "memory");
    return median_ns(5, pick_iters(n), [&] {
        uint32_t used = hydra::detail::mul_limbs(
            a.data(), n, b.data(), n, out.data());
        asm volatile ("" :: "r"(used), "r"(out.data()) : "memory");
    });
}

static double bench_karat(uint32_t n) {
    auto a = random_limbs(n, 0xA11CE ^ n);
    auto b = random_limbs(n, 0xB0B   ^ n);
    std::vector<uint64_t> out(2 * n);
    asm volatile ("" ::: "memory");
    return median_ns(5, pick_iters(n), [&] {
        uint32_t used = hydra::detail::mul_karatsuba(
            a.data(), b.data(), n, out.data());
        asm volatile ("" :: "r"(used), "r"(out.data()) : "memory");
    });
}

// ── dispatched-path bench (full operator*) ─────────────────────────────

static double bench_dispatch(uint32_t n) {
    Hydra a = make_large(n, 0xA11CE ^ n);
    Hydra b = make_large(n, 0xB0B   ^ n);
    asm volatile ("" ::: "memory");
    return median_ns(5, pick_iters(n), [&] {
        Hydra c = a * b;
        asm volatile ("" :: "r"(&c) : "memory");
    });
}

// ── correctness probe: dispatch produces matching limbs ────────────────

static bool verify_dispatch(uint32_t n) {
    Hydra a = make_large(n, 0xC0DE ^ n);
    Hydra b = make_large(n, 0xBEEF ^ n);
    Hydra c = a * b;

    auto av = a.limb_view();
    auto bv = b.limb_view();
    std::vector<uint64_t> ref(av.count + bv.count, 0);
    uint32_t ref_used = hydra::detail::mul_limbs(
        av.ptr, av.count, bv.ptr, bv.count, ref.data());

    auto cv = c.limb_view();
    if (cv.count != ref_used) return false;
    return std::memcmp(cv.ptr, ref.data(), ref_used * sizeof(uint64_t)) == 0;
}

// ── heap-allocation sentinel hook ──────────────────────────────────────
//
// Overrides operator new to count allocations inside a scoped region.
// We cannot actually replace global new (the program is already past
// static init), so we instead probe via `alloc_probe()` — call operator*
// in a tight loop and count how many times the vector-backed allocator
// is hit, by observing steady-state rss-ish deltas through getrusage.

#include <sys/resource.h>

static long rss_kib() {
    struct rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_maxrss;
}

int main() {
    std::printf("# mul_general dispatch-integration profile\n");
    std::printf("# KARATSUBA_THRESHOLD_LIMBS = %u\n",
                hydra::detail::KARATSUBA_THRESHOLD_LIMBS);
    std::printf("# KARATSUBA_RECURSION_BASE  = %u\n",
                hydra::detail::KARATSUBA_RECURSION_BASE);
    std::printf("\n");

    // ── § 1 Main sweep: dispatch vs kernels at the threshold ───────────
    std::printf("== § 1 main sweep (ns/op, median of 5) ==\n");
    std::printf("%-8s %12s %12s %14s %10s\n",
                "limbs", "school ns", "karat  ns", "dispatch ns", "path");
    std::printf("%-8s %12s %12s %14s %10s\n",
                "-----", "---------", "---------", "-----------", "----");
    for (uint32_t n : {uint32_t{16}, uint32_t{32}, uint32_t{64}, uint32_t{128}}) {
        double s = bench_school(n);
        double k = bench_karat(n);
        double d = bench_dispatch(n);
        const char* path =
            (n >= hydra::detail::KARATSUBA_THRESHOLD_LIMBS)
                ? "karatsuba" : "schoolbk";
        std::printf("%-8u %12.1f %12.1f %14.1f %10s\n",
                    n, s, k, d, path);
    }

    // ── § 2 Dispatch overhead for below-threshold shapes ───────────────
    std::printf("\n== § 2 dispatch overhead, below-threshold shapes ==\n");
    std::printf("# These shapes bypass the specialised 3×3 / 4×4 / 8×8\n");
    std::printf("# kernels and land on the generic schoolbook path.  The\n");
    std::printf("# only added cost post-integration is a single\n");
    std::printf("# never-taken `max_limbs >= 32` branch.\n");
    std::printf("%-8s %14s\n", "limbs", "dispatch ns");
    std::printf("%-8s %14s\n", "-----", "-----------");
    for (uint32_t n : {uint32_t{5}, uint32_t{9}, uint32_t{15},
                       uint32_t{24}, uint32_t{31}}) {
        double d = bench_dispatch(n);
        std::printf("%-8u %14.1f\n", n, d);
    }

    // ── § 3 Correctness sanity check ──────────────────────────────────
    std::printf("\n== § 3 correctness probe ==\n");
    for (uint32_t n : {uint32_t{16}, uint32_t{31}, uint32_t{32},
                       uint32_t{33}, uint32_t{64}, uint32_t{128}}) {
        bool ok = verify_dispatch(n);
        std::printf("  operator* %3ux%-3u == mul_limbs reference : %s\n",
                    n, n, ok ? "ok" : "FAIL");
        if (!ok) return 1;
    }

    // ── § 4 Recursion-path heap-use sanity check ──────────────────────
    //
    // Perform a large number of threshold-crossing multiplications and
    // make sure RSS does not grow unboundedly.  The current Karatsuba
    // prototype uses std::vector for scratch, but each frame releases
    // its scratch on return.  A recursion-path heap explosion would
    // show up as monotonically growing RSS.
    std::printf("\n== § 4 recursion-path heap-use sanity check ==\n");
    {
        Hydra a = make_large(128, 0xAA);
        Hydra b = make_large(128, 0xBB);
        long rss0 = rss_kib();
        for (int i = 0; i < 2000; ++i) {
            Hydra c = a * b;
            asm volatile ("" :: "r"(&c) : "memory");
        }
        long rss1 = rss_kib();
        std::printf("  128×128 ops x 2000: RSS %ld → %ld KiB "
                    "(delta %ld KiB)\n", rss0, rss1, rss1 - rss0);

        // Repeat at a larger width to stress multi-level recursion.
        Hydra c = make_large(256, 0xCC);
        Hydra d = make_large(256, 0xDD);
        long rss2 = rss_kib();
        for (int i = 0; i < 500; ++i) {
            Hydra e = c * d;
            asm volatile ("" :: "r"(&e) : "memory");
        }
        long rss3 = rss_kib();
        std::printf("  256×256 ops x  500: RSS %ld → %ld KiB "
                    "(delta %ld KiB)\n", rss2, rss3, rss3 - rss2);
    }

    std::printf("\n# done.\n");
    return 0;
}
