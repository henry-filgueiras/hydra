// bench/bench_mont_backends.cpp — head-to-head Montgomery backend comparison
//
// Compares three Montgomery multiply backends at the kernel level:
//   1. Fused CIOS (interleaved multiply-reduce)
//   2. Separate schoolbook product + REDC
//   3. Separate Karatsuba product + REDC
//
// Also measures end-to-end pow_mod with each backend force-selected.
//
// Build:
//   g++ -std=c++20 -O3 -DNDEBUG -I. bench/bench_mont_backends.cpp \
//       -o build-rel/bench_mont_backends
//
// Usage:
//   ./build-rel/bench_mont_backends

#include "../hydra.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <algorithm>
#include <numeric>

using clk = std::chrono::steady_clock;

// ─────────────────────────────────────────────────────────────────────
// § 0  Operand generation
// ─────────────────────────────────────────────────────────────────────

static void make_random_limbs(uint64_t* out, uint32_t k, std::mt19937_64& rng) {
    for (uint32_t i = 0; i < k; ++i) out[i] = rng();
    out[0] |= 1u;            // odd (for modulus)
    out[k - 1] |= (1ull << 63); // full-width
}

// ─────────────────────────────────────────────────────────────────────
// § 1  Kernel-level benchmarks
// ─────────────────────────────────────────────────────────────────────

struct KernelResult {
    double fused_ns;
    double schoolbook_ns;
    double karatsuba_ns;
};

