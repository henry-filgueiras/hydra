// bench/probe_sqr_halved.cpp — halved FIOS squaring vs FIOS sqr-as-mul.
//
// montgomery_sqr_fios_halved exploits cross-term symmetry inside the
// fused pass: ~1.5k² MACs vs 2k².  The MAC saving is partially offset
// by (a) the doubled product's 65-bit carry bookkeeping and (b) the
// reduce chain running unpaired in the j < i region (~half the reduce
// MACs averaged over rows).  This probe measures what survives.
//
// Layer 0: bit-identity cross-check vs montgomery_sqr_fios at every
//          k = 1..64 (random + carry-adversarial).  The two kernels
//          compute the same m_i sequence, so outputs must match
//          exactly; any mismatch aborts before timing.
// Layer 1: isolated kernel sqr A/B.
// Layer 2: pow_mod cadence — 5 sqr + 1 mul, rotating accumulator.
//
// Build:
//   clang++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       bench/probe_sqr_halved.cpp -o build-rel/probe_sqr_halved

#include "../hydra.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

template <typename Fn>
static double median_ns(Fn&& fn, int reps, int warmup) {
    std::vector<double> samples;
    for (int i = 0; i < 5; ++i) samples.push_back(bench_ns(fn, reps, warmup));
    std::sort(samples.begin(), samples.end());
    return samples[2];
}

// ─── Layer 0: bit-identity check, every k ───────────────────
static void cross_check() {
    int checked = 0;
    for (uint32_t k = 1; k <= 64; ++k) {
        std::mt19937_64 rng(0x5A11D000ull + k);
        for (int trial = 0; trial < 8; ++trial) {
            std::vector<uint64_t> mod(k), a(k);
            for (auto& l : mod) l = rng();
            mod[0] |= 1u;
            mod[k - 1] |= (1ull << 63);
            if (trial == 0) {
                // carry-adversarial: a = mod - 1
                a = mod;
                a[0] -= 1;
            } else {
                for (auto& l : a) l = rng();
                a[k - 1] &= (1ull << 63) - 1;   // a < mod
            }
            uint64_t n0inv = hd::montgomery_n0inv(mod[0]);

            std::vector<uint64_t> out_ref(k), out_new(k);
            std::vector<uint64_t> work(k + 2, 0xBADC0FFEE0DDF00Dull);
            hd::montgomery_sqr_fios(a.data(), k, mod.data(), n0inv,
                                    out_ref.data(), work.data());
            std::fill(work.begin(), work.end(), 0xBADC0FFEE0DDF00Dull);
            hd::montgomery_sqr_fios_halved(a.data(), k, mod.data(), n0inv,
                                           out_new.data(), work.data());
            if (out_ref != out_new) {
                std::printf("MISMATCH at k=%u trial=%d — aborting\n",
                            k, trial);
                std::exit(1);
            }
            ++checked;
        }
    }
    std::printf("Layer 0: %d cross-checks passed (k=1..64, "
                "bit-identical)\n\n", checked);
}

// ─── Layer 1: isolated kernel ───────────────────────────────
static void bench_kernel(uint32_t k) {
    std::mt19937_64 rng(0x5A11D100ull + k);
    std::vector<uint64_t> mod(k), a(k);
    for (auto& l : mod) l = rng();
    mod[0] |= 1u;
    mod[k - 1] |= (1ull << 63);
    for (auto& l : a) l = rng();
    a[k - 1] &= (1ull << 63) - 1;
    uint64_t n0inv = hd::montgomery_n0inv(mod[0]);

    std::vector<uint64_t> out(k), work(k + 2);

    const int reps   = (k <= 8) ? 400'000 : (k <= 24) ? 100'000 : 40'000;
    const int warmup = reps / 10;

    double fios_sqr = median_ns([&]() {
        hd::montgomery_sqr_fios(a.data(), k, mod.data(), n0inv,
                                out.data(), work.data());
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    double halved = median_ns([&]() {
        hd::montgomery_sqr_fios_halved(a.data(), k, mod.data(), n0inv,
                                       out.data(), work.data());
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    std::printf("k=%-2u  fios_sqr %8.1f ns   halved %8.1f ns   Δ %+6.1f%%\n",
                k, fios_sqr, halved, ((halved / fios_sqr) - 1.0) * 100.0);
}

// ─── Layer 2: pow_mod cadence (5 sqr + 1 mul, rotating) ─────
static void bench_cadence(uint32_t k) {
    std::mt19937_64 rng(0x5A11D200ull + k);
    std::vector<uint64_t> mod(k), a(k), b(k);
    for (auto& l : mod) l = rng();
    mod[0] |= 1u;
    mod[k - 1] |= (1ull << 63);
    for (auto& l : a) l = rng();
    a[k - 1] &= (1ull << 63) - 1;
    for (auto& l : b) l = rng();
    b[k - 1] &= (1ull << 63) - 1;
    uint64_t n0inv = hd::montgomery_n0inv(mod[0]);

    std::vector<uint64_t> acc(k), tmp(k), work(k + 2);

    const int reps   = (k <= 16) ? 40'000 : 8'000;
    const int warmup = reps / 10;

    auto cadence = [&](auto&& sqr_fn) {
        std::memcpy(acc.data(), a.data(), k * sizeof(uint64_t));
        return median_ns([&]() {
            for (int s = 0; s < 5; ++s) {
                sqr_fn(acc.data(), k, mod.data(), n0inv,
                       tmp.data(), work.data());
                std::swap(acc, tmp);
            }
            hd::montgomery_mul_fios(acc.data(), b.data(), k, mod.data(),
                                    n0inv, tmp.data(), work.data());
            std::swap(acc, tmp);
            asm volatile("" : : "r"(acc.data()) : "memory");
        }, reps, warmup);
    };

    double fios_c   = cadence(hd::montgomery_sqr_fios);
    double halved_c = cadence(hd::montgomery_sqr_fios_halved);

    std::printf("k=%-2u  cadence: fios %9.1f ns/iter   halved %9.1f ns/iter"
                "   Δ %+6.1f%%\n",
                k, fios_c, halved_c, ((halved_c / fios_c) - 1.0) * 100.0);
}

int main() {
    cross_check();

    const uint32_t ks[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 40, 48, 56, 64};

    std::printf("── Layer 1: isolated kernel sqr (median of 5) ──\n");
    for (uint32_t k : ks) bench_kernel(k);

    std::printf("\n── Layer 2: pow_mod cadence, 5 sqr + 1 mul ──\n");
    for (uint32_t k : ks) bench_cadence(k);

    return 0;
}
