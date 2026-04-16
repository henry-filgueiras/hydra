// hydra.hpp  —  tiered multi-representation integer runtime
//
// Architecture
// ────────────
//   Small  : inline uint64_t (hot path, zero overhead vs native)
//   Medium : inline uint64_t[3] limbs (192-bit, covers u64×u64 products)
//   Large  : tail-allocated LargeRep (arbitrary precision)
//
// Invariant: every value occupies the *smallest* valid representation.
// normalize() enforces this after every mutation.
//
// Phase-1 scope: unsigned arithmetic only.
// The sign bit in metadata is reserved but not yet interpreted.
//
// C++20, GCC/Clang, x86-64 / aarch64.

#pragma once

#include <algorithm>
#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hydra {

// ─────────────────────────────────────────────────────────
// Representation kind
// ─────────────────────────────────────────────────────────

enum class Kind : uint8_t {
    Small  = 0,
    Medium = 1,
    Large  = 2,
};

// ─────────────────────────────────────────────────────────
// Metadata word bit layout
// ─────────────────────────────────────────────────────────
//
//  bits  0..1  : Kind (0=Small, 1=Medium, 2=Large)
//  bit      2  : sign (0=positive, reserved for phase 2)
//  bits  3..7  : reserved
//  bits  8..15 : medium limb count (0..3, only meaningful for Medium)
//  bits 16..63 : reserved for future use
//
namespace bits {
    inline constexpr uint64_t KIND_MASK  = 0x3ull;
    inline constexpr uint64_t SIGN_BIT   = 0x4ull;
    inline constexpr uint64_t USED_SHIFT = 8;
    inline constexpr uint64_t USED_MASK  = 0xFFull << USED_SHIFT;
} // namespace bits

// ─────────────────────────────────────────────────────────
// Tail-allocated large representation
//
//   layout: [ LargeRep header | uint64_t limbs[capacity] ]
//
// Limbs are stored least-significant first.
// ─────────────────────────────────────────────────────────

struct LargeRep {
    uint32_t used;      // number of significant limbs
    uint32_t capacity;  // allocated limbs

    // Limb array lives immediately after the header.
    [[nodiscard]] uint64_t* limbs() noexcept {
        return reinterpret_cast<uint64_t*>(this + 1);
    }
    [[nodiscard]] const uint64_t* limbs() const noexcept {
        return reinterpret_cast<const uint64_t*>(this + 1);
    }

    [[nodiscard]] static LargeRep* create(uint32_t capacity) {
        const size_t bytes = sizeof(LargeRep) + capacity * sizeof(uint64_t);
        auto* rep = static_cast<LargeRep*>(::operator new(bytes));
        rep->used     = 0;
        rep->capacity = capacity;
        return rep;
    }

    [[nodiscard]] static LargeRep* clone(const LargeRep* src) {
        auto* dst = create(src->capacity);
        dst->used = src->used;
        std::memcpy(dst->limbs(), src->limbs(), src->used * sizeof(uint64_t));
        return dst;
    }

    static void destroy(LargeRep* rep) noexcept {
        ::operator delete(rep);
    }
};

// RAII guard for exception-safety in arithmetic temporaries.
struct DestroyLarge {
    void operator()(LargeRep* p) const noexcept {
        if (p) LargeRep::destroy(p);
    }
};
using LargeGuard = std::unique_ptr<LargeRep, DestroyLarge>;

