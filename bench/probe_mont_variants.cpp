// bench/probe_mont_variants.cpp — isolate where SOS costs come from.
//
// Variants under loop-with-rotating-operands harness:
//   A. fused CIOS (baseline)
//   B. SOS via mul_limbs (current implementation)
//   C. SOS hand-coded single-row (bypasses mul_limbs/mac_row_2)
//   D. SOS hand-coded dual-row (mac_row_2 inlined directly)

#include "../hydra.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;
namespace hd = hydra::detail;

// Variant C: hand-coded single-row schoolbook + REDC, fully inlined.
static inline void mont_mul_sos_singlerow(
    const uint64_t* a, const uint64_t* b, uint32_t k,
    const uint64_t* mod, uint64_t n0inv,
    uint64_t* out, uint64_t* work)
{
    std::memset(work, 0, (2 * k + 1) * sizeof(uint64_t));
    // Schoolbook: T = a*b
    for (uint32_t i = 0; i < k; ++i) {
        uint64_t ai = a[i];
        uint64_t carry = 0;
        for (uint32_t j = 0; j < k; ++j) {
            unsigned __int128 t =
                (unsigned __int128)ai * b[j] + work[i + j] + carry;
            work[i + j] = (uint64_t)t;
            carry = (uint64_t)(t >> 64);
        }
        // Carry into work[i+k]
        unsigned __int128 t =
            (unsigned __int128)work[i + k] + carry;
        work[i + k] = (uint64_t)t;
        uint64_t ch = (uint64_t)(t >> 64);
        for (uint32_t p = i + k + 1; ch && p <= 2 * k; ++p) {
            uint64_t s = work[p] + ch;
            ch = (s < ch) ? 1u : 0u;
            work[p] = s;
        }
    }
    // REDC
    hd::montgomery_redc(work, k, mod, n0inv, out);
}

template <typename Fn>
static double bench_op(Fn&& fn, int reps, int warmup) {
    for (int i = 0; i < warmup; ++i) fn();
    auto t0 = clk::now();
    for (int i = 0; i < reps; ++i) fn();
    auto t1 = clk::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
}

int main() {
    std::printf("k     fused      sos_mullimbs    sos_singlerow    "
                "vs_fused(B)  vs_fused(C)\n");
    std::printf("--    -----      ------------    -------------    "
                "-----------  -----------\n");

    constexpr uint32_t TABLE = 8;
    for (uint32_t k : {4u, 8u, 12u, 16u, 24u, 31u}) {
        std::mt19937_64 rng(0x12340000ull + k);
        std::vector<uint64_t> mod(k);
        for (auto& l : mod) l = rng();
        mod[0] |= 1u;
        mod[k - 1] |= (1ull << 63);
        std::vector<std::vector<uint64_t>> tbl(TABLE,
                                               std::vector<uint64_t>(k));
        for (auto& t : tbl) {
            for (auto& l : t) l = rng();
        }
        std::vector<uint64_t> result(k), temp(k);
        for (auto& l : result) l = rng();
        uint64_t n0inv = hd::montgomery_n0inv(mod[0]);

        std::vector<uint64_t> work_fused(k + 2);
        std::vector<uint64_t> work_sos(2 * k + 1);

        int reps = (k <= 8) ? 100000 : (k <= 16) ? 30000 : 10000;
        int warmup = reps / 10;

        auto run_fused = [&]() {
            uint32_t idx = 0;
            for (int q = 0; q < 8; ++q) {
                hd::montgomery_sqr_fused(result.data(), k, mod.data(), n0inv,
                                         temp.data(), work_fused.data());
                std::memcpy(result.data(), temp.data(), k * sizeof(uint64_t));
                hd::montgomery_mul_fused(result.data(),
                                          tbl[idx & (TABLE - 1)].data(), k,
                                          mod.data(), n0inv,
                                          temp.data(), work_fused.data());
                std::memcpy(result.data(), temp.data(), k * sizeof(uint64_t));
                idx = (idx + 3) & (TABLE - 1);
            }
            asm volatile("" : : "r"(result.data()) : "memory");
        };

        auto run_sos_mullimbs = [&]() {
            uint32_t idx = 0;
            for (int q = 0; q < 8; ++q) {
                hd::montgomery_sqr_sos(result.data(), k, mod.data(), n0inv,
                                       temp.data(), work_sos.data());
                std::memcpy(result.data(), temp.data(), k * sizeof(uint64_t));
                hd::montgomery_mul_sos(result.data(),
                                        tbl[idx & (TABLE - 1)].data(), k,
                                        mod.data(), n0inv,
                                        temp.data(), work_sos.data());
                std::memcpy(result.data(), temp.data(), k * sizeof(uint64_t));
                idx = (idx + 3) & (TABLE - 1);
            }
            asm volatile("" : : "r"(result.data()) : "memory");
        };

        auto run_sos_singlerow = [&]() {
            uint32_t idx = 0;
            for (int q = 0; q < 8; ++q) {
                mont_mul_sos_singlerow(result.data(), result.data(), k,
                                        mod.data(), n0inv,
                                        temp.data(), work_sos.data());
                std::memcpy(result.data(), temp.data(), k * sizeof(uint64_t));
                mont_mul_sos_singlerow(result.data(),
                                        tbl[idx & (TABLE - 1)].data(), k,
                                        mod.data(), n0inv,
                                        temp.data(), work_sos.data());
                std::memcpy(result.data(), temp.data(), k * sizeof(uint64_t));
                idx = (idx + 3) & (TABLE - 1);
            }
            asm volatile("" : : "r"(result.data()) : "memory");
        };

        double f = bench_op(run_fused,            reps / 16, warmup / 16);
        double b = bench_op(run_sos_mullimbs,     reps / 16, warmup / 16);
        double c = bench_op(run_sos_singlerow,    reps / 16, warmup / 16);

        std::printf("%-3u   %8.1f   %8.1f       %8.1f         "
                    "%+6.1f%%      %+6.1f%%\n",
                    k, f, b, c,
                    ((b / f) - 1.0) * 100.0,
                    ((c / f) - 1.0) * 100.0);
    }
    return 0;
}
