// bench/probe_mont_k32_band.cpp — FIOS vs Karatsuba+REDC at k=31..64.
//
// KARATSUBA_MONT_THRESHOLD = 32 was benchmark-derived on 2026-04-16,
// when the alternative at k >= 8 was *fused CIOS* (Karatsuba+REDC won
// −9% at k=32, −16% at k=64).  FIOS then replaced fused CIOS for the
// k=1..31 band (−22% vs fused at k=32 on the kernel probe) — but the
// FIOS-vs-Karatsuba comparison at k >= 32 was never run.  Same trap
// the FUSED_THRESHOLD cleanup found: threshold constants carry forward
// silently when the backend on one side of the boundary changes.
//
// Dispatch bands today (pad_ok = n_padded <= 1.25k):
//   k = 32            → Karatsuba+REDC   (2048-bit pow_mod)
//   k = 33..51        → FIOS             (pad guard rejects Karatsuba)
//   k = 52..64        → Karatsuba+REDC   (4096-bit pow_mod at k=64)
//
// Layer 1: isolated kernel microbench (mul and sqr separately).
// Layer 2: "pow_mod cadence" — 5 sqr + 1 mul per iter, rotating the
//          accumulator through the output buffer (the measurement that
//          decided both the SOS null and the FIOS win).
//
// Build:
//   clang++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       bench/probe_mont_k32_band.cpp -o build-rel/probe_mont_k32_band

#include "../hydra.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;
namespace hd = hydra::detail;

template <typename Fn>
static double bench_ns(Fn&& fn, int reps, int warmup) {
    for (int i = 0; i < warmup; ++i) fn();
    auto t0 = clk::now();
    for (int i = 0; i < reps; ++i) fn();
    auto t1 = clk::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
}

// Median of 5 runs of `fn`, each run is `reps` iterations averaged.
template <typename Fn>
static double median_ns(Fn&& fn, int reps, int warmup) {
    std::vector<double> samples;
    samples.reserve(5);
    for (int i = 0; i < 5; ++i)
        samples.push_back(bench_ns(fn, reps, warmup));
    std::sort(samples.begin(), samples.end());
    return samples[2];
}

static uint32_t next_pow2(uint32_t k) {
    uint32_t n = 1;
    while (n < k) n <<= 1;
    return n;
}

struct Operands {
    std::vector<uint64_t> mod, a, b;
    uint64_t n0inv;
    explicit Operands(uint32_t k, uint64_t seed) : mod(k), a(k), b(k) {
        std::mt19937_64 rng(seed ^ k);
        for (auto& l : mod) l = rng();
        mod[0] |= 1u;
        mod[k - 1] |= (1ull << 63);
        for (auto& l : a) l = rng();
        for (auto& l : b) l = rng();
        n0inv = hd::montgomery_n0inv(mod[0]);
    }
};