// ─────────────────────────────────────────────────────────
// Low-level limb-array kernels (LSB-first, unsigned)
// ─────────────────────────────────────────────────────────
namespace detail {

// Add two limb arrays. out must have room for max(na, nb)+1 limbs.
// Returns the number of significant result limbs (trimming not applied here).
inline uint32_t add_limbs(
    const uint64_t* a, uint32_t na,
    const uint64_t* b, uint32_t nb,
    uint64_t* out) noexcept
{
    if (na < nb) { std::swap(a, b); std::swap(na, nb); } // ensure na >= nb

    uint64_t carry = 0;
    uint32_t i = 0;

    // Paired limbs
    for (; i < nb; ++i) {
        unsigned __int128 s =
            static_cast<unsigned __int128>(a[i]) + b[i] + carry;
        out[i] = static_cast<uint64_t>(s);
        carry  = static_cast<uint64_t>(s >> 64);
    }
    // Remaining limbs from a (carry propagation)
    for (; i < na; ++i) {
        unsigned __int128 s =
            static_cast<unsigned __int128>(a[i]) + carry;
        out[i] = static_cast<uint64_t>(s);
        carry  = static_cast<uint64_t>(s >> 64);
    }
    if (carry) {
        out[i++] = carry;
    }
    return i;
}

// Subtract b from a (a >= b assumed, unsigned). Returns used limb count.
// out must have at least na limbs.
inline uint32_t sub_limbs(
    const uint64_t* a, uint32_t na,
    const uint64_t* b, uint32_t nb,
    uint64_t* out) noexcept
{
    assert(na >= nb);
    uint64_t borrow = 0;

    uint32_t i = 0;
    for (; i < nb; ++i) {
        uint64_t ai = a[i];
        uint64_t bi = b[i];
        uint64_t d1 = ai - bi;
        uint64_t b1 = (d1 > ai) ? 1u : 0u;   // underflow ai - bi
        uint64_t d2 = d1 - borrow;
        uint64_t b2 = (d2 > d1) ? 1u : 0u;   // underflow d1 - borrow
        out[i] = d2;
        borrow  = b1 + b2;                    // b1+b2 ≤ 1 (proof in header)
    }
    for (; i < na; ++i) {
        uint64_t ai = a[i];
        uint64_t d  = ai - borrow;
        borrow = (d > ai) ? 1u : 0u;
        out[i] = d;
    }
    assert(borrow == 0); // caller must guarantee a >= b

    // Trim trailing zeros to find used count
    uint32_t used = na;
    while (used > 0 && out[used - 1] == 0) --used;
    return used;
}

// Schoolbook O(n²) multiply. out must have na+nb zeroed limbs.
inline uint32_t mul_limbs(
    const uint64_t* a, uint32_t na,
    const uint64_t* b, uint32_t nb,
    uint64_t* out) noexcept
{
    std::memset(out, 0, (na + nb) * sizeof(uint64_t));

    for (uint32_t i = 0; i < na; ++i) {
        if (a[i] == 0) continue;
        uint64_t carry = 0;
        for (uint32_t j = 0; j < nb; ++j) {
            unsigned __int128 t =
                static_cast<unsigned __int128>(a[i]) * b[j]
                + out[i + j]
                + carry;
            out[i + j] = static_cast<uint64_t>(t);
            carry       = static_cast<uint64_t>(t >> 64);
        }
        // Propagate final carry into higher limbs.
        for (uint32_t k = i + nb; carry; ++k) {
            uint64_t t = out[k] + carry;
            carry = (t < carry) ? 1u : 0u;
            out[k] = t;
        }
    }

    // Trim
    uint32_t used = na + nb;
    while (used > 0 && out[used - 1] == 0) --used;
    return used;
}

// ── Row-based multiply-accumulate macro ────────────────────
//
// Accumulates a[i] * b[j] + out[i+j] + carry into __int128.
// This never overflows because:
//   max(out[k]) = 2^64-1, max(carry) = 2^64-1,
//   max(a*b) = (2^64-1)^2 = 2^128 - 2^65 + 1
//   Total ≤ 2^128 - 1, which fits in unsigned __int128.
//
// After the MAC: out[k] = low 64 bits, carry = high 64 bits.
//
#define HYDRA_ROW_MAC(out_k, carry, ai, bj) \
    do { \
        unsigned __int128 _t = \
            static_cast<unsigned __int128>(ai) * (bj) \
            + (out_k) + (carry); \
        (out_k) = static_cast<uint64_t>(_t); \
        (carry) = static_cast<uint64_t>(_t >> 64); \
    } while (0)

// ── Hand-unrolled 3×3 multiply (up to 6-limb result) ──────
//
// Row-based unrolling: for each a[i], multiply by all b[j] and
// accumulate into out[i+j]. Each row is a 3-step chain.
// The __int128 accumulator never overflows (proof above).
//
inline uint32_t mul_3x3(
    const uint64_t* a, uint32_t na,
    const uint64_t* b, uint32_t nb,
    uint64_t* out) noexcept
{
    // Pad with zero for inputs smaller than 3 limbs.
    uint64_t a0 = a[0], a1 = (na > 1) ? a[1] : 0, a2 = (na > 2) ? a[2] : 0;
    uint64_t b0 = b[0], b1 = (nb > 1) ? b[1] : 0, b2 = (nb > 2) ? b[2] : 0;
    uint64_t carry;

    // Zero output
    out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = 0;

    // Row 0: a0 * b[0..2]
    carry = 0;
    HYDRA_ROW_MAC(out[0], carry, a0, b0);
    HYDRA_ROW_MAC(out[1], carry, a0, b1);
    HYDRA_ROW_MAC(out[2], carry, a0, b2);
    out[3] = carry;

    // Row 1: a1 * b[0..2]
    carry = 0;
    HYDRA_ROW_MAC(out[1], carry, a1, b0);
    HYDRA_ROW_MAC(out[2], carry, a1, b1);
    HYDRA_ROW_MAC(out[3], carry, a1, b2);
    out[4] = carry;

    // Row 2: a2 * b[0..2]
    carry = 0;
    HYDRA_ROW_MAC(out[2], carry, a2, b0);
    HYDRA_ROW_MAC(out[3], carry, a2, b1);
    HYDRA_ROW_MAC(out[4], carry, a2, b2);
    out[5] = carry;

    // Trim
    uint32_t used = 6;
    while (used > 0 && out[used - 1] == 0) --used;
    return used;
}

// ── Hand-unrolled 4×4 multiply (up to 8-limb result) ──────
//
// Targets 256-bit × 256-bit (large_mul_256).
// 4 rows × 4 MACs = 16 multiply-accumulates, fully unrolled.
//
inline uint32_t mul_4x4(
    const uint64_t* a, const uint64_t* b,
    uint64_t* out) noexcept
{
    uint64_t carry;

    // Zero output
    out[0] = out[1] = out[2] = out[3] = 0;
    out[4] = out[5] = out[6] = out[7] = 0;

    // Row 0: a[0] * b[0..3]
    carry = 0;
    HYDRA_ROW_MAC(out[0], carry, a[0], b[0]);
    HYDRA_ROW_MAC(out[1], carry, a[0], b[1]);
    HYDRA_ROW_MAC(out[2], carry, a[0], b[2]);
    HYDRA_ROW_MAC(out[3], carry, a[0], b[3]);
    out[4] = carry;

    // Row 1: a[1] * b[0..3]
    carry = 0;
    HYDRA_ROW_MAC(out[1], carry, a[1], b[0]);
    HYDRA_ROW_MAC(out[2], carry, a[1], b[1]);
    HYDRA_ROW_MAC(out[3], carry, a[1], b[2]);
    HYDRA_ROW_MAC(out[4], carry, a[1], b[3]);
    out[5] = carry;

    // Row 2: a[2] * b[0..3]
    carry = 0;
    HYDRA_ROW_MAC(out[2], carry, a[2], b[0]);
    HYDRA_ROW_MAC(out[3], carry, a[2], b[1]);
    HYDRA_ROW_MAC(out[4], carry, a[2], b[2]);
    HYDRA_ROW_MAC(out[5], carry, a[2], b[3]);
    out[6] = carry;

    // Row 3: a[3] * b[0..3]
    carry = 0;
    HYDRA_ROW_MAC(out[3], carry, a[3], b[0]);
    HYDRA_ROW_MAC(out[4], carry, a[3], b[1]);
    HYDRA_ROW_MAC(out[5], carry, a[3], b[2]);
    HYDRA_ROW_MAC(out[6], carry, a[3], b[3]);
    out[7] = carry;

    // Trim
    uint32_t used = 8;
    while (used > 0 && out[used - 1] == 0) --used;
    return used;
}

// ── Hand-unrolled 8×8 multiply (up to 16-limb result) ─────
//
// Targets 512-bit × 512-bit (large_mul_512).
// 8 rows × 8 MACs = 64 multiply-accumulates, fully unrolled.
// Row-based approach: each row multiplies a[i] by all of b[0..7],
// accumulating into out[i..i+7]. The __int128 accumulator never
// overflows (see proof at HYDRA_ROW_MAC).
//
inline uint32_t mul_8x8(
    const uint64_t* a, const uint64_t* b,
    uint64_t* out) noexcept
{
    uint64_t carry;

    // Zero output (128 bytes)
    for (int i = 0; i < 16; ++i) out[i] = 0;

    // Row 0: a[0] * b[0..7]
    carry = 0;
    HYDRA_ROW_MAC(out[0], carry, a[0], b[0]);
    HYDRA_ROW_MAC(out[1], carry, a[0], b[1]);
    HYDRA_ROW_MAC(out[2], carry, a[0], b[2]);
    HYDRA_ROW_MAC(out[3], carry, a[0], b[3]);
    HYDRA_ROW_MAC(out[4], carry, a[0], b[4]);
    HYDRA_ROW_MAC(out[5], carry, a[0], b[5]);
    HYDRA_ROW_MAC(out[6], carry, a[0], b[6]);
    HYDRA_ROW_MAC(out[7], carry, a[0], b[7]);
    out[8] = carry;

    // Row 1: a[1] * b[0..7]
    carry = 0;
    HYDRA_ROW_MAC(out[1], carry, a[1], b[0]);
    HYDRA_ROW_MAC(out[2], carry, a[1], b[1]);
    HYDRA_ROW_MAC(out[3], carry, a[1], b[2]);
    HYDRA_ROW_MAC(out[4], carry, a[1], b[3]);
    HYDRA_ROW_MAC(out[5], carry, a[1], b[4]);
    HYDRA_ROW_MAC(out[6], carry, a[1], b[5]);
    HYDRA_ROW_MAC(out[7], carry, a[1], b[6]);
    HYDRA_ROW_MAC(out[8], carry, a[1], b[7]);
    out[9] = carry;

    // Row 2: a[2] * b[0..7]
    carry = 0;
    HYDRA_ROW_MAC(out[2], carry, a[2], b[0]);
    HYDRA_ROW_MAC(out[3], carry, a[2], b[1]);
    HYDRA_ROW_MAC(out[4], carry, a[2], b[2]);
    HYDRA_ROW_MAC(out[5], carry, a[2], b[3]);
    HYDRA_ROW_MAC(out[6], carry, a[2], b[4]);
    HYDRA_ROW_MAC(out[7], carry, a[2], b[5]);
    HYDRA_ROW_MAC(out[8], carry, a[2], b[6]);
    HYDRA_ROW_MAC(out[9], carry, a[2], b[7]);
    out[10] = carry;

    // Row 3: a[3] * b[0..7]
    carry = 0;
    HYDRA_ROW_MAC(out[3], carry, a[3], b[0]);
    HYDRA_ROW_MAC(out[4], carry, a[3], b[1]);
    HYDRA_ROW_MAC(out[5], carry, a[3], b[2]);
    HYDRA_ROW_MAC(out[6], carry, a[3], b[3]);
    HYDRA_ROW_MAC(out[7], carry, a[3], b[4]);
    HYDRA_ROW_MAC(out[8], carry, a[3], b[5]);
    HYDRA_ROW_MAC(out[9], carry, a[3], b[6]);
    HYDRA_ROW_MAC(out[10], carry, a[3], b[7]);
    out[11] = carry;

    // Row 4: a[4] * b[0..7]
    carry = 0;
    HYDRA_ROW_MAC(out[4], carry, a[4], b[0]);
    HYDRA_ROW_MAC(out[5], carry, a[4], b[1]);
    HYDRA_ROW_MAC(out[6], carry, a[4], b[2]);
    HYDRA_ROW_MAC(out[7], carry, a[4], b[3]);
    HYDRA_ROW_MAC(out[8], carry, a[4], b[4]);
    HYDRA_ROW_MAC(out[9], carry, a[4], b[5]);
    HYDRA_ROW_MAC(out[10], carry, a[4], b[6]);
    HYDRA_ROW_MAC(out[11], carry, a[4], b[7]);
    out[12] = carry;

    // Row 5: a[5] * b[0..7]
    carry = 0;
    HYDRA_ROW_MAC(out[5], carry, a[5], b[0]);
    HYDRA_ROW_MAC(out[6], carry, a[5], b[1]);
    HYDRA_ROW_MAC(out[7], carry, a[5], b[2]);
    HYDRA_ROW_MAC(out[8], carry, a[5], b[3]);
    HYDRA_ROW_MAC(out[9], carry, a[5], b[4]);
    HYDRA_ROW_MAC(out[10], carry, a[5], b[5]);
    HYDRA_ROW_MAC(out[11], carry, a[5], b[6]);
    HYDRA_ROW_MAC(out[12], carry, a[5], b[7]);
    out[13] = carry;

    // Row 6: a[6] * b[0..7]
    carry = 0;
    HYDRA_ROW_MAC(out[6], carry, a[6], b[0]);
    HYDRA_ROW_MAC(out[7], carry, a[6], b[1]);
    HYDRA_ROW_MAC(out[8], carry, a[6], b[2]);
    HYDRA_ROW_MAC(out[9], carry, a[6], b[3]);
    HYDRA_ROW_MAC(out[10], carry, a[6], b[4]);
    HYDRA_ROW_MAC(out[11], carry, a[6], b[5]);
    HYDRA_ROW_MAC(out[12], carry, a[6], b[6]);
    HYDRA_ROW_MAC(out[13], carry, a[6], b[7]);
    out[14] = carry;

    // Row 7: a[7] * b[0..7]
    carry = 0;
    HYDRA_ROW_MAC(out[7], carry, a[7], b[0]);
    HYDRA_ROW_MAC(out[8], carry, a[7], b[1]);
    HYDRA_ROW_MAC(out[9], carry, a[7], b[2]);
    HYDRA_ROW_MAC(out[10], carry, a[7], b[3]);
    HYDRA_ROW_MAC(out[11], carry, a[7], b[4]);
    HYDRA_ROW_MAC(out[12], carry, a[7], b[5]);
    HYDRA_ROW_MAC(out[13], carry, a[7], b[6]);
    HYDRA_ROW_MAC(out[14], carry, a[7], b[7]);
    out[15] = carry;

    // Trim
    uint32_t used = 16;
    while (used > 0 && out[used - 1] == 0) --used;
    return used;
}

#undef HYDRA_ROW_MAC

// Lexicographic compare of two limb arrays (MSB first logically, LSB stored first).
// Returns -1, 0, +1.
inline int cmp_limbs(
    const uint64_t* a, uint32_t na,
    const uint64_t* b, uint32_t nb) noexcept
{
    if (na != nb) return (na < nb) ? -1 : 1;
    for (uint32_t i = na; i-- > 0;) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return  1;
    }
    return 0;
}

