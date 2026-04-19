// bench/probe_mont_fios.cpp — A/B microbench: FIOS (dual-row CIOS) vs fused CIOS.
//
// Goal: isolate the structural per-kernel delta between canonical fused
// CIOS and the FIOS variant that merges the two inner row-loops into
// one loop with two independent mul/umulh chains.  This runs
// under fixed operands (no rotating-operand pow_mod cadence), so the
// result characterises the raw kernel speed, not the L1 behaviour seen
// end-to-end.
//
// Build:
//   clang++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       bench/probe_mont_fios.cpp -o build-rel/probe_mont_fios

#include "../hydra.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;

template <typename Fn>
static double bench_op(Fn&& fn, int reps, int warmup) {
    for (int i = 0; i < warmup; ++i) fn();
    auto t0 = clk::now();
    for (int i = 0; i < reps; ++i) fn();
    auto t1 = clk::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
}

int main() {
    std::printf("k    fused       fios        fios_sqr     Δ fios/fused\n");
    std::printf("--   ----------  ----------  ----------   ------------\n");

    for (uint32_t k : {4u, 6u, 8u, 12u, 16u, 24u, 31u, 32u}) {
        std::mt19937_64 rng(0xF10500ull + k);
        std::vector<uint64_t> mod(k), a(k), b(k);
        for (auto& l : mod) l = rng();
        mod[0] |= 1u;
        mod[k - 1] |= (1ull << 63);
        for (auto& l : a) l = rng();
        for (auto& l : b) l = rng();
        uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);

        std::vector<uint64_t> out(k);
        std::vector<uint64_t> work_fused(k + 2);
        std::vector<uint64_t> work_fios(k + 2);

        int reps = (k <= 8) ? 200000 : (k <= 16) ? 100000 : 40000;
        int warmup = reps / 10;

        double fused_ns = bench_op([&]() {
            hydra::detail::montgomery_mul_fused(
                a.data(), b.data(), k, mod.data(), n0inv,
                out.data(), work_fused.data());
            asm volatile("" : : "r"(out.data()) : "memory");
        }, reps, warmup);

        double fios_ns = bench_op([&]() {
            hydra::detail::montgomery_mul_fios(
                a.data(), b.data(), k, mod.data(), n0inv,
                out.data(), work_fios.data());
            asm volatile("" : : "r"(out.data()) : "memory");
        }, reps, warmup);

        double fios_sqr_ns = bench_op([&]() {
            hydra::detail::montgomery_sqr_fios(
                a.data(), k, mod.data(), n0inv,
                out.data(), work_fios.data());
            asm volatile("" : : "r"(out.data()) : "memory");
        }, reps, warmup);

        double delta = ((fios_ns / fused_ns) - 1.0) * 100.0;
        std::printf("%-3u  %8.1f ns %8.1f ns %8.1f ns   %+6.1f%%\n",
                    k, fused_ns, fios_ns, fios_sqr_ns, delta);
    }
    return 0;
}
