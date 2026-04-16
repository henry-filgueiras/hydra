// bench/attribution_pass.cpp — Attribution pass for previous optimization round
//
// Isolates the contribution of three interventions:
//   1. Dedicated Montgomery squaring (vs generic mul(a,a))
//   2. 4-bit sliding window exponentiation (vs binary square-and-multiply)
//   3. Stack scratch buffers (vs heap allocation)
//
// Method: we call pow_mod_montgomery with surgical variants that revert
// one intervention at a time, measuring the delta.
//
// Since the interventions are baked into hydra.hpp, we create local
// variants here that copy the relevant code with specific features reverted.

#include "../hydra.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace hydra;
using clk = std::chrono::steady_clock;

// ─────────────────────────────────────────────────────────────────────────
// Operand generation (same as bench_pow_mod.cpp)
// ─────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────
// Variant A: Revert squaring — use montgomery_mul(a,a) instead of montgomery_sqr
// ─────────────────────────────────────────────────────────────────────────

static void montgomery_sqr_via_mul(
    const uint64_t* a, uint32_t k,
    const uint64_t* mod, uint64_t n0inv,
    uint64_t* out, uint64_t* work) noexcept
{
    detail::montgomery_mul(a, a, k, mod, n0inv, out, work);
}

[[nodiscard]] static Hydra pow_mod_no_dedicated_sqr(
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
        detail::montgomery_mul(a_padded, ctx.r_sq.data(), k, ctx.mod_limbs.data(), ctx.n0inv, table[0], work_buf);
    }

    uint64_t base_sq[MAX_K];
    // KEY CHANGE: use mul(a,a) instead of dedicated sqr
    montgomery_sqr_via_mul(table[0], k, ctx.mod_limbs.data(), ctx.n0inv, base_sq, work_buf);

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
            // KEY CHANGE: use mul(a,a) instead of dedicated sqr
            montgomery_sqr_via_mul(result_mont_buf, k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
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
                montgomery_sqr_via_mul(result_mont_buf, k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
                std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            }
            uint32_t table_idx = (wval - 1) / 2;
            detail::montgomery_mul(result_mont_buf, table[table_idx], k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
            std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            for (int i = 0; i < trailing_zeros; ++i) {
                montgomery_sqr_via_mul(result_mont_buf, k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
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

// ─────────────────────────────────────────────────────────────────────────
// Variant B: Revert sliding window — use binary square-and-multiply
// ─────────────────────────────────────────────────────────────────────────

[[nodiscard]] static Hydra pow_mod_binary_exp(
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
    uint64_t base_mont_buf[MAX_K];

    std::memset(work_buf, 0, (2 * k + 1) * sizeof(uint64_t));
    std::memset(temp_buf, 0, k * sizeof(uint64_t));
    std::memset(result_mont_buf, 0, k * sizeof(uint64_t));

    // Convert base to Montgomery form
    {
        uint64_t a_padded[MAX_K];
        std::memset(a_padded, 0, k * sizeof(uint64_t));
        uint32_t copy_count = (base_lv.count < k) ? base_lv.count : k;
        std::memcpy(a_padded, base_lv.ptr, copy_count * sizeof(uint64_t));
        detail::montgomery_mul(a_padded, ctx.r_sq.data(), k, ctx.mod_limbs.data(), ctx.n0inv, base_mont_buf, work_buf);
    }

    // result = 1 in Montgomery form
    {
        uint64_t one_padded[MAX_K];
        std::memset(one_padded, 0, k * sizeof(uint64_t));
        one_padded[0] = 1;
        detail::montgomery_mul(one_padded, ctx.r_sq.data(), k, ctx.mod_limbs.data(), ctx.n0inv, result_mont_buf, work_buf);
    }

    // Binary square-and-multiply (LSB to MSB)
    auto exp_lv = exp.limb_view();
    uint32_t exp_limb_count = exp_lv.count;

    for (uint32_t li = 0; li < exp_limb_count; ++li) {
        uint64_t limb = exp_lv.ptr[li];
        uint32_t nbits = (li == exp_limb_count - 1) ? (64 - __builtin_clzll(limb)) : 64;
        for (uint32_t bi = 0; bi < nbits; ++bi) {
            if ((limb >> bi) & 1u) {
                // result *= base
                detail::montgomery_mul(result_mont_buf, base_mont_buf, k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
                std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            }
            // base *= base (square)
            detail::montgomery_sqr(base_mont_buf, k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
            std::memcpy(base_mont_buf, temp_buf, k * sizeof(uint64_t));
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

// ─────────────────────────────────────────────────────────────────────────
// Variant C: Revert stack scratch — use heap allocation
// ─────────────────────────────────────────────────────────────────────────

[[nodiscard]] static Hydra pow_mod_heap_scratch(
    Hydra base, Hydra exp, const Hydra& mod)
{
    auto mod_lv = mod.limb_view();
    uint32_t k = mod_lv.count;
    MontgomeryContext ctx = MontgomeryContext::build(mod_lv.ptr, k);
    ctx.compute_r_sq();
    base = base % mod;
    if (base.is_negative()) base = base + mod;
    auto base_lv = base.limb_view();

    // HEAP scratch instead of stack
    std::vector<uint64_t> work_vec(2 * k + 1, 0);
    std::vector<uint64_t> temp_vec(k, 0);
    std::vector<uint64_t> result_mont_vec(k, 0);
    uint64_t* work_buf = work_vec.data();
    uint64_t* temp_buf = temp_vec.data();
    uint64_t* result_mont_buf = result_mont_vec.data();

    constexpr uint32_t WINDOW = 4;
    constexpr uint32_t TABLE_SIZE = 1u << (WINDOW - 1);
    constexpr uint32_t MAX_K = 64;
    // Table on heap
    std::vector<std::vector<uint64_t>> table(TABLE_SIZE, std::vector<uint64_t>(k, 0));

    {
        std::vector<uint64_t> a_padded(k, 0);
        uint32_t copy_count = (base_lv.count < k) ? base_lv.count : k;
        std::memcpy(a_padded.data(), base_lv.ptr, copy_count * sizeof(uint64_t));
        detail::montgomery_mul(a_padded.data(), ctx.r_sq.data(), k, ctx.mod_limbs.data(), ctx.n0inv, table[0].data(), work_buf);
    }

    std::vector<uint64_t> base_sq(k, 0);
    detail::montgomery_sqr(table[0].data(), k, ctx.mod_limbs.data(), ctx.n0inv, base_sq.data(), work_buf);

    for (uint32_t i = 1; i < TABLE_SIZE; ++i) {
        detail::montgomery_mul(table[i - 1].data(), base_sq.data(), k, ctx.mod_limbs.data(), ctx.n0inv, table[i].data(), work_buf);
    }

    {
        std::vector<uint64_t> one_padded(k, 0);
        one_padded[0] = 1;
        detail::montgomery_mul(one_padded.data(), ctx.r_sq.data(), k, ctx.mod_limbs.data(), ctx.n0inv, result_mont_buf, work_buf);
    }

    auto exp_lv = exp.limb_view();
    uint32_t exp_limb_count = exp_lv.count;
    if (exp_limb_count == 0) {
        std::vector<uint64_t> result_limbs(k, 0);
        ctx.from_montgomery(result_mont_buf, result_limbs.data(), work_buf);
        uint32_t used = k;
        while (used > 0 && result_limbs[used - 1] == 0) --used;
        if (used == 0) return Hydra{0u};
        return Hydra::from_limbs(result_limbs.data(), used);
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
            detail::montgomery_mul(result_mont_buf, table[table_idx].data(), k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
            std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            for (int i = 0; i < trailing_zeros; ++i) {
                detail::montgomery_sqr(result_mont_buf, k, ctx.mod_limbs.data(), ctx.n0inv, temp_buf, work_buf);
                std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            }
            bit_pos -= (window_len + trailing_zeros);
        }
    }

    std::vector<uint64_t> result_limbs(k, 0);
    ctx.from_montgomery(result_mont_buf, result_limbs.data(), work_buf);
    uint32_t used = k;
    while (used > 0 && result_limbs[used - 1] == 0) --used;
    if (used == 0) return Hydra{0u};
    return Hydra::from_limbs(result_limbs.data(), used);
}

// ─────────────────────────────────────────────────────────────────────────
// Timing harness
// ─────────────────────────────────────────────────────────────────────────

struct TimingResult {
    double median_us;
};

template <typename F>
TimingResult measure(F&& fn, int warmup = 3, int samples = 30) {
    for (int i = 0; i < warmup; ++i) fn();

    std::vector<double> times(samples);
    for (int i = 0; i < samples; ++i) {
        auto t0 = clk::now();
        auto r = fn();
        auto t1 = clk::now();
        asm volatile("" : : "r"(&r) : "memory");
        times[i] = std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
    std::sort(times.begin(), times.end());
    return {times[samples / 2]};
}

int main() {
    uint32_t widths[] = {1024, 2048, 4096};

    printf("%-8s  %12s  %12s  %12s  %12s\n",
           "Width", "Current", "No-DedSqr", "Binary-Exp", "Heap-Scratch");
    printf("%-8s  %12s  %12s  %12s  %12s\n",
           "-----", "-------", "---------", "----------", "------------");

    for (uint32_t w : widths) {
        Hydra base = make_hydra(w, 42);
        Hydra exp  = make_hydra(w, 99);
        Hydra mod  = make_hydra(w, 7);

        // Correctness cross-check first
        Hydra ref = pow_mod(base, exp, mod);
        Hydra r1 = pow_mod_no_dedicated_sqr(base, exp, mod);
        Hydra r2 = pow_mod_binary_exp(base, exp, mod);
        Hydra r3 = pow_mod_heap_scratch(base, exp, mod);

        if (r1 != ref) { printf("FAIL: no_ded_sqr at %u bits\n", w); return 1; }
        if (r2 != ref) { printf("FAIL: binary_exp at %u bits\n", w); return 1; }
        if (r3 != ref) { printf("FAIL: heap_scratch at %u bits\n", w); return 1; }

        auto t_current    = measure([&]{ return pow_mod(base, exp, mod); });
        auto t_no_sqr     = measure([&]{ return pow_mod_no_dedicated_sqr(base, exp, mod); });
        auto t_binary     = measure([&]{ return pow_mod_binary_exp(base, exp, mod); });
        auto t_heap       = measure([&]{ return pow_mod_heap_scratch(base, exp, mod); });

        auto fmt = [](double us) {
            static char buf[32];
            if (us >= 1000) snprintf(buf, sizeof(buf), "%8.2f ms", us / 1000.0);
            else snprintf(buf, sizeof(buf), "%8.2f us", us);
            return buf;
        };

        printf("%5u     %12s  %12s  %12s  %12s\n",
               w, fmt(t_current.median_us), fmt(t_no_sqr.median_us),
               fmt(t_binary.median_us), fmt(t_heap.median_us));
    }

    printf("\nDelta vs Current (positive = regression from reverting feature):\n\n");
    printf("%-8s  %12s  %12s  %12s\n",
           "Width", "Sqr-Revert", "Window-Revert", "Heap-Revert");
    printf("%-8s  %12s  %12s  %12s\n",
           "-----", "----------", "-------------", "-----------");

    for (uint32_t w : widths) {
        Hydra base = make_hydra(w, 42);
        Hydra exp  = make_hydra(w, 99);
        Hydra mod  = make_hydra(w, 7);

        auto t_current = measure([&]{ return pow_mod(base, exp, mod); });
        auto t_no_sqr  = measure([&]{ return pow_mod_no_dedicated_sqr(base, exp, mod); });
        auto t_binary  = measure([&]{ return pow_mod_binary_exp(base, exp, mod); });
        auto t_heap    = measure([&]{ return pow_mod_heap_scratch(base, exp, mod); });

        double pct_sqr  = (t_no_sqr.median_us - t_current.median_us) / t_current.median_us * 100;
        double pct_win  = (t_binary.median_us - t_current.median_us) / t_current.median_us * 100;
        double pct_heap = (t_heap.median_us - t_current.median_us) / t_current.median_us * 100;

        printf("%5u     %+10.1f%%  %+12.1f%%  %+10.1f%%\n", w, pct_sqr, pct_win, pct_heap);
    }

    return 0;
}