// ── Left-shift a limb array by `shift` bits ───────────────
//
// Writes result into `out`.  Caller must provide space for
// na + (shift/64) + 1 limbs.  Positions below (shift/64) are
// zeroed by this function.  Precondition: na > 0, shift > 0.
// Returns significant limb count (trailing zeros trimmed).
//
inline uint32_t shl_limbs(
    const uint64_t* a, uint32_t na,
    unsigned shift,
    uint64_t* out) noexcept
{
    const unsigned whole = shift / 64;   // whole-limb offset
    const unsigned bits  = shift % 64;   // intra-limb shift

    // Zero the whole-limb prefix (these contribute nothing from `a`).
    for (unsigned i = 0; i < whole; ++i) out[i] = 0;

    if (bits == 0) {
        // Pure limb-shift: no intra-limb carry needed.
        std::memcpy(out + whole, a, na * sizeof(uint64_t));
        uint32_t used = na + whole;
        while (used > 0 && out[used - 1] == 0) --used;
        return used;
    }

    // bits in [1..63]: each output limb receives the low part of a[i]
    // shifted left plus the high part of a[i-1] shifted right.
    // 64 - bits is safe here because bits != 0.
    uint64_t carry = 0;
    for (uint32_t i = 0; i < na; ++i) {
        out[i + whole] = (a[i] << bits) | carry;
        carry           = a[i] >> (64 - bits);
    }
    uint32_t used = na + whole;
    if (carry) out[used++] = carry;
    while (used > 0 && out[used - 1] == 0) --used;
    return used;
}

