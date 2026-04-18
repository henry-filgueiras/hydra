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
#include <random>
#include <sstream>
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

// ── Karatsuba prototype correctness ───────────────────────────────────
//
// Cross-check mul_karatsuba against mul_limbs at every power-of-2 width
// the prototype supports.  Also probe worst-case carry behaviour
// (all-ones operands) and the boundary between base case and first
// recursive split (n == KARATSUBA_RECURSION_BASE vs n == 2× that).

static void test_karatsuba_matches_school_random(uint32_t n, uint64_t seed) {
    auto next = [v = seed]() mutable {
        v ^= v << 13; v ^= v >> 7; v ^= v << 17;
        return v;
    };
    std::vector<uint64_t> a(n), b(n);
    for (uint32_t i = 0; i < n; ++i) a[i] = next() | 1u;
    for (uint32_t i = 0; i < n; ++i) b[i] = next() | 1u;
    a.back() |= (1ull << 63);
    b.back() |= (1ull << 63);

    std::vector<uint64_t> out_k(2 * n), out_s(2 * n);
    hydra::detail::mul_karatsuba(a.data(), b.data(), n, out_k.data());
    hydra::detail::mul_limbs(a.data(), n, b.data(), n, out_s.data());

    bool eq = std::memcmp(out_k.data(), out_s.data(), 2 * n * sizeof(uint64_t)) == 0;
    std::string msg = "karatsuba " + std::to_string(n) + "x" + std::to_string(n)
                    + " matches schoolbook (seed " + std::to_string(seed) + ")";
    CHECK(eq, msg.c_str());
}

static void test_karatsuba_4x4()   { test_karatsuba_matches_school_random(4,  0xA11CE); }
static void test_karatsuba_8x8()   { test_karatsuba_matches_school_random(8,  0xB0B);   }
static void test_karatsuba_16x16() { test_karatsuba_matches_school_random(16, 0xC0FFEE);}
static void test_karatsuba_32x32() { test_karatsuba_matches_school_random(32, 0xD00DAD); }
static void test_karatsuba_64x64() { test_karatsuba_matches_school_random(64, 0xE41C);  }

static void test_karatsuba_all_ones() {
    // Worst-case carry propagation: operands are 2^(64n) - 1.
    // (2^N - 1)^2 = 2^(2N) - 2^(N+1) + 1, which has a specific limb pattern:
    //   limbs [0..n-1] == 1, 0, 0, ..., 0
    //   limbs [n..2n-1] == -2, -1, -1, ..., -1   (i.e. 0xFFFF...FE, then all-ones)
    // But rather than hard-code the structure, just cross-check against mul_limbs.
    for (uint32_t n : {4u, 8u, 16u, 32u}) {
        std::vector<uint64_t> a(n, UINT64_MAX), b(n, UINT64_MAX);
        std::vector<uint64_t> out_k(2 * n), out_s(2 * n);
        hydra::detail::mul_karatsuba(a.data(), b.data(), n, out_k.data());
        hydra::detail::mul_limbs(a.data(), n, b.data(), n, out_s.data());
        bool eq = std::memcmp(out_k.data(), out_s.data(), 2 * n * sizeof(uint64_t)) == 0;
        std::string msg = "karatsuba all-ones " + std::to_string(n) + "x" + std::to_string(n);
        CHECK(eq, msg.c_str());
    }
}

static void test_karatsuba_recursion_boundary() {
    // n == KARATSUBA_RECURSION_BASE triggers the base case (schoolbook).
    // n == 2× that triggers exactly one level of recursion.
    // Both must produce the same result as schoolbook.
    for (uint32_t n : {
            hydra::detail::KARATSUBA_RECURSION_BASE,
            hydra::detail::KARATSUBA_RECURSION_BASE * 2 }) {
        test_karatsuba_matches_school_random(n, 0x900D1DEA);
    }
}

// ── ScratchWorkspace regression tests ──────────────────────────
//
// Every Karatsuba operation now consumes bump-allocated scratch out
// of a ScratchWorkspace.  These tests prove the bump-and-rewind
// semantics survive:
//   1. pre-reserved capacity exactly matches karatsuba_scratch_limbs
//   2. LIFO nesting: opening an inner frame and closing it leaves the
//      outer frame's limbs untouched
//   3. reusing the same workspace for many sequential multiplies does
//      not accumulate garbage across calls

static void test_scratch_capacity_bound() {
    // Compute products at every Karatsuba-recursive size and assert
    // each result matches schoolbook when the workspace is reserved
    // to exactly karatsuba_scratch_limbs(n).  If the bound were an
    // under-estimate, alloc_zeroed's assert (or ASan) would fire.
    for (uint32_t n : {
            hydra::detail::KARATSUBA_RECURSION_BASE * 2,        // 32
            hydra::detail::KARATSUBA_RECURSION_BASE * 4,        // 64
            hydra::detail::KARATSUBA_RECURSION_BASE * 8 }) {    // 128
        std::mt19937_64 rng(0xBAD5C2A7 ^ n);
        std::vector<uint64_t> a(n), b(n);
        for (auto& l : a) l = rng() | 1u;
        for (auto& l : b) l = rng() | 1u;

        hydra::detail::ScratchWorkspace ws;
        ws.reserve_limbs(hydra::detail::karatsuba_scratch_limbs(n));

        std::vector<uint64_t> out_k(2 * n), out_s(2 * n);
        hydra::detail::mul_karatsuba(a.data(), b.data(), n, out_k.data(), ws);
        hydra::detail::mul_limbs(a.data(), n, b.data(), n, out_s.data());

        bool eq = std::memcmp(out_k.data(), out_s.data(),
                               2 * n * sizeof(uint64_t)) == 0;
        std::string msg = "scratch capacity bound exact at n=" + std::to_string(n);
        CHECK(eq, msg.c_str());
    }
}

static void test_scratch_reuse_across_calls() {
    // A single workspace used for many sequential multiplies must
    // produce results identical to a fresh workspace per call.  Any
    // residual state (un-zeroed limbs, stale cursor) would corrupt
    // later products.
    const uint32_t n = 64;
    std::mt19937_64 rng(0xF1EA5ED);

    hydra::detail::ScratchWorkspace ws;
    ws.reserve_limbs(hydra::detail::karatsuba_scratch_limbs(n));

    bool all_ok = true;
    for (int iter = 0; iter < 32; ++iter) {
        std::vector<uint64_t> a(n), b(n);
        for (auto& l : a) l = rng() | 1u;
        for (auto& l : b) l = rng() | 1u;

        std::vector<uint64_t> out_shared(2 * n), out_fresh(2 * n);
        hydra::detail::mul_karatsuba(a.data(), b.data(), n, out_shared.data(), ws);

        hydra::detail::ScratchWorkspace ws_fresh;
        ws_fresh.reserve_limbs(hydra::detail::karatsuba_scratch_limbs(n));
        hydra::detail::mul_karatsuba(a.data(), b.data(), n, out_fresh.data(), ws_fresh);

        if (std::memcmp(out_shared.data(), out_fresh.data(),
                        2 * n * sizeof(uint64_t)) != 0) {
            all_ok = false;
            break;
        }
    }
    CHECK(all_ok, "scratch reuse matches fresh workspace across 32 iterations");
}

static void test_scratch_nested_frames() {
    // Interleave manual outer frames with Karatsuba's internal frames:
    // allocate an outer buffer, run a Karatsuba into a sibling buffer,
    // then verify the outer buffer is bit-identical to what we wrote
    // before the Karatsuba call.  This proves ScratchFrame's LIFO
    // rewind never leaks past its own mark.
    const uint32_t n = 64;
    hydra::detail::ScratchWorkspace ws;
    // Reserve: outer sentinel + operand/output for the inner call +
    // karatsuba recursion scratch.
    ws.reserve_limbs(4 + 4 * n + hydra::detail::karatsuba_scratch_limbs(n));

    hydra::detail::ScratchFrame outer(ws);
    uint64_t* sentinel = outer.take(4);
    for (int i = 0; i < 4; ++i) sentinel[i] = 0xDEADBEEF00000000ull | uint64_t(i);

    // Inner Karatsuba, using the same workspace.
    std::mt19937_64 rng(0xA5D1F00D);
    uint64_t* a = outer.take(n);
    uint64_t* b = outer.take(n);
    uint64_t* out = outer.take(2 * n);
    for (uint32_t i = 0; i < n; ++i) a[i] = rng() | 1u;
    for (uint32_t i = 0; i < n; ++i) b[i] = rng() | 1u;

    hydra::detail::mul_karatsuba(a, b, n, out, ws);

    // After the Karatsuba call (which opened its own inner frames),
    // the sentinel must still read back unchanged.
    bool sentinel_intact = true;
    for (int i = 0; i < 4; ++i) {
        if (sentinel[i] != (0xDEADBEEF00000000ull | uint64_t(i))) {
            sentinel_intact = false;
            break;
        }
    }
    CHECK(sentinel_intact, "scratch nested frames preserve outer sentinel");

    // Cross-check the inner result against schoolbook for good measure.
    std::vector<uint64_t> out_s(2 * n);
    hydra::detail::mul_limbs(a, n, b, n, out_s.data());
    bool eq = std::memcmp(out, out_s.data(), 2 * n * sizeof(uint64_t)) == 0;
    CHECK(eq, "scratch nested frames — Karatsuba matches schoolbook");
}

// ── Dual-row MAC leaf kernel regression tests ──────────────
//
// The NEON-tuned dual-row kernel (mac_row_2 inside mul_limbs) pairs
// up adjacent a-limbs and runs two independent carry chains.  These
// tests verify it produces byte-identical output to a naïve single-
// row reference across:
//   • all nb widths from 1 up to well past the NEON 2-lane unroll
//     boundary (probes the "j + 2 <= nb" tail)
//   • odd na (tests the odd-row tail handler that falls back to
//     mac_row_1 for the last a-limb)
//   • extreme operands (all-ones) for worst-case carry-out pressure

static void test_mul_limbs_dual_row_cross_check() {
    // Naïve reference: pure single-row schoolbook, no fast paths.
    auto ref_mul = [](const uint64_t* a, uint32_t na,
                      const uint64_t* b, uint32_t nb,
                      uint64_t* out) {
        std::memset(out, 0, (na + nb) * sizeof(uint64_t));
        for (uint32_t i = 0; i < na; ++i) {
            uint64_t carry = 0;
            for (uint32_t j = 0; j < nb; ++j) {
                unsigned __int128 t =
                    static_cast<unsigned __int128>(a[i]) * b[j]
                    + out[i + j] + carry;
                out[i + j] = static_cast<uint64_t>(t);
                carry = static_cast<uint64_t>(t >> 64);
            }
            for (uint32_t k = i + nb; carry; ++k) {
                uint64_t t = out[k] + carry;
                carry = (t < carry) ? 1u : 0u;
                out[k] = t;
            }
        }
    };

    std::mt19937_64 rng(0xD1AB10CA);
    bool all_ok = true;

    // Random operands: sweep na ∈ [1..12] × nb ∈ [1..12] to probe the
    // even/odd row-pair boundary and the NEON 2-lane inner tail.
    for (uint32_t na = 1; na <= 12 && all_ok; ++na) {
        for (uint32_t nb = 1; nb <= 12 && all_ok; ++nb) {
            std::vector<uint64_t> a(na), b(nb);
            for (auto& l : a) l = rng();
            for (auto& l : b) l = rng();

            std::vector<uint64_t> got(na + nb + 1), ref(na + nb + 1);
            hydra::detail::mul_limbs(a.data(), na, b.data(), nb, got.data());
            ref_mul(a.data(), na, b.data(), nb, ref.data());

            if (std::memcmp(got.data(), ref.data(),
                            (na + nb) * sizeof(uint64_t)) != 0) {
                std::string msg = "dual-row mul_limbs mismatch at na="
                                  + std::to_string(na)
                                  + " nb=" + std::to_string(nb);
                CHECK(false, msg.c_str());
                all_ok = false;
            }
        }
    }
    CHECK(all_ok, "dual-row mul_limbs matches reference over 12×12 sweep");
}

static void test_mul_limbs_dual_row_all_ones() {
    // All-ones: maximises carry propagation.  (2^(64na) - 1) × (2^(64nb) - 1)
    // produces a product where the carry chain touches every limb.
    auto ref_mul = [](const uint64_t* a, uint32_t na,
                      const uint64_t* b, uint32_t nb,
                      uint64_t* out) {
        std::memset(out, 0, (na + nb) * sizeof(uint64_t));
        for (uint32_t i = 0; i < na; ++i) {
            uint64_t carry = 0;
            for (uint32_t j = 0; j < nb; ++j) {
                unsigned __int128 t =
                    static_cast<unsigned __int128>(a[i]) * b[j]
                    + out[i + j] + carry;
                out[i + j] = static_cast<uint64_t>(t);
                carry = static_cast<uint64_t>(t >> 64);
            }
            for (uint32_t k = i + nb; carry; ++k) {
                uint64_t t = out[k] + carry;
                carry = (t < carry) ? 1u : 0u;
                out[k] = t;
            }
        }
    };

    for (uint32_t n : {2u, 3u, 4u, 7u, 8u, 16u, 17u, 32u}) {
        std::vector<uint64_t> a(n, UINT64_MAX), b(n, UINT64_MAX);
        std::vector<uint64_t> got(2 * n), ref(2 * n);
        hydra::detail::mul_limbs(a.data(), n, b.data(), n, got.data());
        ref_mul(a.data(), n, b.data(), n, ref.data());
        bool eq = std::memcmp(got.data(), ref.data(),
                              2 * n * sizeof(uint64_t)) == 0;
        std::string msg = "dual-row all-ones at n=" + std::to_string(n);
        CHECK(eq, msg.c_str());
    }
}

static void test_mul_limbs_dual_row_nb_one() {
    // nb = 1 must bypass mac_row_2 (it requires nb >= 2) and take the
    // single-row fallback for every a-limb.  Cross-check at several na.
    std::mt19937_64 rng(0xC1A551CA);
    bool all_ok = true;
    for (uint32_t na : {1u, 2u, 3u, 7u, 8u, 16u}) {
        std::vector<uint64_t> a(na);
        for (auto& l : a) l = rng();
        uint64_t b = rng();
        std::vector<uint64_t> got(na + 1), ref(na + 1, 0);
        hydra::detail::mul_limbs(a.data(), na, &b, 1, got.data());
        // Reference: single-row MAC in a naïve loop.
        uint64_t carry = 0;
        for (uint32_t i = 0; i < na; ++i) {
            unsigned __int128 t =
                static_cast<unsigned __int128>(a[i]) * b + carry;
            ref[i] = static_cast<uint64_t>(t);
            carry  = static_cast<uint64_t>(t >> 64);
        }
        ref[na] = carry;
        if (std::memcmp(got.data(), ref.data(),
                        (na + 1) * sizeof(uint64_t)) != 0) {
            all_ok = false;
            break;
        }
    }
    CHECK(all_ok, "dual-row fallback: nb=1 matches single-row reference");
}

