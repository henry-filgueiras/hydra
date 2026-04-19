// bench/probe_mul_leaf.cpp — leaf-level multiply microbench.
//
// Measures:
//   * `mul_limbs` (schoolbook, dual-row via mac_row_2) at n = 4, 8, 16.
//   * `mul_karatsuba` at n = 16, 32, 64 — the Karatsuba recursion
//     bottoms out into `mul_limbs(n=16)`, so the n=16 leaf is the
//     kernel of interest for 2048/4096-bit pow_mod Montgomery.
//
// Used before/after a candidate asm kernel change to measure whether
// the leaf gain translates up the Karatsuba tree.
//
// Build:
//   clang++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       bench/probe_mul_leaf.cpp -o build-rel/probe_mul_leaf

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

template <typename Fn>
static double median_ns(Fn&& fn, int reps, int warmup) {
    std::vector<double> s;
    s.reserve(5);
    for (int i = 0; i < 5; ++i) s.push_back(bench_ns(fn, reps, warmup));
    std::sort(s.begin(), s.end());
    return s[2];
}

static void bench_mul_limbs(uint32_t n) {
    std::mt19937_64 rng(0xA1'1100ull + n);
    std::vector<uint64_t> a(n), b(n), out(2 * n);
    for (auto& l : a) l = rng();
    for (auto& l : b) l = rng();

    int reps = (n <= 8) ? 500'000 : (n <= 16) ? 300'000 : 100'000;
    int warmup = reps / 10;

    double t = median_ns([&]() {
        hd::mul_limbs(a.data(), n, b.data(), n, out.data());
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    std::printf("mul_limbs        n=%-2u   %8.2f ns   (%.2f MAC/ns)\n",
                n, t, static_cast<double>(n) * n / t);
}

static void bench_mul_kara(uint32_t n) {
    std::mt19937_64 rng(0xA2'2200ull + n);
    std::vector<uint64_t> a(n), b(n), out(2 * n);
    for (auto& l : a) l = rng();
    for (auto& l : b) l = rng();

    hd::ScratchWorkspace ws;
    ws.reserve_limbs(hd::karatsuba_scratch_limbs(n));

    int reps = (n <= 16) ? 200'000 : (n <= 32) ? 80'000 : 20'000;
    int warmup = reps / 10;

    double t = median_ns([&]() {
        hd::mul_karatsuba(a.data(), b.data(), n, out.data(), ws);
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    std::printf("mul_karatsuba    n=%-2u   %8.2f ns\n", n, t);
}

// Synthetic mac_row_2 microbench, isolated from mul_limbs overhead.
// 1000 iterations of `mac_row_2(a0, a1, b, nb, out)` with rotating
// offsets so the output-buffer cache line stays warm but distinct.
static void bench_mac_row_2(uint32_t nb) {
    std::mt19937_64 rng(0xA3'3300ull + nb);
    std::vector<uint64_t> a(64), b(nb);
    std::vector<uint64_t> out(2 * 64, 0);
    for (auto& l : a) l = rng();
    for (auto& l : b) l = rng();
    const uint32_t stride = 2;  // pair-step through a[]

    int reps = (nb <= 8) ? 2'000'000 : (nb <= 16) ? 1'000'000 : 500'000;
    int warmup = reps / 10;

    double t = median_ns([&]() {
        uint32_t base = 0;
        hd::mac_row_2(a[base], a[base + 1], b.data(), nb, out.data() + base);
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    (void)stride;
    std::printf("mac_row_2        nb=%-2u  %8.2f ns   (%.2f MAC/ns)\n",
                nb, t, 2.0 * nb / t);
}

int main() {
    std::printf("── mac_row_2 (isolated dual-row leaf kernel) ─────────\n");
    for (uint32_t nb : {4u, 8u, 12u, 16u, 24u, 32u}) bench_mac_row_2(nb);

    std::printf("\n── mul_limbs (schoolbook, full n×n) ──────────────────\n");
    for (uint32_t n : {4u, 8u, 12u, 16u, 24u, 32u}) bench_mul_limbs(n);

    std::printf("\n── mul_karatsuba (bottoms at n=16 → mul_limbs) ───────\n");
    for (uint32_t n : {16u, 32u, 64u}) bench_mul_kara(n);

    return 0;
}
