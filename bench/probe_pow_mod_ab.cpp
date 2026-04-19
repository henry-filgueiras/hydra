// bench/probe_pow_mod_ab.cpp
//
// Inline copy of pow_mod_montgomery's inner loop, parameterised over the
// backend choice (SOS vs fused CIOS).  Measures end-to-end pow_mod cost
// at the carry-stress widths to attribute the regression seen in
// bench_pow_mod between sprints.

#include "../hydra.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <algorithm>

using clk = std::chrono::steady_clock;

namespace hd = hydra::detail;

enum class Backend { SOS, Fused };

// Stripped-down copy of pow_mod_montgomery's inner exponentiation loop:
// same sliding window, same scratch layout, with the multiply/square
// backend parameterised so we can A/B fused vs SOS at the same widths.
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

    // Note: montgomery_mul_fused / montgomery_sqr_fused already memset
    // work[0..k+1] internally; no caller-side init is needed.
    auto mont_mul = [&](const uint64_t* a, const uint64_t* b, uint64_t* out) {
        if (backend == Backend::SOS) {
            hd::montgomery_mul_sos(a, b, k, ctx.mod_limbs.data(), ctx.n0inv,
                                   out, work_buf);
        } else {
            hd::montgomery_mul_fused(a, b, k, ctx.mod_limbs.data(), ctx.n0inv,
                                      out, work_buf);
        }
    };
    auto mont_sqr = [&](const uint64_t* a, uint64_t* out) {
        if (backend == Backend::SOS) {
            hd::montgomery_sqr_sos(a, k, ctx.mod_limbs.data(), ctx.n0inv,
                                   out, work_buf);
        } else {
            hd::montgomery_sqr_fused(a, k, ctx.mod_limbs.data(), ctx.n0inv,
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

static hydra::Hydra make_random_hydra(uint32_t bits, uint64_t seed) {
    uint32_t n = (bits + 63) / 64;
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> limbs(n);
    for (auto& l : limbs) l = rng();
    limbs[0] |= 1u;
    limbs.back() |= (1ull << 63);
    return hydra::Hydra::from_limbs(limbs.data(), n);
}

template <typename Fn>
static double bench_one(Fn&& fn, int reps, int warmup) {
    for (int i = 0; i < warmup; ++i) fn();
    auto t0 = clk::now();
    for (int i = 0; i < reps; ++i) fn();
    auto t1 = clk::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
}

int main() {
    std::printf("bits   k     fused_us       sos_us         delta\n");
    std::printf("----   --    --------       ------         -----\n");

    for (uint32_t bits : {256u, 512u, 1024u, 1536u, 1984u}) {
        uint32_t k = (bits + 63) / 64;
        hydra::Hydra base = make_random_hydra(bits, 0xCAFE0000ull + bits);
        hydra::Hydra exp_val = make_random_hydra(bits, 0xBEEF0000ull + bits);
        hydra::Hydra mod_val = make_random_hydra(bits, 0xDEAD0000ull + bits);

        // Cross-validate
        hydra::Hydra rs = pow_mod_with_backend(base, exp_val, mod_val,
                                               Backend::SOS);
        hydra::Hydra rf = pow_mod_with_backend(base, exp_val, mod_val,
                                               Backend::Fused);
        if (rs != rf) {
            std::fprintf(stderr,
                "MISMATCH at %u-bit: SOS != Fused\n", bits);
            return 1;
        }

        int reps = (bits <= 512) ? 2000 : (bits <= 1024) ? 500 : 100;
        int warmup = std::max(reps / 10, 5);

        double fused_ns = bench_one([&]() {
            auto r = pow_mod_with_backend(base, exp_val, mod_val,
                                          Backend::Fused);
            asm volatile("" : : "r"(r.meta) : "memory");
        }, reps, warmup);
        double sos_ns = bench_one([&]() {
            auto r = pow_mod_with_backend(base, exp_val, mod_val,
                                          Backend::SOS);
            asm volatile("" : : "r"(r.meta) : "memory");
        }, reps, warmup);

        double delta = ((sos_ns / fused_ns) - 1.0) * 100.0;
        std::printf("%-4u   %-3u   %8.2f us    %8.2f us    %+6.1f%%\n",
                    bits, k, fused_ns / 1e3, sos_ns / 1e3, delta);
    }
    return 0;
}
