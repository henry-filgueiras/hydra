// profile_knuth.cpp — profiler-pass harness for detail::divmod_knuth_limbs
//
// This is a self-contained timing + counter harness purpose-built for the
// division profiler pass.  It sits next to bench/bench_hydra.cpp but does
// *not* link against Google Benchmark (the FetchContent archive shipped
// with this repo is macOS-only; profiling the kernel does not need it).
//
// What it measures
// ────────────────
//   1. End-to-end divmod cost for 256/128, 512/256, 1024/512, 2048/1024.
//   2. Per-region cost via HYDRA_PROFILE_SECTION cycle counters compiled
//      into the kernel when -DHYDRA_PROFILE_KNUTH=1 is set.
//   3. q_hat correction loop and add-back hit frequencies via counters.
//   4. Stack vs heap scratch threshold impact (varies STACK_LIMIT via
//      HYDRA_KNUTH_STACK_LIMIT).
//
// Build:
//   g++ -std=c++20 -O3 -march=native -DNDEBUG \
//       -DHYDRA_PROFILE_KNUTH=1 \
//       bench/profile_knuth.cpp -o profile_knuth
//
// The HYDRA_PROFILE_KNUTH define is inert unless the kernel itself is
// instrumented — guard macros in hydra.hpp.

#include "../hydra.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace hydra;
using clk = std::chrono::steady_clock;

// ─────────────────────────────────────────────────────────────────
// Operand construction.  Uses a fixed seed so every run produces
// identical operand bytes — makes per-section timing comparable.
// ─────────────────────────────────────────────────────────────────

static Hydra make_random_hydra(uint32_t n_limbs, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> limbs(n_limbs);
    for (auto& l : limbs) l = rng();
    // Ensure top limb is non-zero so the Hydra has exactly n_limbs of span.
    if ((limbs.back() >> 63) == 0) limbs.back() |= (1ull << 63);
    return Hydra::from_limbs(limbs.data(), n_limbs);
}

// Direct kernel call.  Mirrors Hydra::divmod's scratch setup so we can
// isolate exactly the kernel cost (no from_limbs / normalize overhead).
static void call_kernel_direct(const Hydra& u, const Hydra& v,
                               std::vector<uint64_t>& work,
                               std::vector<uint64_t>& q,
                               std::vector<uint64_t>& r) {
    auto uv = u.limb_view();
    auto dv = v.limb_view();
    const uint32_t nu = uv.count;
    const uint32_t nv = dv.count;
    const uint32_t nq = nu - nv + 1;
    q.assign(nq, 0);
    r.assign(nv, 0);
    work.assign(static_cast<size_t>(nu) + 1u + nv, 0);
    detail::divmod_knuth_limbs(uv.ptr, nu, dv.ptr, nv,
                               q.data(), r.data(), work.data());
}

// End-to-end divmod for cost of the full user-facing path.
static void call_divmod(const Hydra& u, const Hydra& v) {
    auto qr = u.divmod(v);
    // Defeat DCE.
    asm volatile("" : : "r"(qr.quotient.meta), "r"(qr.remainder.meta) : "memory");
}

struct ShapeSpec {
    const char* name;
    uint32_t    nu;
    uint32_t    nv;
};

static constexpr ShapeSpec kShapes[] = {
    {"256/128",   4,  2},
    {"512/256",   8,  4},
    {"1024/512", 16,  8},
    {"2048/1024",32, 16},
};

// ─────────────────────────────────────────────────────────────────
// Per-section counters — defined inside hydra.hpp (inline vars)
// when HYDRA_PROFILE_KNUTH=1.  We provide a local reset helper.
// ─────────────────────────────────────────────────────────────────
#if HYDRA_PROFILE_KNUTH
namespace hydra::detail {
    inline void knuth_prof_reset() {
        knuth_prof_normalize_ns = 0;
        knuth_prof_qhat_est_ns = 0;
        knuth_prof_qhat_refine_ns = 0;
        knuth_prof_mulsub_ns = 0;
        knuth_prof_addback_ns = 0;
        knuth_prof_denormalize_ns = 0;
        knuth_prof_refine_iters = 0;
        knuth_prof_addback_hits = 0;
        knuth_prof_outer_steps = 0;
        knuth_prof_qhat_clamps = 0;
    }
}
#endif