// ─── Layer 1: kernel-level ──────────────────────────────────
static void bench_kernel(uint32_t k) {
    Operands op(k, 0xB32'BA9Dull);
    const uint32_t n_padded = next_pow2(k);

    std::vector<uint64_t> out(k);
    std::vector<uint64_t> work_fios(k + 2);
    std::vector<uint64_t> work_wide(2 * k + 1);
    std::vector<uint64_t> pa(n_padded), pb(n_padded), kara_buf(2 * n_padded);
    hydra::detail::ScratchWorkspace ws;

    const int reps   = 40'000;
    const int warmup = 4'000;

    double kara_mul = median_ns([&]() {
        hd::montgomery_mul_karatsuba(op.a.data(), op.b.data(), k,
                                     op.mod.data(), op.n0inv, out.data(),
                                     work_wide.data(), pa.data(), pb.data(),
                                     kara_buf.data(), n_padded, ws);
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    double fios_mul = median_ns([&]() {
        hd::montgomery_mul_fios(op.a.data(), op.b.data(), k, op.mod.data(),
                                op.n0inv, out.data(), work_fios.data());
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    double kara_sqr = median_ns([&]() {
        hd::montgomery_sqr_karatsuba(op.a.data(), k, op.mod.data(), op.n0inv,
                                     out.data(), work_wide.data(), pa.data(),
                                     pb.data(), kara_buf.data(), n_padded, ws);
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    double fios_sqr = median_ns([&]() {
        hd::montgomery_sqr_fios(op.a.data(), k, op.mod.data(), op.n0inv,
                                out.data(), work_fios.data());
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    std::printf("k=%-2u  mul: kara %7.1f ns  fios %7.1f ns  Δ %+6.1f%%"
                "    sqr: kara %7.1f ns  fios %7.1f ns  Δ %+6.1f%%\n",
                k, kara_mul, fios_mul, ((fios_mul / kara_mul) - 1.0) * 100.0,
                kara_sqr, fios_sqr, ((fios_sqr / kara_sqr) - 1.0) * 100.0);
}

// ─── Layer 2: pow_mod cadence (5 sqr + 1 mul, rotating) ─────
static void bench_cadence(uint32_t k) {
    Operands op(k, 0xCADEull << 16);
    const uint32_t n_padded = next_pow2(k);

    std::vector<uint64_t> acc(k), tmp(k);
    std::vector<uint64_t> work_fios(k + 2);
    std::vector<uint64_t> work_wide(2 * k + 1);
    std::vector<uint64_t> pa(n_padded), pb(n_padded), kara_buf(2 * n_padded);
    hydra::detail::ScratchWorkspace ws;

    const int reps   = 8'000;   // each rep = 5 sqr + 1 mul
    const int warmup = 800;

    std::memcpy(acc.data(), op.a.data(), k * sizeof(uint64_t));
    double kara_c = median_ns([&]() {
        for (int i = 0; i < 5; ++i) {
            hd::montgomery_sqr_karatsuba(acc.data(), k, op.mod.data(),
                                         op.n0inv, tmp.data(),
                                         work_wide.data(), pa.data(),
                                         pb.data(), kara_buf.data(),
                                         n_padded, ws);
            std::swap(acc, tmp);
        }
        hd::montgomery_mul_karatsuba(acc.data(), op.b.data(), k,
                                     op.mod.data(), op.n0inv, tmp.data(),
                                     work_wide.data(), pa.data(), pb.data(),
                                     kara_buf.data(), n_padded, ws);
        std::swap(acc, tmp);
        asm volatile("" : : "r"(acc.data()) : "memory");
    }, reps, warmup);

    std::memcpy(acc.data(), op.a.data(), k * sizeof(uint64_t));
    double fios_c = median_ns([&]() {
        for (int i = 0; i < 5; ++i) {
            hd::montgomery_sqr_fios(acc.data(), k, op.mod.data(), op.n0inv,
                                    tmp.data(), work_fios.data());
            std::swap(acc, tmp);
        }
        hd::montgomery_mul_fios(acc.data(), op.b.data(), k, op.mod.data(),
                                op.n0inv, tmp.data(), work_fios.data());
        std::swap(acc, tmp);
        asm volatile("" : : "r"(acc.data()) : "memory");
    }, reps, warmup);

    std::printf("k=%-2u  cadence: kara %8.1f ns/iter  fios %8.1f ns/iter  Δ %+6.1f%%\n",
                k, kara_c, fios_c, ((fios_c / kara_c) - 1.0) * 100.0);
}

int main() {
    const uint32_t ks[] = {31, 32, 36, 40, 48, 52, 56, 60, 64};

    std::printf("── Layer 1: isolated kernel (median of 5 × 40k reps) ──\n");
    for (uint32_t k : ks) bench_kernel(k);

    std::printf("\n── Layer 2: pow_mod cadence, 5 sqr + 1 mul rotating ──\n");
    for (uint32_t k : ks) bench_cadence(k);

    return 0;
}
