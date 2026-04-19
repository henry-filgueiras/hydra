// bench/probe_mont_sos.cpp — A/B microbench: SOS vs fused Montgomery.
//
// Goal: confirm or disprove that montgomery_mul_sos is competitive with
// montgomery_mul_fused at k=4..24, where pow_mod's regression appeared.
//
// Build:
//   g++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       bench/probe_mont_sos.cpp -o build-rel/probe_mont_sos

#include "../hydra.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <algorithm>

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
    std::printf("k     fused_mul      sos_mul       sos_sqr        delta_mul\n");
    std::printf("--    ---------      -------       -------        ---------\n");

    for (uint32_t k : {4u, 6u, 8u, 12u, 16u, 24u, 31u}) {
        std::mt19937_64 rng(0xABCD0000ull + k);
        std::vector<uint64_t> mod(k), a(k), b(k);
        for (auto& l : mod) l = rng();
        mod[0] |= 1u;
        mod[k - 1] |= (1ull << 63);
        for (auto& l : a) l = rng();
        for (auto& l : b) l = rng();
        uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);

        std::vector<uint64_t> out(k);
        std::vector<uint64_t> work_fused(k + 2);
        std::vector<uint64_t> work_sos(2 * k + 1);

        int reps = (k <= 8) ? 200000 : (k <= 16) ? 100000 : 40000;
        int warmup = reps / 10;

        double fused_ns = bench_op([&]() {
            std::memset(work_fused.data(), 0, (k + 2) * sizeof(uint64_t));
            hydra::detail::montgomery_mul_fused(
                a.data(), b.data(), k, mod.data(), n0inv,
                out.data(), work_fused.data());
            asm volatile("" : : "r"(out.data()) : "memory");
        }, reps, warmup);

        double sos_ns = bench_op([&]() {
            hydra::detail::montgomery_mul_sos(
                a.data(), b.data(), k, mod.data(), n0inv,
                out.data(), work_sos.data());
            asm volatile("" : : "r"(out.data()) : "memory");
        }, reps, warmup);

        double sos_sqr_ns = bench_op([&]() {
            hydra::detail::montgomery_sqr_sos(
                a.data(), k, mod.data(), n0inv,
                out.data(), work_sos.data());
            asm volatile("" : : "r"(out.data()) : "memory");
        }, reps, warmup);

        double delta = ((sos_ns / fused_ns) - 1.0) * 100.0;
        std::printf("%-3u   %8.1f ns    %8.1f ns    %8.1f ns    %+6.1f%%\n",
                    k, fused_ns, sos_ns, sos_sqr_ns, delta);
    }
    return 0;
}
