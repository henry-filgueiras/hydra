// hydra_test.cpp — lightweight correctness tests (no test framework dependency)
//
// Compile:  c++ -std=c++20 -O2 -fsanitize=address,undefined hydra_test.cpp -o hydra_test
// Run:      ./hydra_test
//
// Every test prints PASS or FAIL.  The process exits 0 iff all tests passed.

#include "hydra.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using hydra::Hydra;
using hydra::LargeRep;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg)                                              \
    do {                                                               \
        if (cond) { ++g_pass; }                                        \
        else {                                                         \
            ++g_fail;                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                   \
                         __FILE__, __LINE__, msg);                     \
        }                                                              \
    } while (0)

// ── helpers ──────────────────────────────────────────────────────────

static Hydra make_large(uint32_t n_limbs, uint64_t seed = 0xDEAD'BEEFull) {
    std::vector<uint64_t> limbs(n_limbs);
    uint64_t v = seed;
    for (auto& l : limbs) {
        v ^= v << 13; v ^= v >> 7; v ^= v << 17;
        l = v | 1u;
    }
    limbs.back() |= (1ull << 63);   // keep MSL non-zero → stays Large
    return Hydra::from_limbs(limbs.data(), n_limbs);
}

// ── basic arithmetic identity tests ──────────────────────────────────

static void test_small_add() {
    Hydra a{42u}, b{58u};
    Hydra c = a + b;
    CHECK(c.to_string() == "100", "42 + 58 == 100");
}

static void test_small_add_inplace() {
    Hydra a{42u};
    a += Hydra{58u};
    CHECK(a.to_string() == "100", "42 += 58 == 100");
}

// ── in-place += for Large values (capacity reuse fast path) ──────────

static void test_large_add_inplace_basic() {
    // Two 8-limb values; after add, result may be 8 or 9 limbs.
    Hydra a = make_large(8, 0x1111);
    Hydra b = make_large(8, 0x2222);

    // Compute expected via operator+.
    Hydra expected = a + b;

    // Now do it in-place.
    a += b;
    CHECK(a == expected, "large += large matches operator+");
}

static void test_large_add_inplace_carry_grows() {
    // All-ones limbs: adding 1 propagates carry through all limbs.
    const uint32_t n = 6;
    std::vector<uint64_t> ones(n, UINT64_MAX);
    ones.back() |= (1ull << 63);
    Hydra a = Hydra::from_limbs(ones.data(), n);
    Hydra b{1u};

    Hydra expected = a + b;
    a += b;
    CHECK(a == expected, "large += small (carry propagation) matches operator+");
}

static void test_large_add_inplace_self() {
    // a += a  (self-addition).
    Hydra a = make_large(8, 0xAAAA);
    Hydra doubled = a + a;
    a += a;
    CHECK(a == doubled, "a += a matches a + a");
}

static void test_large_add_inplace_different_sizes() {
    // Accumulator has more limbs than addend.
    Hydra a = make_large(16, 0xBBBB);
    Hydra b = make_large(4,  0xCCCC);
    Hydra expected = a + b;
    a += b;
    CHECK(a == expected, "large(16) += large(4) matches operator+");
}

static void test_large_add_inplace_rhs_bigger() {
    // Addend has more limbs than accumulator.
    Hydra a = make_large(4,  0xDDDD);
    Hydra b = make_large(16, 0xEEEE);
    Hydra expected = a + b;
    a += b;
    CHECK(a == expected, "large(4) += large(16) matches operator+");
}

static void test_large_add_inplace_chained() {
    // Chained: a += b; a += b; a += b;  should equal a + 3*b.
    Hydra a = make_large(8, 0x1234);
    Hydra b = make_large(8, 0x5678);
    Hydra expected = a + b + b + b;
    a += b;
    a += b;
    a += b;
    CHECK(a == expected, "chained a += b x3 matches a + b + b + b");
}