// ── mul_general dispatch-seam tests ───────────────────────────────────
//
// These tests exercise the dispatch boundary of mul_general at the
// Karatsuba threshold.  Every test does a Hydra × Hydra multiplication
// through the public operator* and cross-checks against a reference
// product computed directly with the schoolbook kernel on the raw
// limb arrays — so whatever path mul_general picks (schoolbook,
// 4×4/8×8 specialised kernel, or Karatsuba) must produce an identical
// result.
//
// The threshold is KARATSUBA_THRESHOLD_LIMBS = 32.  The tests span the
// near-threshold band explicitly (31, 32, 33 limbs) and then probe a
// selection of mixed widths on either side of the boundary to make
// sure the seam handles non-square, non-power-of-two inputs.

// Produce a full-width n-limb Hydra (MSL bit 63 set) from a seed.
static Hydra make_large_seeded(uint32_t n_limbs, uint64_t seed) {
    std::vector<uint64_t> limbs(n_limbs);
    uint64_t v = seed ? seed : 0x1u;
    for (auto& l : limbs) {
        v ^= v << 13; v ^= v >> 7; v ^= v << 17;
        l = v | 1u;
    }
    limbs.back() |= (1ull << 63);
    return Hydra::from_limbs(limbs.data(), n_limbs);
}

// Cross-check: operator* result == schoolbook mul_limbs on the raw
// limbs.  Both Hydras are decomposed via their limb_view, multiplied
// with detail::mul_limbs into a scratch buffer, then re-assembled
// via from_limbs.  Any dispatch-path bug shows up as a mismatch.
static void check_mul_matches_schoolbook(
    const Hydra& a, const Hydra& b, const char* label)
{
    auto lv = a.limb_view();
    auto rv = b.limb_view();
    std::vector<uint64_t> ref(lv.count + rv.count, 0);
    uint32_t used = hydra::detail::mul_limbs(
        lv.ptr, lv.count, rv.ptr, rv.count, ref.data());
    Hydra expected = Hydra::from_limbs(ref.data(), used);
    Hydra got = a * b;
    CHECK(got == expected, label);
}

// ── explicit near-threshold widths ────────────────────────────────────

static void test_mul_seam_31_limbs() {
    // Just below threshold — should stay on schoolbook fallback.
    Hydra a = make_large_seeded(31, 0x5EA31A);
    Hydra b = make_large_seeded(31, 0x5EA31B);
    check_mul_matches_schoolbook(a, b, "operator* 31x31 (below threshold, schoolbook path)");
}

static void test_mul_seam_32_limbs() {
    // Exactly at threshold — first width that routes to Karatsuba.
    Hydra a = make_large_seeded(32, 0x5EA32A);
    Hydra b = make_large_seeded(32, 0x5EA32B);
    check_mul_matches_schoolbook(a, b, "operator* 32x32 (exact threshold, Karatsuba path)");
}

static void test_mul_seam_33_limbs() {
    // Just above threshold — requires pad-to-64 for the Karatsuba
    // pow2 precondition.  Exercises the "non-power-of-two" pad path.
    Hydra a = make_large_seeded(33, 0x5EA33A);
    Hydra b = make_large_seeded(33, 0x5EA33B);
    check_mul_matches_schoolbook(a, b, "operator* 33x33 (above threshold, padded to 64)");
}

// ── mixed widths near the threshold ───────────────────────────────────

static void test_mul_seam_mixed_32_16() {
    // Wider operand triggers Karatsuba; narrow operand is zero-padded.
    Hydra a = make_large_seeded(32, 0xA1);
    Hydra b = make_large_seeded(16, 0xB2);
    check_mul_matches_schoolbook(a, b, "operator* 32x16 (max triggers Karatsuba, rhs padded)");
}

static void test_mul_seam_mixed_16_32() {
    // Commutativity check on the mixed-width path.
    Hydra a = make_large_seeded(16, 0xC3);
    Hydra b = make_large_seeded(32, 0xD4);
    check_mul_matches_schoolbook(a, b, "operator* 16x32 (max triggers Karatsuba, lhs padded)");
}

static void test_mul_seam_mixed_33_17() {
    // Asymmetric & non-power-of-two on both sides — max=33, pad to 64.
    Hydra a = make_large_seeded(33, 0xE5);
    Hydra b = make_large_seeded(17, 0xF6);
    check_mul_matches_schoolbook(a, b, "operator* 33x17 (asymmetric, padded to 64)");
}

static void test_mul_seam_mixed_31_32() {
    // Threshold straddled by a single limb — rhs hits threshold, lhs
    // does not; max_limbs=32 still dispatches to Karatsuba (pad to 32).
    Hydra a = make_large_seeded(31, 0x77);
    Hydra b = make_large_seeded(32, 0x88);
    check_mul_matches_schoolbook(a, b, "operator* 31x32 (straddles threshold)");
}

// ── integrity checks on the dispatch path ─────────────────────────────

static void test_mul_seam_identity_at_threshold() {
    // (0) · (large) and (1) · (large) must still work when the right
    // operand is at the threshold — the zero/one short-circuit lives
    // in the small-small fast path, but the general path has its own
    // `count==0` zero check that the Karatsuba branch must preserve.
    Hydra big = make_large_seeded(32, 0xD1D00);

    Hydra zero_times = Hydra{0u} * big;
    CHECK(zero_times.to_string() == "0", "0 * threshold_large == 0");

    Hydra one_times = Hydra{1u} * big;
    CHECK(one_times == big, "1 * threshold_large == threshold_large");
}

static void test_mul_seam_64_limbs() {
    // Two levels of recursion above the threshold (64 → 32 → 16 base).
    // Guards against any off-by-one in the recursive output-layout.
    Hydra a = make_large_seeded(64, 0x640A);
    Hydra b = make_large_seeded(64, 0x640B);
    check_mul_matches_schoolbook(a, b, "operator* 64x64 (deep Karatsuba recursion)");
}