// ── Right-shift a limb array by `shift` bits ──────────────
//
// Writes result into `out`.  Caller must provide space for
// na - (shift/64) limbs.  Precondition: na > 0, shift > 0,
// shift/64 < na (checked by caller).
// Returns significant limb count (trailing zeros trimmed).
//
inline uint32_t shr_limbs(
    const uint64_t* a, uint32_t na,
    unsigned shift,
    uint64_t* out) noexcept
{
    const unsigned whole = shift / 64;
    const unsigned bits  = shift % 64;

    if (whole >= na) return 0;   // every bit shifted out

    const uint32_t n = na - whole;   // remaining limbs

    if (bits == 0) {
        // Pure limb-shift.
        std::memcpy(out, a + whole, n * sizeof(uint64_t));
        uint32_t used = n;
        while (used > 0 && out[used - 1] == 0) --used;
        return used;
    }

    // bits in [1..63]: stitch adjacent pairs.
    // 64 - bits is safe because bits != 0.
    for (uint32_t i = 0; i < n - 1; ++i) {
        out[i] = (a[i + whole] >> bits)
               | (a[i + whole + 1] << (64 - bits));
    }
    out[n - 1] = a[na - 1] >> bits;

    uint32_t used = n;
    while (used > 0 && out[used - 1] == 0) --used;
    return used;
}

// ── Short (scalar) division kernel ────────────────────────
//
// Divides the `na`-limb (LSB-first) integer `a` by 64-bit
// scalar `d`.  Writes quotient limbs (exactly `na` of them)
// into `q` and returns the remainder.
//
// Uses the standard two-word / one-word reduction via
// unsigned __int128, which maps to a single `divq` on x86-64
// and `udiv` + `msub` on aarch64.
//
// Precondition: d != 0, na > 0.
//
inline uint64_t divmod_u64_limbs(
    const uint64_t* a, uint32_t na,
    uint64_t d,
    uint64_t* q) noexcept
{
    uint64_t rem = 0;
    for (uint32_t i = na; i-- > 0;) {
        unsigned __int128 cur =
            (static_cast<unsigned __int128>(rem) << 64) | a[i];
        q[i] = static_cast<uint64_t>(cur / d);
        rem  = static_cast<uint64_t>(cur % d);
    }
    return rem;
}

} // namespace detail

// ─────────────────────────────────────────────────────────
// Hydra — the main value type
// ─────────────────────────────────────────────────────────

struct Hydra {
    // ── data ──────────────────────────────────────────────
    uint64_t meta{};

    union Payload {
        uint64_t  small;
        uint64_t  medium[3];
        LargeRep* large;

        constexpr Payload() noexcept : small(0) {}
    } payload;

    // ─────────────────────────────────────────────────────
    // Metadata helpers
    // ─────────────────────────────────────────────────────

    static constexpr uint64_t make_small_meta() noexcept {
        return static_cast<uint64_t>(Kind::Small);
    }
    static constexpr uint64_t make_medium_meta(uint8_t used = 0) noexcept {
        return static_cast<uint64_t>(Kind::Medium)
               | (static_cast<uint64_t>(used) << bits::USED_SHIFT);
    }
    static constexpr uint64_t make_large_meta() noexcept {
        return static_cast<uint64_t>(Kind::Large);
    }

    [[nodiscard]] Kind kind() const noexcept {
        return static_cast<Kind>(meta & bits::KIND_MASK);
    }
    [[nodiscard]] bool is_small()  const noexcept { return kind() == Kind::Small;  }
    [[nodiscard]] bool is_medium() const noexcept { return kind() == Kind::Medium; }
    [[nodiscard]] bool is_large()  const noexcept { return kind() == Kind::Large;  }

    [[nodiscard]] uint8_t used_medium_limbs() const noexcept {
        return static_cast<uint8_t>((meta & bits::USED_MASK) >> bits::USED_SHIFT);
    }
    void set_used_medium_limbs(uint8_t n) noexcept {
        meta = (meta & ~bits::USED_MASK)
               | (static_cast<uint64_t>(n) << bits::USED_SHIFT);
    }