static void test_large_add_inplace_capacity_fallback() {
    // Force the fallback path: create a Large with minimal capacity
    // and add a value that exceeds it.
    //
    // from_limbs allocates exactly count limbs of capacity.
    // Adding two n-limb values can produce n+1 limbs.
    // If both MSLs have bit 63 set, carry is guaranteed → n+1 limbs.
    const uint32_t n = 4;
    std::vector<uint64_t> limbs_a(n, 0x8000'0000'0000'0001ull);
    std::vector<uint64_t> limbs_b(n, 0x8000'0000'0000'0001ull);
    Hydra a = Hydra::from_limbs(limbs_a.data(), n);
    Hydra b = Hydra::from_limbs(limbs_b.data(), n);

    // a's capacity is exactly 4.  Result needs 5 limbs (carry from MSL).
    // This must fall back to the allocating path but still be correct.
    Hydra expected = a + b;
    a += b;
    CHECK(a == expected, "capacity-insufficient fallback still correct");
}

static void test_large_add_inplace_normalizes_to_medium() {
    // After addition, if high limbs are zero, normalize should demote.
    // Build a 4-limb Large where only limb 0 is non-zero.
    uint64_t limbs[4] = {42, 0, 0, 0};
    // Force it into Large representation by using create directly.
    auto* rep = LargeRep::create(8);
    rep->used = 4;
    std::memcpy(rep->limbs(), limbs, 4 * sizeof(uint64_t));

    Hydra a;
    // Manually wire up as Large.
    a.meta = Hydra::make_large_meta();
    a.payload.large = rep;

    Hydra b{10u};
    a += b;
    // 42 + 10 = 52, which should normalize to Small.
    CHECK(a.to_string() == "52", "large += small normalizes to small (value=52)");
    CHECK(a.is_small(), "result demoted to Small");
}

static void test_large_add_inplace_commutativity() {
    Hydra a = make_large(8, 0xF00D);
    Hydra b = make_large(8, 0xCAFE);
    Hydra ab = a;  ab += b;
    Hydra ba = b;  ba += a;
    CHECK(ab == ba, "a += b == b += a (commutativity)");
}

static void test_large_add_inplace_associativity() {
    Hydra a = make_large(6, 0x111);
    Hydra b = make_large(6, 0x222);
    Hydra c = make_large(6, 0x333);

    // (a + b) + c
    Hydra lhs = a; lhs += b; lhs += c;
    // a + (b + c)
    Hydra bc = b; bc += c;
    Hydra rhs = a; rhs += bc;
    CHECK(lhs == rhs, "(a+b)+c == a+(b+c) (associativity)");
}

// ── multiplication kernel correctness tests ─────────────────────────

// Verify mul_3x3 via cross-check against generic mul_limbs.
static void test_mul_small_small() {
    Hydra a{UINT64_MAX}, b{UINT64_MAX};
    Hydra c = a * b;
    // (2^64-1)^2 = 2^128 - 2^65 + 1
    // = 0xFFFFFFFFFFFFFFFE_0000000000000001
    auto lv = c.limb_view();
    CHECK(lv.count == 2, "small*small → 2 limbs");
    CHECK(lv.ptr[0] == 1, "low limb of (2^64-1)^2 is 1");
    CHECK(lv.ptr[1] == 0xFFFFFFFFFFFFFFFEull, "high limb of (2^64-1)^2");
}

static void test_mul_medium_medium() {
    // 2-limb × 2-limb → up to 4-limb (exercises mul_3x3 path)
    Hydra a = Hydra::make_medium(UINT64_MAX, UINT64_MAX, 0, 2);
    Hydra b = Hydra::make_medium(UINT64_MAX, UINT64_MAX, 0, 2);
    Hydra c = a * b;

    // Cross-check: compute via generic kernel
    uint64_t la[2] = {UINT64_MAX, UINT64_MAX};
    uint64_t lb[2] = {UINT64_MAX, UINT64_MAX};
    uint64_t out[4] = {};
    hydra::detail::mul_limbs(la, 2, lb, 2, out);
    Hydra expected = Hydra::from_limbs(out, 4);

    CHECK(c == expected, "medium*medium matches generic kernel");
}

static void test_mul_3limb() {
    // Full 3-limb × 3-limb (exercises mul_3x3 with max inputs)
    Hydra a = Hydra::make_medium(0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, 0xCCCCCCCCCCCCCCCCull, 3);
    Hydra b = Hydra::make_medium(0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull, 3);
    Hydra c = a * b;

    uint64_t la[3] = {0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, 0xCCCCCCCCCCCCCCCCull};
    uint64_t lb[3] = {0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull};
    uint64_t out[6] = {};
    hydra::detail::mul_limbs(la, 3, lb, 3, out);
    Hydra expected = Hydra::from_limbs(out, 6);

    CHECK(c == expected, "3-limb*3-limb matches generic kernel");
}

static void test_mul_3x3_asymmetric() {
    // 1-limb × 3-limb (mul_3x3 with padding)
    Hydra a{0xDEADBEEFull};
    Hydra b = Hydra::make_medium(0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull, 3);
    Hydra c = a * b;

    uint64_t la[1] = {0xDEADBEEFull};
    uint64_t lb[3] = {0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull};
    uint64_t out[4] = {};
    hydra::detail::mul_limbs(la, 1, lb, 3, out);
    Hydra expected = Hydra::from_limbs(out, 4);

    CHECK(c == expected, "1-limb*3-limb matches generic kernel");
}

static void test_mul_4x4() {
    // 4-limb × 4-limb (exercises mul_4x4 kernel)
    Hydra a = make_large(4, 0xAAAA);
    Hydra b = make_large(4, 0xBBBB);
    Hydra c = a * b;

    auto la = a.limb_view();
    auto lb = b.limb_view();
    uint64_t out[8] = {};
    hydra::detail::mul_limbs(la.ptr, la.count, lb.ptr, lb.count, out);
    Hydra expected = Hydra::from_limbs(out, 8);

    CHECK(c == expected, "4x4 mul matches generic kernel");
}

static void test_mul_4x4_max_values() {
    // Worst-case carry: all-ones limbs
    uint64_t all_ones[4] = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    Hydra a = Hydra::from_limbs(all_ones, 4);
    Hydra b = Hydra::from_limbs(all_ones, 4);
    Hydra c = a * b;

    uint64_t out[8] = {};
    hydra::detail::mul_limbs(all_ones, 4, all_ones, 4, out);
    Hydra expected = Hydra::from_limbs(out, 8);

    CHECK(c == expected, "4x4 all-ones matches generic kernel");
}

static void test_mul_8x8() {
    // 8-limb × 8-limb (exercises mul_8x8 kernel)
    Hydra a = make_large(8, 0x1234);
    Hydra b = make_large(8, 0x5678);
    Hydra c = a * b;

    auto la = a.limb_view();
    auto lb = b.limb_view();
    uint64_t out[16] = {};
    hydra::detail::mul_limbs(la.ptr, la.count, lb.ptr, lb.count, out);
    Hydra expected = Hydra::from_limbs(out, 16);

    CHECK(c == expected, "8x8 mul matches generic kernel");
}

static void test_mul_8x8_max_values() {
    // Worst-case carry: all-ones limbs
    uint64_t all_ones[8];
    for (int i = 0; i < 8; ++i) all_ones[i] = UINT64_MAX;
    Hydra a = Hydra::from_limbs(all_ones, 8);
    Hydra b = Hydra::from_limbs(all_ones, 8);
    Hydra c = a * b;

    uint64_t out[16] = {};
    hydra::detail::mul_limbs(all_ones, 8, all_ones, 8, out);
    Hydra expected = Hydra::from_limbs(out, 16);

    CHECK(c == expected, "8x8 all-ones matches generic kernel");
}

static void test_mul_8x8_varied_seeds() {
    // Multiple random seeds to catch edge cases
    uint64_t seeds[] = {0xCAFE, 0xBEEF, 0xDEAD, 0xF00D, 0x1337};
    for (auto seed : seeds) {
        Hydra a = make_large(8, seed);
        Hydra b = make_large(8, seed ^ 0xFFFF);
        Hydra c = a * b;

        auto la = a.limb_view();
        auto lb = b.limb_view();
        uint64_t out[16] = {};
        hydra::detail::mul_limbs(la.ptr, la.count, lb.ptr, lb.count, out);
        Hydra expected = Hydra::from_limbs(out, 16);

        CHECK(c == expected, "8x8 varied-seed mul matches generic kernel");
    }
}

static void test_mul_commutativity() {
    // a*b == b*a for all kernel paths
    Hydra sm{42u};
    Hydra med = Hydra::make_medium(0xAAAA, 0xBBBB, 0xCCCC, 3);
    Hydra l4 = make_large(4, 0x9999);
    Hydra l8 = make_large(8, 0x7777);

    CHECK(sm * med == med * sm,  "commutativity: small*medium");
    CHECK(sm * l4  == l4 * sm,   "commutativity: small*large4");
    CHECK(med * l4 == l4 * med,  "commutativity: medium*large4");
    CHECK(l4 * l8  == l8 * l4,   "commutativity: large4*large8");
}

static void test_mul_identity_and_zero() {
    Hydra zero{0u};
    Hydra one{1u};
    Hydra val = make_large(8, 0xDEAD);

    CHECK((val * zero) == zero, "val * 0 == 0");
    CHECK((zero * val) == zero, "0 * val == 0");
    CHECK((val * one) == val,   "val * 1 == val");
    CHECK((one * val) == val,   "1 * val == val");
}

static void test_mul_generic_fallback() {
    // 5-limb × 5-limb → goes through generic kernel (not 4x4 or 8x8)
    Hydra a = make_large(5, 0xAAAA);
    Hydra b = make_large(5, 0xBBBB);
    Hydra c = a * b;

    auto la = a.limb_view();
    auto lb = b.limb_view();
    uint64_t out[10] = {};
    hydra::detail::mul_limbs(la.ptr, la.count, lb.ptr, lb.count, out);
    Hydra expected = Hydra::from_limbs(out, 10);

    CHECK(c == expected, "5x5 (generic fallback) matches");
}

// ── medium / small path regression tests ─────────────────────────────

static void test_medium_add_inplace() {
    // Overflow small to medium, then add in-place.
    Hydra a = Hydra{UINT64_MAX};
    a += Hydra{1u};  // → medium (2 limbs: [0, 1])
    CHECK(a.is_medium(), "UINT64_MAX + 1 is Medium");

    a += Hydra{5u};
    Hydra expected = Hydra{UINT64_MAX} + Hydra{1u} + Hydra{5u};
    CHECK(a == expected, "medium += small correct");
}

static void test_small_add_inplace_no_regress() {
    // Tight loop of small additions shouldn't be broken.
    Hydra acc{0u};
    for (int i = 0; i < 1000; ++i)
        acc += Hydra{1u};
    CHECK(acc.to_string() == "1000", "1000 small additions = 1000");
}

// ── bit shift tests ───────────────────────────────────────────────────

static void test_shl_zero_shift() {
    // Any value << 0 is identity.
    Hydra a{0xDEAD'BEEF'CAFE'BABEull};
    CHECK(a << 0 == a, "small << 0 == identity");

    Hydra m = Hydra::make_medium(1, 2, 3, 3);
    CHECK(m << 0 == m, "medium << 0 == identity");

    Hydra l = make_large(6, 0xABCD);
    CHECK(l << 0 == l, "large << 0 == identity");
}

static void test_shl_small_stays_small() {
    Hydra a{1u};
    Hydra r = a << 3;
    CHECK(r.is_small(), "1 << 3 stays Small");
    CHECK(r.to_u64() == 8u, "1 << 3 == 8");
}

static void test_shl_small_grows_to_medium() {
    // Shift bit 0 to position 64 → 2 limbs.
    Hydra a{1u};
    Hydra r = a << 64;
    CHECK(r.is_medium(), "1 << 64 is Medium");
    auto lv = r.limb_view();
    CHECK(lv.count == 2, "1 << 64 has 2 limbs");
    CHECK(lv.ptr[0] == 0u,  "1 << 64 low limb = 0");
    CHECK(lv.ptr[1] == 1u,  "1 << 64 high limb = 1");
}

static void test_shl_small_intra_limb_carry() {
    // UINT64_MAX << 1 should produce 2 limbs.
    Hydra a{UINT64_MAX};
    Hydra r = a << 1;
    auto lv = r.limb_view();
    CHECK(lv.count == 2, "UINT64_MAX << 1 has 2 limbs");
    CHECK(lv.ptr[0] == 0xFFFF'FFFF'FFFF'FFFEull, "UINT64_MAX << 1 low limb");
    CHECK(lv.ptr[1] == 1u,                        "UINT64_MAX << 1 high limb");
}

static void test_shl_small_cross_limb_boundary() {
    // 1 << 65 should put bit at position 65 → 2 limbs, low=0, high=2.
    Hydra a{1u};
    Hydra r = a << 65;
    CHECK(r.is_medium(), "1 << 65 is Medium");
    auto lv = r.limb_view();
    CHECK(lv.count == 2, "1 << 65 has 2 limbs");
    CHECK(lv.ptr[0] == 0u, "1 << 65 low limb = 0");
    CHECK(lv.ptr[1] == 2u, "1 << 65 high limb = 2");
}

static void test_shl_medium_to_large() {
    // 3-limb medium shifted enough to become 4+ limbs → Large.
    Hydra m = Hydra::make_medium(1, 0, 0, 1);   // value = 1 as medium
    // Shift by 192 bits → needs limb position 3 → Large.
    Hydra r = m << 192;
    CHECK(r.is_large(), "1 << 192 is Large");
    auto lv = r.limb_view();
    CHECK(lv.count == 4, "1 << 192 has 4 limbs");
    CHECK(lv.ptr[3] == 1u, "1 << 192 highest limb = 1");
    CHECK(lv.ptr[0] == 0u && lv.ptr[1] == 0u && lv.ptr[2] == 0u,
          "1 << 192 low limbs = 0");
}

static void test_shl_large_multi_limb() {
    // A 4-limb value shifted by 64 → 5 limbs.
    Hydra a = make_large(4, 0x1111);
    Hydra r = a << 64;
    auto lv_a = a.limb_view();
    auto lv_r = r.limb_view();
    // Each limb should be shifted up one position.
    CHECK(lv_r.ptr[0] == 0u,          "large << 64: limb[0] = 0");
    CHECK(lv_r.ptr[1] == lv_a.ptr[0], "large << 64: limb[1] = a[0]");
    CHECK(lv_r.ptr[2] == lv_a.ptr[1], "large << 64: limb[2] = a[1]");
    CHECK(lv_r.ptr[3] == lv_a.ptr[2], "large << 64: limb[3] = a[2]");
}

static void test_shl_cross_check_multiply() {
    // For small n, a << n == a * 2^n.  Verify with multiply.
    for (unsigned n : {1u, 7u, 13u, 32u, 63u}) {
        Hydra a{0xDEAD'BEEFull};
        Hydra shifted  = a << n;

        // Build 2^n as Hydra.
        Hydra two_n{1u};
        for (unsigned i = 0; i < n; ++i) two_n = two_n + two_n;
        Hydra product = a * two_n;

        CHECK(shifted == product, "a << n == a * 2^n");
    }
}

// ── right-shift tests ─────────────────────────────────────────────────

static void test_shr_zero_shift() {
    Hydra a{0xCAFEull};
    CHECK(a >> 0 == a, "small >> 0 == identity");

    Hydra m = Hydra::make_medium(1, 2, 3, 3);
    CHECK(m >> 0 == m, "medium >> 0 == identity");

    Hydra l = make_large(6, 0x1234);
    CHECK(l >> 0 == l, "large >> 0 == identity");
}

static void test_shr_small() {
    Hydra a{256u};
    CHECK((a >> 1).to_u64() == 128u, "256 >> 1 == 128");
    CHECK((a >> 8).to_u64() == 1u,   "256 >> 8 == 1");
    CHECK((a >> 9).to_u64() == 0u,   "256 >> 9 == 0");
    CHECK((a >> 64).to_u64() == 0u,  "256 >> 64 == 0");
}

static void test_shr_large_shift_clears() {
    // Shifting out all bits returns zero.
    Hydra a = make_large(4, 0xBEEF);
    CHECK((a >> 256) == Hydra{}, "large >> 256 == 0 (4-limb value)");
    CHECK((a >> 300) == Hydra{}, "large >> 300 == 0 (beyond all bits)");
}

static void test_shr_medium_demotes() {
    // 2-limb medium shifted by 64 should demote to Small.
    Hydra m = Hydra::make_medium(0xDEADull, 0x0000'0000'0000'0005ull, 0, 2);
    // Value = 5 * 2^64 + 0xDEAD
    Hydra r = m >> 64;
    CHECK(r.is_small(), "medium >> 64 demotes to Small");
    CHECK(r.to_u64() == 5u, "medium >> 64 value == 5");
}

static void test_shr_intra_limb_stitch() {
    // Build a 2-limb value [0xFFFF_FFFF_0000_0000, 0x0000_0000_FFFF_FFFF]
    // and shift right by 32 — expect exactly [0xFFFF_FFFF_FFFF_FFFF].
    uint64_t limbs[2] = {0xFFFF'FFFF'0000'0000ull, 0x0000'0000'FFFF'FFFFull};
    Hydra a = Hydra::from_limbs(limbs, 2);
    Hydra r = a >> 32;
    CHECK(r.is_small() || r.is_medium(), "shr cross-limb stitch fits in 1 limb");
    auto lv = r.limb_view();
    CHECK(lv.count == 1, "shr stitch: 1 significant limb");
    CHECK(lv.ptr[0] == 0xFFFF'FFFF'FFFF'FFFFull, "shr cross-limb stitch value");
}

static void test_shr_large_partial() {
    // 8-limb value >> 32 reduces to 8 limbs (or 7 if MSL fits).
    Hydra a = make_large(8, 0x9999);
    Hydra r = a >> 32;
    auto lv_a = a.limb_view();
    auto lv_r = r.limb_view();
    // r should be non-zero and have fewer or equal limbs.
    CHECK(lv_r.count <= lv_a.count, "large >> 32: not more limbs than before");
    CHECK(lv_r.count > 0,           "large >> 32: non-zero");
    // Verify round-trip: (a >> 32) << 32 == a with low 32 bits cleared.
    Hydra trip = r << 32;
    // The low 32 bits of a were discarded; compare with a & ~mask32.
    // Build expected by zeroing low 32 bits of a[0].
    std::vector<uint64_t> expected_limbs(lv_a.count);
    for (uint32_t i = 0; i < lv_a.count; ++i)
        expected_limbs[i] = lv_a.ptr[i];
    expected_limbs[0] &= ~(uint64_t)0xFFFF'FFFFull;
    Hydra expected = Hydra::from_limbs(expected_limbs.data(), lv_a.count);
    CHECK(trip == expected, "large: (a >> 32) << 32 matches a with low bits cleared");
}

static void test_shl_shr_roundtrip() {
    // For shift n, (a << n) >> n == a (when no bits lost from the top).
    // Use a value with top bit clear to ensure no information is lost.
    Hydra a{0x0FFF'FFFF'FFFF'FFFFull};
    for (unsigned n : {1u, 3u, 7u, 16u, 32u, 60u}) {
        CHECK((a << n) >> n == a, "small: (a << n) >> n roundtrip");
    }
    // 2-limb medium, top bits clear to prevent MSL overflow.
    uint64_t med_limbs[2] = {0xAAAA'BBBB'CCCC'DDDDull, 0x0FFF'FFFF'FFFF'FFFFull};
    Hydra m = Hydra::from_limbs(med_limbs, 2);
    for (unsigned n : {1u, 13u, 32u, 63u}) {
        CHECK((m << n) >> n == m, "medium: (m << n) >> n roundtrip");
    }
}

// ── div_u64 tests ─────────────────────────────────────────────────────

static void test_div_u64_zero_numerator() {
    CHECK(Hydra{}.div_u64(7) == Hydra{}, "0 / 7 = 0");
}

static void test_div_u64_small_exact() {
    Hydra a{100u};
    CHECK(a.div_u64(10).to_u64() == 10u, "100 / 10 = 10");
    CHECK(a.div_u64(1).to_u64()  == 100u, "100 / 1 = 100");
    CHECK(a.div_u64(100).to_u64() == 1u,  "100 / 100 = 1");
}

static void test_div_u64_small_with_remainder() {
    Hydra a{17u};
    CHECK(a.div_u64(5).to_u64() == 3u, "17 / 5 = 3");
    CHECK(a.div_u64(6).to_u64() == 2u, "17 / 6 = 2");
    CHECK(a.div_u64(17).to_u64() == 1u, "17 / 17 = 1");
    CHECK(a.div_u64(18).to_u64() == 0u, "17 / 18 = 0");
}

static void test_div_u64_max_small() {
    Hydra a{UINT64_MAX};
    CHECK(a.div_u64(1).to_u64() == UINT64_MAX, "UINT64_MAX / 1 = UINT64_MAX");
    CHECK(a.div_u64(UINT64_MAX).to_u64() == 1u, "UINT64_MAX / UINT64_MAX = 1");
    CHECK(a.div_u64(2).to_u64() == (UINT64_MAX / 2), "UINT64_MAX / 2");
}

static void test_div_u64_medium() {
    // 2^64 / 2 = 2^63.  Build 2^64 as a 2-limb medium.
    uint64_t limbs[2] = {0, 1};   // value = 1 * 2^64
    Hydra a = Hydra::from_limbs(limbs, 2);
    Hydra q = a.div_u64(2);
    // 2^64 / 2 = 2^63 = 0x8000_0000_0000_0000.
    CHECK(q.is_small(), "2^64 / 2 demotes to Small");
    CHECK(q.to_u64() == (1ull << 63), "2^64 / 2 = 2^63");
}

static void test_div_u64_large() {
    // 8-limb value: quotient should be correct and normalized.
    Hydra a = make_large(8, 0xDEAD);
    uint64_t d = 1000000007ull;
    Hydra q = a.div_u64(d);
    uint64_t r = a.mod_u64(d);
    // Verify: q * d + r == a.
    Hydra reconstructed = q * Hydra{d} + Hydra{r};
    CHECK(reconstructed == a, "large: q * d + r == a");
}

static void test_div_u64_by_large_divisor() {
    // Divisor larger than the top limb value — quotient should be small.
    uint64_t limbs[4] = {0xAAAAull, 0xBBBBull, 0xCCCCull, 0x1ull};
    Hydra a = Hydra::from_limbs(limbs, 4);
    Hydra q = a.div_u64(UINT64_MAX);
    uint64_t r = a.mod_u64(UINT64_MAX);
    Hydra reconstructed = q * Hydra{UINT64_MAX} + Hydra{r};
    CHECK(reconstructed == a, "div by UINT64_MAX: q * d + r == a");
}

static void test_div_u64_throws_on_zero() {
    bool threw = false;
    try { (void)Hydra{42u}.div_u64(0); }
    catch (const std::domain_error&) { threw = true; }
    CHECK(threw, "div_u64(0) throws domain_error");
}

static void test_div_u64_power_of_two_matches_shift() {
    // a.div_u64(2^n) should equal a >> n (modulo rounding direction).
    Hydra a = make_large(8, 0x5555);
    for (unsigned n : {1u, 7u, 13u, 32u, 64u}) {
        uint64_t d = 0;
        // Build d = 2^n via shift on plain integer (avoid Hydra for simplicity).
        if (n < 64) d = 1ull << n;
        else continue;  // 2^64 doesn't fit in uint64_t

        Hydra by_div   = a.div_u64(d);
        Hydra by_shift = a >> n;
        CHECK(by_div == by_shift, "div_u64(2^n) == a >> n");
    }
}

// ── mod_u64 tests ─────────────────────────────────────────────────────

static void test_mod_u64_zero_numerator() {
    CHECK(Hydra{}.mod_u64(7) == 0u, "0 % 7 = 0");
}

static void test_mod_u64_small() {
    CHECK(Hydra{17u}.mod_u64(5) == 2u,  "17 % 5 = 2");
    CHECK(Hydra{17u}.mod_u64(17) == 0u, "17 % 17 = 0");
    CHECK(Hydra{17u}.mod_u64(18) == 17u,"17 % 18 = 17");
    CHECK(Hydra{17u}.mod_u64(1) == 0u,  "17 % 1 = 0");
}

static void test_mod_u64_medium() {
    // (5 * 2^64 + 7) % 5 = 7 % 5 = 2.
    uint64_t limbs[2] = {7, 5};
    Hydra a = Hydra::from_limbs(limbs, 2);
    CHECK(a.mod_u64(5) == 2u, "(5*2^64+7) % 5 = 2");
    CHECK(a.mod_u64(1) == 0u, "anything % 1 = 0");
}

static void test_mod_u64_large() {
    Hydra a = make_large(8, 0xF00D);
    uint64_t d = 999999937ull;  // large prime
    uint64_t r = a.mod_u64(d);
    // Cross-check via div: a == q * d + r.
    Hydra q = a.div_u64(d);
    Hydra reconstructed = q * Hydra{d} + Hydra{r};
    CHECK(reconstructed == a, "large: mod check q*d+r == a");
}

static void test_mod_u64_throws_on_zero() {
    bool threw = false;
    try { (void)Hydra{42u}.mod_u64(0); }
    catch (const std::domain_error&) { threw = true; }
    CHECK(threw, "mod_u64(0) throws domain_error");
}

static void test_divmod_identity_exhaustive() {
    // For many (value, divisor) pairs: q * d + r == value.
    std::vector<Hydra> values = {
        Hydra{0u},
        Hydra{1u},
        Hydra{UINT64_MAX},
        make_large(2, 0x1111),
        make_large(4, 0xAAAA),
        make_large(8, 0x5678),
    };
    std::vector<uint64_t> divisors = {1, 2, 3, 7, 10, 1000000007ull, UINT64_MAX};
    for (const auto& v : values) {
        for (auto d : divisors) {
            Hydra q = v.div_u64(d);
            uint64_t r = v.mod_u64(d);
            CHECK(r < d, "remainder < divisor");
            Hydra reconstructed = q * Hydra{d} + Hydra{r};
            CHECK(reconstructed == v, "q * d + r == v (exhaustive)");
        }
    }
}

// ── normalization / demotion from shift/div ───────────────────────────

static void test_shift_result_normalized() {
    // (a << 1) >> 1 must equal a exactly in bignum arithmetic.
    //
    // Proof: a << 1 computes 2*a (never overflows bignum); then
    // >> 1 computes floor(2*a / 2) = a because 2*a is always even.
    // No bits are lost — the intermediate form simply grows by one limb.
    Hydra a = make_large(4, 0xC0C0);
    Hydra r = (a << 1) >> 1;
    CHECK(r == a, "large: (a << 1) >> 1 == a exactly");

    // Verify that a right-shift that actually discards bits is correct.
    // (a << 1) has bit 0 == 0; then (a << 1) | 1 has bit 0 == 1.
    // Shifting that right by 1 should give (a | (1 >> 1)) = a.
    // Alternatively: verify bit-loss for an explicit odd value.
    Hydra odd{7u};
    CHECK((odd >> 1).to_u64() == 3u, "7 >> 1 == 3 (LSB discarded)");
    CHECK((odd << 1) >> 1 == odd,    "7: (v<<1)>>1 == v (no loss in bignum shift)");
}

static void test_div_u64_result_normalized_to_small() {
    // A 4-limb Large with a very small quotient should demote.
    uint64_t limbs[4] = {0xFFFF'FFFF'FFFF'FFFFull, 0, 0, 0};
    Hydra a = Hydra::from_limbs(limbs, 4);
    // a = UINT64_MAX; div_u64 by 1 → UINT64_MAX (small).
    Hydra q = a.div_u64(1);
    CHECK(q.is_small(), "div_u64 quotient demotes to Small when it fits");
    CHECK(q.to_u64() == UINT64_MAX, "div_u64 quotient value correct");
}

// ── entry point ──────────────────────────────────────────────────────

int main() {
    test_small_add();
    test_small_add_inplace();
    test_large_add_inplace_basic();
    test_large_add_inplace_carry_grows();
    test_large_add_inplace_self();
    test_large_add_inplace_different_sizes();
    test_large_add_inplace_rhs_bigger();
    test_large_add_inplace_chained();
    test_large_add_inplace_capacity_fallback();
    test_large_add_inplace_normalizes_to_medium();
    test_large_add_inplace_commutativity();
    test_large_add_inplace_associativity();
    test_medium_add_inplace();
    test_small_add_inplace_no_regress();

    // Multiplication kernel tests
    test_mul_small_small();
    test_mul_medium_medium();
    test_mul_3limb();
    test_mul_3x3_asymmetric();
    test_mul_4x4();
    test_mul_4x4_max_values();
    test_mul_8x8();
    test_mul_8x8_max_values();
    test_mul_8x8_varied_seeds();
    test_mul_commutativity();
    test_mul_identity_and_zero();
    test_mul_generic_fallback();

    // Bit-shift tests
    test_shl_zero_shift();
    test_shl_small_stays_small();
    test_shl_small_grows_to_medium();
    test_shl_small_intra_limb_carry();
    test_shl_small_cross_limb_boundary();
    test_shl_medium_to_large();
    test_shl_large_multi_limb();
    test_shl_cross_check_multiply();

    test_shr_zero_shift();
    test_shr_small();
    test_shr_large_shift_clears();
    test_shr_medium_demotes();
    test_shr_intra_limb_stitch();
    test_shr_large_partial();
    test_shl_shr_roundtrip();

    // div_u64 / mod_u64 tests
    test_div_u64_zero_numerator();
    test_div_u64_small_exact();
    test_div_u64_small_with_remainder();
    test_div_u64_max_small();
    test_div_u64_medium();
    test_div_u64_large();
    test_div_u64_by_large_divisor();
    test_div_u64_throws_on_zero();
    test_div_u64_power_of_two_matches_shift();

    test_mod_u64_zero_numerator();
    test_mod_u64_small();
    test_mod_u64_medium();
    test_mod_u64_large();
    test_mod_u64_throws_on_zero();
    test_divmod_identity_exhaustive();

    // Normalization / demotion
    test_shift_result_normalized();
    test_div_u64_result_normalized_to_small();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