// ─────────────────────────────────────────────────────────────────
// Timing helpers.
// ─────────────────────────────────────────────────────────────────
template <typename F>
static double time_ns_per_call(F&& f, uint64_t reps) {
    // Warm up.
    for (uint64_t i = 0; i < std::min<uint64_t>(reps/10 + 1, 1000); ++i) f();
    auto t0 = clk::now();
    for (uint64_t i = 0; i < reps; ++i) f();
    auto t1 = clk::now();
    auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return static_cast<double>(dt) / static_cast<double>(reps);
}

// Pick reps such that we get ≥ ~200 ms total wall time per measurement.
static uint64_t pick_reps(double ns_hint) {
    double target = 2e8;  // 200 ms in ns
    uint64_t r = static_cast<uint64_t>(target / std::max(ns_hint, 1.0));
    return std::max<uint64_t>(r, 10000);
}

// ─────────────────────────────────────────────────────────────────
// Main experiment matrix.
// ─────────────────────────────────────────────────────────────────
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    printf("# Knuth-D profiler pass (divmod_knuth_limbs)\n");
#if HYDRA_PROFILE_KNUTH
    printf("# Instrumentation: ENABLED (per-section RDTSC counters live in kernel)\n");
#else
    printf("# Instrumentation: DISABLED — timing only (full-kernel)\n");
#endif
    printf("#\n");
    printf("# Notes:\n");
    printf("#  - direct = calls detail::divmod_knuth_limbs with pre-assigned\n");
    printf("#            scratch; excludes from_limbs / allocate / normalize cost.\n");
    printf("#  - divmod = full Hydra::divmod pipeline (incl. scratch + result).\n");
    printf("#\n");

    // Fixed operand pairs, one per shape.
    struct Bench {
        const ShapeSpec* shape;
        Hydra u, v;
    };
    std::vector<Bench> benches;
    for (const auto& s : kShapes) {
        Hydra u = make_random_hydra(s.nu, 0xC0FFEE + s.nu);
        Hydra v = make_random_hydra(s.nv, 0xBEEF   + s.nv);
        benches.push_back({&s, std::move(u), std::move(v)});
    }

    // ─── End-to-end + direct kernel timing ──────────────────────
    printf("## Full-kernel timing\n");
    printf("%-12s %14s %14s %10s\n",
           "shape", "direct ns/op", "divmod ns/op", "overhead%");
    printf("%-12s %14s %14s %10s\n",
           "-----", "------------", "------------", "---------");
    for (auto& b : benches) {
        std::vector<uint64_t> w, q, r;
        // Warm up allocations.
        call_kernel_direct(b.u, b.v, w, q, r);
        // Time direct kernel.
        double t_direct = time_ns_per_call([&]{
            call_kernel_direct(b.u, b.v, w, q, r);
        }, pick_reps(200.0));
        double t_full   = time_ns_per_call([&]{
            call_divmod(b.u, b.v);
        }, pick_reps(200.0));
        double overhead = 100.0 * (t_full - t_direct) / t_full;
        printf("%-12s %14.2f %14.2f %9.1f%%\n",
               b.shape->name, t_direct, t_full, overhead);
    }
    printf("\n");

    // ─── Per-section breakdown (needs instrumentation) ──────────