    // ─────────────────────────────────────────────────────
    // Limb view — a uniform read-only span over this value's limbs
    // ─────────────────────────────────────────────────────

    struct LimbView {
        const uint64_t* ptr;
        uint32_t        count;
    };

    [[nodiscard]] LimbView limb_view() const noexcept {
        switch (kind()) {
        case Kind::Small:
            // Zero is represented as 0 limbs so arithmetic stays clean.
            return { &payload.small,
                     payload.small ? 1u : 0u };
        case Kind::Medium:
            return { payload.medium, used_medium_limbs() };
        case Kind::Large:
            return { payload.large->limbs(), payload.large->used };
        }
        __builtin_unreachable();
    }

    // ─────────────────────────────────────────────────────
    // Destruction
    // ─────────────────────────────────────────────────────

    void destroy_if_large() noexcept {
        if (is_large() && payload.large) {
            LargeRep::destroy(payload.large);
            payload.large = nullptr;
        }
    }

    // ─────────────────────────────────────────────────────
    // Constructors / rule of five
    // ─────────────────────────────────────────────────────

    // Default — zero.
    constexpr Hydra() noexcept
        : meta(make_small_meta()), payload() {}

    // From uint64_t — always Small.
    constexpr explicit Hydra(uint64_t v) noexcept
        : meta(make_small_meta()), payload()
    { payload.small = v; }

    // Implicit conversion from all unsigned integer types.
    template<std::unsigned_integral T>
    constexpr Hydra(T v) noexcept               // NOLINT(google-explicit-constructor)
        : meta(make_small_meta()), payload()
    { payload.small = static_cast<uint64_t>(v); }

    // Copy
    Hydra(const Hydra& o) : meta(o.meta), payload() {
        switch (o.kind()) {
        case Kind::Small:
            payload.small = o.payload.small;
            break;
        case Kind::Medium:
            std::memcpy(payload.medium, o.payload.medium, sizeof(payload.medium));
            break;
        case Kind::Large:
            payload.large = LargeRep::clone(o.payload.large);
            break;
        }
    }

    // Move
    Hydra(Hydra&& o) noexcept : meta(o.meta), payload() {
        std::memcpy(&payload, &o.payload, sizeof(payload));
        o.meta = make_small_meta();
        o.payload.small = 0;
    }

    ~Hydra() { destroy_if_large(); }

    // Copy assignment
    Hydra& operator=(const Hydra& o) {
        if (this == &o) return *this;
        destroy_if_large();
        meta = o.meta;
        switch (o.kind()) {
        case Kind::Small:
            payload.small = o.payload.small;
            break;
        case Kind::Medium:
            std::memcpy(payload.medium, o.payload.medium, sizeof(payload.medium));
            break;
        case Kind::Large:
            payload.large = LargeRep::clone(o.payload.large);
            break;
        }
        return *this;
    }

    // Move assignment
    Hydra& operator=(Hydra&& o) noexcept {
        if (this == &o) return *this;
        destroy_if_large();
        meta = o.meta;
        std::memcpy(&payload, &o.payload, sizeof(payload));
        o.meta = make_small_meta();
        o.payload.small = 0;
        return *this;
    }

    // ─────────────────────────────────────────────────────
    // Construction helpers (internal)
    // ─────────────────────────────────────────────────────

    // Build a Medium from up to 3 limbs (LSB-first).
    // Caller must ensure 2 ≤ used ≤ 3 and high limbs match used.
    [[nodiscard]] static Hydra make_medium(
        uint64_t l0, uint64_t l1, uint64_t l2, uint8_t used) noexcept
    {
        Hydra r;
        r.meta            = make_medium_meta(used);
        r.payload.medium[0] = l0;
        r.payload.medium[1] = l1;
        r.payload.medium[2] = l2;
        return r;
    }

    // Build from a raw limb array. Normalizes immediately.
    [[nodiscard]] static Hydra from_limbs(
        const uint64_t* limbs, uint32_t count)
    {
        // Trim
        while (count > 0 && limbs[count - 1] == 0) --count;

        if (count == 0)  return Hydra{};
        if (count == 1)  return Hydra{ limbs[0] };
        if (count <= 3) {
            uint64_t m[3] = {};
            std::memcpy(m, limbs, count * sizeof(uint64_t));
            return make_medium(m[0], m[1], m[2], static_cast<uint8_t>(count));
        }

        // Heap path
        auto* rep = LargeRep::create(count);
        rep->used = count;
        std::memcpy(rep->limbs(), limbs, count * sizeof(uint64_t));
        Hydra r;
        r.meta = make_large_meta();
        r.payload.large = rep;
        return r;
    }

    // ─────────────────────────────────────────────────────
    // normalize()
    //
    // Called after any in-place mutation; ensures the value is
    // in the smallest valid representation.
    // ─────────────────────────────────────────────────────

    void normalize() noexcept {
        switch (kind()) {
        case Kind::Small:
            // Already minimal.
            break;

        case Kind::Medium: {
            // Re-count significant limbs from the top.
            uint8_t u = 3;
            while (u > 0 && payload.medium[u - 1] == 0) --u;

            if (u == 0) {
                meta = make_small_meta();
                payload.small = 0;
            } else if (u == 1) {
                uint64_t v = payload.medium[0];
                meta = make_small_meta();
                payload.small = v;
            } else {
                set_used_medium_limbs(u);
            }
            break;
        }

        case Kind::Large: {
            LargeRep* rep = payload.large;
            // Trim leading zeros
            while (rep->used > 0 && rep->limbs()[rep->used - 1] == 0)
                --rep->used;

            uint32_t u = rep->used;

            if (u == 0) {
                LargeRep::destroy(rep);
                meta = make_small_meta();
                payload.small = 0;
            } else if (u == 1) {
                uint64_t v = rep->limbs()[0];
                LargeRep::destroy(rep);
                meta = make_small_meta();
                payload.small = v;
            } else if (u <= 3) {
                // Demote to Medium: copy limbs before freeing rep.
                uint64_t tmp[3] = {};
                std::memcpy(tmp, rep->limbs(), u * sizeof(uint64_t));
                LargeRep::destroy(rep);
                meta            = make_medium_meta(static_cast<uint8_t>(u));
                payload.medium[0] = tmp[0];
                payload.medium[1] = tmp[1];
                payload.medium[2] = tmp[2];
            }
            // else: stays Large, used already trimmed.
            break;
        }
        }
    }

