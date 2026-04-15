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

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
