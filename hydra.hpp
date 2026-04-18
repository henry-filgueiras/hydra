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
// Phase-2 semantic completeness: signed arithmetic is now supported.
// Representation: sign-magnitude — the `limbs` hold |value|, and the
// sign is encoded in meta bit 2 (bits::SIGN_BIT).  Zero is always
// non-negative (canonical form: SIGN_BIT clear when limb_count() == 0).
//
// Division truncates toward zero (C++ semantics):
//   quotient sign = sign(dividend) XOR sign(divisor)
//   remainder has sign(dividend), |remainder| < |divisor|
//   invariant: dividend = divisor * quotient + remainder
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
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if HYDRA_PROFILE_KNUTH
#include <chrono>
#endif

// ── NEON detection (ARMv8 / Apple Silicon) ──
//
// All NEON-tuned kernels live inside `#if HYDRA_HAS_NEON` guards so the
// scalar fallback stays the authoritative reference on every other ISA.
// The intrinsics we use are the standard ARM C Language Extensions
// subset (vld1q_u64 / vst1q_u64 / vgetq_lane_u64); no Apple-specific
// hardware-revision probes.
#if defined(__aarch64__) && defined(__ARM_NEON)
#  define HYDRA_HAS_NEON 1
#  include <arm_neon.h>
#else
#  define HYDRA_HAS_NEON 0
#endif

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

// ─────────────────────────────────────────────────────────
// Knuth-D profiling instrumentation (compile-time opt-in)
//
// Enabled only when the TU that *includes* this header defines
// HYDRA_PROFILE_KNUTH=1.  The counters live at namespace scope
// so they are linker-visible; the timing uses high_resolution
// clock sampled once per section per call (not per inner-loop
// iteration) to keep perturbation bounded.
//
// Production builds (HYDRA_PROFILE_KNUTH undefined) expand the
// section macros to empty statements — zero cost, zero symbols.
// ─────────────────────────────────────────────────────────
#if HYDRA_PROFILE_KNUTH
inline uint64_t knuth_prof_normalize_ns   = 0;
inline uint64_t knuth_prof_qhat_est_ns    = 0;
inline uint64_t knuth_prof_qhat_refine_ns = 0;
inline uint64_t knuth_prof_mulsub_ns      = 0;
inline uint64_t knuth_prof_addback_ns     = 0;
inline uint64_t knuth_prof_denormalize_ns = 0;
inline uint64_t knuth_prof_refine_iters   = 0;
inline uint64_t knuth_prof_addback_hits   = 0;
inline uint64_t knuth_prof_outer_steps    = 0;
inline uint64_t knuth_prof_qhat_clamps    = 0;

