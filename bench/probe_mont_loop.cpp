// bench/probe_mont_loop.cpp
//
// Like probe_mont_sos but with rotating operands and a sliding-window
// of "table-like" precomputed operands per call.  This more closely
// mirrors what pow_mod_montgomery exercises: ping-ponging a result
// through ~8 different "table[i]" operands per multiplication.

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
    std::printf("k     fused         sos_via_mul_limbs    delta\n");
    std::printf("--    -----         -----------------    -----\n");

    constexpr uint32_t TABLE = 8;
    for (uint32_t k : {4u, 8u, 12u, 16u, 24u, 31u}) {
        std::mt19937_64 rng(0x12340000ull + k);
        std::vector<uint64_t> mod(k);
        for (auto& l : mod) l = rng();
        mod[0] |= 1u;
        mod[k - 1] |= (1ull << 63);
        // Table of TABLE distinct operands, each k limbs.
        std::vector<std::vector<uint64_t>> tbl(TABLE,
                                               std::vector<uint64_t>(k));
        for (auto& t : tbl) {
            for (auto& l : t) l = rng();
        }
        std::vector<uint64_t> result(k), temp(k);
        for (auto& l : result) l = rng();
        uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);

        std::vector<uint64_t> work_fused(k + 2);
        std::vector<uint64_t> work_sos(2 * k + 1);

        // Reps × (sqr + mul w/ rotating table entry) — same shape as
        // sliding-window pow_mod's per-bit body, in miniature.
        int reps = (k <= 8) ? 100000 : (k <= 16) ? 30000 : 10000;
        int warmup = reps / 10;

        double fused_ns = bench_op([&]() {
            uint32_t idx = 0;
            // 8-iteration mini-loop matching pow_mod's per-window cost
            for (int q = 0; q < 8; ++q) {
                hydra::detail::montgomery_sqr_fused(
                    result.data(), k, mod.data(), n0inv,
                    temp.data(), work_fused.data());
                std::memcpy(result.data(), temp.data(), k * sizeof(uint64_t));
                hydra::detail::montgomery_mul_fused(
                    result.data(), tbl[idx & (TABLE - 1)].data(), k,
                    mod.data(), n0inv, temp.data(), work_fused.data());
                std::memcpy(result.data(), temp.data(), k * sizeof(uint64_t));
                idx = (idx + 3) & (TABLE - 1);
            }
            asm volatile("" : : "r"(result.data()) : "memory");
        }, reps / 16, warmup / 16);

        double sos_ns = bench_op([&]() {
            uint32_t idx = 0;
            for (int q = 0; q < 8; ++q) {
                hydra::detail::montgomery_sqr_sos(
                    result.data(), k, mod.data(), n0inv,
                    temp.data(), work_sos.data());
                std::memcpy(result.data(), temp.data(), k * sizeof(uint64_t));
                hydra::detail::montgomery_mul_sos(
                    result.data(), tbl[idx & (TABLE - 1)].data(), k,
                    mod.data(), n0inv, temp.data(), work_sos.data());
                std::memcpy(result.data(), temp.data(), k * sizeof(uint64_t));
                idx = (idx + 3) & (TABLE - 1);
            }
            asm volatile("" : : "r"(result.data()) : "memory");
        }, reps / 16, warmup / 16);

        double delta = ((sos_ns / fused_ns) - 1.0) * 100.0;
        std::printf("%-3u   %8.1f ns   %8.1f ns          %+6.1f%%\n",
                    k, fused_ns, sos_ns, delta);
    }
    return 0;
}