    // ─────────────────────────────────────────────────────
    // Value accessors / conversion
    // ─────────────────────────────────────────────────────

    // True if this value fits in a uint64_t.
    [[nodiscard]] bool fits_u64() const noexcept { return is_small(); }

    [[nodiscard]] uint64_t to_u64() const {
        if (!is_small()) throw std::overflow_error("Hydra: value too large for uint64_t");
        return payload.small;
    }

    // Number of significant limbs (0 = zero value).
    [[nodiscard]] uint32_t limb_count() const noexcept {
        return limb_view().count;
    }

    // ─────────────────────────────────────────────────────
    // Comparison
    // ─────────────────────────────────────────────────────

    [[nodiscard]] int compare(const Hydra& o) const noexcept {
        auto lv = limb_view();
        auto rv = o.limb_view();
        return detail::cmp_limbs(lv.ptr, lv.count, rv.ptr, rv.count);
    }

    [[nodiscard]] friend bool operator==(const Hydra& a, const Hydra& b) noexcept {
        return a.compare(b) == 0;
    }
    [[nodiscard]] friend std::strong_ordering operator<=>(
        const Hydra& a, const Hydra& b) noexcept
    {
        int c = a.compare(b);
        if (c < 0) return std::strong_ordering::less;
        if (c > 0) return std::strong_ordering::greater;
        return std::strong_ordering::equal;
    }

    // ─────────────────────────────────────────────────────
    // Addition kernels
    // ─────────────────────────────────────────────────────

    // ── hot path: Small + Small ──────────────────────────
    //
    // The compiler sees a single branch on overflow; no heap, no
    // function-pointer dispatch. On x86-64/aarch64 this folds into
    // two adds and a conditional branch.
    //
    [[nodiscard]] static Hydra add_small_small(
        uint64_t a, uint64_t b) noexcept
    {
        uint64_t sum;
        if (!__builtin_add_overflow(a, b, &sum))
            return Hydra{ sum };

        // Overflow → result is [wrapped_sum, 1] in two Medium limbs.
        //   a + b = 2^64 + sum  (sum = low bits, carry = 1)
        return make_medium(sum, 1, 0, 2);
    }

    // ── general path via limb arrays ────────────────────

    [[nodiscard]] static Hydra add_general(
        const Hydra& a, const Hydra& b)
    {
        auto lv = a.limb_view();
        auto rv = b.limb_view();

        // Maximum result size: max(na, nb) + 1 limbs.
        uint32_t max_limbs = std::max(lv.count, rv.count) + 1;

        // Stack path: result fits in ≤ 4 limbs — no heap involved at all.
        if (max_limbs <= 4) {
            uint64_t out[4];
            uint32_t used = detail::add_limbs(
                lv.ptr, lv.count, rv.ptr, rv.count, out);
            return from_limbs(out, used);
        }

        // Heap path — write the kernel output *directly* into the final
        // LargeRep, skipping the intermediate std::vector scratch buffer
        // and the subsequent memcpy/memmove that from_limbs would perform.
        //
        // LargeGuard provides exception-safety: if add_limbs or anything
        // downstream throws, the rep is freed before propagation.
        LargeGuard rep{ LargeRep::create(max_limbs) };
        rep->used = detail::add_limbs(
            lv.ptr, lv.count, rv.ptr, rv.count, rep->limbs());

        // Commit the rep into a Hydra, then let normalize() handle trimming
        // and potential demotion to Medium or Small if high limbs are zero.
        Hydra result;
        result.meta          = make_large_meta();
        result.payload.large = rep.release();
        result.normalize();
        return result;
    }

    // ─────────────────────────────────────────────────────
    // Subtraction kernels (unsigned: result saturates at 0 if b > a)
    // ─────────────────────────────────────────────────────

    [[nodiscard]] static Hydra sub_small_small(
        uint64_t a, uint64_t b) noexcept
    {
        // Unsigned: saturate at 0.
        return Hydra{ (a >= b) ? a - b : 0u };
    }

    [[nodiscard]] static Hydra sub_general(
        const Hydra& a, const Hydra& b)
    {
        // Saturate at zero if b > a.
        if (a.compare(b) <= 0) {
            if (a.compare(b) == 0) return Hydra{};
            return Hydra{};   // b > a: underflow → 0 (unsigned sat)
        }

        auto lv = a.limb_view();
        auto rv = b.limb_view();

        if (lv.count <= 4) {
            uint64_t out[4];
            uint32_t used = detail::sub_limbs(
                lv.ptr, lv.count, rv.ptr, rv.count, out);
            return from_limbs(out, used);
        }
        std::vector<uint64_t> out(lv.count);
        uint32_t used = detail::sub_limbs(
            lv.ptr, lv.count, rv.ptr, rv.count, out.data());
        return from_limbs(out.data(), used);
    }

    // ─────────────────────────────────────────────────────
    // Multiplication kernels
    // ─────────────────────────────────────────────────────

    // ── hot path: Small × Small ──────────────────────────
    //
    // Uses __uint128_t — a single mul instruction on most targets.
    //
    [[nodiscard]] static Hydra mul_small_small(
        uint64_t a, uint64_t b) noexcept
    {
        if (a == 0 || b == 0) return Hydra{};

        unsigned __int128 p =
            static_cast<unsigned __int128>(a) * b;

        uint64_t lo = static_cast<uint64_t>(p);
        uint64_t hi = static_cast<uint64_t>(p >> 64);

        if (hi == 0) return Hydra{ lo };
        return make_medium(lo, hi, 0, 2);
    }

    // ── general path ─────────────────────────────────────

