// bench/probe_fios_small_k.cpp — FIOS vs separate schoolbook+REDC at k=4..7.
//
// The 2026-04-18 FIOS landing set FUSED_THRESHOLD=8, keeping k<8 on the
// "separate mul+REDC" path.  This probe asks whether the threshold should
// move down to 4 (or somewhere in between).
//
// At small k the comparison is asymmetric:
//   * mul: FIOS (dual-row CIOS)   vs  plain schoolbook product + REDC
//   * sqr: FIOS (forwards to mul) vs  cross-term-symmetry squaring + REDC
// The sqr asymmetry matters — sliding-window pow_mod does ~5× more sqr
// than mul per exponent bit, so kernel-level sqr speed dominates.
//
// Layer 1: isolated kernel microbench per op.
// Layer 2: "pow_mod cadence" — ~5 sqr per 1 mul, rotating operands.
// Layer 3: full pow_mod with both backends force-selected, at 256/320/384/448
//          bits (k = 4/5/6/7).
//
// Build:
//   clang++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       bench/probe_fios_small_k.cpp -o build-rel/probe_fios_small_k

#include "../hydra.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;
namespace hd = hydra::detail;

// ─── bench harness ──────────────────────────────────────────
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

// ─── Layer 1: kernel-level ──────────────────────────────────
static void bench_kernel(uint32_t k) {
    std::mt19937_64 rng(0xF105'0005ull ^ k);
    std::vector<uint64_t> mod(k), a(k), b(k);
    for (auto& l : mod) l = rng();
    mod[0] |= 1u;
    mod[k - 1] |= (1ull << 63);
    for (auto& l : a) l = rng();
    for (auto& l : b) l = rng();

    uint64_t n0inv = hd::montgomery_n0inv(mod[0]);

    std::vector<uint64_t> out(k);
    std::vector<uint64_t> work_fios(k + 2);
    std::vector<uint64_t> work_wide(2 * k + 1);

    const int reps   = 400'000;
    const int warmup = 40'000;

    double sep_mul = median_ns([&]() {
        hd::montgomery_mul(a.data(), b.data(), k, mod.data(),
                           n0inv, out.data(), work_wide.data());
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    double fios_mul = median_ns([&]() {
        hd::montgomery_mul_fios(a.data(), b.data(), k, mod.data(),
                                 n0inv, out.data(), work_fios.data());
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    double sep_sqr = median_ns([&]() {
        hd::montgomery_sqr(a.data(), k, mod.data(),
                           n0inv, out.data(), work_wide.data());
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    double fios_sqr = median_ns([&]() {
        hd::montgomery_sqr_fios(a.data(), k, mod.data(),
                                 n0inv, out.data(), work_fios.data());
        asm volatile("" : : "r"(out.data()) : "memory");
    }, reps, warmup);

    double d_mul = ((fios_mul / sep_mul) - 1.0) * 100.0;
    double d_sqr = ((fios_sqr / sep_sqr) - 1.0) * 100.0;

    std::printf("k=%-2u  mul: sep %6.1f ns  fios %6.1f ns  Δ %+6.1f%%"
                "    sqr: sep %6.1f ns  fios %6.1f ns  Δ %+6.1f%%\n",
                k, sep_mul, fios_mul, d_mul,
                sep_sqr, fios_sqr, d_sqr);
}

// ─── Layer 2: "pow_mod cadence" —─────────────────────────────
// 5 sqr + 1 mul per bit, rotating the input through result_mont_buf.
// This approximates the hot loop of sliding-window pow_mod.
static void bench_cadence(uint32_t k) {
    std::mt19937_64 rng(0xCADE'0000ull + k);
    std::vector<uint64_t> mod(k), a(k), b(k);
    for (auto& l : mod) l = rng();
    mod[0] |= 1u;
    mod[k - 1] |= (1ull << 63);
    for (auto& l : a) l = rng();
    for (auto& l : b) l = rng();

    uint64_t n0inv = hd::montgomery_n0inv(mod[0]);

    std::vector<uint64_t> acc(k), tmp(k), base_tbl(k);
    std::memcpy(acc.data(), a.data(), k * sizeof(uint64_t));
    std::memcpy(base_tbl.data(), b.data(), k * sizeof(uint64_t));

    std::vector<uint64_t> work_fios(k + 2);
    std::vector<uint64_t> work_wide(2 * k + 1);

    const int reps   = 50'000;      // each rep = 5 sqr + 1 mul
    const int warmup = 5'000;

    double sep_c = median_ns([&]() {
        for (int i = 0; i < 5; ++i) {
            hd::montgomery_sqr(acc.data(), k, mod.data(),
                               n0inv, tmp.data(), work_wide.data());
            std::swap(acc, tmp);
        }
        hd::montgomery_mul(acc.data(), base_tbl.data(), k, mod.data(),
                           n0inv, tmp.data(), work_wide.data());
        std::swap(acc, tmp);
        asm volatile("" : : "r"(acc.data()) : "memory");
    }, reps, warmup);

    // reset accumulator for fair comparison
    std::memcpy(acc.data(), a.data(), k * sizeof(uint64_t));

    double fios_c = median_ns([&]() {
        for (int i = 0; i < 5; ++i) {
            hd::montgomery_sqr_fios(acc.data(), k, mod.data(),
                                     n0inv, tmp.data(), work_fios.data());
            std::swap(acc, tmp);
        }
        hd::montgomery_mul_fios(acc.data(), base_tbl.data(), k, mod.data(),
                                 n0inv, tmp.data(), work_fios.data());
        std::swap(acc, tmp);
        asm volatile("" : : "r"(acc.data()) : "memory");
    }, reps, warmup);

    double delta = ((fios_c / sep_c) - 1.0) * 100.0;
    std::printf("k=%-2u  cadence(5sqr+1mul): sep %7.1f ns  fios %7.1f ns  Δ %+6.1f%%\n",
                k, sep_c, fios_c, delta);
}

// ─── Layer 3: end-to-end pow_mod with forced backend ─────────
enum class Backend { Separate, FIOS };

static hydra::Hydra pow_mod_with_backend(
    hydra::Hydra base, hydra::Hydra exp, const hydra::Hydra& mod,
    Backend backend)
{
    auto mod_lv = mod.limb_view();
    uint32_t k = mod_lv.count;

    hydra::MontgomeryContext ctx =
        hydra::MontgomeryContext::build(mod_lv.ptr, k);
    ctx.compute_r_sq();

    base = base % mod;
    if (base.is_negative()) base = base + mod;
    auto base_lv = base.limb_view();

    constexpr uint32_t MAX_K = 64;
    uint64_t work_buf[2 * MAX_K + 1];
    uint64_t temp_buf[MAX_K];
    uint64_t result_mont_buf[MAX_K];
    std::memset(work_buf, 0, (2 * k + 1) * sizeof(uint64_t));
    std::memset(temp_buf, 0, k * sizeof(uint64_t));
    std::memset(result_mont_buf, 0, k * sizeof(uint64_t));

    constexpr uint32_t WINDOW = 4;
    constexpr uint32_t TABLE_SIZE = 1u << (WINDOW - 1);
    uint64_t table[TABLE_SIZE][MAX_K];

    auto mont_mul = [&](const uint64_t* a, const uint64_t* b, uint64_t* out) {
        if (backend == Backend::FIOS) {
            hd::montgomery_mul_fios(a, b, k, ctx.mod_limbs.data(), ctx.n0inv,
                                     out, work_buf);
        } else {
            hd::montgomery_mul(a, b, k, ctx.mod_limbs.data(), ctx.n0inv,
                                out, work_buf);
        }
    };
    auto mont_sqr = [&](const uint64_t* a, uint64_t* out) {
        if (backend == Backend::FIOS) {
            hd::montgomery_sqr_fios(a, k, ctx.mod_limbs.data(), ctx.n0inv,
                                     out, work_buf);
        } else {
            hd::montgomery_sqr(a, k, ctx.mod_limbs.data(), ctx.n0inv,
                                out, work_buf);
        }
    };

    {
        uint64_t a_padded[MAX_K];
        std::memset(a_padded, 0, k * sizeof(uint64_t));
        uint32_t cc = (base_lv.count < k) ? base_lv.count : k;
        std::memcpy(a_padded, base_lv.ptr, cc * sizeof(uint64_t));
        mont_mul(a_padded, ctx.r_sq.data(), table[0]);
    }

    uint64_t base_sq[MAX_K];
    mont_sqr(table[0], base_sq);
    for (uint32_t i = 1; i < TABLE_SIZE; ++i)
        mont_mul(table[i - 1], base_sq, table[i]);

    {
        uint64_t one_padded[MAX_K];
        std::memset(one_padded, 0, k * sizeof(uint64_t));
        one_padded[0] = 1;
        mont_mul(one_padded, ctx.r_sq.data(), result_mont_buf);
    }

    auto exp_lv = exp.limb_view();
    uint32_t exp_limb_count = exp_lv.count;
    if (exp_limb_count == 0) return hydra::Hydra{1u} % mod;

    uint32_t top_limb_idx = exp_limb_count - 1;
    uint64_t top_limb = exp_lv.ptr[top_limb_idx];
    int top_bit = 63 - __builtin_clzll(top_limb);
    int total_bits = static_cast<int>(top_limb_idx) * 64 + top_bit;

    int bit_pos = total_bits;
    while (bit_pos >= 0) {
        uint32_t limb_idx = static_cast<uint32_t>(bit_pos) / 64;
        uint32_t bit_idx = static_cast<uint32_t>(bit_pos) % 64;
        uint64_t cur_bit = (exp_lv.ptr[limb_idx] >> bit_idx) & 1u;

        if (cur_bit == 0) {
            mont_sqr(result_mont_buf, temp_buf);
            std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            --bit_pos;
        } else {
            int window_len = WINDOW;
            if (bit_pos < static_cast<int>(WINDOW) - 1) window_len = bit_pos + 1;
            uint32_t wval = 0;
            for (int i = 0; i < window_len; ++i) {
                int bp = bit_pos - i;
                uint32_t li = static_cast<uint32_t>(bp) / 64;
                uint32_t bi = static_cast<uint32_t>(bp) % 64;
                uint32_t b = (exp_lv.ptr[li] >> bi) & 1u;
                wval = (wval << 1) | b;
            }
            int trailing_zeros = 0;
            while (window_len > 1 && (wval & 1u) == 0) {
                wval >>= 1; window_len--; trailing_zeros++;
            }
            for (int i = 0; i < window_len; ++i) {
                mont_sqr(result_mont_buf, temp_buf);
                std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            }
            uint32_t table_idx = (wval - 1) / 2;
            mont_mul(result_mont_buf, table[table_idx], temp_buf);
            std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            for (int i = 0; i < trailing_zeros; ++i) {
                mont_sqr(result_mont_buf, temp_buf);
                std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            }
            bit_pos -= (window_len + trailing_zeros);
        }
    }

    uint64_t result_limbs_buf[MAX_K];
    std::memset(result_limbs_buf, 0, k * sizeof(uint64_t));
    ctx.from_montgomery(result_mont_buf, result_limbs_buf, work_buf);
    uint32_t used = k;
    while (used > 0 && result_limbs_buf[used - 1] == 0) --used;
    if (used == 0) return hydra::Hydra{0u};
    return hydra::Hydra::from_limbs(result_limbs_buf, used);
}

static hydra::Hydra make_rand(uint32_t bits, uint64_t seed) {
    uint32_t n = (bits + 63) / 64;
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> limbs(n);
    for (auto& l : limbs) l = rng();
    limbs[0] |= 1u;
    limbs.back() |= (1ull << 63);
    return hydra::Hydra::from_limbs(limbs.data(), n);
}

static void bench_e2e(uint32_t bits) {
    uint32_t k = (bits + 63) / 64;

    hydra::Hydra base    = make_rand(bits, 0xCAFE'0000ull + bits);
    hydra::Hydra exp_val = make_rand(bits, 0xBEEF'0000ull + bits);
    hydra::Hydra mod_val = make_rand(bits, 0xDEAD'0000ull + bits);

    // Cross-validate
    auto r_sep  = pow_mod_with_backend(base, exp_val, mod_val, Backend::Separate);
    auto r_fios = pow_mod_with_backend(base, exp_val, mod_val, Backend::FIOS);
    if (!(r_sep == r_fios)) {
        std::fprintf(stderr, "MISMATCH at %u-bit (k=%u)\n", bits, k);
        std::exit(1);
    }

    const int reps   = (bits <= 384) ? 4000 : 2000;
    const int warmup = std::max(reps / 10, 5);

    double sep_ns = median_ns([&]() {
        auto r = pow_mod_with_backend(base, exp_val, mod_val, Backend::Separate);
        asm volatile("" : : "r"(r.meta) : "memory");
    }, reps, warmup);

    double fios_ns = median_ns([&]() {
        auto r = pow_mod_with_backend(base, exp_val, mod_val, Backend::FIOS);
        asm volatile("" : : "r"(r.meta) : "memory");
    }, reps, warmup);

    double delta = ((fios_ns / sep_ns) - 1.0) * 100.0;
    std::printf("%4u-bit (k=%u)  sep %8.2f us  fios %8.2f us   Δ %+6.1f%%\n",
                bits, k, sep_ns / 1e3, fios_ns / 1e3, delta);
}

int main() {
    std::printf("── Layer 1: kernel-level ─────────────────────────────\n");
    for (uint32_t k : {1u, 2u, 3u, 4u, 5u, 6u, 7u}) bench_kernel(k);

    std::printf("\n── Layer 2: pow_mod cadence (5 sqr + 1 mul, rotating) ──\n");
    for (uint32_t k : {1u, 2u, 3u, 4u, 5u, 6u, 7u}) bench_cadence(k);

    std::printf("\n── Layer 3: end-to-end pow_mod ──────────────────────\n");
    for (uint32_t bits : {64u, 128u, 192u, 256u, 320u, 384u, 448u}) bench_e2e(bits);

    return 0;
}
