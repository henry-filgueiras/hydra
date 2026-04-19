// hydra_mac_aarch64.S
// ──────────────────────────────────────────────────────────────
// Out-of-line aarch64 implementation of the dual-row MAC kernel.
// Companion to `hydra::detail::mac_row_2` in hydra.hpp — when
// HYDRA_AARCH64_ASM is defined at build time, mac_row_2 forwards
// the body to `hydra_mac_row_2_aarch64` (declared here).
//
// Kernel semantics — identical to the scalar fallback:
//
//   For j in [0, nb):
//     out[j]     += a0 * b[j] + c0_in   ;  c0 = overflow
//     out[j + 1] += a1 * b[j] + c1_in   ;  c1 = overflow
//   On exit, fold c0 into out[nb] and c1 into out[nb + 1], then
//   propagate any further carry upward through out[] until it
//   dies.
//
// Machine-level advantages over the C++ version Clang/GCC generate:
//   * Explicit `adcs` carry chaining — the compiler generates
//     separate `adds`/add-with-flags-clobbered sequences around
//     the __int128 lowering, forcing extra instructions.
//   * Register-resident "sliding window" — chain B's write to
//     out[j+1] is forwarded in-register to chain A's read at
//     j+1.  The C++ version cannot prove `b` and `out` don't
//     alias and round-trips the value through memory.
//   * Single `str` per inner iteration instead of two.  Chain B's
//     interior stores are elided; only the final boundary value
//     is written.  At nb=16 that saves ~14 stores per call.
//   * One full pass of the inner loop issues 4 mul/umulh + 6
//     adds/adcs — well-matched to the M-series' ~4 mul/cycle
//     throughput and two-cycle adcs latency.
//
// Arg passing (AAPCS64):
//   x0 = a0
//   x1 = a1
//   x2 = b            (const uint64_t*)
//   w3 = nb           (uint32_t, zero-extended in x3 by callee)
//   x4 = out          (uint64_t*)
//
// Clobbers: x5..x17 and flags.  Preserves callee-saved registers
// (x19..x28) and the frame chain (x29, x30).  No stack use.
//
// Correctness: exercised by the existing hydra_test suite through
// every `mul_limbs`, `mul_karatsuba`, and pow_mod test that runs
// when HYDRA_AARCH64_ASM is defined.

#if defined(__aarch64__) || defined(_M_ARM64)

#if defined(__APPLE__)
# define HY_NAME(sym) _##sym
#else
# define HY_NAME(sym) sym
#endif

    .text
#if !defined(__APPLE__)
    .type   HY_NAME(hydra_mac_row_2_aarch64), %function
#endif
    .globl  HY_NAME(hydra_mac_row_2_aarch64)
    .p2align 4
HY_NAME(hydra_mac_row_2_aarch64):
    // Carry registers, initialised to zero.
    mov     x5, xzr                 // c0 (chain A running carry)
    mov     x6, xzr                 // c1 (chain B running carry)

    // nb == 0: skip the MAC loop, still honour the C++ semantics
    // (fold zero carries into out[0] and out[1] — which means no
    // change in value, but two safe no-op stores are acceptable).
    cbz     w3, .Lnb_zero

    // Preload chain A's initial accumulator (= out[0]).  The inner
    // loop keeps this value in x9; when chain B produces out[j+1]
    // it forwards that register-resident value to chain A for the
    // next iteration.
    ldr     x9, [x4]

    // nb == 1: process one step, then fold.
    cmp     w3, #1
    b.eq    .Ltail1

    // Loop counter (decrement-to-zero).
    mov     w8, w3

    // ── Main inner loop ────────────────────────────────────────