struct KnuthProfSection {
    uint64_t& sink;
    std::chrono::steady_clock::time_point t0;
    explicit KnuthProfSection(uint64_t& s) noexcept
        : sink(s), t0(std::chrono::steady_clock::now()) {}
    ~KnuthProfSection() noexcept {
        auto t1 = std::chrono::steady_clock::now();
        sink += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
};
#  define HYDRA_KP_CAT2(a, b) a##b
#  define HYDRA_KP_CAT(a, b)  HYDRA_KP_CAT2(a, b)
#  define HYDRA_KNUTH_PROF_SECTION(sink) \
    ::hydra::detail::KnuthProfSection HYDRA_KP_CAT(_knuth_prof_sect_, __LINE__){sink}
#  define HYDRA_KNUTH_PROF_INC(counter, by) \
    ((::hydra::detail::counter) += (by))
#else
#  define HYDRA_KNUTH_PROF_SECTION(sink) ((void)0)
#  define HYDRA_KNUTH_PROF_INC(counter, by) ((void)0)
#endif

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

// ── Single-row multiply-accumulate helper ────────────────────
//
// Computes  out[0..nb-1] += a_i * b[0..nb-1]  and returns the final
// carry-out from the top limb.  Pure scalar — one MUL+UMULH+ADDS+ADCS
// chain per iteration.  The compiler reliably turns this into tight
// aarch64 code; the OoO core happily speculates the mul/umulh of
// j+1 ahead of the j-carry resolution, so throughput saturates the
// mul/umulh ports without further unrolling.
inline uint64_t mac_row_1(
    uint64_t        a_i,
    const uint64_t* b, uint32_t nb,
    uint64_t*       out) noexcept
{
    uint64_t carry = 0;
    for (uint32_t j = 0; j < nb; ++j) {
        unsigned __int128 t =
            static_cast<unsigned __int128>(a_i) * b[j]
            + out[j]
            + carry;
        out[j] = static_cast<uint64_t>(t);
        carry  = static_cast<uint64_t>(t >> 64);
    }
    return carry;
}

// ── Dual-row multiply-accumulate helper ──────────────────────
//
// Computes, for each j in [0, nb):
//   out[j]   += a0 * b[j] + carry0_in
//   out[j+1] += a1 * b[j] + carry1_in
// keeping two INDEPENDENT scalar carry chains.
//
// Why this helps on Apple Silicon (M1+ / M5):
//   • The M-series issues 3-4 MUL/UMULH pairs per cycle.
//   • A single-row MAC (mac_row_1 above) has one chain and saturates
//     ~1-1.5 mul/umulh per cycle — leaving the remaining ports idle.
//   • Running two chains doubles the in-flight multiplies while
//     keeping the serial dependency (`out[j+1]` in chain 0 at j+1
//     reads what chain 1 wrote at j) one step apart — OoO hides it.
//   • NEON vld1q_u64 pair-loads `b[]` into one register; the two
//     lanes are then consumed by the two independent mul chains.
//
// Preconditions:
//   out[0..nb+1] are all readable; out[nb], out[nb+1] hold the
//   accumulator tail (typically zero on entry when called from
//   mul_limbs).  Caller is responsible for folding the returned
//   pair (c0, c1) into out[nb] and out[nb+1] if they aren't
//   already zero-valued on entry.
//
// Returns nothing — the pair's final carries are folded into
// out[nb] (c0) and out[nb+1] (c1) inside this function.
inline void mac_row_2(
    uint64_t        a0, uint64_t a1,
    const uint64_t* b, uint32_t nb,
    uint64_t*       out) noexcept
{
    uint64_t c0 = 0, c1 = 0;

#if HYDRA_HAS_NEON
    // NEON pair-load path: vld1q_u64 fetches two `b[]` limbs into one
    // 128-bit register, which the compiler cleanly splits into the
    // two scalar operands for the MUL/UMULH issued below.  On an
    // out-of-order core this is equivalent to two scalar loads, but
    // consistently places the loads next to the muls so the
    // scheduler doesn't stretch the dependency chain.
    uint32_t j = 0;
    for (; j + 2 <= nb; j += 2) {
        const uint64x2_t bv = vld1q_u64(b + j);
        const uint64_t   bj0 = vgetq_lane_u64(bv, 0);
        const uint64_t   bj1 = vgetq_lane_u64(bv, 1);

        // j: chain 0 writes out[j], chain 1 writes out[j+1].
        unsigned __int128 t0 =
            static_cast<unsigned __int128>(a0) * bj0 + out[j]     + c0;
        unsigned __int128 t1 =
            static_cast<unsigned __int128>(a1) * bj0 + out[j + 1] + c1;
        out[j]     = static_cast<uint64_t>(t0);
        c0         = static_cast<uint64_t>(t0 >> 64);
        const uint64_t mid_hi = static_cast<uint64_t>(t1 >> 64);
        const uint64_t mid_lo = static_cast<uint64_t>(t1);
        // The next iteration of chain 0 will read out[j+1], which we
        // have just produced in (mid_lo).  Fold immediately so the
        // read is satisfied from a register, not from memory.
        out[j + 1] = mid_lo;
        c1         = mid_hi;

        // j+1: chain 0 writes out[j+1], chain 1 writes out[j+2].
        unsigned __int128 t2 =
            static_cast<unsigned __int128>(a0) * bj1 + out[j + 1] + c0;
        unsigned __int128 t3 =
            static_cast<unsigned __int128>(a1) * bj1 + out[j + 2] + c1;
        out[j + 1] = static_cast<uint64_t>(t2);
        c0         = static_cast<uint64_t>(t2 >> 64);
        out[j + 2] = static_cast<uint64_t>(t3);
        c1         = static_cast<uint64_t>(t3 >> 64);
    }
    // Tail (odd nb): one scalar iteration.
    if (j < nb) {
        const uint64_t bj = b[j];
        unsigned __int128 t0 =
            static_cast<unsigned __int128>(a0) * bj + out[j]     + c0;
        unsigned __int128 t1 =
            static_cast<unsigned __int128>(a1) * bj + out[j + 1] + c1;
        out[j]     = static_cast<uint64_t>(t0);
        c0         = static_cast<uint64_t>(t0 >> 64);
        out[j + 1] = static_cast<uint64_t>(t1);
        c1         = static_cast<uint64_t>(t1 >> 64);
        ++j;
    }
#else
    // Scalar fallback: identical data flow, no NEON loads.
    for (uint32_t j = 0; j < nb; ++j) {
        const uint64_t bj = b[j];
        unsigned __int128 t0 =
            static_cast<unsigned __int128>(a0) * bj + out[j]     + c0;
        unsigned __int128 t1 =
            static_cast<unsigned __int128>(a1) * bj + out[j + 1] + c1;
        out[j]     = static_cast<uint64_t>(t0);
        c0         = static_cast<uint64_t>(t0 >> 64);
        out[j + 1] = static_cast<uint64_t>(t1);
        c1         = static_cast<uint64_t>(t1 >> 64);
    }
#endif

    // Fold the two tail carries.  c0 lands at out[nb]; c1 at out[nb+1].
    // On entry to mac_row_2 these slots are normally zero (mul_limbs
    // memsets the output), so the "+ old" reads are simple loads.
    unsigned __int128 t = static_cast<unsigned __int128>(out[nb]) + c0;
    out[nb]     = static_cast<uint64_t>(t);
    uint64_t ch = static_cast<uint64_t>(t >> 64);

    t           = static_cast<unsigned __int128>(out[nb + 1]) + c1 + ch;
    out[nb + 1] = static_cast<uint64_t>(t);
    ch          = static_cast<uint64_t>(t >> 64);

    // Rare: the fold of c1 into out[nb+1] produced its own carry.
    // Propagate upward until it dies.
    for (uint32_t k = nb + 2; ch; ++k) {
        uint64_t s = out[k] + ch;
        ch = (s < out[k]) ? 1u : 0u;
        out[k] = s;
    }
}

// Schoolbook O(n²) multiply. out must have na+nb zeroed limbs.
//
// Row dispatch:
//   • Small leaves (na < 4 or nb < 4) go through the single-row MAC
//     helper — the dual-row setup isn't worth it below that.
//   • Larger leaves pair up `a` two rows at a time and call mac_row_2,
//     which issues two independent mul/umulh chains per inner MAC.
//     This is the only structural change from the prior implementation.
//
// The `a[i] == 0` fast-path from the old loop is dropped: on random
// operand distributions (pow_mod, Karatsuba sum-halves) the branch
// mispredicts often enough to cost more than it saves, and the MACs
// are already cheap when a[i] is zero.
inline uint32_t mul_limbs(
    const uint64_t* a, uint32_t na,
    const uint64_t* b, uint32_t nb,
    uint64_t* out) noexcept
{
    std::memset(out, 0, (na + nb) * sizeof(uint64_t));

    uint32_t i = 0;

    // Dual-row kernel: process a[i], a[i+1] together.  Requires at
    // least 2 a-limbs and nb >= 2 (mac_row_2 writes out[nb] and
    // out[nb+1] as carry slots, so the output window must straddle
    // two extra limbs, which is already true for na+nb >= 4).
    if (nb >= 2) {
        for (; i + 2 <= na; i += 2) {
            mac_row_2(a[i], a[i + 1], b, nb, out + i);
        }
    }

    // Single-row tail: at most one a-limb left (or nb==1).
    for (; i < na; ++i) {
        uint64_t carry = mac_row_1(a[i], b, nb, out + i);
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

// ─────────────────────────────────────────────────────────────
// Karatsuba multiplication (prototype) — Phase 2 benchmark-science
// ─────────────────────────────────────────────────────────────
//
// Recursive Karatsuba over same-sized n-limb operands.  Designed to
// be correct and clean for benchmark-driven crossover measurement
// rather than for peak performance.  Will be wired into mul_general
// only once the measured crossover point justifies it.
//
// Algorithm (for same-size n-limb operands a, b):
//   Split a = a_hi·B + a_lo, b = b_hi·B + b_lo, where B = 2^(64·m),
//   m = n/2, and each half is m limbs (n assumed to be a power of 2
//   so the split is exact; callers pad if needed).
//
//   z0 = a_lo * b_lo       (2m limbs)
//   z2 = a_hi * b_hi       (2m limbs)
//   z1 = (a_lo + a_hi) * (b_lo + b_hi) - z0 - z2
//
//   result = z2·B² + z1·B + z0    (2n limbs)
//
// The three sub-multiplies use mul_karatsuba recursively for z0/z2
// (same-sized) and schoolbook mul_limbs for the middle product
// (operands may be m or m+1 limbs after the carry-producing adds).
// This is a standard hybrid — recursive on the "corners" where the
// subproblems keep the same shape, schoolbook on the "middle" where
// shapes can diverge.
//
// ─────────────────────────────────────────────────────────────

// In-place add: out[0..nout-1] += b[0..nb-1].  Returns the carry-out
// from the highest limb of `out` (0 or 1).  Caller is responsible for
// ensuring out has enough room — if carry-out is nonzero, the result
// did not fit.  Used by Karatsuba's z1 assembly.
inline uint64_t addto_limbs(
    uint64_t* out, uint32_t nout,
    const uint64_t* b, uint32_t nb) noexcept
{
    uint64_t carry = 0;
    uint32_t i = 0;
    const uint32_t paired = (nb < nout) ? nb : nout;
    for (; i < paired; ++i) {
        unsigned __int128 s =
            static_cast<unsigned __int128>(out[i]) + b[i] + carry;
        out[i] = static_cast<uint64_t>(s);
        carry  = static_cast<uint64_t>(s >> 64);
    }
    // If b is longer than out, merge remaining b limbs (plus carry).
    for (; i < nb; ++i) {
        unsigned __int128 s =
            static_cast<unsigned __int128>(b[i]) + carry;
        // No `out[i]` term — we're past out's active region, but
        // caller passed nout to include room for carry. If the caller
        // sized nout == nb, this branch never fires.
        (void)s;
        carry = static_cast<uint64_t>(s >> 64);
    }
    // Propagate remaining carry through out's tail.
    for (; i < nout && carry; ++i) {
        uint64_t t = out[i] + carry;
        carry = (t < carry) ? 1u : 0u;
        out[i] = t;
    }
    return carry;
}

// In-place subtract: out[0..nout-1] -= b[0..nb-1].  Caller must
// guarantee no underflow (the value held in out is >= the value held
// in b).  This is the case for Karatsuba's z1 assembly because
// (a_lo+a_hi)*(b_lo+b_hi) >= a_lo*b_lo + a_hi*b_hi algebraically.
inline void subfrom_limbs(
    uint64_t* out, uint32_t nout,
    const uint64_t* b, uint32_t nb) noexcept
{
    assert(nout >= nb);
    uint64_t borrow = 0;
    uint32_t i = 0;
    for (; i < nb; ++i) {
        uint64_t oi = out[i];
        uint64_t bi = b[i];
        uint64_t d1 = oi - bi;
        uint64_t b1 = (d1 > oi) ? 1u : 0u;
        uint64_t d2 = d1 - borrow;
        uint64_t b2 = (d2 > d1) ? 1u : 0u;
        out[i] = d2;
        borrow = b1 + b2;
    }
    for (; i < nout && borrow; ++i) {
        uint64_t oi = out[i];
        uint64_t d  = oi - borrow;
        borrow = (d > oi) ? 1u : 0u;
        out[i] = d;
    }
    assert(borrow == 0);
}

// ─── Karatsuba thresholds (benchmark-derived) ────────────────
//
// KARATSUBA_THRESHOLD_LIMBS — dispatch threshold.  Below this
//   width, mul_general should stay on the schoolbook/specialised
//   kernels; at or above it, Karatsuba becomes a measured win.
//
//   Measured on Linux aarch64 (sandbox, g++ 11 -O3, median of 3×0.5s
//   repetitions, CV < 0.5%).  Crossover table:
//
//     limbs   schoolbook   karatsuba    winner
//       2        2.6 ns       4.5 ns     schoolbook (+70%)
//       4        7.8 ns       8.8 ns     schoolbook (+13%)
//       8       29.3 ns      28.4 ns     tied
//      16        132 ns       134 ns     tied (K hits base case)
//      32        559 ns       482 ns     karatsuba (−14%)
//      64       2561 ns      1725 ns     karatsuba (−33%)
//
//   32 limbs = 2048 bits is the smallest width where Karatsuba
//   reliably beats schoolbook.  At 16 limbs the measurement is
//   uninformative — the prototype's recursion bottoms out into
//   schoolbook at 16, so the two functions run identical code.
//
//   This constant is declared now but NOT yet wired into
//   mul_general.  The director's Phase-2 policy is "benchmark-
//   derived only, do not replace the existing multiplication path
//   until the crossover is clear."  The crossover is clear; the
//   integration patch is a follow-up once the prototype is
//   promoted out of std::vector-backed scratch.
//
// KARATSUBA_RECURSION_BASE — recursion base case.  Inside the
//   Karatsuba kernel, when the recursive half-size drops to this
//   many limbs we switch to schoolbook.  This is a PROTOTYPE-
//   INTERNAL tuning parameter, not a dispatch threshold.
//
//   Empirically tested at 4 and 16; 16 wins at every measured
//   size because the prototype's per-recursion allocator cost
//   (std::vector in z1 assembly) swamps the algorithmic savings
//   at small half-sizes.  A re-tune to lower values is expected
//   once the scratch strategy switches to a caller-supplied
//   arena.
//
constexpr uint32_t KARATSUBA_THRESHOLD_LIMBS = 32;
constexpr uint32_t KARATSUBA_RECURSION_BASE  = 16;

// ─── ScratchWorkspace — bump arena for limb-level temporaries ───
//
// Hydra's Karatsuba kernel needs O(m) limbs of transient storage per
// recursion frame (two (m+1)-limb sums + one (2m+2)-limb product).
// The original prototype used `std::vector<uint64_t>` per frame, which
// added a measurable allocator tax (~30 ns/frame) that dominated at
// the small recursion depths pow_mod actually hits.
//
// ScratchWorkspace owns one contiguous backing buffer and hands out
// zeroed slices via a bump cursor.  ScratchFrame captures the cursor
// on construction and restores it on destruction, giving LIFO-nested
// recursion semantics at zero per-frame allocator cost.
//
// Pointer stability: callers MUST pre-reserve enough capacity before
// opening any frame — the backing vector is not resized once cursor_
// is non-zero, so pointers handed out earlier stay valid across later
// allocations in the same frame tree.  `karatsuba_scratch_limbs(n)`
// gives the exact bound.  `alloc_zeroed` asserts on overrun.
//
class ScratchWorkspace {
public:
    // Small-buffer size tuned so that every Karatsuba-dispatch
    // operation up to 4096-bit (k=64) stays fully on the stack:
    //
    //     mul_general at n=64:   pa+pb+pout + M(64) = 256 + 200 = 456
    //     pow_mod    at n=64:              M(64)   =           200
    //     pow_mod    at n=32:              M(32)   =            68
    //
    // A 512-limb (4 KB) SBO covers all three cleanly.  Larger
    // operations fall back to heap via the unique_ptr path; they
    // remain correct, just slightly less cache-friendly.
    static constexpr uint32_t SBO_LIMBS = 512;

    ScratchWorkspace() noexcept : buf_(sbo_), cap_(SBO_LIMBS) {}
    ScratchWorkspace(const ScratchWorkspace&) = delete;
    ScratchWorkspace& operator=(const ScratchWorkspace&) = delete;

    // Ensure backing storage holds at least `cap` limbs.  The buffer
    // is intentionally left UNINITIALIZED on grow — `alloc_zeroed`
    // zeroes every region it hands out, so value-initializing the
    // backing on top of that would just double-zero the common case.
    void reserve_limbs(uint32_t cap) {
        if (cap_ < cap) {
            heap_ = std::unique_ptr<uint64_t[]>(new uint64_t[cap]);
            buf_ = heap_.get();
            cap_ = cap;
        }
    }

    uint64_t* alloc_zeroed(uint32_t n) noexcept {
        assert(cursor_ + n <= cap_);
        uint64_t* p = buf_ + cursor_;
        std::memset(p, 0, n * sizeof(uint64_t));
        cursor_ += n;
        return p;
    }

    uint32_t mark() const noexcept { return cursor_; }
    void     rewind(uint32_t m) noexcept { cursor_ = m; }

private:
    uint64_t                    sbo_[SBO_LIMBS];  // stack-inline storage
    std::unique_ptr<uint64_t[]> heap_;            // spill-over allocation
    uint64_t*                   buf_;             // → sbo_ or heap_.get()
    uint32_t                    cap_;
    uint32_t                    cursor_ = 0;
};

// RAII bump-frame: rewinds ScratchWorkspace to its prior cursor on
// destruction.  Supports arbitrary LIFO nesting; each `take(n)` carves
// out a zeroed slice whose lifetime is bounded by the enclosing frame.
class ScratchFrame {
public:
    explicit ScratchFrame(ScratchWorkspace& ws) noexcept
        : ws_(ws), mark_(ws.mark()) {}
    ~ScratchFrame() noexcept { ws_.rewind(mark_); }
    ScratchFrame(const ScratchFrame&) = delete;
    ScratchFrame& operator=(const ScratchFrame&) = delete;

    uint64_t* take(uint32_t n) noexcept { return ws_.alloc_zeroed(n); }

private:
    ScratchWorkspace& ws_;
    uint32_t mark_;
};

// Exact limb-count bound for a single top-level Karatsuba call at
// size n.  At each recursion level only one branch is live at a time
// (the three z0/z2/z1 subproblems run sequentially), so the peak
// scratch footprint is the sum down a single recursion path:
//
//     M(n) = (4·m + 4) + M(m),   m = n/2,   M(base) = 0
//
// where (4m+4) covers sum_a (m+1) + sum_b (m+1) + z1 (2m+2).
inline uint32_t karatsuba_scratch_limbs(uint32_t n) noexcept {
    uint32_t total = 0;
    while (n > KARATSUBA_RECURSION_BASE) {
        const uint32_t m = n >> 1;
        total += 4 * m + 4;
        n = m;
    }
    return total;
}

// Karatsuba multiplication of two same-sized n-limb operands.
// `n` must be a power of 2 and >= 2.  Callers that violate this
// should fall back to schoolbook; the recursion would otherwise
// produce mismatched half-sizes.
//
// `out` must point to 2n limbs of writable storage.  Contents are
// fully overwritten.  Returns the trimmed used-limb count.
//
// `ws` must have at least `karatsuba_scratch_limbs(n)` free limbs
// above its current cursor.  The function opens a ScratchFrame, so
// all scratch is released back to the workspace on return.
inline uint32_t mul_karatsuba(
    const uint64_t* a, const uint64_t* b, uint32_t n,
    uint64_t* out, ScratchWorkspace& ws) noexcept
{
    assert(n >= 2 && (n & (n - 1)) == 0);  // power of two

    if (n <= KARATSUBA_RECURSION_BASE) {
        // Schoolbook base case.  mul_limbs zeroes `out` itself and
        // returns the trimmed used-count.
        return mul_limbs(a, n, b, n, out);
    }

    const uint32_t m = n >> 1;      // half size (exact because n is a pow2)

    const uint64_t* a_lo = a;
    const uint64_t* a_hi = a + m;
    const uint64_t* b_lo = b;
    const uint64_t* b_hi = b + m;

    // Compute z0 directly into out[0..2m-1] and z2 into out[2m..2n-1].
    // The two subproblems are disjoint ranges of `out`, so this is safe.
    (void)mul_karatsuba(a_lo, b_lo, m, out,         ws);
    (void)mul_karatsuba(a_hi, b_hi, m, out + 2 * m, ws);

    // Scratch for z1 assembly, all carved from the bump workspace:
    //   sum_a  : m+1 limbs  (a_lo + a_hi, plus carry slot)
    //   sum_b  : m+1 limbs
    //   z1     : 2m+2 limbs (product (m+1)·(m+1))
    // Total per frame: 4m+4 limbs.  Released when `frame` destructs.
    //
    // One fused `take(4m+4)` is noticeably faster than three separate
    // takes: a single 4m+4-limb memset vectorizes cleanly, whereas
    // three tiny memsets don't hit the SIMD fast path.
    ScratchFrame frame(ws);
    const uint32_t z1_cap = (m + 1) * 2;
    uint64_t* buf   = frame.take(2 * (m + 1) + z1_cap);
    uint64_t* sum_a = buf;
    uint64_t* sum_b = buf + (m + 1);
    uint64_t* z1    = buf + 2 * (m + 1);

    uint32_t sa_used = add_limbs(a_lo, m, a_hi, m, sum_a);
    uint32_t sb_used = add_limbs(b_lo, m, b_hi, m, sum_b);

    // Middle multiply: (sum_a) * (sum_b).  Max size (m+1)*(m+1) = 2m+2.
    (void)mul_limbs(sum_a, sa_used, sum_b, sb_used, z1);

    // z1 := z1 - z0 - z2.  out[0..2m-1] = z0, out[2m..4m-1] = z2.
    //
    // Pass the FULL z1 capacity (2m+2) as the LHS size rather than the
    // trimmed z1_used.  For sparse operands (e.g. zero-padded halves
    // from mul_general's pow2 padding) z1_used can be < 2m, even though
    // algebraically z1 ≥ z0 + z2 always holds.  The trailing positions
    // of z1 are zero-initialized, so subfrom_limbs treats them as zero
    // minuends and propagates borrow exactly as expected.
    subfrom_limbs(z1, z1_cap, out,           2 * m);
    subfrom_limbs(z1, z1_cap, out + 2 * m,   2 * m);

    // Retrim: the subtract may have written into limbs past the
    // original used count.
    uint32_t z1_used = z1_cap;
    while (z1_used > 0 && z1[z1_used - 1] == 0) --z1_used;

    // Accumulate z1 into out at offset m (middle position).
    uint64_t final_carry = addto_limbs(
        out + m, 2 * n - m,
        z1, z1_used);
    assert(final_carry == 0);  // mathematically impossible
    (void)final_carry;

    // Trim.
    uint32_t used = 2 * n;
    while (used > 0 && out[used - 1] == 0) --used;
    return used;
}

// Convenience overload for call sites that lack an outer workspace
// (standalone benchmarks, tests).  Creates a local workspace sized
// exactly for this call.  Hot paths (mul_general, pow_mod_montgomery)
// should use the explicit-workspace overload to amortize backing
// storage across calls.
inline uint32_t mul_karatsuba(
    const uint64_t* a, const uint64_t* b, uint32_t n,
    uint64_t* out)
{
    ScratchWorkspace ws;
    ws.reserve_limbs(karatsuba_scratch_limbs(n));
    return mul_karatsuba(a, b, n, out, ws);
}

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

// ── Knuth Algorithm D (multi-precision long division) ────
//
// Computes quotient = u / v and remainder = u % v for
// unsigned, positive-integer operands.  Implementation
// follows Knuth TAOCP Vol. 2 §4.3.1 Algorithm D.
//
// Input:
//   u_in : dividend,  nu limbs (LSB first), nu >= nv
//   v_in : divisor,   nv limbs (LSB first), nv >= 2,
//                     v_in[nv-1] != 0
//
// Output:
//   q    : quotient,  exactly (nu - nv + 1) limbs (LSB first).
//                     Caller must provide this buffer.
//                     The high limb may be 0 — trim at the API layer.
//   r    : remainder, exactly nv limbs (LSB first).
//                     Caller must provide this buffer.
//                     May have leading zeros — trim at the API layer.
//
// Scratch:
//   work : at least (nu + 1) + nv limbs.  Layout is
//          [ u_norm : nu+1 limbs | v_norm : nv limbs ].
//          Caller owns the buffer; this function zeroes what
//          it needs.
//
// Algorithm:
//   D1.  Normalize: left-shift v until v[nv-1] has bit 63 set.
//        Apply the same shift to u (gains one extra high limb).
//   D2.  For j from (nu-nv) down to 0:
//      D3. Estimate q_hat from the top 2 limbs of u divided
//          by the top limb of v.  Correct q_hat with a 2-step
//          back-off loop using v[nv-2] and u[j+nv-2].
//      D4. Multiply-subtract: u[j..j+nv] -= q_hat * v.
//      D5/D6. If multiply-subtract underflowed, q_hat was too
//          large by exactly 1.  Correct by decrementing q_hat
//          and adding v back (the add's final carry cancels
//          the subtract's borrow — discarded by design).
//      D7. Store q_hat into q[j].
//   D8.  Denormalize: right-shift u[0..nv-1] back into r.
//
// Worst-case q_hat overestimate is 2 before correction; with
// the v[nv-2]/u[j+nv-2] refinement, the add-back case is hit
// at most once per step and — per Knuth's analysis — occurs
// on only ~2/B of inputs (negligible in practice).
//
inline void divmod_knuth_limbs(
    const uint64_t* u_in, uint32_t nu,
    const uint64_t* v_in, uint32_t nv,
    uint64_t* q,
    uint64_t* r,
    uint64_t* work) noexcept
{
    assert(nv >= 2);
    assert(nu >= nv);
    assert(v_in[nv - 1] != 0);

    uint64_t* u = work;                    // nu+1 limbs
    uint64_t* v = work + (nu + 1);         // nv limbs

    // ── D1. Normalize ───────────────────────────────────────
    // Left-shift so the MSL of v has its top bit set.
    const unsigned d = static_cast<unsigned>(
        __builtin_clzll(v_in[nv - 1]));

    {
        HYDRA_KNUTH_PROF_SECTION(knuth_prof_normalize_ns);
        if (d == 0) {
            std::memcpy(u, u_in, nu * sizeof(uint64_t));
            u[nu] = 0;
            std::memcpy(v, v_in, nv * sizeof(uint64_t));
        } else {
            // Shift v left by d bits.
            {
                uint64_t carry = 0;
                for (uint32_t i = 0; i < nv; ++i) {
                    v[i] = (v_in[i] << d) | carry;
                    carry = v_in[i] >> (64 - d);
                }
                // carry must be 0 because clz chose d exactly to land
                // the top bit at position 63.
                assert(carry == 0);
            }
            // Shift u left by d bits; high carry stored in u[nu].
            {
                uint64_t carry = 0;
                for (uint32_t i = 0; i < nu; ++i) {
                    u[i] = (u_in[i] << d) | carry;
                    carry = u_in[i] >> (64 - d);
                }
                u[nu] = carry;
            }
        }
    }

    const uint64_t v_hi = v[nv - 1];   // leading limb, top bit set
    const uint64_t v_lo = v[nv - 2];   // second-from-top limb

    // ── D2–D7. Main loop ────────────────────────────────────
    // Iterate from the top quotient position down to 0.
    const uint32_t m = nu - nv;
    for (int64_t jj = static_cast<int64_t>(m); jj >= 0; --jj) {
        const uint32_t j = static_cast<uint32_t>(jj);

        const uint64_t u_top = u[j + nv];
        const uint64_t u_mid = u[j + nv - 1];

        // ── D3. Estimate q_hat ──────────────────────────────
        uint64_t q_hat;
        uint64_t r_hat;
        bool     r_hat_overflowed = false;

        {
            HYDRA_KNUTH_PROF_SECTION(knuth_prof_qhat_est_ns);
            // Branch-hint experiment (2026-04-16 profiler pass):
            // we measured 0 qhat clamps / step across 1000 random
            // pairs at 256/128, 512/256, 1024/512, 2048/1024 — the
            // `u_top >= v_hi` path is genuinely cold.  Tagging it
            // [[unlikely]] *did* change codegen but the layout shift
            // was a net 2-3 ns regression on the 256/128 and 512/256
            // shapes while only marginally helping 1024/512.  The
            // existing structure already compiles correctly; no hint
            // is applied.  See DIRECTORS_NOTES → "Knuth-D Profiler
            // Pass" for the full A/B matrix.
            if (u_top >= v_hi) {
                q_hat = ~uint64_t{0};
                const uint64_t r_tmp = u_mid + v_hi;
                r_hat_overflowed = (r_tmp < u_mid);
                r_hat = r_tmp;
                HYDRA_KNUTH_PROF_INC(knuth_prof_qhat_clamps, 1);
            } else {
                unsigned __int128 num =
                    (static_cast<unsigned __int128>(u_top) << 64) | u_mid;
                q_hat = static_cast<uint64_t>(num / v_hi);
                r_hat = static_cast<uint64_t>(num % v_hi);
            }
        }

        // Correction loop: refine q_hat using v[nv-2] and u[j+nv-2].
        // Terminates in at most 2 iterations (Knuth §4.3.1 Thm B).
        {
            HYDRA_KNUTH_PROF_SECTION(knuth_prof_qhat_refine_ns);
            if (!r_hat_overflowed) {
                const uint64_t u_low = u[j + nv - 2];
                while (true) {
                    unsigned __int128 lhs =
                        static_cast<unsigned __int128>(q_hat) * v_lo;
                    unsigned __int128 rhs =
                        (static_cast<unsigned __int128>(r_hat) << 64) | u_low;
                    if (lhs <= rhs) break;
                    --q_hat;
                    HYDRA_KNUTH_PROF_INC(knuth_prof_refine_iters, 1);
                    uint64_t new_r = r_hat + v_hi;
                    if (new_r < r_hat) break;  // r_hat now ≥ B; stop
                    r_hat = new_r;
                }
            }
        }

        // ── D4. Multiply-subtract: u[j..j+nv] -= q_hat * v ─
        bool need_add_back = false;
        {
            HYDRA_KNUTH_PROF_SECTION(knuth_prof_mulsub_ns);
            uint64_t borrow = 0;
            uint64_t carry  = 0;
            for (uint32_t i = 0; i < nv; ++i) {
                unsigned __int128 prod =
                    static_cast<unsigned __int128>(q_hat) * v[i] + carry;
                const uint64_t p_lo = static_cast<uint64_t>(prod);
                carry = static_cast<uint64_t>(prod >> 64);

                const uint64_t ui = u[j + i];
                const uint64_t d1 = ui - p_lo;
                const uint64_t b1 = (d1 > ui) ? 1u : 0u;
                const uint64_t d2 = d1 - borrow;
                const uint64_t b2 = (d2 > d1) ? 1u : 0u;
                u[j + i] = d2;
                borrow = b1 + b2;           // provably ≤ 1
            }
            // Top limb: subtract final carry and the loop's borrow.
            const uint64_t ui = u[j + nv];
            const uint64_t d1 = ui - carry;
            const uint64_t b1 = (d1 > ui) ? 1u : 0u;
            const uint64_t d2 = d1 - borrow;
            const uint64_t b2 = (d2 > d1) ? 1u : 0u;
            u[j + nv] = d2;
            need_add_back = (b1 + b2) != 0;
        }

        // ── D5/D6. Add-back correction ──────────────────
        // Triggered when q_hat was too high by exactly 1.
        // Decrement q_hat and add v back; the final carry
        // cancels the subtract's borrow (discarded).
        //
        // Measured frequency: 0 hits/step across 1000 random pairs at
        // every benchmarked shape (see DIRECTORS_NOTES → "Knuth-D
        // Profiler Pass").  Knuth §4.3.1 Thm B bounds this at ~2/B
        // where B = 2^64, so it is unreachable on realistic inputs.
        // Measured 2026-04-16: adding [[unlikely]] here was
        // performance-neutral at every shape (compiler + modern branch
        // predictor already treat the path as cold).  Kept hint-free
        // to avoid the layout perturbation that the paired
        // `[[unlikely]]` on the qhat clamp caused.
        if (need_add_back) {
            HYDRA_KNUTH_PROF_SECTION(knuth_prof_addback_ns);
            HYDRA_KNUTH_PROF_INC(knuth_prof_addback_hits, 1);
            --q_hat;
            uint64_t c = 0;
            for (uint32_t i = 0; i < nv; ++i) {
                unsigned __int128 s =
                    static_cast<unsigned __int128>(u[j + i]) + v[i] + c;
                u[j + i] = static_cast<uint64_t>(s);
                c = static_cast<uint64_t>(s >> 64);
            }
            u[j + nv] += c;   // overflow cancels subtract-borrow
        }

        // ── D7. Store quotient digit ───────────────────────
        q[j] = q_hat;
        HYDRA_KNUTH_PROF_INC(knuth_prof_outer_steps, 1);
    }

    // ── D8. Denormalize: r = u[0..nv-1] >> d ───────────────
    {
        HYDRA_KNUTH_PROF_SECTION(knuth_prof_denormalize_ns);
        if (d == 0) {
            std::memcpy(r, u, nv * sizeof(uint64_t));
        } else {
            for (uint32_t i = 0; i + 1 < nv; ++i) {
                r[i] = (u[i] >> d) | (u[i + 1] << (64 - d));
            }
            r[nv - 1] = u[nv - 1] >> d;
        }
    }
}

// ─── Montgomery multiplication primitives ───────────────────
//
// Montgomery multiplication replaces the expensive modular
// division in pow_mod's inner loop with cheaper shift-and-
// multiply operations.  The idea is to work in "Montgomery
// space" where a value `a` is represented as `a·R mod n`,
// and the reduction step uses only multiplications and shifts.
//
// R = 2^(64·k) where k = limb count of the modulus.
//
// Key precomputed constants (stored in MontgomeryCtx):
//   n0inv  — least-significant limb of -n^{-1} mod 2^64
//   r_sq   — R² mod n, used to convert into Montgomery form
//   n_limbs, mod_limbs — modulus metadata
//
// All limb arrays are LSB-first, matching Hydra's convention.
//

// Compute -n^{-1} mod 2^64 using Hensel lifting.
// Precondition: n0 is odd (i.e. n0 & 1 == 1).
inline uint64_t montgomery_n0inv(uint64_t n0) noexcept {
    // Start with 2-bit inverse and Hensel-lift to 64 bits.
    // inv * n0 ≡ 1 (mod 2^k) at each step; we want -inv mod 2^64.
    uint64_t inv = 1;
    for (int i = 0; i < 6; ++i) {
        inv *= 2 - n0 * inv;   // doubles the number of correct bits
    }
    // inv now satisfies inv * n0 ≡ 1 mod 2^64.
    // We want -n^{-1} mod 2^64 = -inv mod 2^64 = ~inv + 1.
    return 0ull - inv;
}

// Montgomery reduction (REDC): given T (up to 2k limbs),
// compute T·R^{-1} mod n.
//
// Input:
//   t        — product, up to 2·k limbs (may be modified in-place)
//   t_limbs  — number of limbs in t (≤ 2·k)
//   mod      — modulus, k limbs
//   k        — limb count of modulus
//   n0inv    — -n^{-1} mod 2^64
//   out      — result buffer, k limbs
//
// Algorithm (word-by-word Montgomery reduction):
//   for i = 0 to k-1:
//     m = t[i] * n0inv  (mod 2^64)
//     t += m * mod * 2^(64*i)
//   result = t >> (64*k)
//   if result >= mod: result -= mod
//
// We operate on a work buffer of (2k+1) limbs to hold carries.
//
inline void montgomery_redc(
    uint64_t* work,       // scratch of at least 2k+1 limbs; t is copied here
    uint32_t  k,
    const uint64_t* mod,
    uint64_t  n0inv,
    uint64_t* out) noexcept
{
    // Word-by-word reduction
    for (uint32_t i = 0; i < k; ++i) {
        uint64_t m = work[i] * n0inv;

        // work[i..i+k] += m * mod[0..k-1]
        uint64_t carry = 0;
        for (uint32_t j = 0; j < k; ++j) {
            unsigned __int128 t =
                static_cast<unsigned __int128>(m) * mod[j]
                + work[i + j]
                + carry;
            work[i + j] = static_cast<uint64_t>(t);
            carry = static_cast<uint64_t>(t >> 64);
        }
        // Propagate carry through the upper limbs
        for (uint32_t j = i + k; carry && j < 2 * k + 1; ++j) {
            uint64_t s = work[j] + carry;
            carry = (s < work[j]) ? 1u : 0u;
            work[j] = s;
        }
    }

    // Result is work[k .. 2k-1] (the upper half after shifting by R)
    const uint64_t* upper = work + k;

    // Conditional subtraction: if upper >= mod, subtract mod
    // Compare from MSL to LSL
    bool need_sub = false;
    if (work[2 * k] != 0) {
        need_sub = true;  // overflow into extra limb
    } else {
        // Compare upper[0..k-1] against mod[0..k-1]
        for (uint32_t i = k; i-- > 0;) {
            if (upper[i] > mod[i]) { need_sub = true; break; }
            if (upper[i] < mod[i]) { need_sub = false; break; }
        }
        // If all equal, upper == mod → subtract (result should be 0)
        if (!need_sub) {
            bool all_equal = true;
            for (uint32_t i = 0; i < k; ++i) {
                if (upper[i] != mod[i]) { all_equal = false; break; }
            }
            if (all_equal) need_sub = true;
        }
    }

    if (need_sub) {
        uint64_t borrow = 0;
        for (uint32_t i = 0; i < k; ++i) {
            uint64_t ui = upper[i];
            uint64_t mi = mod[i];
            uint64_t d1 = ui - mi;
            uint64_t b1 = (d1 > ui) ? 1u : 0u;
            uint64_t d2 = d1 - borrow;
            uint64_t b2 = (d2 > d1) ? 1u : 0u;
            out[i] = d2;
            borrow = b1 + b2;
        }
    } else {
        std::memcpy(out, upper, k * sizeof(uint64_t));
    }
}

// ─── Fused Montgomery multiply-reduce (CIOS) ─────────────────
//
// Coarsely Integrated Operand Scanning: interleaves multiplication
// and reduction row-by-row, keeping the accumulator at k+2 limbs
// instead of the 2k+1 required by the separate mul+REDC approach.
//
// For each outer row i:
//   1. Multiply-accumulate: T += a[i] * b[0..k-1]
//   2. Reduce: m = T[0] * n0inv; T += m * mod[0..k-1]; T >>= 64
//
// After k rows, T is at most k+1 limbs. A final conditional
// subtraction brings it into [0, mod).
//
// Benefits over separate mul + REDC:
//   - Halved scratch footprint: k+2 limbs vs 2k+1
//   - Better cache locality: accumulator stays in L1 throughout
//   - Fewer memory passes: one fused pass vs two separate O(k²) passes
//
// out[] must have at least k limbs.
// work[] must have at least k+2 limbs.
//
inline void montgomery_mul_fused(
    const uint64_t* a, const uint64_t* b,
    uint32_t k,
    const uint64_t* mod,
    uint64_t n0inv,
    uint64_t* out,
    uint64_t* work) noexcept
{
    // T = work[0..k+1], initialized to zero.
    // T has k+2 limbs to hold the running accumulator.
    const uint32_t tlen = k + 2;
    std::memset(work, 0, tlen * sizeof(uint64_t));

    for (uint32_t i = 0; i < k; ++i) {
        // Step 1: T += a[i] * b[0..k-1]
        uint64_t ai = a[i];
        uint64_t carry = 0;
        for (uint32_t j = 0; j < k; ++j) {
            unsigned __int128 t =
                static_cast<unsigned __int128>(ai) * b[j]
                + work[j]
                + carry;
            work[j] = static_cast<uint64_t>(t);
            carry = static_cast<uint64_t>(t >> 64);
        }
        // Propagate carry into T[k] and T[k+1]
        {
            unsigned __int128 t =
                static_cast<unsigned __int128>(work[k]) + carry;
            work[k] = static_cast<uint64_t>(t);
            work[k + 1] += static_cast<uint64_t>(t >> 64);
        }

        // Step 2: Reduction — m = T[0] * n0inv; T += m * mod; T >>= 64
        uint64_t m = work[0] * n0inv;
        carry = 0;
        for (uint32_t j = 0; j < k; ++j) {
            unsigned __int128 t =
                static_cast<unsigned __int128>(m) * mod[j]
                + work[j]
                + carry;
            work[j] = static_cast<uint64_t>(t);
            carry = static_cast<uint64_t>(t >> 64);
        }
        {
            unsigned __int128 t =
                static_cast<unsigned __int128>(work[k]) + carry;
            work[k] = static_cast<uint64_t>(t);
            work[k + 1] += static_cast<uint64_t>(t >> 64);
        }

        // Shift right by one limb: T >>= 64
        // After the reduction step, work[0] is guaranteed to be 0
        // (by construction: T[0] + m*mod[0] ≡ 0 mod 2^64).
        // So we just shift down.
        for (uint32_t j = 0; j < k + 1; ++j) {
            work[j] = work[j + 1];
        }
        work[k + 1] = 0;
    }

    // Result is in work[0..k-1], with possible overflow in work[k].
    const uint64_t* T = work;

    bool need_sub = false;
    if (T[k] != 0) {
        need_sub = true;
    } else {
        for (uint32_t i = k; i-- > 0;) {
            if (T[i] > mod[i]) { need_sub = true; break; }
            if (T[i] < mod[i]) { need_sub = false; break; }
        }
        if (!need_sub) {
            bool all_equal = true;
            for (uint32_t i = 0; i < k; ++i) {
                if (T[i] != mod[i]) { all_equal = false; break; }
            }
            if (all_equal) need_sub = true;
        }
    }

    if (need_sub) {
        uint64_t borrow = 0;
        for (uint32_t i = 0; i < k; ++i) {
            uint64_t wi = T[i];
            uint64_t mi = mod[i];
            uint64_t d1 = wi - mi;
            uint64_t b1 = (d1 > wi) ? 1u : 0u;
            uint64_t d2 = d1 - borrow;
            uint64_t b2 = (d2 > d1) ? 1u : 0u;
            out[i] = d2;
            borrow = b1 + b2;
        }
    } else {
        std::memcpy(out, T, k * sizeof(uint64_t));
    }
}

// ─── Fused Montgomery squaring (CIOS variant) ────────────────
//
// Same CIOS structure as montgomery_mul_fused, but exploits the
// symmetry a[i]*a[j] == a[j]*a[i] to halve the cross-term work.
//
// For each outer row i, the product-accumulate step is split:
//   - Cross-terms a[i]*a[j] for j > i are doubled and accumulated
//   - The diagonal a[i]*a[i] is added once
//   - Then reduction proceeds as in the multiply case
//
// out[] must have at least k limbs.
// work[] must have at least k+2 limbs.
//
inline void montgomery_sqr_fused(
    const uint64_t* a,
    uint32_t k,
    const uint64_t* mod,
    uint64_t n0inv,
    uint64_t* out,
    uint64_t* work) noexcept
{
    // For squaring, the CIOS row-interleaved approach is trickier because
    // the symmetry optimization requires seeing the full product structure.
    // The cleanest fused squaring that actually wins is to use the existing
    // dedicated squaring product (cross-terms + diagonal) with CIOS reduction
    // interleaved. However, this breaks the row-by-row structure.
    //
    // Instead, we use the straightforward CIOS with a = b (same as mul_fused
    // but with identical operands). The compiler can see the aliasing and
    // the register pressure is identical. The dedicated sqr kernel's advantage
    // comes from halving the product-phase MACs, but in CIOS the product
    // and reduction are interleaved per-row, so we can't easily apply the
    // triangle trick without a fundamentally different algorithm (e.g., SOS).
    //
    // Benchmarking will determine if this is sufficient or if a dedicated
    // SOS (Separated Operand Scanning) squaring is needed.
    montgomery_mul_fused(a, a, k, mod, n0inv, out, work);
}

// Montgomery multiplication: compute a·b·R^{-1} mod n
// Both a and b must be in Montgomery form (i.e. a·R mod n).
// Result is also in Montgomery form.
//
// work must have at least (2k+1) limbs.
//
inline void montgomery_mul(
    const uint64_t* a, const uint64_t* b,
    uint32_t k,
    const uint64_t* mod,
    uint64_t n0inv,
    uint64_t* out,
    uint64_t* work) noexcept
{
    // Step 1: compute product T = a * b (up to 2k limbs)
    // Use the existing mul_limbs kernel.
    uint32_t prod_size = 2 * k;
    std::memset(work, 0, (prod_size + 1) * sizeof(uint64_t));

    // Schoolbook multiply into work buffer
    for (uint32_t i = 0; i < k; ++i) {
        if (a[i] == 0) continue;
        uint64_t carry = 0;
        for (uint32_t j = 0; j < k; ++j) {
            unsigned __int128 t =
                static_cast<unsigned __int128>(a[i]) * b[j]
                + work[i + j]
                + carry;
            work[i + j] = static_cast<uint64_t>(t);
            carry = static_cast<uint64_t>(t >> 64);
        }
        work[i + k] += carry;
        // Note: no further carry propagation needed because
        // work[i+k] was 0 before (from schoolbook structure) + carry < 2^64,
        // so the sum is at most 2^64-1 + 2^64-1 = 2^65-2 which can overflow.
        // We need to propagate:
        if (work[i + k] < carry) {
            for (uint32_t jj = i + k + 1; jj <= prod_size; ++jj) {
                work[jj]++;
                if (work[jj] != 0) break;
            }
        }
    }

    // Step 2: Montgomery reduction
    montgomery_redc(work, k, mod, n0inv, out);
}

// Dedicated Montgomery squaring: compute a²·R^{-1} mod n
// Exploits symmetry of cross-terms: for k limbs, the full product a*a has
// k² term multiplications, but a² only needs k*(k-1)/2 cross-terms
// (each doubled) plus k diagonal terms.  This saves ~25-40% of the
// multiplications in the product phase vs calling montgomery_mul(a,a,...).
//
// work must have at least (2k+1) limbs.
//
inline void montgomery_sqr(
    const uint64_t* a,
    uint32_t k,
    const uint64_t* mod,
    uint64_t n0inv,
    uint64_t* out,
    uint64_t* work) noexcept
{
    uint32_t prod_size = 2 * k;
    std::memset(work, 0, (prod_size + 1) * sizeof(uint64_t));

    // Step 1: Compute cross-terms (i < j) and accumulate doubled.
    // For each pair (i, j) where i < j, a[i]*a[j] appears twice in a².
    for (uint32_t i = 0; i < k; ++i) {
        if (a[i] == 0) continue;
        uint64_t carry = 0;
        for (uint32_t j = i + 1; j < k; ++j) {
            unsigned __int128 t =
                static_cast<unsigned __int128>(a[i]) * a[j]
                + work[i + j]
                + carry;
            work[i + j] = static_cast<uint64_t>(t);
            carry = static_cast<uint64_t>(t >> 64);
        }
        work[i + k] += carry;
        if (work[i + k] < carry) {
            for (uint32_t jj = i + k + 1; jj <= prod_size; ++jj) {
                work[jj]++;
                if (work[jj] != 0) break;
            }
        }
    }

    // Step 2: Double all cross-terms (left-shift the entire work array by 1 bit).
    uint64_t prev_carry = 0;
    for (uint32_t i = 0; i <= prod_size; ++i) {
        uint64_t cur = work[i];
        work[i] = (cur << 1) | prev_carry;
        prev_carry = cur >> 63;
    }

    // Step 3: Add diagonal terms a[i]*a[i] at positions 2*i, 2*i+1.
    uint64_t diag_carry = 0;
    for (uint32_t i = 0; i < k; ++i) {
        unsigned __int128 sq = static_cast<unsigned __int128>(a[i]) * a[i];
        uint64_t lo = static_cast<uint64_t>(sq);
        uint64_t hi = static_cast<uint64_t>(sq >> 64);

        // Add lo to work[2*i]
        unsigned __int128 s = static_cast<unsigned __int128>(work[2 * i])
                              + lo + diag_carry;
        work[2 * i] = static_cast<uint64_t>(s);
        uint64_t c = static_cast<uint64_t>(s >> 64);

        // Add hi to work[2*i+1]
        s = static_cast<unsigned __int128>(work[2 * i + 1]) + hi + c;
        work[2 * i + 1] = static_cast<uint64_t>(s);
        diag_carry = static_cast<uint64_t>(s >> 64);
    }
    // Propagate any final carry
    for (uint32_t i = 2 * k; diag_carry && i <= prod_size; ++i) {
        uint64_t s = work[i] + diag_carry;
        diag_carry = (s < work[i]) ? 1u : 0u;
        work[i] = s;
    }

    // Step 4: Montgomery reduction
    montgomery_redc(work, k, mod, n0inv, out);
}

// ─── Karatsuba-backed Montgomery multiply (separate product + REDC) ───
//
// For large k (≥ KARATSUBA_THRESHOLD_LIMBS), the O(k²) schoolbook product
// inside the fused CIOS path becomes the dominant cost.  This alternate
// backend computes the full 2k-limb product using Karatsuba (O(k^1.585)),
// then applies word-by-word REDC.
//
// Trade-offs vs fused CIOS:
//   + Asymptotically faster product phase (sub-quadratic)
//   - Needs 2k+1 limbs of work buffer (vs k+2 for fused CIOS)
//   - Two separate O(k²) passes (product + REDC) vs one fused pass
//   - Karatsuba scratch (std::vector per recursion frame) adds allocator cost
//
// The crossover must be measured, not guessed.  This function exists to
// enable that measurement.
//
// work[] must have at least 2k+1 limbs.
// kara_buf[] must have at least 2*n_padded limbs (where n_padded is next pow2 >= k).
// pa[], pb[] must have at least n_padded limbs each.
//
inline void montgomery_mul_karatsuba(
    const uint64_t* a, const uint64_t* b,
    uint32_t k,
    const uint64_t* mod,
    uint64_t n0inv,
    uint64_t* out,
    uint64_t* work,
    uint64_t* pa, uint64_t* pb, uint64_t* kara_buf,
    uint32_t n_padded,
    ScratchWorkspace& ws) noexcept
{
    // Step 1: Pad operands to next power of 2 for Karatsuba
    std::memcpy(pa, a, k * sizeof(uint64_t));
    if (n_padded > k) std::memset(pa + k, 0, (n_padded - k) * sizeof(uint64_t));
    std::memcpy(pb, b, k * sizeof(uint64_t));
    if (n_padded > k) std::memset(pb + k, 0, (n_padded - k) * sizeof(uint64_t));

    // Step 2: Karatsuba product → kara_buf (2*n_padded limbs).
    // Uses the caller-supplied ScratchWorkspace to amortize backing
    // storage across every Montgomery multiply in the pow_mod loop.
    (void)mul_karatsuba(pa, pb, n_padded, kara_buf, ws);

    // Step 3: Copy the relevant 2k+1 limbs into work for REDC
    std::memcpy(work, kara_buf, 2 * k * sizeof(uint64_t));
    work[2 * k] = 0;  // overflow slot for REDC carry propagation

    // Step 4: Montgomery reduction
    montgomery_redc(work, k, mod, n0inv, out);
}

// Karatsuba-backed Montgomery squaring.
// Same as montgomery_mul_karatsuba but with a == b, allowing the
// dedicated squaring product (cross-terms + diagonal) to be used
// for the product phase.  For now, we reuse mul_karatsuba with
// identical operands — the squaring optimization inside Karatsuba
// is a future follow-up.
inline void montgomery_sqr_karatsuba(
    const uint64_t* a,
    uint32_t k,
    const uint64_t* mod,
    uint64_t n0inv,
    uint64_t* out,
    uint64_t* work,
    uint64_t* pa, uint64_t* pb, uint64_t* kara_buf,
    uint32_t n_padded,
    ScratchWorkspace& ws) noexcept
{
    montgomery_mul_karatsuba(a, a, k, mod, n0inv, out, work,
                             pa, pb, kara_buf, n_padded, ws);
}

} // namespace detail

// ─────────────────────────────────────────────────────────
// Montgomery context — precomputed constants for a modulus
// ─────────────────────────────────────────────────────────

struct MontgomeryContext {
    std::vector<uint64_t> mod_limbs;  // modulus limb array (k limbs)
    std::vector<uint64_t> r_sq;       // R² mod n (k limbs), for to_mont
    uint64_t n0inv;                   // -n^{-1} mod 2^64
    uint32_t k;                       // limb count of modulus

    // Build context for an odd modulus.
    // mod_ptr points to k_in limbs (LSB first).
    static MontgomeryContext build(
        const uint64_t* mod_ptr, uint32_t k_in)
    {
        MontgomeryContext ctx;
        ctx.k = k_in;
        ctx.mod_limbs.assign(mod_ptr, mod_ptr + k_in);

        // n0inv = -n^{-1} mod 2^64
        ctx.n0inv = detail::montgomery_n0inv(mod_ptr[0]);

        // Compute R² mod n.
        // R = 2^(64*k).  R mod n is computed by creating a (k+1)-limb
        // number with 1 in position k and dividing by n.
        // R² mod n = (R mod n)² mod n, but that still needs a mod.
        //
        // Simpler: compute R mod n first, then square and reduce.
        // R is represented as a (k+1)-limb number: zeros at [0..k-1], 1 at [k].
        //
        // Actually, we compute R mod n using long division of 2^(64*k) by n.
        // Then R² mod n = (R mod n)² mod n using long division again.
        //
        // For the precomputation phase, cost doesn't matter (it's done once),
        // so we use the existing Knuth D machinery via Hydra's divmod.
        //
        // But we need Hydra to be fully defined... Since MontgomeryContext
        // is defined before Hydra, we defer r_sq computation to a separate
        // init function called after Hydra is available.
        ctx.r_sq.resize(k_in, 0);

        return ctx;
    }

    // Helper: compute a mod n at the limb level, handling both
    // single-limb and multi-limb moduli.
    void limb_mod(const uint64_t* a, uint32_t na,
                  uint64_t* r_out) const
    {
        // Trim a
        while (na > 0 && a[na - 1] == 0) --na;
        if (na == 0) {
            std::memset(r_out, 0, k * sizeof(uint64_t));
            return;
        }

        if (k == 1) {
            // Single-limb modulus: use scalar divmod_u64_limbs.
            // We need a quotient buffer (discarded) to satisfy the API.
            std::vector<uint64_t> q(na);
            uint64_t rem = detail::divmod_u64_limbs(a, na, mod_limbs[0], q.data());
            r_out[0] = rem;
            return;
        }

        if (na < k) {
            // a < mod (since a has fewer limbs), so a mod n = a
            std::memcpy(r_out, a, na * sizeof(uint64_t));
            std::memset(r_out + na, 0, (k - na) * sizeof(uint64_t));
            return;
        }

        // General case: Knuth D
        uint32_t q_count = na - k + 1;
        std::vector<uint64_t> q(q_count);
        std::vector<uint64_t> work(na + 1 + k);

        detail::divmod_knuth_limbs(
            a, na,
            mod_limbs.data(), k,
            q.data(), r_out, work.data());
    }

    // Finalize r_sq using limb-level arithmetic (called after Hydra is defined).
    // This computes R² mod n where R = 2^(64*k).
    void compute_r_sq() {
        // Build R = 2^(64*k) as a (k+1)-limb number: zeros at [0..k-1], 1 at [k].
        std::vector<uint64_t> r_val(k + 1, 0);
        r_val[k] = 1;

        // R mod n
        std::vector<uint64_t> r_mod_n(k, 0);
        limb_mod(r_val.data(), k + 1, r_mod_n.data());

        // Compute R² mod n = (R mod n)² mod n.
        // Square: r_mod_n * r_mod_n → up to 2k limbs.
        std::vector<uint64_t> sq(2 * k, 0);
        detail::mul_limbs(r_mod_n.data(), k, r_mod_n.data(), k, sq.data());

        // sq mod n
        uint32_t sq_used = 2 * k;
        while (sq_used > 0 && sq[sq_used - 1] == 0) --sq_used;

        if (sq_used == 0) {
            r_sq.assign(k, 0);
            return;
        }

        r_sq.resize(k, 0);
        limb_mod(sq.data(), sq_used, r_sq.data());
    }

    // Convert a value to Montgomery form: a_mont = a * R mod n
    // Input a must be in [0, n).  a_limbs has up to k limbs.
    void to_montgomery(const uint64_t* a_limbs, uint32_t a_count,
                       uint64_t* out, uint64_t* work) const
    {
        // a_mont = montgomery_mul(a, R² mod n)
        // = a · R² · R^{-1} mod n = a · R mod n
        //
        // Pad a to k limbs on stack (max 64 limbs = 512 bytes)
        constexpr uint32_t MAX_K = 64;
        uint64_t a_padded[MAX_K];
        uint32_t copy_count = (a_count < k) ? a_count : k;
        std::memcpy(a_padded, a_limbs, copy_count * sizeof(uint64_t));
        if (copy_count < k)
            std::memset(a_padded + copy_count, 0,
                        (k - copy_count) * sizeof(uint64_t));

        detail::montgomery_mul(
            a_padded, r_sq.data(),
            k, mod_limbs.data(), n0inv, out, work);
    }

    // Convert from Montgomery form: a = a_mont * R^{-1} mod n
    // = montgomery_redc(a_mont) with b=1 trick:
    // just run redc on a_mont padded to 2k limbs.
    void from_montgomery(const uint64_t* a_mont,
                         uint64_t* out, uint64_t* work) const
    {
        // Copy a_mont into work[0..k-1], zero work[k..2k]
        std::memcpy(work, a_mont, k * sizeof(uint64_t));
        std::memset(work + k, 0, (k + 1) * sizeof(uint64_t));

        detail::montgomery_redc(work, k, mod_limbs.data(), n0inv, out);
    }
};

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

    // ── Sign helpers ──────────────────────────────────────
    //
    // Sign-magnitude encoding: meta bit 2 stores the sign.
    // Invariant: zero is ALWAYS non-negative (SIGN_BIT clear).
    //
    [[nodiscard]] bool is_negative() const noexcept {
        return (meta & bits::SIGN_BIT) != 0;
    }
    [[nodiscard]] bool is_positive() const noexcept {
        return !is_negative() && limb_view().count > 0;
    }
    [[nodiscard]] bool is_zero() const noexcept {
        return limb_view().count == 0;
    }
    void set_negative() noexcept {
        meta |= bits::SIGN_BIT;
    }
    void clear_sign() noexcept {
        meta &= ~bits::SIGN_BIT;
    }
    // Flip the sign. No-op on zero (preserves zero-sign invariant).
    void negate() noexcept {
        if (limb_view().count > 0) meta ^= bits::SIGN_BIT;
    }

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

    // Implicit conversion from all signed integer types.
    // Handles INT64_MIN correctly via unsigned two's-complement arithmetic.
    template<std::signed_integral T>
    constexpr Hydra(T v) noexcept               // NOLINT(google-explicit-constructor)
        : meta(make_small_meta()), payload()
    {
        if (v < 0) {
            // 0 - unsigned(v) is a well-defined two's-complement negate.
            payload.small = 0ull - static_cast<uint64_t>(
                static_cast<int64_t>(v));
            meta |= bits::SIGN_BIT;
        } else {
            payload.small = static_cast<uint64_t>(v);
        }
    }

    // ── String parse constructor ────────────────────────
    //
    // Parses a decimal string with optional leading sign ('+' / '-').
    // Leading zeros are tolerated.  Zero canonicalizes to non-negative.
    // Throws std::invalid_argument on empty input or invalid characters.
    //
    // Strategy: acc = acc * 10 + digit, using existing arithmetic.
    // For performance we accumulate up to 18 decimal digits at a time
    // into a uint64_t (10^18 < 2^63), then multiply-add the chunk:
    //   acc = acc * 10^chunk_len + chunk_value
    //
    explicit Hydra(std::string_view s) : meta(make_small_meta()), payload() {
        if (s.empty())
            throw std::invalid_argument("Hydra: empty string");

        size_t pos = 0;
        bool neg = false;

        // Optional leading sign.
        if (s[0] == '-') { neg = true; ++pos; }
        else if (s[0] == '+') { ++pos; }

        // Must have at least one digit.
        if (pos >= s.size())
            throw std::invalid_argument("Hydra: no digits in string");

        // Skip leading zeros.
        while (pos < s.size() && s[pos] == '0') ++pos;

        // All zeros (or empty after stripping).
        if (pos == s.size()) {
            payload.small = 0;
            return;  // zero is non-negative
        }

        // Validate remaining chars are all digits.
        for (size_t i = pos; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9')
                throw std::invalid_argument(
                    "Hydra: invalid character in string");
        }

        const size_t digit_count = s.size() - pos;

        // Small fast path: ≤ 19 digits might fit in uint64_t.
        // (UINT64_MAX = 18446744073709551615, 20 digits.)
        if (digit_count <= 19) {
            uint64_t v = 0;
            bool overflow = false;
            for (size_t i = pos; i < s.size(); ++i) {
                uint64_t d = static_cast<uint64_t>(s[i] - '0');
                // Check overflow: v * 10 + d > UINT64_MAX
                if (v > (UINT64_MAX - d) / 10) {
                    overflow = true;
                    break;
                }
                v = v * 10 + d;
            }
            if (!overflow) {
                payload.small = v;
                if (neg && v != 0) meta |= bits::SIGN_BIT;
                return;
            }
        }

        // General path: chunk 18 decimal digits at a time.
        // 10^18 = 1000000000000000000 (fits in uint64_t).
        static constexpr uint64_t CHUNK_BASE = 1000000000000000000ull; // 10^18
        static constexpr size_t   CHUNK_LEN  = 18;

        // First chunk may be shorter than CHUNK_LEN.
        size_t first_len = digit_count % CHUNK_LEN;
        if (first_len == 0) first_len = CHUNK_LEN;

        // Parse first chunk.
        uint64_t chunk_val = 0;
        for (size_t i = 0; i < first_len; ++i) {
            chunk_val = chunk_val * 10
                        + static_cast<uint64_t>(s[pos + i] - '0');
        }
        pos += first_len;

        // Start accumulator from first chunk.
        Hydra acc{chunk_val};

        // Process remaining full chunks.
        while (pos < s.size()) {
            chunk_val = 0;
            for (size_t i = 0; i < CHUNK_LEN; ++i) {
                chunk_val = chunk_val * 10
                            + static_cast<uint64_t>(s[pos + i] - '0');
            }
            pos += CHUNK_LEN;

            // acc = acc * CHUNK_BASE + chunk_val
            Hydra base{CHUNK_BASE};
            acc = acc * base + Hydra{chunk_val};
        }

        // Steal result into *this.
        meta = acc.meta;
        std::memcpy(&payload, &acc.payload, sizeof(payload));
        acc.meta = make_small_meta();
        acc.payload.small = 0;

        if (neg && limb_view().count > 0) meta |= bits::SIGN_BIT;
    }

    // Convenience: construct from const char*.
    explicit Hydra(const char* s) : Hydra(std::string_view{s}) {}

    // Convenience: construct from std::string.
    explicit Hydra(const std::string& s) : Hydra(std::string_view{s}) {}

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
        // Preserve sign across Kind transitions.
        // Zero is ALWAYS non-negative (sign bit cleared when magnitude == 0).
        const uint64_t sign = meta & bits::SIGN_BIT;

        switch (kind()) {
        case Kind::Small:
            // Already minimal; enforce zero-sign invariant.
            if (payload.small == 0) meta &= ~bits::SIGN_BIT;
            break;

        case Kind::Medium: {
            // Re-count significant limbs from the top.
            uint8_t u = 3;
            while (u > 0 && payload.medium[u - 1] == 0) --u;

            if (u == 0) {
                meta = make_small_meta();  // zero → clear sign
                payload.small = 0;
            } else if (u == 1) {
                uint64_t v = payload.medium[0];
                meta = make_small_meta() | sign;
                payload.small = v;
            } else {
                set_used_medium_limbs(u);
                // sign bit preserved (set_used_medium_limbs only touches USED_MASK)
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
                meta = make_small_meta();  // zero → clear sign
                payload.small = 0;
            } else if (u == 1) {
                uint64_t v = rep->limbs()[0];
                LargeRep::destroy(rep);
                meta = make_small_meta() | sign;
                payload.small = v;
            } else if (u <= 3) {
                // Demote to Medium: copy limbs before freeing rep.
                uint64_t tmp[3] = {};
                std::memcpy(tmp, rep->limbs(), u * sizeof(uint64_t));
                LargeRep::destroy(rep);
                meta            = make_medium_meta(static_cast<uint8_t>(u)) | sign;
                payload.medium[0] = tmp[0];
                payload.medium[1] = tmp[1];
                payload.medium[2] = tmp[2];
            }
            // else: stays Large, used already trimmed, sign already preserved.
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

    // Magnitude-only compare (ignores sign).
    [[nodiscard]] int compare_magnitude(const Hydra& o) const noexcept {
        auto lv = limb_view();
        auto rv = o.limb_view();
        return detail::cmp_limbs(lv.ptr, lv.count, rv.ptr, rv.count);
    }

    // Full signed compare: positive > negative, magnitudes compared
    // with reversal for both-negative.
    [[nodiscard]] int compare(const Hydra& o) const noexcept {
        const bool a_neg = is_negative();
        const bool b_neg = o.is_negative();

        // Different signs: positive beats negative.
        if (a_neg && !b_neg) return -1;
        if (!a_neg && b_neg) return  1;

        // Same sign: compare magnitudes, reverse if both negative.
        auto lv = limb_view();
        auto rv = o.limb_view();
        int c = detail::cmp_limbs(lv.ptr, lv.count, rv.ptr, rv.count);
        return a_neg ? -c : c;
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
    // Addition kernels (magnitude-level helpers)
    //
    // These operate on magnitude (payload) and return a
    // non-negative result.  The sign-aware operators below
    // set the sign bit as needed.
    // ─────────────────────────────────────────────────────

    // ── hot path: Small + Small (magnitudes) ────────────
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

    // ── general magnitude addition via limb arrays ──────

    [[nodiscard]] static Hydra add_magnitudes(
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
        LargeGuard rep{ LargeRep::create(max_limbs) };
        rep->used = detail::add_limbs(
            lv.ptr, lv.count, rv.ptr, rv.count, rep->limbs());

        Hydra result;
        result.meta          = make_large_meta();
        result.payload.large = rep.release();
        result.normalize();
        return result;
    }

    // ── general magnitude subtraction (|a| >= |b| assumed) ──

    [[nodiscard]] static Hydra sub_magnitudes(
        const Hydra& a, const Hydra& b)
    {
        auto lv = a.limb_view();
        auto rv = b.limb_view();

        if (lv.count == 0) return Hydra{};  // 0 - 0 = 0

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

    // ── sign-aware addition helper (for non-hot-path) ───

    [[nodiscard]] static Hydra add_signed(
        const Hydra& a, const Hydra& b)
    {
        const bool a_neg = a.is_negative();
        const bool b_neg = b.is_negative();

        if (a_neg == b_neg) {
            // Same sign: add magnitudes, keep the common sign.
            Hydra r = add_magnitudes(a, b);
            if (a_neg && r.limb_view().count > 0) r.set_negative();
            return r;
        }

        // Opposite signs: subtract smaller magnitude from larger.
        int c = a.compare_magnitude(b);
        if (c == 0) return Hydra{};   // cancel out

        if (c > 0) {
            // |a| > |b|: result has sign of a
            Hydra r = sub_magnitudes(a, b);
            if (a_neg && r.limb_view().count > 0) r.set_negative();
            return r;
        } else {
            // |b| > |a|: result has sign of b
            Hydra r = sub_magnitudes(b, a);
            if (b_neg && r.limb_view().count > 0) r.set_negative();
            return r;
        }
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

        // ── Karatsuba dispatch seam (Large-tier only) ────────
        //
        // Benchmark-derived crossover: above KARATSUBA_THRESHOLD_LIMBS
        // the recursive kernel's O(n^log2 3) scaling overtakes
        // schoolbook's O(n²).  Below the threshold (including the
        // entire Small and Medium tiers) we fall through to the
        // existing mul_limbs fallback — a single branch of dispatch
        // overhead for those shapes.
        //
        // The Karatsuba kernel requires BOTH operands to be the same
        // size AND the common size to be a power of two.  General
        // Hydra × Hydra multiplication sees arbitrary mixed widths,
        // so we pad both operands up to the next power of two at or
        // above max_limbs.  Zero-padding is correctness-safe (zeros
        // contribute nothing to the product); mul_limbs' `if (a[i]==0)
        // continue` fast-path in the recursion leaves recovers most
        // of the padded waste.
        //
        // The result is always ≤ lv.count + rv.count limbs; anything
        // beyond that is guaranteed zero so from_limbs → normalize
        // trims correctly.
        if (max_limbs >= detail::KARATSUBA_THRESHOLD_LIMBS) {
            uint32_t n = 1;
            while (n < max_limbs) n <<= 1;

            // One heap block for the full Karatsuba operation: padded
            // operands (n + n), output buffer (2n), and recursion
            // scratch.  The vector grows exactly once per top-level
            // mul_general call instead of once per recursion frame.
            detail::ScratchWorkspace ws;
            ws.reserve_limbs(4 * n + detail::karatsuba_scratch_limbs(n));

            detail::ScratchFrame frame(ws);
            uint64_t* pa   = frame.take(n);
            uint64_t* pb   = frame.take(n);
            uint64_t* pout = frame.take(2 * n);

            std::memcpy(pa, lv.ptr, lv.count * sizeof(uint64_t));
            std::memcpy(pb, rv.ptr, rv.count * sizeof(uint64_t));

            uint32_t used = detail::mul_karatsuba(pa, pb, n, pout, ws);
            // `used` is already trimmed by mul_karatsuba; from_limbs
            // handles any final Large→Medium→Small demotion.
            return from_limbs(pout, used);
        }

        // ── Generic fallback (schoolbook) ────────────────────
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

    // ─────────────────────────────────────────────────────
    // Unary operators
    // ─────────────────────────────────────────────────────

    [[nodiscard]] Hydra operator-() const {
        Hydra r = *this;
        r.negate();
        return r;
    }

    [[nodiscard]] Hydra operator+() const { return *this; }

    // ─────────────────────────────────────────────────────
    // Binary arithmetic operators (sign-aware)
    // ─────────────────────────────────────────────────────

    [[nodiscard]] friend Hydra operator+(const Hydra& a, const Hydra& b) {
        // ── hot path: both Small, both non-negative ─────
        // (a.meta | b.meta) == 0 ⟹ both Kind::Small AND both sign-clear.
        if ((a.meta | b.meta) == 0) [[likely]]
            return add_small_small(a.payload.small, b.payload.small);

        // ── sign-aware general path ─────────────────────
        return add_signed(a, b);
    }

    [[nodiscard]] friend Hydra operator-(const Hydra& a, const Hydra& b) {
        // ── hot path: both Small, both non-negative ─────
        if ((a.meta | b.meta) == 0) [[likely]] {
            uint64_t av = a.payload.small, bv = b.payload.small;
            if (av >= bv) return Hydra{av - bv};
            // a < b → negative result
            Hydra r{bv - av};
            r.set_negative();
            return r;
        }

        // General: a - b = a + (-b)
        Hydra neg_b = b;
        neg_b.negate();
        return add_signed(a, neg_b);
    }

    [[nodiscard]] friend Hydra operator*(const Hydra& a, const Hydra& b) {
        const bool sign = a.is_negative() != b.is_negative();

        // ── hot path: both Small (magnitudes) ───────────
        Hydra r;
        if (a.is_small() && b.is_small()) [[likely]]
            r = mul_small_small(a.payload.small, b.payload.small);
        else
            r = mul_general(a, b);

        if (sign && r.limb_view().count > 0) r.set_negative();
        return r;
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
        // ── fast path: this is Large, same sign, capacity sufficient ───
        // The same-sign guard ensures we always add magnitudes (never
        // subtract), so the existing aliasing-safe add_limbs path works.
        if (is_large() && is_negative() == rhs.is_negative()) {
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
    // Bitwise operators (&, |, ^, ~)
    //
    // &, |, ^ operate on non-negative operands only.
    // Throws std::domain_error on negative inputs.
    //
    // ~ uses the two's complement identity: ~x = -(x + 1)
    // This is the standard infinite-precision semantic
    // (matches Python, Java BigInteger).
    // ─────────────────────────────────────────────────────

    [[nodiscard]] Hydra operator~() const {
        // ~x = -(x + 1) for non-negative x
        // ~(-x) = x - 1  for negative x (since ~(~y) = y)
        if (is_negative()) {
            // ~(-|x|) = |x| - 1
            Hydra mag = *this;
            mag.clear_sign();
            return mag - Hydra{1u};
        }
        // ~(+x) = -(x + 1)
        Hydra r = *this + Hydra{1u};
        r.set_negative();
        return r;
    }

    [[nodiscard]] friend Hydra operator&(const Hydra& a, const Hydra& b) {
        if (a.is_negative() || b.is_negative())
            throw std::domain_error("Hydra: bitwise & requires non-negative operands");
        auto lv = a.limb_view();
        auto rv = b.limb_view();
        uint32_t n = std::min(lv.count, rv.count);
        if (n == 0) return Hydra{};

        if (n <= 4) {
            uint64_t out[4];
            for (uint32_t i = 0; i < n; ++i) out[i] = lv.ptr[i] & rv.ptr[i];
            return from_limbs(out, n);
        }
        std::vector<uint64_t> out(n);
        for (uint32_t i = 0; i < n; ++i) out[i] = lv.ptr[i] & rv.ptr[i];
        return from_limbs(out.data(), n);
    }

    [[nodiscard]] friend Hydra operator|(const Hydra& a, const Hydra& b) {
        if (a.is_negative() || b.is_negative())
            throw std::domain_error("Hydra: bitwise | requires non-negative operands");
        auto lv = a.limb_view();
        auto rv = b.limb_view();
        uint32_t n = std::max(lv.count, rv.count);
        if (n == 0) return Hydra{};

        auto fetch = [](const LimbView& v, uint32_t i) -> uint64_t {
            return (i < v.count) ? v.ptr[i] : 0u;
        };
        if (n <= 4) {
            uint64_t out[4];
            for (uint32_t i = 0; i < n; ++i) out[i] = fetch(lv, i) | fetch(rv, i);
            return from_limbs(out, n);
        }
        std::vector<uint64_t> out(n);
        for (uint32_t i = 0; i < n; ++i) out[i] = fetch(lv, i) | fetch(rv, i);
        return from_limbs(out.data(), n);
    }

    [[nodiscard]] friend Hydra operator^(const Hydra& a, const Hydra& b) {
        if (a.is_negative() || b.is_negative())
            throw std::domain_error("Hydra: bitwise ^ requires non-negative operands");
        auto lv = a.limb_view();
        auto rv = b.limb_view();
        uint32_t n = std::max(lv.count, rv.count);
        if (n == 0) return Hydra{};

        auto fetch = [](const LimbView& v, uint32_t i) -> uint64_t {
            return (i < v.count) ? v.ptr[i] : 0u;
        };
        if (n <= 4) {
            uint64_t out[4];
            for (uint32_t i = 0; i < n; ++i) out[i] = fetch(lv, i) ^ fetch(rv, i);
            return from_limbs(out, n);
        }
        std::vector<uint64_t> out(n);
        for (uint32_t i = 0; i < n; ++i) out[i] = fetch(lv, i) ^ fetch(rv, i);
        return from_limbs(out.data(), n);
    }

    Hydra& operator&=(const Hydra& o) { return *this = *this & o; }
    Hydra& operator|=(const Hydra& o) { return *this = *this | o; }
    Hydra& operator^=(const Hydra& o) { return *this = *this ^ o; }

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
    // Full Hydra ÷ Hydra division (Knuth Algorithm D)
    //
    //   divmod(v) → { quotient, remainder } in one pass
    //   div(v)    → delegates to divmod(v).quotient
    //   mod(v)    → delegates to divmod(v).remainder
    //
    // All three preserve the normalization invariant — every
    // returned Hydra occupies the smallest valid tier.
    //
    // Algorithm choice: classical Knuth D over Burnikel–Ziegler.
    // BZ is asymptotically O(n^log2(3)) via recursive halving
    // but its speedup only materialises when the base-case
    // multiply is sub-quadratic (Karatsuba/Toom).  Hydra's
    // current multiplier is schoolbook O(n²) up to 8 limbs
    // with hand-unrolled kernels, so BZ would pay its
    // recursion + allocation overhead without a compensating
    // asymptotic win.  Revisit once Karatsuba lands (Phase 2
    // roadmap item: "Karatsuba / Toom-Cook").
    //
    // Dispatch:
    //   divisor == 0   → std::domain_error
    //   dividend == 0  → { 0, 0 }
    //   dividend < div → { 0, dividend }
    //   div == 1 limb  → delegate to div_u64 / mod_u64
    //   otherwise      → Knuth D via detail::divmod_knuth_limbs
    //
    // Scratch buffer policy:
    //   nu ≤ 32 limbs  → stack buffers (zero heap)
    //   nu >  32 limbs → std::vector (one allocation for
    //                    scratch, plus whatever
    //                    from_limbs() needs for the result)
    //
    // Phase-2 scope: positive integers only.  Signed division
    // (Euclidean vs. truncated) is a separate design decision
    // reserved for when signed arithmetic lands.
    // ─────────────────────────────────────────────────────

    // DivModResult is defined at namespace scope (immediately after
    // this class) because a nested struct cannot contain full Hydra
    // members while Hydra's own definition is still incomplete.
    // The three member functions are declared here and defined
    // out-of-line below DivModResult.
    struct DivModResult;

    [[nodiscard]] inline DivModResult divmod(const Hydra& divisor) const;
    [[nodiscard]] inline Hydra        div   (const Hydra& divisor) const;
    [[nodiscard]] inline Hydra        mod   (const Hydra& divisor) const;

    // ─────────────────────────────────────────────────────
    // Debug / inspection
    // ─────────────────────────────────────────────────────

    // Returns decimal string representation.
    //
    // Small path: direct uint64_t → digits, zero allocations.
    // Medium / Large path: chunked base-10^18 extraction via
    // divmod(10^18), producing 18 decimal digits per division.
    // This is ~18× fewer divisions than the naive mod_u64(10) loop.
    //
    [[nodiscard]] std::string to_string() const {
        const bool neg = is_negative();

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
            if (neg) buf[--i] = '-';
            return std::string(buf + i);
        }

        // Medium / Large: chunked base-10^18 extraction.
        //
        // 10^18 = 1000000000000000000 fits in uint64_t.
        // Each divmod step yields a 18-digit chunk (with leading
        // zeros preserved internally) plus a quotient for the next
        // iteration.
        static constexpr uint64_t CHUNK_BASE = 1000000000000000000ull;
        static constexpr int      CHUNK_LEN  = 18;

        Hydra copy = *this;
        copy.clear_sign();

        // Collect chunks in reverse order.  Each chunk is a
        // uint64_t < 10^18 representing exactly CHUNK_LEN digits
        // (except the most-significant chunk which may have fewer).
        std::vector<uint64_t> chunks;
        chunks.reserve(16);

        while (copy.limb_view().count > 0) {
            uint64_t rem = copy.mod_u64(CHUNK_BASE);
            chunks.push_back(rem);
            copy = copy.div_u64(CHUNK_BASE);
        }

        if (chunks.empty()) return "0";  // shouldn't happen (caught by small path)

        // Build the string: most-significant chunk first (no leading zeros),
        // then remaining chunks with zero-padding to CHUNK_LEN.
        std::string result;
        result.reserve(chunks.size() * CHUNK_LEN + 2);

        if (neg) result.push_back('-');

        // First chunk: variable-width (no leading zeros).
        {
            char buf[20];
            int i = 19;
            buf[i] = '\0';
            uint64_t v = chunks.back();
            if (v == 0) {
                buf[--i] = '0';
            } else {
                while (v) {
                    buf[--i] = '0' + static_cast<char>(v % 10);
                    v /= 10;
                }
            }
            result.append(buf + i);
        }

        // Remaining chunks: zero-padded to exactly CHUNK_LEN digits.
        for (int ci = static_cast<int>(chunks.size()) - 2; ci >= 0; --ci) {
            char buf[CHUNK_LEN + 1];
            uint64_t v = chunks[ci];
            for (int j = CHUNK_LEN - 1; j >= 0; --j) {
                buf[j] = '0' + static_cast<char>(v % 10);
                v /= 10;
            }
            buf[CHUNK_LEN] = '\0';
            result.append(buf);
        }

        return result;
    }

    // Stream insertion operator.
    friend std::ostream& operator<<(std::ostream& os, const Hydra& h) {
        return os << h.to_string();
    }
};

// ─────────────────────────────────────────────────────────
// DivModResult (nested-but-out-of-line)
//
// Declared inside Hydra as a forward declaration; defined
// here now that Hydra is complete.  Keeps the API pair
// (quotient, remainder) returned in a single call without
// forcing callers to pattern-match on an optional.
// ─────────────────────────────────────────────────────────

struct Hydra::DivModResult {
    Hydra quotient;
    Hydra remainder;
};

// ─────────────────────────────────────────────────────────
// Out-of-line definitions for divmod / div / mod
// ─────────────────────────────────────────────────────────

inline Hydra::DivModResult Hydra::divmod(const Hydra& divisor) const {
    // ── Divisor == 0: throw ─────────────────────────────
    auto dv = divisor.limb_view();
    if (dv.count == 0)
        throw std::domain_error("Hydra::divmod: division by zero");

    // ── Dividend == 0: trivially { 0, 0 } ──────────────
    auto uv = limb_view();
    if (uv.count == 0)
        return { Hydra{}, Hydra{} };

    // ── Sign handling (truncation toward zero) ──────────
    //
    // C++ semantics:
    //   quotient sign  = sign(dividend) XOR sign(divisor)
    //   remainder sign = sign(dividend)
    //   invariant: dividend == divisor * quotient + remainder
    //
    const bool u_neg = is_negative();
    const bool v_neg = divisor.is_negative();
    const bool q_neg = u_neg != v_neg;     // quotient sign
    const bool r_neg = u_neg;              // remainder sign

    // ── Magnitude ordering short-circuits ───────────────
    const int cmp = compare_magnitude(divisor);
    if (cmp < 0)  {
        // |dividend| < |divisor| → q=0, r=dividend (preserve dividend sign)
        return { Hydra{}, *this };
    }
    if (cmp == 0) {
        // |dividend| == |divisor| → q=±1, r=0
        Hydra q{1u};
        if (q_neg) q.set_negative();
        return { q, Hydra{} };
    }

    // ── Single-limb divisor: delegate to scalar path ───
    if (dv.count == 1) {
        const uint64_t d = dv.ptr[0];
        Hydra q = div_u64(d);     // magnitude quotient
        Hydra r{mod_u64(d)};      // magnitude remainder
        if (q_neg && q.limb_view().count > 0) q.set_negative();
        if (r_neg && r.limb_view().count > 0) r.set_negative();
        return { q, r };
    }

    // ── General multi-limb case: Knuth Algorithm D ─────
    const uint32_t nu = uv.count;
    const uint32_t nv = dv.count;
    const uint32_t nq = nu - nv + 1;

    constexpr uint32_t STACK_LIMIT = 32;

    uint64_t  q_stack[STACK_LIMIT + 1];
    uint64_t  r_stack[STACK_LIMIT];
    uint64_t  work_stack[(STACK_LIMIT + 1) + STACK_LIMIT];

    uint64_t* q_buf    = nullptr;
    uint64_t* r_buf    = nullptr;
    uint64_t* work_buf = nullptr;

    std::vector<uint64_t> q_heap, r_heap, work_heap;

    if (nu <= STACK_LIMIT) {
        q_buf    = q_stack;
        r_buf    = r_stack;
        work_buf = work_stack;
    } else {
        q_heap.resize(nq);
        r_heap.resize(nv);
        work_heap.resize(static_cast<size_t>(nu) + 1u + nv);
        q_buf    = q_heap.data();
        r_buf    = r_heap.data();
        work_buf = work_heap.data();
    }

    detail::divmod_knuth_limbs(
        uv.ptr, nu,
        dv.ptr, nv,
        q_buf, r_buf, work_buf);

    // Build magnitude results, then apply signs.
    Hydra q = Hydra::from_limbs(q_buf, nq);
    Hydra r = Hydra::from_limbs(r_buf, nv);
    if (q_neg && q.limb_view().count > 0) q.set_negative();
    if (r_neg && r.limb_view().count > 0) r.set_negative();
    return { q, r };
}

inline Hydra Hydra::div(const Hydra& divisor) const {
    return divmod(divisor).quotient;
}

inline Hydra Hydra::mod(const Hydra& divisor) const {
    return divmod(divisor).remainder;
}

// ─────────────────────────────────────────────────────────
// Convenience: abs, operator/, operator%
// ─────────────────────────────────────────────────────────

[[nodiscard]] inline Hydra abs(Hydra x) {
    if (x.is_negative()) return -x;
    return x;
}

[[nodiscard]] inline Hydra operator/(const Hydra& a, const Hydra& b) {
    return a.div(b);
}

[[nodiscard]] inline Hydra operator%(const Hydra& a, const Hydra& b) {
    return a.mod(b);
}

inline Hydra& operator/=(Hydra& a, const Hydra& b) { return a = a / b; }
inline Hydra& operator%=(Hydra& a, const Hydra& b) { return a = a % b; }

// ─────────────────────────────────────────────────────────
// Number theory: gcd, extended_gcd, pow_mod
// ─────────────────────────────────────────────────────────

// Euclid's algorithm.  Result is always non-negative.
// gcd(0, x) == abs(x), gcd(0, 0) == 0.
[[nodiscard]] inline Hydra gcd(Hydra a, Hydra b) {
    a = abs(std::move(a));
    b = abs(std::move(b));

    while (b != Hydra{0u}) {
        auto r = a % b;
        a = std::move(b);
        b = std::move(r);
    }
    return a;
}

// Bézout coefficients: a*x + b*y == gcd(a,b), gcd >= 0.
struct EGCDResult {
    Hydra gcd;
    Hydra x;
    Hydra y;
};

[[nodiscard]] inline EGCDResult extended_gcd(const Hydra& a, const Hydra& b) {
    // Iterative extended Euclidean on magnitudes, then fix signs.
    Hydra old_r = abs(a), r = abs(b);
    Hydra old_s{1u},      s{0u};
    Hydra old_t{0u},      t{1u};

    while (r != Hydra{0u}) {
        Hydra q = old_r / r;

        {   // (old_r, r) = (r, old_r - q*r)
            Hydra tmp = old_r - q * r;
            old_r = std::move(r);
            r     = std::move(tmp);
        }
        {   // (old_s, s) = (s, old_s - q*s)
            Hydra tmp = old_s - q * s;
            old_s = std::move(s);
            s     = std::move(tmp);
        }
        {   // (old_t, t) = (t, old_t - q*t)
            Hydra tmp = old_t - q * t;
            old_t = std::move(t);
            t     = std::move(tmp);
        }
    }

    // old_r = gcd(|a|, |b|)
    // old_s * |a| + old_t * |b| == old_r
    // Adjust signs: if a was negative, negate x; if b was negative, negate y.
    if (a.is_negative()) old_s = -old_s;
    if (b.is_negative()) old_t = -old_t;

    return { std::move(old_r), std::move(old_s), std::move(old_t) };
}

// Binary modular exponentiation: (base^exp) mod mod.
// Requires mod > 0, exp >= 0.  Throws std::domain_error otherwise.
//
// Dispatch:
//   - odd modulus → Montgomery fast path (avoids division in inner loop)
//   - even modulus → fallback naive path (division-based)
//

// ── Naive (division-based) path ─────────────────────────
// Preserved as reference implementation and fallback for even moduli.
[[nodiscard]] inline Hydra pow_mod_naive(Hydra base, Hydra exp, const Hydra& mod) {
    Hydra result{1u};
    base = base % mod;
    if (base.is_negative()) base = base + mod;

    while (exp > Hydra{0u}) {
        if (!((exp & Hydra{1u}).is_zero())) {
            result = (result * base) % mod;
        }
        base = (base * base) % mod;
        exp >>= 1;
    }
    return result;
}

// ── Montgomery fast path (odd modulus) ──────────────────
//
// All squarings and multiplications in the inner loop use
// Montgomery reduction instead of full modular division.
// The conversion to/from Montgomery form (2 mul+redc each)
// is a one-time cost amortised over O(n) squarings.
//
// Uses a 4-bit sliding window for exponentiation:
//   - Precompute odd powers base^1, base^3, ..., base^15 in Montgomery form
//   - Process exponent in 4-bit windows from MSB to LSB
//   - Reduces multiplications from ~n/2 to ~n/4 for n-bit exponents
//
// All scratch buffers use stack arrays for k <= MONTGOMERY_MAX_LIMBS (64),
// avoiding heap allocation in the hot path.
//
[[nodiscard]] inline Hydra pow_mod_montgomery(
    Hydra base, Hydra exp, const Hydra& mod)
{
    auto mod_lv = mod.limb_view();
    uint32_t k = mod_lv.count;

    // Build Montgomery context
    MontgomeryContext ctx = MontgomeryContext::build(mod_lv.ptr, k);
    ctx.compute_r_sq();

    // Reduce base into [0, mod)
    base = base % mod;
    if (base.is_negative()) base = base + mod;

    auto base_lv = base.limb_view();

    // ── Stack-based scratch buffers ──
    // MONTGOMERY_MAX_LIMBS is 64 (4096 bits).  All buffers fit on stack.
    //
    // Three Montgomery multiply backends:
    //   1. Separate schoolbook + REDC  (k < FUSED_THRESHOLD)
    //   2. Fused CIOS                  (FUSED_THRESHOLD <= k < KARATSUBA_MONT_THRESHOLD)
    //   3. Separate Karatsuba + REDC   (k >= KARATSUBA_MONT_THRESHOLD)
    //
    // The Karatsuba backend replaces the O(k²) product phase with
    // O(k^1.585), then runs the same word-by-word REDC.  The REDC
    // itself is still O(k²), but at large k the product phase dominates
    // total cost in the fused path (both multiply-accumulate and reduce
    // are O(k²) per row × k rows).
    //
    // The threshold is benchmark-derived.  Set to 0 (disabled) initially;
    // the benchmark pass below will measure the actual crossover and
    // update this constant.
    constexpr uint32_t MAX_K = 64;
    constexpr uint32_t FUSED_THRESHOLD = 8;  // k >= 8 → use fused CIOS
    // Next power of 2 >= MAX_K for Karatsuba padding
    constexpr uint32_t MAX_K_PADDED = 64;    // MAX_K is already a power of 2

    // KARATSUBA_MONT_THRESHOLD: minimum k for Karatsuba-backed Montgomery.
    // Benchmark-derived: Karatsuba + REDC beats fused CIOS at k=32 (−9%)
    // and k=64 (−16%), but LOSES at k=48 (+22%) because 48 pads to 64.
    // Guard: only use Karatsuba when the padding overhead is bounded —
    // n_padded / k <= 1.25 (i.e. at most 25% wasted work from padding).
    constexpr uint32_t KARATSUBA_MONT_THRESHOLD = 32;

    uint32_t n_padded_test = 1;
    while (n_padded_test < k) n_padded_test <<= 1;
    // Padding ratio guard: reject if padding would waste > 25% of work
    const bool pad_ok = (n_padded_test <= k + k / 4);
    const bool use_karatsuba = (k >= KARATSUBA_MONT_THRESHOLD && pad_ok);
    const bool use_fused = (!use_karatsuba && k >= FUSED_THRESHOLD);

    uint64_t work_buf[2 * MAX_K + 1];
    uint64_t temp_buf[MAX_K];
    uint64_t result_mont_buf[MAX_K];

    // Zero-init the working buffers
    std::memset(work_buf, 0, (2 * k + 1) * sizeof(uint64_t));
    std::memset(temp_buf, 0, k * sizeof(uint64_t));
    std::memset(result_mont_buf, 0, k * sizeof(uint64_t));

    // ── Karatsuba scratch buffers (only used when use_karatsuba) ──
    // n_padded = next power of 2 >= k (Karatsuba requires power-of-2 sizes)
    uint32_t n_padded = 1;
    if (use_karatsuba) {
        while (n_padded < k) n_padded <<= 1;
    }
    // Stack-allocate at MAX_K_PADDED for bounded stack usage.
    // pa, pb: padded operand copies (n_padded limbs each)
    // kara_buf: Karatsuba output (2 * n_padded limbs)
    uint64_t kara_pa[MAX_K_PADDED];
    uint64_t kara_pb[MAX_K_PADDED];
    uint64_t kara_buf[2 * MAX_K_PADDED];

    // Karatsuba recursion scratch — one heap block, reused across
    // every mont_mul / mont_sqr call in the entire exponentiation
    // loop.  Without this, the old code paid ~3 std::vector allocs
    // per frame × 12 frames per mul × ~1000 muls per pow_mod at
    // 4096-bit.  We pre-reserve the exact worst-case depth bound
    // once; no further allocation occurs.
    detail::ScratchWorkspace kara_ws;
    if (use_karatsuba) {
        kara_ws.reserve_limbs(detail::karatsuba_scratch_limbs(n_padded));
    }

    // ── Sliding window precomputation ──
    // Window size W=4: precompute base^1, base^3, base^5, ..., base^15
    // in Montgomery form.  table[i] = base^(2i+1) for i in [0..7].
    constexpr uint32_t WINDOW = 4;
    constexpr uint32_t TABLE_SIZE = 1u << (WINDOW - 1);  // 8
    uint64_t table[TABLE_SIZE][MAX_K];

    // ── Montgomery mul/sqr dispatch helpers ──
    // Three-tier dispatch:
    //   k >= KARATSUBA_MONT_THRESHOLD → Karatsuba product + REDC
    //   k >= FUSED_THRESHOLD          → fused CIOS (interleaved multiply-reduce)
    //   k < FUSED_THRESHOLD           → separate schoolbook + REDC
    auto mont_mul = [&](const uint64_t* a, const uint64_t* b,
                        uint64_t* out) {
        if (use_karatsuba)
            detail::montgomery_mul_karatsuba(a, b, k, ctx.mod_limbs.data(),
                                              ctx.n0inv, out, work_buf,
                                              kara_pa, kara_pb, kara_buf,
                                              n_padded, kara_ws);
        else if (use_fused)
            detail::montgomery_mul_fused(a, b, k, ctx.mod_limbs.data(),
                                          ctx.n0inv, out, work_buf);
        else
            detail::montgomery_mul(a, b, k, ctx.mod_limbs.data(),
                                    ctx.n0inv, out, work_buf);
    };
    auto mont_sqr = [&](const uint64_t* a, uint64_t* out) {
        if (use_karatsuba)
            detail::montgomery_sqr_karatsuba(a, k, ctx.mod_limbs.data(),
                                              ctx.n0inv, out, work_buf,
                                              kara_pa, kara_pb, kara_buf,
                                              n_padded, kara_ws);
        else if (use_fused)
            detail::montgomery_sqr_fused(a, k, ctx.mod_limbs.data(),
                                          ctx.n0inv, out, work_buf);
        else
            detail::montgomery_sqr(a, k, ctx.mod_limbs.data(),
                                    ctx.n0inv, out, work_buf);
    };

    // table[0] = base in Montgomery form
    {
        // Pad base to k limbs on stack
        uint64_t a_padded[MAX_K];
        std::memset(a_padded, 0, k * sizeof(uint64_t));
        uint32_t copy_count = (base_lv.count < k) ? base_lv.count : k;
        std::memcpy(a_padded, base_lv.ptr, copy_count * sizeof(uint64_t));

        mont_mul(a_padded, ctx.r_sq.data(), table[0]);
    }

    // base_sq = base² in Montgomery form (used to build odd powers)
    uint64_t base_sq[MAX_K];
    mont_sqr(table[0], base_sq);

    // table[i] = table[i-1] * base_sq = base^(2i+1)
    for (uint32_t i = 1; i < TABLE_SIZE; ++i) {
        mont_mul(table[i - 1], base_sq, table[i]);
    }

    // result_mont = 1 in Montgomery form = R mod n
    {
        uint64_t one_padded[MAX_K];
        std::memset(one_padded, 0, k * sizeof(uint64_t));
        one_padded[0] = 1;
        mont_mul(one_padded, ctx.r_sq.data(), result_mont_buf);
    }

    // ── Extract exponent bits ──
    // We need to process from MSB to LSB for sliding window.
    // Extract all exponent limbs and find the highest set bit.
    auto exp_lv = exp.limb_view();
    uint32_t exp_limb_count = exp_lv.count;
    if (exp_limb_count == 0) {
        // exp == 0 → result is 1 mod n (already handled by dispatch, but be safe)
        uint64_t result_limbs_buf[MAX_K];
        std::memset(result_limbs_buf, 0, k * sizeof(uint64_t));
        ctx.from_montgomery(result_mont_buf, result_limbs_buf, work_buf);
        uint32_t used = k;
        while (used > 0 && result_limbs_buf[used - 1] == 0) --used;
        if (used == 0) return Hydra{0u};
        return Hydra::from_limbs(result_limbs_buf, used);
    }

    // Find highest set bit position (0-indexed from LSB across all limbs)
    uint32_t top_limb_idx = exp_limb_count - 1;
    uint64_t top_limb = exp_lv.ptr[top_limb_idx];
    int top_bit = 63 - __builtin_clzll(top_limb);  // bit within top limb
    int total_bits = static_cast<int>(top_limb_idx) * 64 + top_bit;

    // ── MSB-to-LSB sliding window exponentiation ──
    // Process exponent bits from position total_bits down to 0.
    // Uses fused CIOS kernels for k >= FUSED_THRESHOLD, separate
    // mul+REDC below.
    int bit_pos = total_bits;
    while (bit_pos >= 0) {
        // Get current bit
        uint32_t limb_idx = static_cast<uint32_t>(bit_pos) / 64;
        uint32_t bit_idx = static_cast<uint32_t>(bit_pos) % 64;
        uint64_t cur_bit = (exp_lv.ptr[limb_idx] >> bit_idx) & 1u;

        if (cur_bit == 0) {
            // Square and move on
            mont_sqr(result_mont_buf, temp_buf);
            std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            --bit_pos;
        } else {
            // Collect a window of up to WINDOW bits starting at bit_pos.
            // The window value is the integer formed by bits [bit_pos .. bit_pos - len + 1].
            // We want the window to end with a 1 bit (odd value) for table lookup.
            int window_len = WINDOW;
            if (bit_pos < static_cast<int>(WINDOW) - 1) window_len = bit_pos + 1;

            // Extract window_len bits starting from bit_pos (MSB first)
            uint32_t wval = 0;
            for (int i = 0; i < window_len; ++i) {
                int bp = bit_pos - i;
                uint32_t li = static_cast<uint32_t>(bp) / 64;
                uint32_t bi = static_cast<uint32_t>(bp) % 64;
                uint32_t b = (exp_lv.ptr[li] >> bi) & 1u;
                wval = (wval << 1) | b;
            }

            // Strip trailing zeros from window to ensure odd lookup
            int trailing_zeros = 0;
            while (window_len > 1 && (wval & 1u) == 0) {
                wval >>= 1;
                window_len--;
                trailing_zeros++;
            }

            // Square window_len times
            for (int i = 0; i < window_len; ++i) {
                mont_sqr(result_mont_buf, temp_buf);
                std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            }

            // Multiply by table[(wval-1)/2] (wval is odd)
            uint32_t table_idx = (wval - 1) / 2;
            mont_mul(result_mont_buf, table[table_idx], temp_buf);
            std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));

            // Square for each trailing zero
            for (int i = 0; i < trailing_zeros; ++i) {
                mont_sqr(result_mont_buf, temp_buf);
                std::memcpy(result_mont_buf, temp_buf, k * sizeof(uint64_t));
            }

            bit_pos -= (window_len + trailing_zeros);
        }
    }

    // Convert result back from Montgomery form
    uint64_t result_limbs_buf[MAX_K];
    std::memset(result_limbs_buf, 0, k * sizeof(uint64_t));
    ctx.from_montgomery(result_mont_buf, result_limbs_buf, work_buf);

    // Trim and build Hydra
    uint32_t used = k;
    while (used > 0 && result_limbs_buf[used - 1] == 0) --used;
    if (used == 0) return Hydra{0u};
    return Hydra::from_limbs(result_limbs_buf, used);
}

// ── Public dispatch ─────────────────────────────────────
[[nodiscard]] inline Hydra pow_mod(Hydra base, Hydra exp, const Hydra& mod) {
    if (mod <= Hydra{0u})
        throw std::domain_error("pow_mod: modulus must be positive");
    if (exp.is_negative())
        throw std::domain_error("pow_mod: exponent must be non-negative");

    // Handle mod == 1 early: any number mod 1 == 0.
    if (mod == Hydra{1u}) return Hydra{0u};

    // Dispatch: odd modulus → Montgomery, even → naive fallback.
    //
    // Montgomery's advantage comes from replacing per-iteration
    // division with multiplication-only reduction.  With dedicated
    // squaring, sliding-window exponentiation, and stack-based
    // scratch buffers, Montgomery is a clear win at all sizes up to
    // 64 limbs (4096 bits).  The threshold was raised from 48 to 64
    // after eliminating per-call heap allocation.
    constexpr uint32_t MONTGOMERY_MAX_LIMBS = 64;

    auto mod_lv = mod.limb_view();
    if (mod_lv.count > 0 && mod_lv.count <= MONTGOMERY_MAX_LIMBS
        && (mod_lv.ptr[0] & 1u)) {
        return pow_mod_montgomery(base, std::move(exp), mod);
    }
    return pow_mod_naive(std::move(base), std::move(exp), mod);
}

// ─────────────────────────────────────────────────────────
// Convenience literals
// ─────────────────────────────────────────────────────────

inline namespace literals {
    [[nodiscard]] inline Hydra operator""_h(unsigned long long v) noexcept {
        return Hydra{ static_cast<uint64_t>(v) };
    }
} // namespace literals

} // namespace hydra