static void test_mul_seam_commutativity_at_threshold() {
    // a*b == b*a for threshold-sized operands.
    Hydra a = make_large_seeded(32, 0x0C0FFEE);
    Hydra b = make_large_seeded(33, 0x0BADF00D);
    Hydra ab = a * b;
    Hydra ba = b * a;
    CHECK(ab == ba, "operator* commutative across Karatsuba seam (32x33 vs 33x32)");
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

static void test_shr_large_demotes_to_small() {
    // Multi-tier demotion in a single shift: Large (4-limb) → Small (1-limb).
    // Build a 4-limb value whose top three limbs each hold a single bit that
    // all land inside the low limb after shifting right by 192.
    uint64_t limbs[4] = {
        0u,
        0x1111ull,
        0x2222ull,
        0x3333ull,
    };
    Hydra a = Hydra::from_limbs(limbs, 4);
    CHECK(a.is_large() || a.is_medium(), "input large-ish");
    Hydra r = a >> 192;
    // Value after >>192 is just the top limb: 0x3333.
    CHECK(r.is_small(), "large >> 192 demotes to Small");
    CHECK(r.to_u64() == 0x3333ull, "large >> 192 value correct");
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

// ── Hydra ÷ Hydra (Knuth Algorithm D) tests ──────────────────────────
//
// Each test verifies the fundamental invariant:
//   a == q * b + r,  0 ≤ r < b
// using Hydra's own multiply and add.  Because mul+add are already
// covered by earlier tests, this is a closed-loop check independent
// of any reference implementation.

static bool divmod_identity(const Hydra& a, const Hydra& b) {
    auto qr = a.divmod(b);
    Hydra reconstruct = qr.quotient * b + qr.remainder;
    if (reconstruct != a) return false;
    // Remainder must be strictly less than divisor (positive integers).
    if (b.limb_count() > 0 && !(qr.remainder < b)) return false;
    return true;
}

static void test_divmod_zero_dividend() {
    Hydra a{0u};
    Hydra b = make_large(4, 0x1111);
    auto qr = a.divmod(b);
    CHECK(qr.quotient  == Hydra{}, "0 / large: quotient = 0");
    CHECK(qr.remainder == Hydra{}, "0 / large: remainder = 0");
}

static void test_divmod_divisor_greater_than_dividend() {
    Hydra a = make_large(2, 0x1111);
    Hydra b = make_large(4, 0x2222);
    // b > a → quotient 0, remainder a.
    auto qr = a.divmod(b);
    CHECK(qr.quotient  == Hydra{}, "small / large: q = 0");
    CHECK(qr.remainder == a,       "small / large: r = dividend");
}

static void test_divmod_equal_values() {
    Hydra a = make_large(4, 0xCAFE);
    auto qr = a.divmod(a);
    CHECK(qr.quotient  == Hydra{1u}, "a / a: q = 1");
    CHECK(qr.remainder == Hydra{},   "a / a: r = 0");
}

static void test_divmod_throws_on_zero_divisor() {
    bool threw = false;
    try { (void)make_large(4, 0xDEAD).divmod(Hydra{}); }
    catch (const std::domain_error&) { threw = true; }
    CHECK(threw, "divmod by 0 throws domain_error");
}

static void test_divmod_exact_divisibility() {
    // q * b should divide cleanly with remainder 0.
    Hydra b = make_large(3, 0x1111);
    Hydra q = make_large(4, 0x2222);
    Hydra a = q * b;
    auto qr = a.divmod(b);
    CHECK(qr.quotient  == q,      "(q*b) / b: quotient matches q");
    CHECK(qr.remainder == Hydra{},"(q*b) / b: remainder = 0");
}

static void test_divmod_power_of_two_divisor() {
    // For b = 2^k, division should match right-shift quotient and
    // (a & (2^k - 1)) remainder.  Use Hydra-native operations only.
    Hydra a = make_large(8, 0xBEEF);
    for (unsigned k : {64u, 128u, 192u, 256u, 320u}) {
        Hydra two_k = Hydra{1u} << k;
        auto qr = a.divmod(two_k);
        Hydra q_expected = a >> k;
        CHECK(qr.quotient == q_expected, "a / 2^k == a >> k");
        // a - q*2^k must equal the low-k-bit residue.
        Hydra reconstruct = qr.quotient * two_k + qr.remainder;
        CHECK(reconstruct == a, "a / 2^k: q*b+r == a");
    }
}

static void test_divmod_small_divisor_same_as_div_u64() {
    // Single-limb divisor path should agree with div_u64 / mod_u64.
    Hydra a = make_large(8, 0xF00D);
    const uint64_t divisors[] = {3ull, 7ull, 1000000007ull, UINT64_MAX};
    for (uint64_t d : divisors) {
        Hydra b{d};
        auto qr = a.divmod(b);
        CHECK(qr.quotient  == a.div_u64(d),       "divmod(1-limb) ≡ div_u64");
        CHECK(qr.remainder == Hydra{a.mod_u64(d)},"divmod(1-limb).r ≡ mod_u64");
    }
}

static void test_divmod_two_limb_divisor() {
    // Multi-limb divisor exercising Knuth D directly.
    // Two-limb divisor just barely normalised.
    uint64_t d_limbs[2] = {0xDEAD'BEEFull, 0x1u}; // small high limb
    Hydra b = Hydra::from_limbs(d_limbs, 2);
    Hydra a = make_large(6, 0xABCD);
    CHECK(divmod_identity(a, b), "2-limb divisor, 6-limb dividend");
}

static void test_divmod_v_top_bit_already_set() {
    // Divisor already normalised: d == 0 shift path.
    uint64_t d_limbs[2] = {0x1234ull, 0x8000'0000'0000'0000ull};
    Hydra b = Hydra::from_limbs(d_limbs, 2);
    Hydra a = make_large(4, 0xBEEF);
    CHECK(divmod_identity(a, b), "pre-normalised divisor (d=0 path)");
}

static void test_divmod_worst_case_q_hat() {
    // Construct inputs that historically triggered q_hat overestimate
    // by 2 in naive implementations: divisor with v[nv-1] just above
    // 2^63 and v[nv-2] large, and dividend whose top limb equals
    // v[nv-1] so the initial q_hat = B-1.
    //
    // Build: v = [0xFFFF...FF, 0xFFFF...FF, 0x8000...01]
    uint64_t v_limbs[3] = {
        0xFFFF'FFFF'FFFF'FFFFull,
        0xFFFF'FFFF'FFFF'FFFFull,
        0x8000'0000'0000'0001ull,
    };
    Hydra v = Hydra::from_limbs(v_limbs, 3);
    // u = v * q + r for some chosen q and r.
    Hydra q_chosen = make_large(3, 0x4242);
    Hydra r_chosen = Hydra{0x12345ull};  // small remainder; ensure r < v
    Hydra u = v * q_chosen + r_chosen;
    auto qr = u.divmod(v);
    CHECK(qr.quotient  == q_chosen, "worst-case q_hat: quotient");
    CHECK(qr.remainder == r_chosen, "worst-case q_hat: remainder");
}

static void test_divmod_add_back_scenario() {
    // Try many random pairs to exercise the rare add-back correction
    // path (~2/B chance per step — rare but must be correct when it
    // fires).  200 random pairs × up to 10 steps = 2000 opportunities;
    // at B = 2^64 the expected hit count is tiny but each iteration
    // still runs the full multiply-subtract-correct path.
    uint64_t seed = 0xC0DE;
    for (int i = 0; i < 200; ++i) {
        // Pseudo-random seed advance (same xorshift as make_large).
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint32_t nu = 2 + (seed % 7);           // 2..8 limbs
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint32_t nv = 2 + (seed % (nu - 1));    // 2..(nu-1) limbs
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint64_t sa = seed;
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint64_t sb = seed;

        Hydra a = make_large(nu, sa);
        Hydra b = make_large(nv, sb);
        // If b > a, divmod_identity still works: q=0, r=a.
        CHECK(divmod_identity(a, b), "random large / large identity");
    }
}

static void test_divmod_large_dividend_forces_heap_scratch() {
    // nu > STACK_LIMIT (=32) forces the std::vector scratch path.
    // Stress with 40-limb dividend, 20-limb divisor.
    Hydra a = make_large(40, 0xF00D'BEEFull);
    Hydra b = make_large(20, 0xDEAD'C0DEull);
    CHECK(divmod_identity(a, b), "40/20-limb (heap scratch path)");
}

static void test_divmod_divisor_equals_stack_limit() {
    // Exactly at the stack/heap boundary: 32-limb dividend.
    Hydra a = make_large(32, 0xA5A5);
    Hydra b = make_large(16, 0x5A5A);
    CHECK(divmod_identity(a, b), "32/16-limb (stack scratch boundary)");
}

static void test_divmod_128_over_64() {
    // Classic 128-bit ÷ 64-bit: dividend is 2-limb, divisor is 1-limb
    // (routes through div_u64 path internally; sanity-check API).
    uint64_t a_limbs[2] = {0xFEDC'BA98'7654'3210ull, 0x0123'4567'89ABull};
    Hydra a = Hydra::from_limbs(a_limbs, 2);
    Hydra b{0xDEAD'BEEFull};
    auto qr = a.divmod(b);
    Hydra reconstruct = qr.quotient * b + qr.remainder;
    CHECK(reconstruct == a,                "128/64: q*b+r == a");
    CHECK(qr.remainder < b,                "128/64: r < b");
}

static void test_divmod_192_over_128() {
    // 3-limb ÷ 2-limb.
    Hydra a = make_large(3, 0xAB01);
    uint64_t b_limbs[2] = {0x1234'5678'9ABC'DEF0ull, 0x0000'FEDC'BA98'7654ull};
    Hydra b = Hydra::from_limbs(b_limbs, 2);
    CHECK(divmod_identity(a, b), "192/128");
}

static void test_divmod_512_over_256() {
    // 8-limb ÷ 4-limb — the Knuth D "shape" at the ceiling of the
    // existing hand-unrolled multiply kernels.
    Hydra a = make_large(8, 0xBADC);
    Hydra b = make_large(4, 0xD00D);
    CHECK(divmod_identity(a, b), "512/256");
}

static void test_divmod_1024_over_512() {
    // 16-limb ÷ 8-limb.
    Hydra a = make_large(16, 0xDEAF);
    Hydra b = make_large(8,  0xBEAD);
    CHECK(divmod_identity(a, b), "1024/512");
}

static void test_div_mod_delegate_consistency() {
    // div(v) and mod(v) must agree with divmod(v).
    Hydra a = make_large(8, 0x9999);
    Hydra b = make_large(4, 0xAAAA);
    auto qr = a.divmod(b);
    CHECK(a.div(b) == qr.quotient,  "div ≡ divmod.quotient");
    CHECK(a.mod(b) == qr.remainder, "mod ≡ divmod.remainder");
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

// ── signed arithmetic, native interop, comparison, bitwise tests ─────

// ── Signed construction ────────────────────────────────────────────

static void test_signed_constructor_positive() {
    Hydra a{42};       // int
    CHECK(a.to_string() == "42", "Hydra(42) == 42");
    CHECK(!a.is_negative(), "Hydra(42) is not negative");
}

static void test_signed_constructor_negative() {
    Hydra a{-7};
    CHECK(a.to_string() == "-7", "Hydra(-7) == -7");
    CHECK(a.is_negative(), "Hydra(-7) is negative");
}

static void test_signed_constructor_zero() {
    Hydra a{0};
    CHECK(a.to_string() == "0", "Hydra(0) == 0");
    CHECK(!a.is_negative(), "Hydra(0) is not negative");
}

static void test_signed_constructor_int64_min() {
    // INT64_MIN = -9223372036854775808
    Hydra a{INT64_MIN};
    CHECK(a.is_negative(), "INT64_MIN is negative");
    CHECK(a.to_string() == "-9223372036854775808", "INT64_MIN to_string");
}

static void test_signed_constructor_int64_max() {
    Hydra a{INT64_MAX};
    CHECK(!a.is_negative(), "INT64_MAX is positive");
    CHECK(a.to_string() == "9223372036854775807", "INT64_MAX to_string");
}

static void test_signed_constructor_int8() {
    Hydra a{int8_t{-128}};
    CHECK(a.to_string() == "-128", "int8_t(-128) to_string");
}

static void test_signed_constructor_int16() {
    Hydra a{int16_t{-32768}};
    CHECK(a.to_string() == "-32768", "int16_t(-32768) to_string");
}

static void test_signed_constructor_int32() {
    Hydra a{int32_t{-2147483648}};
    CHECK(a.to_string() == "-2147483648", "int32_t(INT32_MIN) to_string");
}

// ── Signed addition ────────────────────────────────────────────────

static void test_signed_add_pos_pos() {
    Hydra a{5}, b{3};
    CHECK((a + b).to_string() == "8", "5 + 3 == 8");
}

static void test_signed_add_neg_neg() {
    Hydra a{-5}, b{-3};
    CHECK((a + b).to_string() == "-8", "-5 + -3 == -8");
}

static void test_signed_add_pos_neg_pos_wins() {
    Hydra a{10}, b{-3};
    CHECK((a + b).to_string() == "7", "10 + (-3) == 7");
}

static void test_signed_add_pos_neg_neg_wins() {
    Hydra a{3}, b{-10};
    CHECK((a + b).to_string() == "-7", "3 + (-10) == -7");
}

static void test_signed_add_cancel_to_zero() {
    Hydra a{42}, b{-42};
    Hydra c = a + b;
    CHECK(c.to_string() == "0", "42 + (-42) == 0");
    CHECK(!c.is_negative(), "42 + (-42) is not negative");
}

static void test_signed_add_large_neg() {
    Hydra a = make_large(4, 0x1111);
    Hydra b = make_large(4, 0x2222);
    b.negate();  // b is negative
    // a + (-b) = a - |b|
    Hydra c = a + b;
    // If |a| > |b|: positive; else negative
    // Doesn't matter which — check invariant
    Hydra d = c - a;
    CHECK(d == b, "signed add/sub roundtrip with Large");
}

// ── Signed subtraction ─────────────────────────────────────────────

static void test_signed_sub_to_negative() {
    Hydra a{3u}, b{10u};
    Hydra c = a - b;
    CHECK(c.to_string() == "-7", "3 - 10 == -7");
    CHECK(c.is_negative(), "3 - 10 is negative");
}

static void test_signed_sub_neg_from_pos() {
    Hydra a{5}, b{-3};
    CHECK((a - b).to_string() == "8", "5 - (-3) == 8");
}

static void test_signed_sub_neg_from_neg() {
    Hydra a{-5}, b{-3};
    CHECK((a - b).to_string() == "-2", "-5 - (-3) == -2");
}

static void test_signed_sub_symmetric() {
    Hydra a{42u}, b{58u};
    Hydra c = a - b;
    Hydra d = b - a;
    CHECK(c.to_string() == "-16", "42 - 58 == -16");
    CHECK(d.to_string() == "16", "58 - 42 == 16");
    CHECK((c + d).to_string() == "0", "(a-b) + (b-a) == 0");
}

// ── Signed multiplication ──────────────────────────────────────────

static void test_signed_mul_pos_pos() {
    Hydra a{7}, b{6};
    CHECK((a * b).to_string() == "42", "7 * 6 == 42");
}

static void test_signed_mul_pos_neg() {
    Hydra a{7}, b{-6};
    CHECK((a * b).to_string() == "-42", "7 * (-6) == -42");
}

static void test_signed_mul_neg_neg() {
    Hydra a{-7}, b{-6};
    CHECK((a * b).to_string() == "42", "(-7) * (-6) == 42");
}

static void test_signed_mul_neg_zero() {
    Hydra a{-7}, b{0};
    Hydra c = a * b;
    CHECK(c.to_string() == "0", "(-7) * 0 == 0");
    CHECK(!c.is_negative(), "(-7) * 0 is not negative");
}

static void test_signed_mul_large_cross_sign() {
    Hydra a = make_large(4, 0x1111);
    Hydra b = make_large(4, 0x2222);
    b.negate();
    Hydra c = a * b;
    CHECK(c.is_negative(), "positive * negative → negative");
    Hydra d = a * (-b);  // a * |b|
    Hydra e = -c;        // |a*b|
    CHECK(d == e, "a * |b| == |a * (-b)|");
}

// ── Signed division (truncation toward zero) ───────────────────────

static void test_signed_divmod_pos_pos() {
    Hydra a{7}, b{3};
    auto [q, r] = a.divmod(b);
    CHECK(q.to_string() == "2", "7 / 3 == 2");
    CHECK(r.to_string() == "1", "7 % 3 == 1");
    CHECK(a == b * q + r, "7 == 3*2 + 1 invariant");
}

static void test_signed_divmod_neg_pos() {
    // -7 / 3 → q = -2, r = -1  (truncate toward zero)
    Hydra a{-7}, b{3};
    auto [q, r] = a.divmod(b);
    CHECK(q.to_string() == "-2", "-7 / 3 == -2");
    CHECK(r.to_string() == "-1", "-7 % 3 == -1");
    CHECK(a == b * q + r, "-7 == 3*(-2) + (-1) invariant");
}

static void test_signed_divmod_pos_neg() {
    // 7 / -3 → q = -2, r = 1
    Hydra a{7}, b{-3};
    auto [q, r] = a.divmod(b);
    CHECK(q.to_string() == "-2", "7 / (-3) == -2");
    CHECK(r.to_string() == "1", "7 % (-3) == 1");
    CHECK(a == b * q + r, "7 == (-3)*(-2) + 1 invariant");
}

static void test_signed_divmod_neg_neg() {
    // -7 / -3 → q = 2, r = -1
    Hydra a{-7}, b{-3};
    auto [q, r] = a.divmod(b);
    CHECK(q.to_string() == "2", "(-7) / (-3) == 2");
    CHECK(r.to_string() == "-1", "(-7) % (-3) == -1");
    CHECK(a == b * q + r, "-7 == (-3)*2 + (-1) invariant");
}

static void test_signed_divmod_exact() {
    Hydra a{-12}, b{4};
    auto [q, r] = a.divmod(b);
    CHECK(q.to_string() == "-3", "-12 / 4 == -3");
    CHECK(r.to_string() == "0", "-12 % 4 == 0");
    CHECK(!r.is_negative(), "remainder 0 is not negative");
    CHECK(a == b * q + r, "-12 == 4 * (-3) + 0 invariant");
}

static void test_signed_divmod_dividend_smaller() {
    Hydra a{-2}, b{5};
    auto [q, r] = a.divmod(b);
    CHECK(q.to_string() == "0", "-2 / 5 == 0");
    CHECK(r.to_string() == "-2", "-2 % 5 == -2");
    CHECK(a == b * q + r, "-2 == 5*0 + (-2) invariant");
}

static void test_signed_divmod_large_invariant() {
    Hydra a = make_large(8, 0xAAAA);
    a.negate();
    Hydra b = make_large(4, 0xBBBB);
    auto [q, r] = a.divmod(b);
    // invariant: a == b * q + r
    CHECK(a == b * q + r, "signed large divmod invariant");
    CHECK(q.is_negative(), "negative / positive → negative quotient");
}

// ── Unary negation ─────────────────────────────────────────────────

static void test_negate_positive() {
    Hydra a{42};
    Hydra b = -a;
    CHECK(b.to_string() == "-42", "-(42) == -42");
}

static void test_negate_negative() {
    Hydra a{-42};
    Hydra b = -a;
    CHECK(b.to_string() == "42", "-(-42) == 42");
}

static void test_negate_zero() {
    Hydra a{0};
    Hydra b = -a;
    CHECK(b.to_string() == "0", "-(0) == 0");
    CHECK(!b.is_negative(), "-(0) is not negative");
}

static void test_double_negate() {
    Hydra a{-999};
    CHECK((-(-a)) == a, "--a == a");
}

// ── Comparison completeness ────────────────────────────────────────

static void test_cmp_pos_neg() {
    Hydra a{5}, b{-5};
    CHECK(a > b, "5 > -5");
    CHECK(b < a, "-5 < 5");
    CHECK(a != b, "5 != -5");
    CHECK(a >= b, "5 >= -5");
    CHECK(b <= a, "-5 <= 5");
}

static void test_cmp_neg_neg() {
    Hydra a{-3}, b{-10};
    CHECK(a > b, "-3 > -10");
    CHECK(b < a, "-10 < -3");
}

static void test_cmp_zero_neg() {
    Hydra a{0}, b{-1};
    CHECK(a > b, "0 > -1");
    CHECK(a >= b, "0 >= -1");
}

static void test_cmp_equal_neg() {
    Hydra a{-42}, b{-42};
    CHECK(a == b, "-42 == -42");
    CHECK(a >= b, "-42 >= -42");
    CHECK(a <= b, "-42 <= -42");
}

static void test_cmp_with_int_literal() {
    Hydra a{100};
    CHECK(a > 50, "Hydra{100} > 50");
    CHECK(a == 100, "Hydra{100} == 100");
    CHECK(a < 200, "Hydra{100} < 200");
    CHECK(a > -5, "Hydra{100} > -5");
}

static void test_cmp_large_signed() {
    Hydra a = make_large(4, 0x1111);
    Hydra b = make_large(4, 0x2222);
    Hydra neg_b = -b;
    CHECK(a > neg_b, "positive Large > negative Large");
    CHECK(neg_b < Hydra{0}, "negative Large < 0");
}

// ── Native interop (mixed arithmetic) ──────────────────────────────

static void test_interop_add_int() {
    Hydra a{100u};
    Hydra b = a + 50;
    CHECK(b.to_string() == "150", "Hydra + int literal");
}

static void test_interop_sub_int() {
    Hydra a{100u};
    Hydra b = a - 150;
    CHECK(b.to_string() == "-50", "Hydra(100) - 150 == -50");
}

static void test_interop_mul_int() {
    Hydra a{7};
    Hydra b = a * -6;
    CHECK(b.to_string() == "-42", "Hydra(7) * -6 == -42");
}

static void test_interop_compare_u64() {
    Hydra a{UINT64_MAX};
    CHECK(a > 0u, "UINT64_MAX > 0");
    CHECK(a == UINT64_MAX, "Hydra{UINT64_MAX} == UINT64_MAX");
}

static void test_interop_compare_i64() {
    Hydra a{-1};
    CHECK(a < 0u, "-1 < 0u (unsigned 0)");
    CHECK(a == -1, "Hydra{-1} == -1");
}

static void test_interop_add_negative_int() {
    Hydra a = make_large(4, 0x1234);
    Hydra b = a + (-1);
    Hydra c = a - Hydra{1u};
    CHECK(b == c, "large + (-1) == large - 1");
}

// ── Bitwise operators ──────────────────────────────────────────────

static void test_bitwise_and_basic() {
    Hydra a{0xFF00u}, b{0x0FF0u};
    Hydra c = a & b;
    CHECK(c == Hydra{0x0F00u}, "0xFF00 & 0x0FF0 == 0x0F00");
}

static void test_bitwise_and_zero() {
    Hydra a{42u}, b{0u};
    CHECK((a & b) == Hydra{0u}, "x & 0 == 0");
}

static void test_bitwise_or_basic() {
    Hydra a{0xFF00u}, b{0x00FFu};
    CHECK((a | b) == Hydra{0xFFFFu}, "0xFF00 | 0x00FF == 0xFFFF");
}

static void test_bitwise_or_zero() {
    Hydra a{42u};
    CHECK((a | Hydra{0u}) == a, "x | 0 == x");
}

static void test_bitwise_xor_basic() {
    Hydra a{0xAAAAu}, b{0x5555u};
    CHECK((a ^ b) == Hydra{0xFFFFu}, "0xAAAA ^ 0x5555 == 0xFFFF");
}

static void test_bitwise_xor_self() {
    Hydra a{42u};
    CHECK((a ^ a) == Hydra{0u}, "x ^ x == 0");
}

static void test_bitwise_not_zero() {
    Hydra a{0u};
    Hydra b = ~a;
    CHECK(b.to_string() == "-1", "~0 == -1");
}

static void test_bitwise_not_positive() {
    Hydra a{0u};
    Hydra b{5u};
    Hydra c = ~b;  // ~5 = -(5+1) = -6
    CHECK(c.to_string() == "-6", "~5 == -6");
}

static void test_bitwise_not_negative() {
    Hydra a{-6};
    Hydra b = ~a;  // ~(-6) = |-6|-1 = 5
    CHECK(b.to_string() == "5", "~(-6) == 5");
}

static void test_bitwise_not_roundtrip() {
    Hydra a{42u};
    CHECK(~(~a) == a, "~~x == x");
}

static void test_bitwise_and_medium() {
    // Medium (2-limb) AND
    Hydra a = Hydra::make_medium(UINT64_MAX, 0xFF00FF00FF00FF00ull, 0, 2);
    Hydra b = Hydra::make_medium(0x0F0F0F0F0F0F0F0Full, UINT64_MAX, 0, 2);
    Hydra c = a & b;
    auto lv = c.limb_view();
    CHECK(lv.count == 2, "medium & medium → 2 limbs");
    CHECK(lv.ptr[0] == 0x0F0F0F0F0F0F0F0Full, "AND limb 0");
    CHECK(lv.ptr[1] == 0xFF00FF00FF00FF00ull, "AND limb 1");
}

static void test_bitwise_or_large() {
    Hydra a = make_large(4, 0x1111);
    Hydra b = make_large(4, 0x2222);
    Hydra c = a | b;
    // c should have all bits set that are in either a or b
    CHECK((c & a) == a, "(a|b) & a == a");
    CHECK((c & b) == b, "(a|b) & b == b");
}

static void test_bitwise_xor_large() {
    Hydra a = make_large(4, 0x1111);
    Hydra b = make_large(4, 0x2222);
    Hydra c = a ^ b;
    CHECK((c ^ a) == b, "(a^b) ^ a == b");
    CHECK((c ^ b) == a, "(a^b) ^ b == a");
}

static void test_bitwise_negative_throws() {
    Hydra a{-1}, b{1};
    bool threw_and = false, threw_or = false, threw_xor = false;
    try { (void)(a & b); } catch (const std::domain_error&) { threw_and = true; }
    try { (void)(a | b); } catch (const std::domain_error&) { threw_or = true; }
    try { (void)(a ^ b); } catch (const std::domain_error&) { threw_xor = true; }
    CHECK(threw_and, "bitwise & with negative throws");
    CHECK(threw_or, "bitwise | with negative throws");
    CHECK(threw_xor, "bitwise ^ with negative throws");
}

static void test_bitwise_compound_assign() {
    Hydra a{0xFF00u};
    a &= Hydra{0x0FF0u};
    CHECK(a == Hydra{0x0F00u}, "&= works");
    a |= Hydra{0x000Fu};
    CHECK(a == Hydra{0x0F0Fu}, "|= works");
    a ^= Hydra{0xFFFFu};
    CHECK(a == Hydra{0xF0F0u}, "^= works");
}

// ── Mixed-tier signed arithmetic ───────────────────────────────────

static void test_signed_add_small_neg_plus_large() {
    Hydra a{-1};
    Hydra b = make_large(4, 0x1234);
    Hydra c = a + b;
    Hydra d = b + a;
    CHECK(c == d, "commutativity: small neg + large");
    // c should be b - 1
    Hydra expected = b - Hydra{1u};
    CHECK(c == expected, "(-1) + large == large - 1");
}

static void test_signed_mul_medium_neg_times_large() {
    Hydra a = Hydra::make_medium(7, 1, 0, 2);
    a.negate();
    Hydra b = make_large(4, 0x5678);
    Hydra c = a * b;
    Hydra d = (-a) * b;
    CHECK(c.is_negative(), "neg medium * pos large → negative");
    CHECK((-c) == d, "|neg * pos| == |neg| * pos");
}

// ── Normalize preserves sign across tier demotion ──────────────────

static void test_normalize_sign_large_to_small() {
    // Build a Large with a single non-zero limb + sign bit.
    auto* rep = LargeRep::create(8);
    rep->used = 4;
    rep->limbs()[0] = 42;
    rep->limbs()[1] = 0;
    rep->limbs()[2] = 0;
    rep->limbs()[3] = 0;
    Hydra a;
    a.meta = Hydra::make_large_meta() | hydra::bits::SIGN_BIT;
    a.payload.large = rep;
    a.normalize();
    CHECK(a.is_small(), "Large(1-limb) demotes to Small");
    CHECK(a.is_negative(), "sign preserved across Large→Small");
    CHECK(a.to_string() == "-42", "Large→Small with sign == -42");
}

static void test_normalize_sign_zero_clears() {
    Hydra a{42u};
    a.set_negative();
    // Manually set payload to 0 (simulating a sub result)
    a.payload.small = 0;
    a.normalize();
    CHECK(!a.is_negative(), "zero clears sign after normalize");
}

// ── Adversarial signed edge cases ──────────────────────────────────

static void test_signed_add_overflow_to_medium() {
    // UINT64_MAX + 1 as addition; now test -(UINT64_MAX) + (-1) → -UINT64_MAX-1
    Hydra a{UINT64_MAX};
    a.negate();  // -UINT64_MAX
    Hydra b{-1};
    Hydra c = a + b;
    // Expected: -(UINT64_MAX + 1) = -(2^64)
    CHECK(c.is_negative(), "negative overflow to medium");
    CHECK(c.is_medium(), "negative overflow lands in medium");
    // Magnitude should be 2^64 = [0, 1] in limbs
    auto lv = c.limb_view();
    CHECK(lv.count == 2, "magnitude has 2 limbs");
    CHECK(lv.ptr[0] == 0, "limb 0 == 0");
    CHECK(lv.ptr[1] == 1, "limb 1 == 1");
}

static void test_signed_sub_medium_to_small() {
    // -2^64 - (-UINT64_MAX) = -(2^64) + UINT64_MAX = -(2^64 - UINT64_MAX) = -(1) = -1
    Hydra a = Hydra::make_medium(0, 1, 0, 2);  // 2^64
    a.negate();  // -2^64
    Hydra b{UINT64_MAX};
    b.negate();  // -UINT64_MAX
    Hydra c = a - b;  // -2^64 - (-UINT64_MAX) = -2^64 + UINT64_MAX = -(2^64 - UINT64_MAX) = -1
    CHECK(c.to_string() == "-1", "-2^64 - (-UINT64_MAX) == -1");
    CHECK(c.is_small(), "result demotes to Small");
}

static void test_signed_divmod_int64_min_by_neg1() {
    // INT64_MIN / (-1) = INT64_MAX + 1 = 2^63 (overflows int64_t but Hydra handles it)
    Hydra a{INT64_MIN};
    Hydra b{-1};
    auto [q, r] = a.divmod(b);
    // q = -INT64_MIN = 2^63 = 9223372036854775808
    CHECK(q.to_string() == "9223372036854775808", "INT64_MIN / (-1) overflow into medium");
    CHECK(r.to_string() == "0", "INT64_MIN % (-1) == 0");
    CHECK(!r.is_negative(), "remainder 0 is not negative");
    CHECK(a == b * q + r, "divmod invariant for INT64_MIN / -1");
}

// ── entry point ──────────────────────────────────────────────────────

// ── String parse constructor tests ──────────────────────────────────

static void test_parse_simple_positive() {
    Hydra x("12345");
    CHECK(x.to_string() == "12345", "parse simple positive");
}

static void test_parse_zero() {
    Hydra x("0");
    CHECK(x.to_string() == "0", "parse zero");
    CHECK(!x.is_negative(), "parsed zero is non-negative");
}

static void test_parse_negative_zero() {
    Hydra x("-0");
    CHECK(x.to_string() == "0", "parse negative zero → '0'");
    CHECK(!x.is_negative(), "negative zero canonicalizes to non-negative");
}

static void test_parse_leading_zeros() {
    Hydra x("000042");
    CHECK(x.to_string() == "42", "leading zeros stripped");
}

static void test_parse_leading_sign_plus() {
    Hydra x("+999");
    CHECK(x.to_string() == "999", "parse with + sign");
}

static void test_parse_negative() {
    Hydra x("-42");
    CHECK(x.to_string() == "-42", "parse negative");
    CHECK(x.is_negative(), "parse negative sets sign");
}

static void test_parse_uint64_max() {
    // UINT64_MAX = 18446744073709551615
    Hydra x("18446744073709551615");
    CHECK(x.to_string() == "18446744073709551615", "parse UINT64_MAX");
}

static void test_parse_uint64_max_plus_one() {
    // 2^64 = 18446744073709551616 — should promote to Medium
    Hydra x("18446744073709551616");
    CHECK(x.to_string() == "18446744073709551616", "parse UINT64_MAX+1");
}

static void test_parse_large_negative() {
    Hydra x("-18446744073709551616");
    CHECK(x.to_string() == "-18446744073709551616", "parse large negative");
    CHECK(x.is_negative(), "large negative has sign set");
}

static void test_parse_int64_boundaries() {
    // INT64_MAX = 9223372036854775807
    Hydra a("9223372036854775807");
    CHECK(a == Hydra{INT64_MAX}, "parse INT64_MAX");

    // INT64_MIN = -9223372036854775808
    Hydra b("-9223372036854775808");
    CHECK(b == Hydra{INT64_MIN}, "parse INT64_MIN");
}

static void test_parse_invalid_empty() {
    bool threw = false;
    try { Hydra x(""); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "parse empty string throws");
}

static void test_parse_invalid_chars() {
    bool threw = false;
    try { Hydra x("123abc"); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "parse invalid chars throws");
}

static void test_parse_sign_only() {
    bool threw = false;
    try { Hydra x("-"); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw, "parse sign-only throws");
}

static void test_parse_power_of_two() {
    // 2^128 = 340282366920938463463374607431768211456
    Hydra x("340282366920938463463374607431768211456");
    Hydra expected = Hydra{1u} << 128;
    CHECK(x == expected, "parse 2^128");
}

static void test_parse_power_of_ten() {
    // 10^30 = 1000000000000000000000000000000
    Hydra x("1000000000000000000000000000000");
    CHECK(x.to_string() == "1000000000000000000000000000000", "parse 10^30 round-trip");
}

// ── Round-trip invariant tests ─────────────────────────────────────

// Helper: canonical form strips leading zeros and normalizes negative zero.
static std::string canonicalize(const std::string& s) {
    if (s.empty()) return "0";
    size_t pos = 0;
    bool neg = false;
    if (s[0] == '-') { neg = true; ++pos; }
    else if (s[0] == '+') { ++pos; }
    while (pos < s.size() && s[pos] == '0') ++pos;
    if (pos == s.size()) return "0";
    std::string result;
    if (neg) result.push_back('-');
    result.append(s, pos);
    return result;
}

static void test_roundtrip_zero() {
    Hydra x("0");
    CHECK(x.to_string() == canonicalize("0"), "roundtrip zero");
    Hydra y(x.to_string());
    CHECK(x == y, "roundtrip zero identity");
}

static void test_roundtrip_negative_zero() {
    Hydra x("-0");
    CHECK(x.to_string() == canonicalize("-0"), "roundtrip -0 → 0");
    Hydra y(x.to_string());
    CHECK(x == y, "roundtrip negative zero identity");
}

static void test_roundtrip_int64_boundaries() {
    // INT64_MAX
    std::string s1 = "9223372036854775807";
    Hydra x1(s1);
    CHECK(x1.to_string() == canonicalize(s1), "roundtrip INT64_MAX string");
    Hydra y1(x1.to_string());
    CHECK(x1 == y1, "roundtrip INT64_MAX identity");

    // INT64_MIN
    std::string s2 = "-9223372036854775808";
    Hydra x2(s2);
    CHECK(x2.to_string() == canonicalize(s2), "roundtrip INT64_MIN string");
    Hydra y2(x2.to_string());
    CHECK(x2 == y2, "roundtrip INT64_MIN identity");

    // UINT64_MAX
    std::string s3 = "18446744073709551615";
    Hydra x3(s3);
    CHECK(x3.to_string() == canonicalize(s3), "roundtrip UINT64_MAX string");
    Hydra y3(x3.to_string());
    CHECK(x3 == y3, "roundtrip UINT64_MAX identity");
}

static void test_roundtrip_powers_of_two() {
    for (unsigned shift = 0; shift <= 256; shift += 32) {
        Hydra val = Hydra{1u} << shift;
        std::string s = val.to_string();
        Hydra back(s);
        CHECK(val == back, "roundtrip 2^N identity");
    }
}

static void test_roundtrip_powers_of_ten() {
    // Build 10^N by repeated multiplication.
    Hydra val{1u};
    for (int i = 0; i < 50; ++i) {
        std::string s = val.to_string();
        Hydra back(s);
        CHECK(val == back, "roundtrip 10^N identity");
        val = val * Hydra{10u};
    }
}

static void test_roundtrip_1000_digit_random() {
    // Build a ~1000-digit random number by chaining:
    //   acc = acc * (10^18) + random_chunk
    std::mt19937_64 rng(42);
    Hydra acc{1u};
    for (int i = 0; i < 56; ++i) {  // 56 * 18 ≈ 1008 digits
        uint64_t chunk = rng() % 1000000000000000000ull;
        acc = acc * Hydra{1000000000000000000ull} + Hydra{chunk};
    }
    std::string s = acc.to_string();
    CHECK(s.size() >= 1000, "random number has ~1000 digits");
    Hydra back(s);
    CHECK(acc == back, "roundtrip 1000-digit random");
}

static void test_roundtrip_signed_random_fuzz() {
    // 200 random signed round-trips.
    std::mt19937_64 rng(0xCAFE);
    int pass_count = 0;
    for (int trial = 0; trial < 200; ++trial) {
        // Random width: 1–20 chunks of 18 digits.
        int n_chunks = 1 + static_cast<int>(rng() % 20);
        Hydra acc{1u};
        for (int i = 0; i < n_chunks; ++i) {
            uint64_t chunk = rng() % 1000000000000000000ull;
            acc = acc * Hydra{1000000000000000000ull} + Hydra{chunk};
        }
        bool neg = rng() & 1;
        if (neg) acc = -acc;

        std::string s = acc.to_string();
        Hydra back(s);
        if (acc == back) ++pass_count;
    }
    CHECK(pass_count == 200, "signed random fuzz: all 200 round-trips");
}

// ── ostream operator<< test ────────────────────────────────────────

static void test_ostream_operator() {
    Hydra x("-12345678901234567890");
    std::ostringstream oss;
    oss << x;
    CHECK(oss.str() == x.to_string(), "ostream << matches to_string");
}

static void test_ostream_zero() {
    Hydra x{0u};
    std::ostringstream oss;
    oss << x;
    CHECK(oss.str() == "0", "ostream << zero");
}

// ── Chunked to_string correctness ──────────────────────────────────

static void test_tostring_medium() {
    // A 2-limb number that goes through the chunked path.
    // 2^64 = 18446744073709551616
    Hydra x = Hydra{1u} << 64;
    CHECK(x.to_string() == "18446744073709551616", "to_string 2^64");
}

static void test_tostring_large_known() {
    // 2^128 = 340282366920938463463374607431768211456
    Hydra x = Hydra{1u} << 128;
    CHECK(x.to_string() == "340282366920938463463374607431768211456",
          "to_string 2^128");
}

static void test_tostring_vs_parse_cross_check() {
    // Build a value via arithmetic, convert to string, parse back.
    Hydra a("999999999999999999999999999999999999");
    Hydra b = a + Hydra{1u};
    CHECK(b.to_string() == "1000000000000000000000000000000000000",
          "to_string cross-check with parse");
}

// ── Number theory: abs, operator/, operator%, gcd, extended_gcd, pow_mod ──

using hydra::abs;
using hydra::gcd;
using hydra::EGCDResult;
using hydra::extended_gcd;
using hydra::pow_mod;

// --- abs ---
static void test_abs_positive() {
    CHECK(abs(Hydra{42u}) == Hydra{42u}, "abs(42) == 42");
}
static void test_abs_negative() {
    CHECK(abs(Hydra{-7}) == Hydra{7u}, "abs(-7) == 7");
}
static void test_abs_zero() {
    CHECK(abs(Hydra{0u}) == Hydra{0u}, "abs(0) == 0");
}

// --- operator/ and operator% ---
static void test_div_mod_operators() {
    Hydra a{100u}, b{7u};
    CHECK(a / b == Hydra{14u}, "100 / 7 == 14");
    CHECK(a % b == Hydra{2u}, "100 % 7 == 2");
}

static void test_divmod_assign() {
    Hydra a{100u};
    a /= Hydra{10u};
    CHECK(a == Hydra{10u}, "100 /= 10 == 10");
    Hydra b{100u};
    b %= Hydra{30u};
    CHECK(b == Hydra{10u}, "100 %= 30 == 10");
}

// --- gcd: zero cases ---
static void test_gcd_zero_zero() {
    CHECK(gcd(Hydra{0u}, Hydra{0u}) == Hydra{0u}, "gcd(0, 0) == 0");
}
static void test_gcd_zero_x() {
    CHECK(gcd(Hydra{0u}, Hydra{42u}) == Hydra{42u}, "gcd(0, 42) == 42");
}
static void test_gcd_x_zero() {
    CHECK(gcd(Hydra{42u}, Hydra{0u}) == Hydra{42u}, "gcd(42, 0) == 42");
}
static void test_gcd_zero_neg() {
    CHECK(gcd(Hydra{0u}, Hydra{-15}) == Hydra{15u}, "gcd(0, -15) == 15");
}

// --- gcd: positive / negative combinations ---
static void test_gcd_positive() {
    CHECK(gcd(Hydra{12u}, Hydra{8u}) == Hydra{4u}, "gcd(12, 8) == 4");
}
static void test_gcd_neg_pos() {
    CHECK(gcd(Hydra{-12}, Hydra{8u}) == Hydra{4u}, "gcd(-12, 8) == 4");
}
static void test_gcd_pos_neg() {
    CHECK(gcd(Hydra{12u}, Hydra{-8}) == Hydra{4u}, "gcd(12, -8) == 4");
}
static void test_gcd_neg_neg() {
    CHECK(gcd(Hydra{-12}, Hydra{-8}) == Hydra{4u}, "gcd(-12, -8) == 4");
}

// --- gcd: co-prime values ---
static void test_gcd_coprime() {
    CHECK(gcd(Hydra{17u}, Hydra{13u}) == Hydra{1u}, "gcd(17, 13) == 1");
}
static void test_gcd_coprime_large() {
    CHECK(gcd(Hydra{97u}, Hydra{89u}) == Hydra{1u}, "gcd(97, 89) == 1");
}

// --- gcd: powers of two ---
static void test_gcd_powers_of_two() {
    CHECK(gcd(Hydra{64u}, Hydra{16u}) == Hydra{16u}, "gcd(64, 16) == 16");
}
static void test_gcd_power_of_two_and_odd() {
    CHECK(gcd(Hydra{128u}, Hydra{35u}) == Hydra{1u}, "gcd(128, 35) == 1");
}

// --- gcd: same value ---
static void test_gcd_same() {
    CHECK(gcd(Hydra{99u}, Hydra{99u}) == Hydra{99u}, "gcd(99, 99) == 99");
}

// --- gcd: large parsed decimal inputs ---
static void test_gcd_large_decimal() {
    Hydra a("123456789012345678901234567890");
    Hydra b("987654321098765432109876543210");
    Hydra g = gcd(a, b);
    // Verify g divides both a and b.
    CHECK(a % g == Hydra{0u}, "gcd divides a");
    CHECK(b % g == Hydra{0u}, "gcd divides b");
    // Verify g > 0
    CHECK(g > Hydra{0u}, "gcd is positive for nonzero inputs");
}
static void test_gcd_large_coprime() {
    // Two large primes (well, likely coprime numbers)
    Hydra a("100000000000000000039");  // known prime
    Hydra b("100000000000000000129");  // known prime
    Hydra g = gcd(a, b);
    CHECK(g == Hydra{1u}, "large coprime gcd == 1");
}

// --- extended_gcd ---
static void test_egcd_basic() {
    auto [g, x, y] = extended_gcd(Hydra{35u}, Hydra{15u});
    CHECK(g == Hydra{5u}, "egcd(35,15).gcd == 5");
    CHECK(Hydra{35u} * x + Hydra{15u} * y == g, "35*x + 15*y == 5");
}
static void test_egcd_coprime() {
    auto [g, x, y] = extended_gcd(Hydra{17u}, Hydra{13u});
    CHECK(g == Hydra{1u}, "egcd(17,13).gcd == 1");
    CHECK(Hydra{17u} * x + Hydra{13u} * y == g, "17*x + 13*y == 1");
}
static void test_egcd_with_zero() {
    auto [g, x, y] = extended_gcd(Hydra{0u}, Hydra{5u});
    CHECK(g == Hydra{5u}, "egcd(0,5).gcd == 5");
    CHECK(Hydra{0u} * x + Hydra{5u} * y == g, "0*x + 5*y == 5");
}
static void test_egcd_signed() {
    auto [g, x, y] = extended_gcd(Hydra{-35}, Hydra{15u});
    CHECK(g == Hydra{5u}, "egcd(-35,15).gcd == 5");
    CHECK(Hydra{-35} * x + Hydra{15u} * y == g, "-35*x + 15*y == 5");
}
static void test_egcd_both_negative() {
    auto [g, x, y] = extended_gcd(Hydra{-24}, Hydra{-18});
    CHECK(g == Hydra{6u}, "egcd(-24,-18).gcd == 6");
    CHECK(Hydra{-24} * x + Hydra{-18} * y == g, "-24*x + -18*y == 6");
}
static void test_egcd_large() {
    Hydra a("123456789012345678901234567890");
    Hydra b("987654321098765432109876543210");
    auto [g, x, y] = extended_gcd(a, b);
    CHECK(a * x + b * y == g, "egcd invariant holds for large values");
    CHECK(!g.is_negative(), "egcd gcd is non-negative");
}

// --- pow_mod ---
static void test_pow_mod_basic() {
    // 2^10 mod 1000 = 1024 mod 1000 = 24
    CHECK(pow_mod(Hydra{2u}, Hydra{10u}, Hydra{1000u}) == Hydra{24u},
          "2^10 mod 1000 == 24");
}
static void test_pow_mod_zero_exp() {
    // x^0 mod m = 1 (when m > 1)
    CHECK(pow_mod(Hydra{7u}, Hydra{0u}, Hydra{5u}) == Hydra{1u},
          "7^0 mod 5 == 1");
}
static void test_pow_mod_one_mod() {
    // x^e mod 1 = 0
    CHECK(pow_mod(Hydra{7u}, Hydra{100u}, Hydra{1u}) == Hydra{0u},
          "7^100 mod 1 == 0");
}
static void test_pow_mod_large_exp() {
    // Fermat's little theorem: a^(p-1) ≡ 1 (mod p) for prime p
    Hydra p{97u};
    CHECK(pow_mod(Hydra{3u}, Hydra{96u}, p) == Hydra{1u},
          "Fermat: 3^96 mod 97 == 1");
}
static void test_pow_mod_negative_base() {
    // base = -2, normalized: (-2) % 5 = -2, + 5 = 3.  3^3 = 27, 27 % 5 = 2.
    Hydra r = pow_mod(Hydra{-2}, Hydra{3u}, Hydra{5u});
    CHECK(r == Hydra{2u}, "(-2)^3 mod 5 == 2");
}
static void test_pow_mod_throws_zero_mod() {
    bool threw = false;
    try { (void)pow_mod(Hydra{2u}, Hydra{3u}, Hydra{0u}); }
    catch (const std::domain_error&) { threw = true; }
    CHECK(threw, "pow_mod throws on mod == 0");
}
static void test_pow_mod_throws_negative_exp() {
    bool threw = false;
    try { (void)pow_mod(Hydra{2u}, Hydra{-1}, Hydra{5u}); }
    catch (const std::domain_error&) { threw = true; }
    CHECK(threw, "pow_mod throws on negative exp");
}
static void test_pow_mod_throws_negative_mod() {
    bool threw = false;
    try { (void)pow_mod(Hydra{2u}, Hydra{3u}, Hydra{-5}); }
    catch (const std::domain_error&) { threw = true; }
    CHECK(threw, "pow_mod throws on negative mod");
}

// --- Toy RSA showcase ---
static void test_rsa_toy() {
    Hydra n{3233u};
    Hydra e{17u};
    Hydra d{2753u};
    Hydra m{65u};

    Hydra c = pow_mod(m, e, n);
    Hydra m2 = pow_mod(c, d, n);
    CHECK(m2 == m, "RSA toy: decrypt(encrypt(m)) == m");
}
static void test_rsa_toy_all_messages() {
    // Verify for a few different messages
    Hydra n{3233u}, e{17u}, d{2753u};
    uint64_t msgs[] = {0, 1, 2, 42, 65, 100, 1000, 3232};
    for (uint64_t mi : msgs) {
        Hydra m{mi};
        Hydra c = pow_mod(m, e, n);
        Hydra m2 = pow_mod(c, d, n);
        CHECK(m2 == m, "RSA toy roundtrip for all messages");
    }
}
static void test_pow_mod_large_parsed() {
    // Larger modulus from parsed decimals
    // p=61, q=53 → n=3233, e=17, d=2753 (same as above but via parse)
    Hydra n("3233"), e("17"), d("2753"), m("42");
    Hydra c = pow_mod(m, e, n);
    Hydra m2 = pow_mod(c, d, n);
    CHECK(m2 == m, "RSA parsed decimal roundtrip");
}

// ─────────────────────────────────────────────────────────
// Montgomery multiplication path tests
// ─────────────────────────────────────────────────────────

static void test_montgomery_n0inv() {
    // n0inv * n0 ≡ -1 mod 2^64  →  (n0inv + 1) * n0 + (n0 - 1) ≡ 0 mod 2^64
    // Actually: n0inv = -n^{-1} mod 2^64, so n0inv * n0 ≡ -1 mod 2^64.
    uint64_t n0 = 3233;  // odd
    uint64_t inv = hydra::detail::montgomery_n0inv(n0);
    // Check: inv * n0 + 1 ≡ 0 mod 2^64
    uint64_t product = inv * n0;
    CHECK(product == static_cast<uint64_t>(-1), "n0inv * n0 == -1 mod 2^64 (small)");

    // Larger odd number
    uint64_t n1 = 0xFFFFFFFFFFFFFFFDull;  // 2^64-3
    uint64_t inv1 = hydra::detail::montgomery_n0inv(n1);
    uint64_t p1 = inv1 * n1;
    CHECK(p1 == static_cast<uint64_t>(-1), "n0inv * n0 == -1 mod 2^64 (large)");

    // Random odd
    uint64_t n2 = 0xDEADBEEFCAFE0001ull;
    uint64_t inv2 = hydra::detail::montgomery_n0inv(n2);
    uint64_t p2 = inv2 * n2;
    CHECK(p2 == static_cast<uint64_t>(-1), "n0inv * n0 == -1 mod 2^64 (random)");
}

static void test_montgomery_redc_basic() {
    // Test REDC with a known small case.
    // mod = 17 (single limb), k = 1, R = 2^64.
    // n0inv = -17^{-1} mod 2^64.
    uint64_t mod_arr[1] = { 17 };
    uint64_t n0inv = hydra::detail::montgomery_n0inv(17);

    // REDC(T) = T * R^{-1} mod 17.
    // T = 5 → should give 5 * R^{-1} mod 17.
    // R = 2^64. R mod 17 = 2^64 mod 17.
    // 2^4 = 16 ≡ -1 mod 17, so 2^8 ≡ 1 mod 17, ...,
    // 2^64 = (2^8)^8 ≡ 1 mod 17. So R ≡ 1 mod 17, R^{-1} ≡ 1 mod 17.
    // REDC(5) = 5 * 1 = 5 mod 17.
    uint64_t work[3] = { 5, 0, 0 };  // 2k+1 = 3 limbs
    uint64_t out[1];
    hydra::detail::montgomery_redc(work, 1, mod_arr, n0inv, out);
    CHECK(out[0] == 5, "REDC basic: T=5 mod 17 with R=2^64≡1");
}

static void test_montgomery_context_build() {
    // Build context for mod=3233 (used in RSA toy)
    uint64_t mod_arr[1] = { 3233 };
    hydra::MontgomeryContext ctx = hydra::MontgomeryContext::build(mod_arr, 1);
    ctx.compute_r_sq();

    // Verify n0inv
    uint64_t p = ctx.n0inv * 3233ull;
    CHECK(p == static_cast<uint64_t>(-1), "MontCtx n0inv correct for 3233");

    // Verify r_sq = R^2 mod n = (2^64)^2 mod 3233 = 2^128 mod 3233
    // We can cross-check with Hydra: Hydra{1} << 128 then mod 3233
    Hydra R128 = Hydra{1u} << 128;
    Hydra expected = R128.mod(Hydra{3233u});
    CHECK(ctx.r_sq[0] == expected.limb_view().ptr[0],
          "MontCtx r_sq correct for 3233");
}

static void test_montgomery_roundtrip_small() {
    // Test that to_montgomery → from_montgomery is identity for a value.
    uint64_t mod_arr[1] = { 3233 };
    hydra::MontgomeryContext ctx = hydra::MontgomeryContext::build(mod_arr, 1);
    ctx.compute_r_sq();

    uint64_t val = 42;
    uint64_t mont[1], back[1];
    uint64_t work[3];  // 2k+1

    ctx.to_montgomery(&val, 1, mont, work);
    ctx.from_montgomery(mont, back, work);

    CHECK(back[0] == 42, "Montgomery roundtrip: 42 mod 3233");
}

static void test_montgomery_mul_basic() {
    // Test montgomery_mul: a*b*R^{-1} mod n in Montgomery space.
    // Use mod=3233, a=42, b=100.
    // In Montgomery form: a_mont = 42*R mod 3233, b_mont = 100*R mod 3233
    // mont_mul(a_mont, b_mont) = a*b*R mod 3233
    // Converting back: a*b mod 3233 = 4200 mod 3233 = 967.
    uint64_t mod_arr[1] = { 3233 };
    hydra::MontgomeryContext ctx = hydra::MontgomeryContext::build(mod_arr, 1);
    ctx.compute_r_sq();

    uint64_t a_val = 42, b_val = 100;
    uint64_t a_mont[1], b_mont[1], prod_mont[1], result[1];
    uint64_t work[3];

    ctx.to_montgomery(&a_val, 1, a_mont, work);
    ctx.to_montgomery(&b_val, 1, b_mont, work);

    hydra::detail::montgomery_mul(a_mont, b_mont, 1, mod_arr,
                                  ctx.n0inv, prod_mont, work);

    ctx.from_montgomery(prod_mont, result, work);

    CHECK(result[0] == (42u * 100u) % 3233u,
          "Montgomery mul basic: 42*100 mod 3233");
}

static void test_pow_mod_montgomery_small() {
    // Same as existing pow_mod tests but explicitly routing through Montgomery
    Hydra r = hydra::pow_mod(Hydra{2u}, Hydra{10u}, Hydra{1000u + 9u});
    // 2^10 = 1024, 1024 mod 1009 = 15
    CHECK(r == Hydra{15u}, "pow_mod Montgomery: 2^10 mod 1009 = 15");
}

static Hydra make_random_hydra(uint32_t n_bits, uint64_t seed) {
    uint32_t n_limbs = (n_bits + 63) / 64;
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> limbs(n_limbs);
    for (auto& l : limbs) l = rng();
    uint32_t top_bits = n_bits % 64;
    if (top_bits != 0)
        limbs.back() &= (1ull << top_bits) - 1;
    if (top_bits != 0)
        limbs.back() |= (1ull << (top_bits - 1));
    else
        limbs.back() |= (1ull << 63);
    limbs[0] |= 1u;  // make odd
    return Hydra::from_limbs(limbs.data(), n_limbs);
}

static void test_pow_mod_montgomery_256bit() {
    Hydra base = make_random_hydra(256, 100);
    Hydra exp  = make_random_hydra(256, 200);
    Hydra mod  = make_random_hydra(256, 300);

    Hydra result = hydra::pow_mod(base, exp, mod);
    // Cross-check with naive path
    Hydra naive = hydra::pow_mod_naive(base % mod, exp, mod);
    CHECK(result == naive, "pow_mod Montgomery 256-bit cross-check");
}

static void test_pow_mod_montgomery_512bit() {
    Hydra base = make_random_hydra(512, 400);
    Hydra exp  = make_random_hydra(512, 500);
    Hydra mod  = make_random_hydra(512, 600);

    Hydra result = hydra::pow_mod(base, exp, mod);
    Hydra naive = hydra::pow_mod_naive(base % mod, exp, mod);
    CHECK(result == naive, "pow_mod Montgomery 512-bit cross-check");
}

static void test_pow_mod_montgomery_1024bit() {
    Hydra base = make_random_hydra(1024, 700);
    Hydra exp  = make_random_hydra(1024, 800);
    Hydra mod  = make_random_hydra(1024, 900);

    Hydra result = hydra::pow_mod(base, exp, mod);
    Hydra naive = hydra::pow_mod_naive(base % mod, exp, mod);
    CHECK(result == naive, "pow_mod Montgomery 1024-bit cross-check");
}

static void test_pow_mod_montgomery_vs_naive() {
    // 10 random cases across various sizes
    uint64_t seed = 42;
    for (uint32_t bits : {64, 128, 192, 256, 384, 512}) {
        Hydra base = make_random_hydra(bits, seed++);
        Hydra exp  = make_random_hydra(bits, seed++);
        Hydra mod  = make_random_hydra(bits, seed++);

        Hydra mont = hydra::pow_mod(base, exp, mod);
        Hydra naive = hydra::pow_mod_naive(base % mod, exp, mod);
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "pow_mod mont vs naive at %u bits", bits);
        CHECK(mont == naive, msg);
    }
}

static void test_pow_mod_montgomery_fermat() {
    // Fermat's little theorem: a^(p-1) ≡ 1 mod p for prime p, gcd(a,p)=1
    // p = 104729 (prime), a = 42
    Hydra p{104729u};
    Hydra a{42u};
    Hydra result = hydra::pow_mod(a, p - 1, p);
    CHECK(result == Hydra{1u}, "Fermat's little theorem: 42^(p-1) mod p = 1");
}

static void test_pow_mod_montgomery_rsa_256bit() {
    // Generate RSA-like test with 128-bit primes (p, q)
    // We'll use known primes and test encrypt/decrypt roundtrip.
    // Small primes for tractability:
    // p = 104729, q = 104743 → n = p*q = 10965066847
    Hydra p{104729u}, q{104743u};
    Hydra n = p * q;
    Hydra phi = (p - 1) * (q - 1);
    // e = 65537 (standard)
    Hydra e{65537u};
    // d = e^{-1} mod phi = modular inverse
    auto eg = hydra::extended_gcd(e, phi);
    Hydra d = eg.x;
    if (d.is_negative()) d = d + phi;

    // Verify e*d ≡ 1 mod phi
    CHECK((e * d) % phi == Hydra{1u}, "RSA: e*d ≡ 1 mod phi");

    // Encrypt and decrypt
    Hydra msg{12345u};
    Hydra cipher = hydra::pow_mod(msg, e, n);
    Hydra decrypted = hydra::pow_mod(cipher, d, n);
    CHECK(decrypted == msg, "RSA roundtrip: decrypt(encrypt(msg)) == msg");
}

static void test_pow_mod_even_mod_fallback() {
    // Even modulus should use the naive fallback
    Hydra result = hydra::pow_mod(Hydra{3u}, Hydra{7u}, Hydra{100u});
    // 3^7 = 2187, 2187 mod 100 = 87
    CHECK(result == Hydra{87u}, "pow_mod even mod fallback: 3^7 mod 100 = 87");
}

static void test_pow_mod_montgomery_base_larger_than_mod() {
    // base > mod; should reduce first
    Hydra result = hydra::pow_mod(Hydra{1000u}, Hydra{3u}, Hydra{7u});
    // 1000 mod 7 = 6, 6^3 = 216, 216 mod 7 = 6
    CHECK(result == Hydra{6u}, "pow_mod Montgomery: base > mod");
}

static void test_pow_mod_montgomery_mod_equals_3() {
    // Smallest interesting odd modulus
    Hydra result = hydra::pow_mod(Hydra{5u}, Hydra{100u}, Hydra{3u});
    // 5 ≡ 2 mod 3, 2^1=2, 2^2=1, so 2^100 = (2^2)^50 = 1^50 = 1 mod 3
    CHECK(result == Hydra{1u}, "pow_mod Montgomery: 5^100 mod 3 = 1");
}

// ── Additional pow_mod edge cases and squaring-specific tests ──

static void test_pow_mod_base_zero() {
    // 0^n mod m = 0 for n > 0
    CHECK(hydra::pow_mod(Hydra{0u}, Hydra{5u}, Hydra{7u}) == Hydra{0u},
          "pow_mod: 0^5 mod 7 = 0");
    CHECK(hydra::pow_mod(Hydra{0u}, Hydra{1u}, Hydra{13u}) == Hydra{0u},
          "pow_mod: 0^1 mod 13 = 0");
}

static void test_pow_mod_exp_zero() {
    // a^0 mod m = 1 for m > 1
    CHECK(hydra::pow_mod(Hydra{999u}, Hydra{0u}, Hydra{7u}) == Hydra{1u},
          "pow_mod: 999^0 mod 7 = 1");
    CHECK(hydra::pow_mod(Hydra{0u}, Hydra{0u}, Hydra{5u}) == Hydra{1u},
          "pow_mod: 0^0 mod 5 = 1");
}

static void test_pow_mod_mod_one() {
    // a^n mod 1 = 0 for all a, n
    CHECK(hydra::pow_mod(Hydra{42u}, Hydra{100u}, Hydra{1u}) == Hydra{0u},
          "pow_mod: 42^100 mod 1 = 0");
    CHECK(hydra::pow_mod(Hydra{0u}, Hydra{0u}, Hydra{1u}) == Hydra{0u},
          "pow_mod: 0^0 mod 1 = 0");
}

static void test_pow_mod_base_less_than_mod() {
    // base < mod, odd mod
    Hydra r = hydra::pow_mod(Hydra{3u}, Hydra{4u}, Hydra{11u});
    // 3^4 = 81, 81 mod 11 = 4
    CHECK(r == Hydra{4u}, "pow_mod: 3^4 mod 11 = 4 (base < mod)");
}

static void test_pow_mod_base_greater_than_mod() {
    // base > mod, odd mod — should reduce first
    Hydra r = hydra::pow_mod(Hydra{100u}, Hydra{3u}, Hydra{13u});
    // 100 mod 13 = 9, 9^3 = 729, 729 mod 13 = 1
    CHECK(r == Hydra{1u}, "pow_mod: 100^3 mod 13 = 1 (base > mod)");
}

static void test_pow_mod_montgomery_sqr_specific() {
    // Test specifically that squaring path gives correct results
    // by verifying a^2 mod m via pow_mod matches (a*a) mod m
    Hydra a("99999999999999999999999999999999");  // ~106 bits
    Hydra mod("1000000000000000000000000000000007");  // odd 110-bit modulus
    Hydra exp{2u};
    Hydra result = hydra::pow_mod(a, exp, mod);
    Hydra expected = (a * a) % mod;
    CHECK(result == expected, "pow_mod sqr specific: a^2 matches a*a mod m");
}

static void test_pow_mod_montgomery_sqr_chain() {
    // a^4 = (a^2)^2 — exercises multiple squarings in sequence
    Hydra a("12345678901234567890123456789012345678901234567890");
    Hydra mod("99999999999999999999999999999999999999999999999989");  // odd
    Hydra r4 = hydra::pow_mod(a, Hydra{4u}, mod);
    Hydra manual = (a * a) % mod;
    manual = (manual * manual) % mod;
    CHECK(r4 == manual, "pow_mod sqr chain: a^4 matches manual squaring");
}

static void test_pow_mod_random_odd_moduli_near_threshold() {
    // Test with moduli near MONTGOMERY_MAX_LIMBS (48 limbs = 3072 bits)
    // Use 2048-bit (32 limbs) and 3072-bit (48 limbs) to exercise near-threshold
    std::mt19937_64 rng(0xDEAD'BEEF);

    auto make_odd_hydra = [&](uint32_t n_limbs) {
        std::vector<uint64_t> limbs(n_limbs);
        for (auto& l : limbs) l = rng();
        limbs[0] |= 1u;  // ensure odd
        limbs.back() |= (1ull << 63);  // ensure top bit set
        return Hydra::from_limbs(limbs.data(), n_limbs);
    };

    for (uint32_t n_limbs : {32u, 48u}) {
        Hydra base = make_odd_hydra(4);  // small base
        Hydra exp{1000u};               // modest exponent
        Hydra mod = make_odd_hydra(n_limbs);

        Hydra mont = hydra::pow_mod(base, exp, mod);
        Hydra naive = hydra::pow_mod_naive(base % mod, exp, mod);
        CHECK(mont == naive,
              "pow_mod near threshold cross-check");
    }
}

static void test_pow_mod_sliding_window_coverage() {
    // Test with exponents that exercise different window patterns:
    // - exp with runs of zeros
    // - exp with runs of ones
    // - small odd exp
    // - exp = 2 (single squaring)
    // - exp = 3 (square + multiply)
    Hydra base("123456789012345678901234567");
    Hydra mod("999999999999999999999999999999999999961");  // odd prime-ish

    for (uint64_t e : {2u, 3u, 7u, 15u, 16u, 17u, 255u, 256u, 1023u, 65535u}) {
        Hydra result = hydra::pow_mod(base, Hydra{e}, mod);
        Hydra naive = hydra::pow_mod_naive(base % mod, Hydra{e}, mod);
        CHECK(result == naive,
              "pow_mod sliding window cross-check");
    }
}

static void test_pow_mod_montgomery_2048bit_cross() {
    // 2048-bit cross-check against naive
    auto make = [](uint32_t bits, uint64_t seed) {
        uint32_t n_limbs = (bits + 63) / 64;
        std::mt19937_64 rng(seed);
        std::vector<uint64_t> limbs(n_limbs);
        for (auto& l : limbs) l = rng();
        limbs[0] |= 1u;
        limbs.back() |= (1ull << 63);
        return Hydra::from_limbs(limbs.data(), n_limbs);
    };

    Hydra base = make(256, 42);      // 256-bit base
    Hydra exp  = make(64, 99);       // 64-bit exponent (keep naive tractable)
    Hydra mod  = make(2048, 7777);   // 2048-bit odd modulus

    Hydra mont = hydra::pow_mod(base, exp, mod);
    Hydra naive = hydra::pow_mod_naive(base % mod, exp, mod);
    CHECK(mont == naive, "pow_mod 2048-bit Montgomery cross-check");
}

// ── Fused CIOS Montgomery tests ──────────────────────────────

// Direct kernel-level test: fused mul matches separate mul+REDC
static void test_fused_montgomery_mul_vs_separate() {
    // Small modulus: 3233 (known from existing tests)
    uint64_t mod[] = {3233};
    uint32_t k = 1;
    uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);

    uint64_t a[] = {42};
    uint64_t b[] = {100};

    uint64_t out_sep[1] = {0}, out_fused[1] = {0};
    uint64_t work_sep[3] = {0};  // 2k+1
    uint64_t work_fused[3] = {0};  // 2k+1

    hydra::detail::montgomery_mul(a, b, k, mod, n0inv, out_sep, work_sep);
    hydra::detail::montgomery_mul_fused(a, b, k, mod, n0inv, out_fused, work_fused);
    CHECK(out_sep[0] == out_fused[0], "fused mul matches separate at k=1");
}

// Multi-limb fused vs separate
static void test_fused_montgomery_mul_multi_limb() {
    // 256-bit modulus (4 limbs)
    std::mt19937_64 rng(12345);
    uint64_t mod[4], a[4], b[4];
    for (auto& l : mod) l = rng();
    mod[0] |= 1;  // odd
    for (auto& l : a) l = rng();
    for (auto& l : b) l = rng();

    uint32_t k = 4;
    uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);

    uint64_t out_sep[4] = {}, out_fused[4] = {};
    uint64_t work_sep[9] = {};   // 2k+1
    uint64_t work_fused[6] = {}; // k+2

    hydra::detail::montgomery_mul(a, b, k, mod, n0inv, out_sep, work_sep);
    hydra::detail::montgomery_mul_fused(a, b, k, mod, n0inv, out_fused, work_fused);

    bool match = true;
    for (uint32_t i = 0; i < k; ++i) {
        if (out_sep[i] != out_fused[i]) { match = false; break; }
    }
    CHECK(match, "fused mul matches separate at k=4 (256-bit)");
}

// Fused squaring cross-check
static void test_fused_montgomery_sqr_cross() {
    std::mt19937_64 rng(67890);
    uint64_t mod[8], a[8];
    for (auto& l : mod) l = rng();
    mod[0] |= 1;
    for (auto& l : a) l = rng();

    uint32_t k = 8;
    uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);

    uint64_t out_sep[8] = {}, out_fused[8] = {};
    uint64_t work_sep[17] = {};   // 2k+1
    uint64_t work_fused[10] = {}; // k+2

    hydra::detail::montgomery_sqr(a, k, mod, n0inv, out_sep, work_sep);
    hydra::detail::montgomery_sqr_fused(a, k, mod, n0inv, out_fused, work_fused);

    bool match = true;
    for (uint32_t i = 0; i < k; ++i) {
        if (out_sep[i] != out_fused[i]) { match = false; break; }
    }
    CHECK(match, "fused sqr matches separate sqr at k=8 (512-bit)");
}

// Random odd moduli at key widths: 1024, 2048, 4096
static void test_fused_pow_mod_random_widths() {
    auto make = [](uint32_t bits, uint64_t seed) {
        uint32_t n_limbs = (bits + 63) / 64;
        std::mt19937_64 rng(seed);
        std::vector<uint64_t> limbs(n_limbs);
        for (auto& l : limbs) l = rng();
        limbs[0] |= 1u;
        limbs.back() |= (1ull << 63);
        return Hydra::from_limbs(limbs.data(), n_limbs);
    };

    // For each width: fused pow_mod vs naive reference
    {
        Hydra base = make(256, 42 + 1024);
        Hydra exp_val = make(64, 99 + 1024);
        Hydra mod_val = make(1024, 7777 + 1024);
        Hydra result = hydra::pow_mod(base, exp_val, mod_val);
        Hydra naive = hydra::pow_mod_naive(base % mod_val, exp_val, mod_val);
        CHECK(result == naive, "fused pow_mod matches naive at 1024 bits");
    }
    {
        Hydra base = make(256, 42 + 2048);
        Hydra exp_val = make(64, 99 + 2048);
        Hydra mod_val = make(2048, 7777 + 2048);
        Hydra result = hydra::pow_mod(base, exp_val, mod_val);
        Hydra naive = hydra::pow_mod_naive(base % mod_val, exp_val, mod_val);
        CHECK(result == naive, "fused pow_mod matches naive at 2048 bits");
    }
}

// Edge: Montgomery boundary width (k=64, 4096 bits)
static void test_fused_pow_mod_4096bit() {
    auto make = [](uint32_t bits, uint64_t seed) {
        uint32_t n_limbs = (bits + 63) / 64;
        std::mt19937_64 rng(seed);
        std::vector<uint64_t> limbs(n_limbs);
        for (auto& l : limbs) l = rng();
        limbs[0] |= 1u;
        limbs.back() |= (1ull << 63);
        return Hydra::from_limbs(limbs.data(), n_limbs);
    };

    Hydra base = make(256, 1111);
    Hydra exp_val = make(32, 2222);    // 32-bit exp for tractability
    Hydra mod_val = make(4096, 3333);

    Hydra result = hydra::pow_mod(base, exp_val, mod_val);
    Hydra naive = hydra::pow_mod_naive(base % mod_val, exp_val, mod_val);
    CHECK(result == naive, "fused pow_mod matches naive at 4096 bits");
}

// Fused kernel with k=1 edge case
static void test_fused_pow_mod_single_limb() {
    // pow(2, 10, 1009) = 15 (Fermat)
    Hydra r = hydra::pow_mod(Hydra{2u}, Hydra{10u}, Hydra{1009u});
    CHECK(r == Hydra{15u}, "fused pow_mod single-limb: 2^10 mod 1009 = 15");
}

// Cross-check: fused mul kernel at all relevant k values
static void test_fused_montgomery_mul_sweep() {
    std::mt19937_64 rng(55555);
    for (uint32_t k : {1u, 2u, 3u, 4u, 8u, 16u, 32u, 64u}) {
        std::vector<uint64_t> mod(k), a(k), b(k);
        for (auto& l : mod) l = rng();
        mod[0] |= 1;
        for (auto& l : a) l = rng();
        for (auto& l : b) l = rng();

        uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);

        std::vector<uint64_t> out_sep(k, 0), out_fused(k, 0);
        std::vector<uint64_t> work_sep(2 * k + 1, 0);
        std::vector<uint64_t> work_fused(k + 2, 0);

        hydra::detail::montgomery_mul(a.data(), b.data(), k, mod.data(), n0inv,
                                       out_sep.data(), work_sep.data());
        hydra::detail::montgomery_mul_fused(a.data(), b.data(), k, mod.data(), n0inv,
                                             out_fused.data(), work_fused.data());

        bool match = true;
        for (uint32_t i = 0; i < k; ++i) {
            if (out_sep[i] != out_fused[i]) { match = false; break; }
        }
        char msg[64];
        std::snprintf(msg, sizeof(msg), "fused mul sweep k=%u", k);
        CHECK(match, msg);
    }
}

// Fused with values near Montgomery width boundaries
static void test_fused_pow_mod_boundary_widths() {
    // Test at k=48 (old threshold) and k=63 (near max)
    auto make = [](uint32_t bits, uint64_t seed) {
        uint32_t n_limbs = (bits + 63) / 64;
        std::mt19937_64 rng(seed);
        std::vector<uint64_t> limbs(n_limbs);
        for (auto& l : limbs) l = rng();
        limbs[0] |= 1u;
        limbs.back() |= (1ull << 63);
        return Hydra::from_limbs(limbs.data(), n_limbs);
    };

    {
        uint32_t k = 48;
        uint32_t bits = k * 64;
        Hydra base = make(128, 42 + k);
        Hydra exp_val = make(32, 99 + k);
        Hydra mod_val = make(bits, 7777 + k);
        Hydra result = hydra::pow_mod(base, exp_val, mod_val);
        Hydra naive = hydra::pow_mod_naive(base % mod_val, exp_val, mod_val);
        CHECK(result == naive, "fused pow_mod boundary k=48");
    }
    {
        uint32_t k = 63;
        uint32_t bits = k * 64;
        Hydra base = make(128, 42 + k);
        Hydra exp_val = make(32, 99 + k);
        Hydra mod_val = make(bits, 7777 + k);
        Hydra result = hydra::pow_mod(base, exp_val, mod_val);
        Hydra naive = hydra::pow_mod_naive(base % mod_val, exp_val, mod_val);
        CHECK(result == naive, "fused pow_mod boundary k=63");
    }
}

// ── Karatsuba-backed Montgomery multiply tests ──────────────

// Direct kernel test: montgomery_mul_karatsuba vs montgomery_mul_fused
static void test_karatsuba_mont_mul_vs_fused() {
    // Test at k=32 (2048-bit), exactly the KARATSUBA_MONT_THRESHOLD
    std::mt19937_64 rng(0xCA4A0001ull);
    const uint32_t k = 32;
    std::vector<uint64_t> mod(k), a(k), b(k);
    for (auto& l : mod) l = rng();
    mod[0] |= 1u;  // odd modulus
    mod[k-1] |= (1ull << 63);  // ensure k limbs used

    uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);

    // Random operands in [0, mod)
    for (auto& l : a) l = rng();
    for (auto& l : b) l = rng();

    // Compute via fused CIOS
    std::vector<uint64_t> out_fused(k), work_fused(k + 2, 0);
    hydra::detail::montgomery_mul_fused(a.data(), b.data(), k, mod.data(),
                                         n0inv, out_fused.data(), work_fused.data());

    // Compute via Karatsuba + REDC
    uint32_t n_padded = 1;
    while (n_padded < k) n_padded <<= 1;
    std::vector<uint64_t> out_kara(k), work_kara(2 * k + 1, 0);
    std::vector<uint64_t> pa(n_padded, 0), pb(n_padded, 0);
    std::vector<uint64_t> kbuf(2 * n_padded, 0);
    hydra::detail::ScratchWorkspace ws;
    ws.reserve_limbs(hydra::detail::karatsuba_scratch_limbs(n_padded));
    hydra::detail::montgomery_mul_karatsuba(
        a.data(), b.data(), k, mod.data(), n0inv,
        out_kara.data(), work_kara.data(),
        pa.data(), pb.data(), kbuf.data(), n_padded, ws);

    bool match = true;
    for (uint32_t i = 0; i < k; ++i) {
        if (out_fused[i] != out_kara[i]) { match = false; break; }
    }
    CHECK(match, "karatsuba mont mul vs fused at k=32");
}

// Direct kernel test at k=64 (4096-bit)
static void test_karatsuba_mont_mul_vs_fused_4096() {
    std::mt19937_64 rng(0xCA4A0002ull);
    const uint32_t k = 64;
    std::vector<uint64_t> mod(k), a(k), b(k);
    for (auto& l : mod) l = rng();
    mod[0] |= 1u;
    mod[k-1] |= (1ull << 63);

    uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);
    for (auto& l : a) l = rng();
    for (auto& l : b) l = rng();

    std::vector<uint64_t> out_fused(k), work_fused(k + 2, 0);
    hydra::detail::montgomery_mul_fused(a.data(), b.data(), k, mod.data(),
                                         n0inv, out_fused.data(), work_fused.data());

    uint32_t n_padded = 64;
    std::vector<uint64_t> out_kara(k), work_kara(2 * k + 1, 0);
    std::vector<uint64_t> pa(n_padded, 0), pb(n_padded, 0);
    std::vector<uint64_t> kbuf(2 * n_padded, 0);
    hydra::detail::ScratchWorkspace ws;
    ws.reserve_limbs(hydra::detail::karatsuba_scratch_limbs(n_padded));
    hydra::detail::montgomery_mul_karatsuba(
        a.data(), b.data(), k, mod.data(), n0inv,
        out_kara.data(), work_kara.data(),
        pa.data(), pb.data(), kbuf.data(), n_padded, ws);

    bool match = true;
    for (uint32_t i = 0; i < k; ++i) {
        if (out_fused[i] != out_kara[i]) { match = false; break; }
    }
    CHECK(match, "karatsuba mont mul vs fused at k=64");
}

// Non-power-of-2 k (e.g. k=33, padded to 64)
static void test_karatsuba_mont_mul_non_pow2() {
    std::mt19937_64 rng(0xCA4A0003ull);
    const uint32_t k = 33;
    std::vector<uint64_t> mod(k), a(k), b(k);
    for (auto& l : mod) l = rng();
    mod[0] |= 1u;
    mod[k-1] |= (1ull << 63);

    uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);
    for (auto& l : a) l = rng();
    for (auto& l : b) l = rng();

    // Fused CIOS reference
    std::vector<uint64_t> out_fused(k), work_fused(k + 2, 0);
    hydra::detail::montgomery_mul_fused(a.data(), b.data(), k, mod.data(),
                                         n0inv, out_fused.data(), work_fused.data());

    // Karatsuba + REDC
    uint32_t n_padded = 64;  // next pow2 >= 33
    std::vector<uint64_t> out_kara(k), work_kara(2 * k + 1, 0);
    std::vector<uint64_t> pa(n_padded, 0), pb(n_padded, 0);
    std::vector<uint64_t> kbuf(2 * n_padded, 0);
    hydra::detail::ScratchWorkspace ws;
    ws.reserve_limbs(hydra::detail::karatsuba_scratch_limbs(n_padded));
    hydra::detail::montgomery_mul_karatsuba(
        a.data(), b.data(), k, mod.data(), n0inv,
        out_kara.data(), work_kara.data(),
        pa.data(), pb.data(), kbuf.data(), n_padded, ws);

    bool match = true;
    for (uint32_t i = 0; i < k; ++i) {
        if (out_fused[i] != out_kara[i]) { match = false; break; }
    }
    CHECK(match, "karatsuba mont mul non-pow2 k=33");
}

// End-to-end pow_mod at 2048-bit (k=32, should use Karatsuba backend)
static void test_karatsuba_pow_mod_2048bit() {
    auto make = [](uint32_t bits, uint64_t seed) {
        uint32_t n_limbs = (bits + 63) / 64;
        std::mt19937_64 rng(seed);
        std::vector<uint64_t> limbs(n_limbs);
        for (auto& l : limbs) l = rng();
        limbs[0] |= 1u;
        limbs.back() |= (1ull << 63);
        return Hydra::from_limbs(limbs.data(), n_limbs);
    };

    Hydra base = make(128, 0xAA01);
    Hydra exp_val = make(64, 0xBB01);
    Hydra mod_val = make(2048, 0xCC01);

    Hydra result = hydra::pow_mod(base, exp_val, mod_val);
    Hydra naive = hydra::pow_mod_naive(base % mod_val, exp_val, mod_val);
    CHECK(result == naive, "karatsuba pow_mod 2048-bit vs naive");
}

// End-to-end pow_mod at 4096-bit (k=64, should use Karatsuba backend)
static void test_karatsuba_pow_mod_4096bit() {
    auto make = [](uint32_t bits, uint64_t seed) {
        uint32_t n_limbs = (bits + 63) / 64;
        std::mt19937_64 rng(seed);
        std::vector<uint64_t> limbs(n_limbs);
        for (auto& l : limbs) l = rng();
        limbs[0] |= 1u;
        limbs.back() |= (1ull << 63);
        return Hydra::from_limbs(limbs.data(), n_limbs);
    };

    Hydra base = make(128, 0xAA02);
    Hydra exp_val = make(32, 0xBB02);  // smaller exp for 4096 to keep test fast
    Hydra mod_val = make(4096, 0xCC02);

    Hydra result = hydra::pow_mod(base, exp_val, mod_val);
    Hydra naive = hydra::pow_mod_naive(base % mod_val, exp_val, mod_val);
    CHECK(result == naive, "karatsuba pow_mod 4096-bit vs naive");
}

// Random odd moduli sweep at widths crossing the Karatsuba threshold
static void test_karatsuba_pow_mod_random_sweep() {
    std::mt19937_64 rng(0xCA4A5EE9ull);
    // Test at k = 32, 40, 48, 56, 64 (all >= KARATSUBA_MONT_THRESHOLD=32)
    for (uint32_t k : {32u, 40u, 48u, 56u, 64u}) {
        uint32_t bits = k * 64;
        for (int trial = 0; trial < 5; ++trial) {
            // Random odd modulus
            std::vector<uint64_t> mod_limbs(k);
            for (auto& l : mod_limbs) l = rng();
            mod_limbs[0] |= 1u;
            mod_limbs[k-1] |= (1ull << 63);
            Hydra mod_val = Hydra::from_limbs(mod_limbs.data(), k);

            // Random base (small, to keep naive fast) and exp
            Hydra base{rng() % 1000000 + 2};
            Hydra exp_val{rng() % 256 + 1};

            Hydra result = hydra::pow_mod(base, exp_val, mod_val);
            Hydra naive = hydra::pow_mod_naive(base % mod_val, exp_val, mod_val);

            std::string label = "karatsuba pow_mod random k=" +
                std::to_string(k) + " trial=" + std::to_string(trial);
            CHECK(result == naive, label.c_str());
        }
    }
}

// Karatsuba Montgomery squaring cross-check
static void test_karatsuba_mont_sqr_cross() {
    std::mt19937_64 rng(0xCA4A5041ull);
    const uint32_t k = 32;
    std::vector<uint64_t> mod(k), a(k);
    for (auto& l : mod) l = rng();
    mod[0] |= 1u;
    mod[k-1] |= (1ull << 63);

    uint64_t n0inv = hydra::detail::montgomery_n0inv(mod[0]);
    for (auto& l : a) l = rng();

    // Squaring via fused CIOS (mul with a=b)
    std::vector<uint64_t> out_fused(k), work_fused(k + 2, 0);
    hydra::detail::montgomery_mul_fused(a.data(), a.data(), k, mod.data(),
                                         n0inv, out_fused.data(), work_fused.data());

    // Squaring via Karatsuba + REDC
    uint32_t n_padded = 32;
    std::vector<uint64_t> out_kara(k), work_kara(2 * k + 1, 0);
    std::vector<uint64_t> pa(n_padded, 0), pb(n_padded, 0);
    std::vector<uint64_t> kbuf(2 * n_padded, 0);
    hydra::detail::ScratchWorkspace ws;
    ws.reserve_limbs(hydra::detail::karatsuba_scratch_limbs(n_padded));
    hydra::detail::montgomery_sqr_karatsuba(
        a.data(), k, mod.data(), n0inv,
        out_kara.data(), work_kara.data(),
        pa.data(), pb.data(), kbuf.data(), n_padded, ws);

    bool match = true;
    for (uint32_t i = 0; i < k; ++i) {
        if (out_fused[i] != out_kara[i]) { match = false; break; }
    }
    CHECK(match, "karatsuba mont sqr vs fused at k=32");
}

// Boundary: k=31 should NOT use Karatsuba (stays on fused CIOS)
// k=32 should use Karatsuba.  Both must produce same pow_mod result.
static void test_karatsuba_threshold_boundary() {
    auto make = [](uint32_t bits, uint64_t seed) {
        uint32_t n_limbs = (bits + 63) / 64;
        std::mt19937_64 rng(seed);
        std::vector<uint64_t> limbs(n_limbs);
        for (auto& l : limbs) l = rng();
        limbs[0] |= 1u;
        limbs.back() |= (1ull << 63);
        return Hydra::from_limbs(limbs.data(), n_limbs);
    };

    // k=31 (below threshold — uses fused CIOS)
    {
        Hydra base = make(64, 0xB0D1ull);
        Hydra exp_val = make(32, 0xB0D2ull);
        Hydra mod_val = make(31 * 64, 0xB0D3ull);
        Hydra result = hydra::pow_mod(base, exp_val, mod_val);
        Hydra naive = hydra::pow_mod_naive(base % mod_val, exp_val, mod_val);
        CHECK(result == naive, "threshold boundary k=31 (fused CIOS)");
    }
    // k=32 (at threshold — uses Karatsuba)
    {
        Hydra base = make(64, 0xB0D4ull);
        Hydra exp_val = make(32, 0xB0D5ull);
        Hydra mod_val = make(32 * 64, 0xB0D6ull);
        Hydra result = hydra::pow_mod(base, exp_val, mod_val);
        Hydra naive = hydra::pow_mod_naive(base % mod_val, exp_val, mod_val);
        CHECK(result == naive, "threshold boundary k=32 (Karatsuba)");
    }
}

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

    // Karatsuba prototype cross-checks (vs schoolbook mul_limbs)
    test_karatsuba_4x4();
    test_karatsuba_8x8();
    test_karatsuba_16x16();
    test_karatsuba_32x32();
    test_karatsuba_64x64();
    test_karatsuba_all_ones();
    test_karatsuba_recursion_boundary();
    test_scratch_capacity_bound();
    test_scratch_reuse_across_calls();
    test_scratch_nested_frames();
    test_mul_limbs_dual_row_cross_check();
    test_mul_limbs_dual_row_all_ones();
    test_mul_limbs_dual_row_nb_one();

    // mul_general dispatch-seam tests (Karatsuba integration)
    test_mul_seam_31_limbs();
    test_mul_seam_32_limbs();
    test_mul_seam_33_limbs();
    test_mul_seam_mixed_32_16();
    test_mul_seam_mixed_16_32();
    test_mul_seam_mixed_33_17();
    test_mul_seam_mixed_31_32();
    test_mul_seam_identity_at_threshold();
    test_mul_seam_64_limbs();
    test_mul_seam_commutativity_at_threshold();

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
    test_shr_large_demotes_to_small();
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

    // Full Hydra ÷ Hydra (Knuth Algorithm D)
    test_divmod_zero_dividend();
    test_divmod_divisor_greater_than_dividend();
    test_divmod_equal_values();
    test_divmod_throws_on_zero_divisor();
    test_divmod_exact_divisibility();
    test_divmod_power_of_two_divisor();
    test_divmod_small_divisor_same_as_div_u64();
    test_divmod_two_limb_divisor();
    test_divmod_v_top_bit_already_set();
    test_divmod_worst_case_q_hat();
    test_divmod_add_back_scenario();
    test_divmod_large_dividend_forces_heap_scratch();
    test_divmod_divisor_equals_stack_limit();
    test_divmod_128_over_64();
    test_divmod_192_over_128();
    test_divmod_512_over_256();
    test_divmod_1024_over_512();
    test_div_mod_delegate_consistency();

    // Signed construction
    test_signed_constructor_positive();
    test_signed_constructor_negative();
    test_signed_constructor_zero();
    test_signed_constructor_int64_min();
    test_signed_constructor_int64_max();
    test_signed_constructor_int8();
    test_signed_constructor_int16();
    test_signed_constructor_int32();

    // Signed addition
    test_signed_add_pos_pos();
    test_signed_add_neg_neg();
    test_signed_add_pos_neg_pos_wins();
    test_signed_add_pos_neg_neg_wins();
    test_signed_add_cancel_to_zero();
    test_signed_add_large_neg();

    // Signed subtraction
    test_signed_sub_to_negative();
    test_signed_sub_neg_from_pos();
    test_signed_sub_neg_from_neg();
    test_signed_sub_symmetric();

    // Signed multiplication
    test_signed_mul_pos_pos();
    test_signed_mul_pos_neg();
    test_signed_mul_neg_neg();
    test_signed_mul_neg_zero();
    test_signed_mul_large_cross_sign();

    // Signed division (truncation toward zero)
    test_signed_divmod_pos_pos();
    test_signed_divmod_neg_pos();
    test_signed_divmod_pos_neg();
    test_signed_divmod_neg_neg();
    test_signed_divmod_exact();
    test_signed_divmod_dividend_smaller();
    test_signed_divmod_large_invariant();

    // Unary negation
    test_negate_positive();
    test_negate_negative();
    test_negate_zero();
    test_double_negate();

    // Comparison completeness
    test_cmp_pos_neg();
    test_cmp_neg_neg();
    test_cmp_zero_neg();
    test_cmp_equal_neg();
    test_cmp_with_int_literal();
    test_cmp_large_signed();

    // Native interop
    test_interop_add_int();
    test_interop_sub_int();
    test_interop_mul_int();
    test_interop_compare_u64();
    test_interop_compare_i64();
    test_interop_add_negative_int();

    // Bitwise operators
    test_bitwise_and_basic();
    test_bitwise_and_zero();
    test_bitwise_or_basic();
    test_bitwise_or_zero();
    test_bitwise_xor_basic();
    test_bitwise_xor_self();
    test_bitwise_not_zero();
    test_bitwise_not_positive();
    test_bitwise_not_negative();
    test_bitwise_not_roundtrip();
    test_bitwise_and_medium();
    test_bitwise_or_large();
    test_bitwise_xor_large();
    test_bitwise_negative_throws();
    test_bitwise_compound_assign();

    // Mixed-tier signed
    test_signed_add_small_neg_plus_large();
    test_signed_mul_medium_neg_times_large();

    // Normalize preserves sign
    test_normalize_sign_large_to_small();
    test_normalize_sign_zero_clears();

    // Adversarial signed edge cases
    test_signed_add_overflow_to_medium();
    test_signed_sub_medium_to_small();
    test_signed_divmod_int64_min_by_neg1();

    // String parse constructor
    test_parse_simple_positive();
    test_parse_zero();
    test_parse_negative_zero();
    test_parse_leading_zeros();
    test_parse_leading_sign_plus();
    test_parse_negative();
    test_parse_uint64_max();
    test_parse_uint64_max_plus_one();
    test_parse_large_negative();
    test_parse_int64_boundaries();
    test_parse_invalid_empty();
    test_parse_invalid_chars();
    test_parse_sign_only();
    test_parse_power_of_two();
    test_parse_power_of_ten();

    // Round-trip invariant tests
    test_roundtrip_zero();
    test_roundtrip_negative_zero();
    test_roundtrip_int64_boundaries();
    test_roundtrip_powers_of_two();
    test_roundtrip_powers_of_ten();
    test_roundtrip_1000_digit_random();
    test_roundtrip_signed_random_fuzz();

    // ostream operator<<
    test_ostream_operator();
    test_ostream_zero();

    // Chunked to_string correctness
    test_tostring_medium();
    test_tostring_large_known();
    test_tostring_vs_parse_cross_check();

    // Number theory: abs
    test_abs_positive();
    test_abs_negative();
    test_abs_zero();

    // Number theory: operator/, operator%
    test_div_mod_operators();
    test_divmod_assign();

    // Number theory: gcd
    test_gcd_zero_zero();
    test_gcd_zero_x();
    test_gcd_x_zero();
    test_gcd_zero_neg();
    test_gcd_positive();
    test_gcd_neg_pos();
    test_gcd_pos_neg();
    test_gcd_neg_neg();
    test_gcd_coprime();
    test_gcd_coprime_large();
    test_gcd_powers_of_two();
    test_gcd_power_of_two_and_odd();
    test_gcd_same();
    test_gcd_large_decimal();
    test_gcd_large_coprime();

    // Number theory: extended_gcd
    test_egcd_basic();
    test_egcd_coprime();
    test_egcd_with_zero();
    test_egcd_signed();
    test_egcd_both_negative();
    test_egcd_large();

    // Number theory: pow_mod
    test_pow_mod_basic();
    test_pow_mod_zero_exp();
    test_pow_mod_one_mod();
    test_pow_mod_large_exp();
    test_pow_mod_negative_base();
    test_pow_mod_throws_zero_mod();
    test_pow_mod_throws_negative_exp();
    test_pow_mod_throws_negative_mod();

    // Showcase: toy RSA
    test_rsa_toy();
    test_rsa_toy_all_messages();
    test_pow_mod_large_parsed();

    // ── Montgomery multiplication path tests ──────────────
    test_montgomery_n0inv();
    test_montgomery_redc_basic();
    test_montgomery_context_build();
    test_montgomery_roundtrip_small();
    test_montgomery_mul_basic();
    test_pow_mod_montgomery_small();
    test_pow_mod_montgomery_256bit();
    test_pow_mod_montgomery_512bit();
    test_pow_mod_montgomery_1024bit();
    test_pow_mod_montgomery_vs_naive();
    test_pow_mod_montgomery_fermat();
    test_pow_mod_montgomery_rsa_256bit();
    test_pow_mod_even_mod_fallback();
    test_pow_mod_montgomery_base_larger_than_mod();
    test_pow_mod_montgomery_mod_equals_3();

    // Additional pow_mod edge cases and squaring-specific tests
    test_pow_mod_base_zero();
    test_pow_mod_exp_zero();
    test_pow_mod_mod_one();
    test_pow_mod_base_less_than_mod();
    test_pow_mod_base_greater_than_mod();
    test_pow_mod_montgomery_sqr_specific();
    test_pow_mod_montgomery_sqr_chain();
    test_pow_mod_random_odd_moduli_near_threshold();
    test_pow_mod_sliding_window_coverage();
    test_pow_mod_montgomery_2048bit_cross();

    // Fused CIOS Montgomery kernel tests
    test_fused_montgomery_mul_vs_separate();
    test_fused_montgomery_mul_multi_limb();
    test_fused_montgomery_sqr_cross();
    test_fused_pow_mod_random_widths();
    test_fused_pow_mod_4096bit();
    test_fused_pow_mod_single_limb();
    test_fused_montgomery_mul_sweep();
    test_fused_pow_mod_boundary_widths();

    // Karatsuba-backed Montgomery tests
    test_karatsuba_mont_mul_vs_fused();
    test_karatsuba_mont_mul_vs_fused_4096();
    test_karatsuba_mont_mul_non_pow2();
    test_karatsuba_pow_mod_2048bit();
    test_karatsuba_pow_mod_4096bit();
    test_karatsuba_pow_mod_random_sweep();
    test_karatsuba_mont_sqr_cross();
    test_karatsuba_threshold_boundary();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