.Lloop:
    ldr     x10, [x2], #8           // bj = *b++
    ldr     x11, [x4, #8]           // out[j+1] (chain B's initial)

    // Chain A: x9 = x9 + a0 * bj + c0, new c0 = high half.
    //
    //   lo  = a0*bj (low 64)
    //   hi  = a0*bj (high 64)
    //   x9  = x9 + c0 (low), hi += carry
    //   x9  = x9 + lo (low), hi += carry
    //   c0  = hi
    mul     x12, x0, x10
    umulh   x13, x0, x10
    adds    x9, x9, x5
    adcs    x13, x13, xzr
    adds    x9, x9, x12
    adc     x5, x13, xzr

    // Chain B: x11 = x11 + a1 * bj + c1, new c1 = high half.
    mul     x14, x1, x10
    umulh   x15, x1, x10
    adds    x11, x11, x6
    adcs    x15, x15, xzr
    adds    x11, x11, x14
    adc     x6, x15, xzr

    // Commit chain A's result to memory and advance `out` by one
    // limb.  Chain B's result is intentionally NOT stored here —
    // it is forwarded in x9 as chain A's next accumulator, and
    // the next iteration's own `str x9, [x4], #8` will persist it
    // in the correct slot.
    str     x9, [x4], #8
    mov     x9, x11

    subs    w8, w8, #1
    b.ne    .Lloop

    // ── End of loop ────────────────────────────────────────────
    //
    // Invariants at this point:
    //   * x4 points at out[nb] (old `out` + nb * 8).
    //   * x9 holds the final chain-B value — the virtual out[nb]
    //     before c0 is folded in.  It has NOT yet been written.
    //   * x5 = c0  (chain A's running carry → goes to out[nb])
    //   * x6 = c1  (chain B's running carry → goes to out[nb+1])
    //
    // The C++ reference code does:
    //     t = out[nb] + c0;          out[nb]   = lo; ch = hi
    //     t = out[nb+1] + c1 + ch;   out[nb+1] = lo; ch = hi
    //     while (ch) propagate
    //
    // We have out[nb]'s "current" value in x9 (since chain B's
    // last write was suppressed), so we don't need to re-load it.
    adds    x9, x9, x5              // + c0 → new out[nb]
    str     x9, [x4], #8            // commit; x4 = out[nb+1]
    ldr     x10, [x4]
    adcs    x10, x10, x6            // + c1 + carry
    str     x10, [x4], #8
    b.cc    .Ldone

.Lprop:                             // rare: carry propagates further
    ldr     x9, [x4]
    adds    x9, x9, #1
    str     x9, [x4], #8
    b.cs    .Lprop

.Ldone:
    ret

    // ── nb == 1 specialisation ─────────────────────────────────
    //
    // Single MAC step inline.  Avoids the main loop's trailing
    // `str` / forward sequence which would not match the "chain A
    // takes chain B's final value" pattern when no next iteration
    // exists.
.Ltail1:
    ldr     x10, [x2]
    ldr     x11, [x4, #8]

    mul     x12, x0, x10
    umulh   x13, x0, x10
    adds    x9, x9, x12
    adc     x13, x13, xzr           // c0 = hi

    mul     x14, x1, x10
    umulh   x15, x1, x10
    adds    x11, x11, x14
    adc     x15, x15, xzr           // c1 = hi

    str     x9, [x4], #8            // out[0] final
    mov     x9, x11                 // x9 = chain B value for fold
    mov     x5, x13
    mov     x6, x15

    adds    x9, x9, x5
    str     x9, [x4], #8
    ldr     x10, [x4]
    adcs    x10, x10, x6
    str     x10, [x4], #8
    b.cc    .Ldone

.Lprop1:
    ldr     x9, [x4]
    adds    x9, x9, #1
    str     x9, [x4], #8
    b.cs    .Lprop1
    ret

    // ── nb == 0 specialisation ─────────────────────────────────
    // No MAC; both carries are zero, so out[0] and out[1] are
    // unchanged.  C++ semantics add zero and write back; we can
    // simply return.
.Lnb_zero:
    ret

#if !defined(__APPLE__)
    .size   HY_NAME(hydra_mac_row_2_aarch64), .-HY_NAME(hydra_mac_row_2_aarch64)
#endif

// Mark the stack as non-executable on ELF platforms (Linux etc.).
#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif

#endif  // __aarch64__