    [[nodiscard]] static Hydra mul_general(
        const Hydra& a, const Hydra& b)
    {
        auto lv = a.limb_view();
        auto rv = b.limb_view();

        if (lv.count == 0 || rv.count == 0) return Hydra{};

        // ── Specialized fast paths for common widths ─────────
        //
        // Dispatch to hand-unrolled kernels when both operands
        // fit within a known width. These avoid memset, branch
        // overhead, and separate carry loops.

        uint32_t max_limbs = std::max(lv.count, rv.count);
        uint32_t out_size  = lv.count + rv.count;

        // 3-limb kernel: covers Medium×Medium and any pair where
        // both operands are ≤ 3 limbs (up to 192-bit × 192-bit).
        if (max_limbs <= 3) {
            uint64_t out[6];
            uint32_t used = detail::mul_3x3(
                lv.ptr, lv.count, rv.ptr, rv.count, out);
            return from_limbs(out, used);
        }

        // 4-limb kernel: covers 256-bit × 256-bit exactly.
        if (lv.count == 4 && rv.count == 4) {
            uint64_t out[8];
            uint32_t used = detail::mul_4x4(lv.ptr, rv.ptr, out);
            return from_limbs(out, used);
        }

        // 8-limb kernel: covers 512-bit × 512-bit exactly.
        if (lv.count == 8 && rv.count == 8) {
            uint64_t out[16];
            uint32_t used = detail::mul_8x8(lv.ptr, rv.ptr, out);
            return from_limbs(out, used);
        }

        // ── Generic fallback ─────────────────────────────────
        if (out_size <= 6) {
            uint64_t out[6];
            uint32_t used = detail::mul_limbs(
                lv.ptr, lv.count, rv.ptr, rv.count, out);
            return from_limbs(out, used);
        }
        std::vector<uint64_t> out(out_size);
        uint32_t used = detail::mul_limbs(
            lv.ptr, lv.count, rv.ptr, rv.count, out.data());
        return from_limbs(out.data(), used);
    }

    // ─────────────────────────────────────────────────────
    // Operator overloads — 2D dispatch
    //
    // The dispatch logic is a nested switch on (lhs.kind, rhs.kind).
    // The (Small, Small) arms are explicitly clean so the compiler can
    // inline and optimise them without seeing through virtual dispatch.
    // ─────────────────────────────────────────────────────

    [[nodiscard]] friend Hydra operator+(const Hydra& a, const Hydra& b) {
        // ── hot path ──────────────────────────────────────
        if (a.is_small() && b.is_small()) [[likely]]
            return add_small_small(a.payload.small, b.payload.small);

        // ── general path (all other Kind pairs) ──────────
        return add_general(a, b);
    }

    [[nodiscard]] friend Hydra operator-(const Hydra& a, const Hydra& b) {
        if (a.is_small() && b.is_small()) [[likely]]
            return sub_small_small(a.payload.small, b.payload.small);

        return sub_general(a, b);
    }

    [[nodiscard]] friend Hydra operator*(const Hydra& a, const Hydra& b) {
        if (a.is_small() && b.is_small()) [[likely]]
            return mul_small_small(a.payload.small, b.payload.small);

        return mul_general(a, b);
    }

    // Compound assignment
    //
    // operator+= has a fast path for Large += (anything) when the existing
    // LargeRep has enough capacity.  This avoids:
    //   • LargeRep::create  — no new allocation
    //   • operator=(&&)     — no move-assign / old-rep destruction
    //   • an entire Hydra temporary
    //
    // Safety argument for in-place add_limbs:
    //   add_limbs processes limbs in ascending index order (i = 0, 1, 2 …).
    //   Each iteration reads a[i] (and b[i]) before writing out[i].
    //   When out aliases a (i.e. this->limbs()), each limb is consumed
    //   before it is overwritten.  The internal na<nb swap only reorders
    //   the a/b pointers, not out, so the aliasing property is preserved.
    //   Self-addition (a += a) is also safe: both reads in
    //   s = a[i] + b[i] + carry happen before the write to out[i].
    //
    Hydra& operator+=(const Hydra& rhs) {
        // ── fast path: this is Large and capacity is sufficient ───
        if (is_large()) {
            auto lv = limb_view();          // snapshot *before* mutation
            auto rv = rhs.limb_view();
            uint32_t max_limbs = std::max(lv.count, rv.count) + 1;

            if (payload.large->capacity >= max_limbs) {
                payload.large->used = detail::add_limbs(
                    lv.ptr, lv.count, rv.ptr, rv.count,
                    payload.large->limbs());
                normalize();
                return *this;
            }
        }
        // ── fallback: allocate via operator+ then move-assign ────
        return *this = *this + rhs;
    }

    Hydra& operator-=(const Hydra& o) { return *this = *this - o; }
    Hydra& operator*=(const Hydra& o) { return *this = *this * o; }

    // ─────────────────────────────────────────────────────
    // Bit-shift operators
    //
    // Both operators preserve the normalization invariant:
    //   • the result always occupies the smallest valid tier.
    //   • cross-limb carry (<<) and borrow (>>) are handled
    //     in the detail kernels.
    //
    // Stack buffers cover results up to 4 limbs (256-bit) with
    // zero heap activity — the same threshold used by add_general.
    // The large path allocates directly into a LargeRep so no
    // intermediate copy is needed.
    // ─────────────────────────────────────────────────────

    [[nodiscard]] Hydra operator<<(unsigned shift) const {
        if (shift == 0) return *this;

        auto lv = limb_view();
        if (lv.count == 0) return Hydra{};   // 0 << n = 0

        // ── Small fast path: shift < 64 ─────────────────────
        // Avoids the general kernel for the overwhelmingly common case.
        if (is_small() && shift < 64) {
            // shift > 0 guaranteed (early-return above).
            // 64 - shift is in [1..63] → no UB.
            const uint64_t hi = payload.small >> (64 - shift);
            const uint64_t lo = payload.small << shift;
            if (hi == 0) return Hydra{lo};
            return make_medium(lo, hi, 0, 2);
        }

        // ── General path ─────────────────────────────────────
        // max output limbs: na + whole_limbs + 1
        const uint32_t whole    = static_cast<uint32_t>(shift / 64);
        const uint32_t max_out  = lv.count + whole + 1u;

        // Stack path: result fits in ≤ 4 limbs.
        if (max_out <= 4) {
            uint64_t out[4] = {};
            const uint32_t used = detail::shl_limbs(
                lv.ptr, lv.count, shift, out);
            return from_limbs(out, used);
        }

        // Heap path: write directly into LargeRep.
        LargeGuard rep{LargeRep::create(max_out)};
        rep->used = detail::shl_limbs(
            lv.ptr, lv.count, shift, rep->limbs());
        Hydra result;
        result.meta          = make_large_meta();
        result.payload.large = rep.release();
        result.normalize();
        return result;
    }