#if HYDRA_PROFILE_KNUTH
    printf("## Per-section breakdown (from embedded counters)\n");
    printf("%-12s %10s %10s %10s %10s %10s %10s %10s %10s %10s %10s\n",
           "shape",
           "norm ns", "qest ns", "qref ns", "mulsub ns", "adback ns",
           "denorm ns",
           "steps", "refIter", "adbackHit", "qhatClamp");
    for (auto& b : benches) {
        std::vector<uint64_t> w, q, r;
        call_kernel_direct(b.u, b.v, w, q, r);  // warmup
        constexpr uint64_t REPS = 20000;

        hydra::detail::knuth_prof_reset();
        for (uint64_t i = 0; i < REPS; ++i) {
            call_kernel_direct(b.u, b.v, w, q, r);
        }
        double norm  = (double)hydra::detail::knuth_prof_normalize_ns / REPS;
        double qest  = (double)hydra::detail::knuth_prof_qhat_est_ns / REPS;
        double qref  = (double)hydra::detail::knuth_prof_qhat_refine_ns / REPS;
        double msub  = (double)hydra::detail::knuth_prof_mulsub_ns / REPS;
        double adb   = (double)hydra::detail::knuth_prof_addback_ns / REPS;
        double den   = (double)hydra::detail::knuth_prof_denormalize_ns / REPS;
        double steps = (double)hydra::detail::knuth_prof_outer_steps / REPS;
        double ri    = (double)hydra::detail::knuth_prof_refine_iters / REPS;
        double ah    = (double)hydra::detail::knuth_prof_addback_hits / REPS;
        double qc    = (double)hydra::detail::knuth_prof_qhat_clamps / REPS;
        printf("%-12s %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.2f %10.3f %10.2f\n",
               b.shape->name, norm, qest, qref, msub, adb, den,
               steps, ri/std::max(steps,1.0), ah/std::max(steps,1.0), qc/std::max(steps,1.0));
    }
    printf("\n");
#endif

    // ─── Branch-predictor-relevant stats (raw rates) ────────────
    printf("## Branch-predictability survey (1000 random pairs per shape)\n");
    printf("%-12s %10s %10s %10s %10s\n",
           "shape", "refineIt/step", "adback/step", "qclamp/step", "avgSteps");
    for (auto& b : benches) {
        const uint32_t nu = b.shape->nu;
        const uint32_t nv = b.shape->nv;
        uint64_t total_steps = 0;
#if HYDRA_PROFILE_KNUTH
        uint64_t total_refine = 0, total_adback = 0, total_clamp = 0;
#endif
        constexpr int N = 1000;
        std::vector<uint64_t> w, q, r;
        for (int i = 0; i < N; ++i) {
            Hydra uu = make_random_hydra(nu, 0xA5A5A5A5 + i * 7u);
            Hydra vv = make_random_hydra(nv, 0x5A5A5A5A + i * 11u);
            // Make sure uu >= vv (otherwise kernel asserts / skips).
            if (uu.compare(vv) < 0) std::swap(uu, vv);
#if HYDRA_PROFILE_KNUTH
            hydra::detail::knuth_prof_reset();
#endif
            call_kernel_direct(uu, vv, w, q, r);
#if HYDRA_PROFILE_KNUTH
            total_steps  += hydra::detail::knuth_prof_outer_steps;
            total_refine += hydra::detail::knuth_prof_refine_iters;
            total_adback += hydra::detail::knuth_prof_addback_hits;
            total_clamp  += hydra::detail::knuth_prof_qhat_clamps;
#else
            total_steps += (nu - nv + 1);
#endif
        }
#if HYDRA_PROFILE_KNUTH
        double ri = (double)total_refine / std::max<uint64_t>(total_steps,1);
        double ab = (double)total_adback / std::max<uint64_t>(total_steps,1);
        double qc = (double)total_clamp  / std::max<uint64_t>(total_steps,1);
        double as = (double)total_steps / N;
        printf("%-12s %10.4f %10.4f %10.4f %10.1f\n",
               b.shape->name, ri, ab, qc, as);
#else
        printf("%-12s %10s %10s %10s %10.1f\n",
               b.shape->name, "n/a", "n/a", "n/a", (double)total_steps / N);
#endif
    }
    printf("\n");

    // ─── Stack vs heap scratch threshold (divmod wrapper cost) ──
    printf("## Full divmod() cost — scratch source visible in end-to-end\n");
    printf("%-12s %14s\n", "shape", "divmod ns/op");
    for (auto& b : benches) {
        double t = time_ns_per_call([&]{ call_divmod(b.u, b.v); }, pick_reps(200.0));
        // Under current STACK_LIMIT=32, all four shapes fit on stack.
        // If user flips HYDRA_KNUTH_STACK_LIMIT to e.g. 4, the 512+ cases
        // fall to std::vector — that's the experiment captured below.
        const char* where = (b.shape->nu <= 32) ? "stack" : "heap";
        printf("%-12s %14.2f  (%s scratch)\n", b.shape->name, t, where);
    }

    return 0;
}
