// pkg/hydra_binding.cpp — C ABI for the hydra-bignum npm package.
//
// Contract: every number crosses the boundary as a non-negative
// magnitude in LSB-first uint64 limbs — the native shape of
// Hydra::from_limbs / limb_view, so interop is memcpy-flat.  Sign
// normalization and preconditions (mod > 0, exp >= 0, n >= 0) are
// enforced by the JS wrapper (pkg/index.mjs); with those held,
// nothing on these paths throws, which lets the module build
// wasm-EH-free (-fwasm-exceptions costs ~15%, see DIRECTORS_NOTES).
//
// Every `out` buffer is caller-allocated; the required capacity is
// documented per function and guaranteed by the wrapper.  Functions
// return the number of limbs written (0 = zero).

#include "../hydra.hpp"

#include <cstdint>
#include <cstring>

using hydra::Hydra;

namespace {

uint32_t emit(const Hydra& r, uint64_t* out) {
    auto lv = r.limb_view();
    if (lv.count) std::memcpy(out, lv.ptr, lv.count * sizeof(uint64_t));
    return lv.count;
}

} // namespace

extern "C" {

// out capacity: mod_count limbs (result < mod).
// Preconditions (wrapper): mod >= 1, base already reduced into [0, mod).
uint32_t hydra_pow_mod(const uint64_t* base, uint32_t base_count,
                       const uint64_t* exp,  uint32_t exp_count,
                       const uint64_t* mod,  uint32_t mod_count,
                       uint64_t* out)
{
    return emit(hydra::pow_mod(Hydra::from_limbs(base, base_count),
                               Hydra::from_limbs(exp,  exp_count),
                               Hydra::from_limbs(mod,  mod_count)), out);
}

// 1 = probably prime (Baillie-PSW + extra MR rounds), 0 = composite.
int32_t hydra_is_probable_prime(const uint64_t* n, uint32_t n_count,
                                uint32_t extra_mr_rounds)
{
    return hydra::is_probable_prime(Hydra::from_limbs(n, n_count),
                                    extra_mr_rounds) ? 1 : 0;
}

// out capacity: n_count + 1 limbs (Bertrand: next_prime(n) < 2n for n >= 2).
uint32_t hydra_next_prime(const uint64_t* n, uint32_t n_count, uint64_t* out)
{
    return emit(hydra::next_prime(Hydra::from_limbs(n, n_count)), out);
}

// out capacity: max(a_count, b_count) limbs.
uint32_t hydra_gcd(const uint64_t* a, uint32_t a_count,
                   const uint64_t* b, uint32_t b_count,
                   uint64_t* out)
{
    return emit(hydra::gcd(Hydra::from_limbs(a, a_count),
                           Hydra::from_limbs(b, b_count)), out);
}

// out capacity: m_count limbs.  Returns 0xFFFFFFFF when gcd(a, m) != 1
// (no inverse exists).  Preconditions (wrapper): m >= 1, a in [0, m).
uint32_t hydra_mod_inverse(const uint64_t* a, uint32_t a_count,
                           const uint64_t* m, uint32_t m_count,
                           uint64_t* out)
{
    Hydra A = Hydra::from_limbs(a, a_count);
    Hydra M = Hydra::from_limbs(m, m_count);
    auto e = hydra::extended_gcd(A, M);
    if (e.gcd != Hydra{1u}) return 0xFFFFFFFFu;
    Hydra inv = e.x % M;               // truncating %: sign follows dividend
    if (inv.is_negative()) inv += M;
    return emit(inv, out);
}

// out capacity: (n_count + 1) / 2 + 1 limbs.
uint32_t hydra_isqrt(const uint64_t* n, uint32_t n_count, uint64_t* out)
{
    return emit(hydra::isqrt(Hydra::from_limbs(n, n_count)), out);
}

int32_t hydra_is_perfect_square(const uint64_t* n, uint32_t n_count)
{
    return hydra::is_perfect_square(Hydra::from_limbs(n, n_count)) ? 1 : 0;
}

// EXPERIMENTAL A/B probe (borrowed-view import — 2026-07-11 devlog
// entry).  Same contract as hydra_is_perfect_square, but the operand
// is read in place from wasm memory (MagnitudeView) instead of being
// copied into an owning Hydra.  Safe because the JS wrapper keeps the
// wasm-stack operand buffer alive for the whole synchronous call and
// nothing retains the view past it.  Not exposed in pkg/index.mjs;
// exercised by pkg/bench/bench_view_probe.mjs.
int32_t hydra_is_perfect_square_view(const uint64_t* n, uint32_t n_count)
{
    return hydra::detail::is_perfect_square_view(
        hydra::detail::MagnitudeView::trimmed(n, n_count)) ? 1 : 0;
}

} // extern "C"