    [[nodiscard]] Hydra operator>>(unsigned shift) const {
        if (shift == 0) return *this;

        auto lv = limb_view();
        if (lv.count == 0) return Hydra{};   // 0 >> n = 0

        const uint32_t whole = static_cast<uint32_t>(shift / 64);
        if (whole >= lv.count) return Hydra{};   // all bits shifted out

        // ── Small fast path ──────────────────────────────────
        // For Small, lv.count == 1, so whole == 0 (checked above).
        // shift > 0 and shift < 64 (whole == 0 ⟹ shift < 64).
        if (is_small()) return Hydra{payload.small >> shift};

        // ── General path ─────────────────────────────────────
        // Output has at most (lv.count - whole) limbs.
        const uint32_t max_out = lv.count - whole;

        // Stack path.
        if (max_out <= 4) {
            uint64_t out[4] = {};
            const uint32_t used = detail::shr_limbs(
                lv.ptr, lv.count, shift, out);
            return from_limbs(out, used);
        }

        // Heap path.
        LargeGuard rep{LargeRep::create(max_out)};
        rep->used = detail::shr_limbs(
            lv.ptr, lv.count, shift, rep->limbs());
        Hydra result;
        result.meta          = make_large_meta();
        result.payload.large = rep.release();
        result.normalize();
        return result;
    }

    Hydra& operator<<=(unsigned shift) { return *this = *this << shift; }
    Hydra& operator>>=(unsigned shift) { return *this = *this >> shift; }

    // ─────────────────────────────────────────────────────
    // Scalar division and modulo
    //
    // div_u64 returns the quotient of *this / divisor.
    // mod_u64 returns the remainder (*this % divisor) as a
    //         raw uint64_t — no Hydra allocation.
    //
    // Both use the "short division" algorithm (process limbs
    // MSL→LSL with a running remainder).  The inner step is
    //   q[i] = (rem*2^64 + limbs[i]) / d
    //   rem  = (rem*2^64 + limbs[i]) % d
    // which maps to a single divq / udiv instruction.
    //
    // This is the exact primitive used in Knuth Algorithm D
    // for estimating trial quotient digits (q-hat), making
    // these functions the direct building block for full
    // large ÷ large division.  See DIRECTORS_NOTES.md for
    // the full roadmap.
    //
    // Throws std::domain_error on divisor == 0.
    // ─────────────────────────────────────────────────────

    [[nodiscard]] Hydra div_u64(uint64_t divisor) const {
        if (divisor == 0)
            throw std::domain_error("Hydra::div_u64: division by zero");

        auto lv = limb_view();
        if (lv.count == 0) return Hydra{};   // 0 / d = 0

        // ── Small fast path ──────────────────────────────────
        if (is_small()) return Hydra{payload.small / divisor};

        // ── Stack path: quotient fits in ≤ 4 limbs ──────────
        if (lv.count <= 4) {
            uint64_t q[4];
            detail::divmod_u64_limbs(lv.ptr, lv.count, divisor, q);
            return from_limbs(q, lv.count);   // from_limbs trims
        }

        // ── Heap path: write quotient directly into LargeRep ─
        LargeGuard rep{LargeRep::create(lv.count)};
        detail::divmod_u64_limbs(
            lv.ptr, lv.count, divisor, rep->limbs());
        rep->used = lv.count;
        Hydra result;
        result.meta          = make_large_meta();
        result.payload.large = rep.release();
        result.normalize();   // quotient MSLs may be zero
        return result;
    }

    [[nodiscard]] uint64_t mod_u64(uint64_t divisor) const {
        if (divisor == 0)
            throw std::domain_error("Hydra::mod_u64: division by zero");

        auto lv = limb_view();
        if (lv.count == 0) return 0u;   // 0 % d = 0

        // ── Small fast path ──────────────────────────────────
        if (is_small()) return payload.small % divisor;

        // ── General path: process MSL→LSL without storing quotient.
        // Zero heap activity regardless of operand size.
        uint64_t rem = 0;
        for (uint32_t i = lv.count; i-- > 0;) {
            unsigned __int128 cur =
                (static_cast<unsigned __int128>(rem) << 64) | lv.ptr[i];
            rem = static_cast<uint64_t>(cur % divisor);
        }
        return rem;
    }

    // ─────────────────────────────────────────────────────
    // Debug / inspection
    // ─────────────────────────────────────────────────────

    // Returns decimal string representation.
    // (Slow — for debugging only; not optimised.)
    [[nodiscard]] std::string to_string() const {
        if (is_small()) {
            if (payload.small == 0) return "0";
            char buf[22];
            int i = 21;
            buf[i] = '\0';
            uint64_t v = payload.small;
            while (v) {
                buf[--i] = '0' + static_cast<char>(v % 10);
                v /= 10;
            }
            return std::string(buf + i);
        }
        // Medium / Large: repeated short division by 10 via mod_u64/div_u64.
        // Each iteration is O(n) with zero heap activity for mod_u64,
        // and one LargeRep allocation for div_u64 (on the heap path).
        Hydra copy = *this;
        std::string digits;
        digits.reserve(64);
        while (copy.limb_count() > 0) {
            digits.push_back('0' + static_cast<char>(copy.mod_u64(10)));
            copy = copy.div_u64(10);
        }
        std::reverse(digits.begin(), digits.end());
        return digits.empty() ? "0" : digits;
    }
};

// ─────────────────────────────────────────────────────────
// Convenience literals
// ─────────────────────────────────────────────────────────

inline namespace literals {
    [[nodiscard]] inline Hydra operator""_h(unsigned long long v) noexcept {
        return Hydra{ static_cast<uint64_t>(v) };
    }
} // namespace literals

} // namespace hydra