static KernelResult bench_kernels(uint32_t k, int reps = 5000, int warmup = 500) {
    std::mt19937_64 rng(0xDEAD'BEEF'0000ull + k);

    std::vector<uint64_t> mod(k), a(k), b(k);
    make_random_limbs(mod.data(), k, rng);
    for (uint32_t i = 0; i < k; ++i) a[i] = rng();
    for (uint32_t i = 0; i < k; ++i) b[i] = rng();

    uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);

    // Buffers for fused CIOS
    std::vector<uint64_t> out(k), work_fused(k + 2, 0);

    // Buffers for schoolbook + REDC
    std::vector<uint64_t> work_school(2 * k + 1, 0);

    // Buffers for Karatsuba + REDC
    uint32_t n_padded = 1;
    while (n_padded < k) n_padded <<= 1;
    std::vector<uint64_t> work_kara(2 * k + 1, 0);
    std::vector<uint64_t> pa(n_padded, 0), pb(n_padded, 0);
    std::vector<uint64_t> kbuf(2 * n_padded, 0);

    KernelResult result{};

    // ── Fused CIOS ──
    for (int i = 0; i < warmup; ++i) {
        std::memset(work_fused.data(), 0, (k + 2) * sizeof(uint64_t));
        hydra::detail::montgomery_mul_fused(a.data(), b.data(), k, mod.data(),
                                             n0inv, out.data(), work_fused.data());
    }
    {
        auto t0 = clk::now();
        for (int i = 0; i < reps; ++i) {
            std::memset(work_fused.data(), 0, (k + 2) * sizeof(uint64_t));
            hydra::detail::montgomery_mul_fused(a.data(), b.data(), k, mod.data(),
                                                 n0inv, out.data(), work_fused.data());
        }
        auto t1 = clk::now();
        result.fused_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
    }

    // ── Separate schoolbook + REDC ──
    for (int i = 0; i < warmup; ++i) {
        std::memset(work_school.data(), 0, (2 * k + 1) * sizeof(uint64_t));
        hydra::detail::montgomery_mul(a.data(), b.data(), k, mod.data(),
                                       n0inv, out.data(), work_school.data());
    }
    {
        auto t0 = clk::now();
        for (int i = 0; i < reps; ++i) {
            std::memset(work_school.data(), 0, (2 * k + 1) * sizeof(uint64_t));
            hydra::detail::montgomery_mul(a.data(), b.data(), k, mod.data(),
                                           n0inv, out.data(), work_school.data());
        }
        auto t1 = clk::now();
        result.schoolbook_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
    }

    // ── Karatsuba + REDC ──
    for (int i = 0; i < warmup; ++i) {
        std::memset(work_kara.data(), 0, (2 * k + 1) * sizeof(uint64_t));
        hydra::detail::montgomery_mul_karatsuba(
            a.data(), b.data(), k, mod.data(), n0inv,
            out.data(), work_kara.data(),
            pa.data(), pb.data(), kbuf.data(), n_padded);
    }
    {
        auto t0 = clk::now();
        for (int i = 0; i < reps; ++i) {
            std::memset(work_kara.data(), 0, (2 * k + 1) * sizeof(uint64_t));
            hydra::detail::montgomery_mul_karatsuba(
                a.data(), b.data(), k, mod.data(), n0inv,
                out.data(), work_kara.data(),
                pa.data(), pb.data(), kbuf.data(), n_padded);
        }
        auto t1 = clk::now();
        result.karatsuba_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────
// § 2  End-to-end pow_mod benchmark
// ─────────────────────────────────────────────────────────────────────

static double bench_pow_mod_e2e(uint32_t bits, int samples = 20) {
    std::mt19937_64 rng(0xE2E0'0000ull + bits);

    uint32_t k = (bits + 63) / 64;
    std::vector<uint64_t> base_limbs(k), exp_limbs(k), mod_limbs(k);

    // Generate operands
    for (auto& l : base_limbs) l = rng();
    for (auto& l : exp_limbs) l = rng();
    for (auto& l : mod_limbs) l = rng();
    mod_limbs[0] |= 1u;
    mod_limbs[k - 1] |= (1ull << 63);
    base_limbs[k - 1] |= (1ull << 63);
    exp_limbs[k - 1] |= (1ull << 63);

    hydra::Hydra base = hydra::Hydra::from_limbs(base_limbs.data(), k);
    hydra::Hydra exp_val = hydra::Hydra::from_limbs(exp_limbs.data(), k);
    hydra::Hydra mod_val = hydra::Hydra::from_limbs(mod_limbs.data(), k);

    // Warmup
    for (int i = 0; i < 2; ++i) {
        auto r = hydra::pow_mod(base, exp_val, mod_val);
        asm volatile("" : : "r"(r.meta) : "memory");
    }

    // Collect
    std::vector<double> times(samples);
    for (int i = 0; i < samples; ++i) {
        auto t0 = clk::now();
        auto r = hydra::pow_mod(base, exp_val, mod_val);
        auto t1 = clk::now();
        asm volatile("" : : "r"(r.meta) : "memory");
        times[i] = std::chrono::duration<double, std::nano>(t1 - t0).count();
    }

    std::sort(times.begin(), times.end());
    return times[samples / 2];  // median
}

// ─────────────────────────────────────────────────────────────────────
// § 3  Main
// ─────────────────────────────────────────────────────────────────────

static std::string fmt_ns(double ns) {
    char buf[32];
    if (ns < 1000.0)
        snprintf(buf, sizeof(buf), "%.1f ns", ns);
    else if (ns < 1e6)
        snprintf(buf, sizeof(buf), "%.2f us", ns / 1e3);
    else if (ns < 1e9)
        snprintf(buf, sizeof(buf), "%.2f ms", ns / 1e6);
    else
        snprintf(buf, sizeof(buf), "%.3f s", ns / 1e9);
    return std::string(buf);
}

int main() {
    fprintf(stderr, "Montgomery backend comparison\n");
    fprintf(stderr, "==============================\n\n");

    // § 1: Kernel-level comparison
    fprintf(stderr, "§ 1  Kernel-level montgomery_mul (single call, ns/op)\n\n");
    fprintf(stderr, "%-8s  %12s  %12s  %12s  %10s  %10s\n",
            "k", "fused_CIOS", "school+REDC", "kara+REDC", "K/F delta", "K/S delta");
    fprintf(stderr, "%-8s  %12s  %12s  %12s  %10s  %10s\n",
            "----", "----------", "-----------", "---------", "---------", "---------");

    uint32_t ks[] = { 8, 16, 32, 48, 64 };
    for (uint32_t k : ks) {
        // Scale reps down for large k to keep runtime reasonable
        int reps = (k <= 16) ? 10000 : (k <= 32) ? 5000 : (k <= 48) ? 2000 : 1000;
        int warmup = reps / 10;

        auto r = bench_kernels(k, reps, warmup);
        double k_vs_f = ((r.karatsuba_ns / r.fused_ns) - 1.0) * 100.0;
        double k_vs_s = ((r.karatsuba_ns / r.schoolbook_ns) - 1.0) * 100.0;

        auto f_s = fmt_ns(r.fused_ns);
        auto s_s = fmt_ns(r.schoolbook_ns);
        auto k_s = fmt_ns(r.karatsuba_ns);
        fprintf(stderr, "%-8u  %12s  %12s  %12s  %+9.1f%%  %+9.1f%%\n",
                k, f_s.c_str(), s_s.c_str(), k_s.c_str(),
                k_vs_f, k_vs_s);
    }

    // § 2: End-to-end pow_mod
    fprintf(stderr, "\n§ 2  End-to-end pow_mod (median, %d samples)\n\n", 20);
    fprintf(stderr, "%-8s  %14s\n", "bits", "pow_mod");
    fprintf(stderr, "%-8s  %14s\n", "----", "-------");

    uint32_t widths[] = { 1024, 2048, 4096 };
    for (uint32_t bits : widths) {
        int samples = (bits <= 2048) ? 20 : 10;
        double ns = bench_pow_mod_e2e(bits, samples);
        auto ns_s = fmt_ns(ns);
        fprintf(stderr, "%-8u  %14s\n", bits, ns_s.c_str());
    }

    fprintf(stderr, "\nDone.\n");
    return 0;
}
