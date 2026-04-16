// bench/fused_ab_test.cpp — A/B test: fused CIOS vs separate mul+REDC
//
// Runs both paths back-to-back on the same operands to minimize
// environmental noise and give a clean relative comparison.

#include "../hydra.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <algorithm>

using namespace hydra;
using clk = std::chrono::steady_clock;

static Hydra make_hydra(uint32_t n_bits, uint64_t seed) {
    const uint32_t n_limbs = (n_bits + 63) / 64;
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> limbs(n_limbs);
    for (auto& l : limbs) l = rng();
    const uint32_t top_bits = n_bits % 64;
    if (top_bits != 0) limbs.back() &= (1ull << top_bits) - 1;
    if (top_bits != 0) limbs.back() |= (1ull << (top_bits - 1));
    else limbs.back() |= (1ull << 63);
    limbs[0] |= 1u;
    return Hydra::from_limbs(limbs.data(), static_cast<uint32_t>(limbs.size()));
}

// ── The separate-path version (old code, copied) ──────────────────────

[[nodiscard]] static Hydra pow_mod_separate(
    Hydra base, Hydra exp, const Hydra& mod)
{
    auto mod_lv = mod.limb_view();
    uint32_t k = mod_lv.count;
    MontgomeryContext ctx = MontgomeryContext::build(mod_lv.ptr, k);
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

    {
        uint64_t a_padded[MAX_K];
        std::memset(a_padded, 0, k * sizeof(uint64_t));
        uint32_t copy_count = (base_lv.count < k) ? base_lv.count : k;
        std::memcpy(a_padded, base_lv.ptr, copy_count * sizeof(uint64_t));
        // Uses SEPARATE mul + REDC
        detail::montgomery_mul(a_padded, ctx.r_sq.data(), k, ctx.mod_limbs.data(), ctx.n0inv, table[0], work_buf);
    }

    uint64_t base_sq[MAX_K];
    detail::montgomery_sqr(table[0], k, ctx.mod_limbs.data(), ctx.n0inv, base_sq, work_buf);

    for (uint32_t i = 1; i < TABLE_SIZE; ++i) {
        detail::montgomery_mul(table[i - 1], base_sq, k, ctx.mod_limbs.data(), ctx.n0inv, table[i], work_buf);
    }

    {
        uint64_t one_padded[MAX_K];
        std::memset(one_padded, 0, k * sizeof(uint64_t));
        one_padded[0] = 1;
        detail::montgomery_mul(one_padded, ctx.r_sq.data(), k, ctx.mod_limbs.data(), ctx.n0inv, result_mont_buf, work_buf);
    }

    auto exp_lv = exp.limb_view();
    uint32_t exp_limb_count = exp_lv.count;
    if (exp_limb_count == 0) {
        uint64_t result_limbs_buf[MAX_K];
        std::memset(result_limbs_buf, 0, k * sizeof(uint64_t));
        ctx.from_montgomery(result_mont_buf, result_limbs_buf, work_buf);
        uint32_t used = k;
        while (used > 0 && result_limbs_buf[used - 1] == 0) --used;
        if (used == 0) return Hydra{0u};
        return Hydra::from_limbs(result_limbs_buf, used);
    }

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
            detail::montgomery_sqr(result_mont_buf, k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
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
                detail::montgomery_sqr(result_mont_buf, k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
                std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            }
            uint32_t table_idx = (wval - 1) / 2;
            detail::montgomery_mul(result_mont_buf, table[table_idx], k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
            std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            for (int i = 0; i < trailing_zeros; ++i) {
                detail::montgomery_sqr(result_mont_buf, k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
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
    if (used == 0) return Hydra{0u};
    return Hydra::from_limbs(result_limbs_buf, used);
}

double median_time(auto&& fn, int warmup = 3, int samples = 40) {
    for (int i = 0; i < warmup; ++i) {
        auto r = fn();
        asm volatile("" : : "r"(&r) : "memory");
    }
    std::vector<double> times(samples);
    for (int i = 0; i < samples; ++i) {
        auto t0 = clk::now();
        auto r = fn();
        auto t1 = clk::now();
        asm volatile("" : : "r"(&r) : "memory");
        times[i] = std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
    std::sort(times.begin(), times.end());
    return times[samples / 2];
}

int main() {
    uint32_t widths[] = {256, 512, 1024, 2048, 4096};

    printf("%-8s  %12s  %12s  %8s\n", "Width", "Separate", "Fused", "Delta");
    printf("%-8s  %12s  %12s  %8s\n", "-----", "--------", "-----", "-----");

    for (uint32_t w : widths) {
        Hydra base = make_hydra(w, 42);
        Hydra exp  = make_hydra(w, 99);
        Hydra mod  = make_hydra(w, 7);

        // Correctness check
        Hydra r_sep = pow_mod_separate(base, exp, mod);
        Hydra r_fused = pow_mod(base, exp, mod);  // current = fused
        if (r_sep != r_fused) {
            printf("FAIL: results differ at %u bits!\n", w);
            return 1;
        }

        double t_sep   = median_time([&]{ return pow_mod_separate(base, exp, mod); });
        double t_fused = median_time([&]{ return pow_mod(base, exp, mod); });

        double pct = (t_fused - t_sep) / t_sep * 100.0;

        auto fmt = [](double us, char* buf, size_t sz) {
            if (us >= 1000) snprintf(buf, sz, "%8.2f ms", us / 1000.0);
            else snprintf(buf, sz, "%8.2f us", us);
        };

        char s1[32], s2[32];
        fmt(t_sep, s1, sizeof(s1));
        fmt(t_fused, s2, sizeof(s2));
        printf("%5u     %12s  %12s  %+7.1f%%\n", w, s1, s2, pct);
    }

    return 0;
}
