// hydra_mul_leaf_aarch64.S
// ──────────────────────────────────────────────────────────────
// Out-of-line aarch64 full-leaf schoolbook multiply for the
// fixed 16×16 Karatsuba base case.  Companion to
// `hydra::detail::mul_limbs` in hydra.hpp — when
// HYDRA_AARCH64_ASM_LEAF is defined at build time and the caller
// has na == nb == 16, mul_limbs forwards the entire product to
// `hydra_mul_limbs_16x16_aarch64` (declared here).
//
// Kernel semantics — identical to the scalar `mul_limbs` fallback
// for the (na=16, nb=16) shape:
//
//   memset(out, 0, 32 * sizeof(uint64_t));
//   for i in [0, 16):
//     for j in [0, 16):
//       out[i+j..i+j+1] += a[i] * b[j]
//
// Structure
// ─────────
// 8 row-pair passes × fully-unrolled 16-step inner body.  Each
// row pair issues the same dual-chain MAC body as
// `hydra_mac_row_2_aarch64` (register-resident chain-B forward,
// suppressed interior stores), but:
//   * nb = 16 is hard-coded → the inner loop is fully unrolled
//     (no `subs w_count / b.ne`, no runtime count tracking);
//   * the b[] base pointer stays put (x1) and every load uses an
//     immediate offset — no `b += 8` per step;
//   * the row-pair outer is a tiny 8-iteration rolled loop
//     (rolling it keeps code size to ~2.5 KB instead of ~20 KB,
//     which matters for I-cache residency when the kernel is
//     called back-to-back inside Karatsuba).
//
// This structure does what the 2026-04-18 row-level asm sprint
// could not: the seam is one C↔asm transition per 16×16 leaf,
// not one per row.  A 4096-bit Montgomery multiply issues 4 calls
// to this kernel, vs 72 calls to the prior mac_row_2 asm.
//
// Why a pair-j-unrolled inner body
// ────────────────────────────────
// At -O3 the portable `mac_row_2` emits a 2-j-per-iteration inner
// loop: `ldp x_b0, x_b1, [b]` pair-loads b[j] and b[j+1], then
// issues 4 mul + 4 umulh for the two j's interleaved.  A 1-j-per-
// iteration asm body *beats* the scalar at the MAC level but
// *regresses* the whole leaf because the doubled branch count
// (16 vs 8 inner ops per row pair) costs more than the saved
// per-MAC instructions.  The full unroll here dodges both
// problems: no branches inside a row pair, and the core sees the
// full 128-MAC-step dependency graph so two chains can run
// genuinely in parallel.
//
// Arg passing (AAPCS64):
//   x0 = a            (const uint64_t*, 16 limbs)
//   x1 = b            (const uint64_t*, 16 limbs)
//   x2 = out          (uint64_t*, 32 limbs writable; contents
//                      fully overwritten)
//
// Clobbers: x3..x17 and flags.  Preserves callee-saved registers
// (x19..x28) and the frame chain (x29, x30).  No stack use.
//
// Correctness: exercised by `test_aarch64_mul_limbs_16x16_
// matches_scalar` (6 operand patterns + 1 dispatch-seam check)
// plus every `mul_limbs` / `mul_karatsuba` / `pow_mod` test.

#if defined(__aarch64__) || defined(_M_ARM64)

#if defined(__APPLE__)
# define HY_NAME(sym) _##sym
#else
# define HY_NAME(sym) sym
#endif

// ──────────────────────────────────────────────────────────────
// One full dual-chain MAC step at offset `off` (bytes into b[]).
//
// Live state:
//   x3  = a0       (chain A multiplier, constant for this row pair)
//   x4  = a1       (chain B multiplier, constant for this row pair)
//   x5  = c0       (running carry, chain A)
//   x6  = c1       (running carry, chain B)
//   x8  = write pointer; on entry at out[row_base + j]; post-incs by 8
//   x9  = chain A accumulator for this step (= out[row_base + j])
//   x1  = b base (immutable through the row pair)
//
// Per step: 1 load of b[j], 1 load of out[row_base + j + 1], 4
// mul+umulh, 8 adds/adcs, 1 store, 1 reg-forward.  Matches the
// existing hydra_mac_aarch64.S body byte-for-byte except nb-loop
// control has been dissolved.
// ──────────────────────────────────────────────────────────────
.macro HYDRA_MAC_STEP off
    ldr     x10, [x1, #\off]        // bj = b[j]
    ldr     x11, [x8, #8]           // out[j+1]; chain B accumulator

    mul     x12, x3, x10            // a0 * bj (lo)
    umulh   x13, x3, x10            // a0 * bj (hi)
    adds    x9, x9, x5              // + c0
    adcs    x13, x13, xzr
    adds    x9, x9, x12
    adc     x5, x13, xzr            // new c0

    mul     x14, x4, x10            // a1 * bj (lo)
    umulh   x15, x4, x10            // a1 * bj (hi)
    adds    x11, x11, x6            // + c1
    adcs    x15, x15, xzr
    adds    x11, x11, x14
    adc     x6, x15, xzr            // new c1

    str     x9, [x8], #8            // commit chain-A's final out[j]
    mov     x9, x11                 // forward chain-B → chain-A for j+1
.endm

    .text
#if !defined(__APPLE__)
    .type   HY_NAME(hydra_mul_limbs_16x16_aarch64), %function
#endif
    .globl  HY_NAME(hydra_mul_limbs_16x16_aarch64)
    .p2align 4
HY_NAME(hydra_mul_limbs_16x16_aarch64):
    // ── Zero out[0..31] ────────────────────────────────────────
    stp     xzr, xzr, [x2, #0]
    stp     xzr, xzr, [x2, #16]
    stp     xzr, xzr, [x2, #32]
    stp     xzr, xzr, [x2, #48]
    stp     xzr, xzr, [x2, #64]
    stp     xzr, xzr, [x2, #80]
    stp     xzr, xzr, [x2, #96]
    stp     xzr, xzr, [x2, #112]
    stp     xzr, xzr, [x2, #128]
    stp     xzr, xzr, [x2, #144]
    stp     xzr, xzr, [x2, #160]
    stp     xzr, xzr, [x2, #176]
    stp     xzr, xzr, [x2, #192]
    stp     xzr, xzr, [x2, #208]
    stp     xzr, xzr, [x2, #224]
    stp     xzr, xzr, [x2, #240]

    // Row-pair counter: 8 row pairs.
    mov     w7, #8

    // Row base pointer (x16) = out + 2*i (bytes = 16*i).
    mov     x16, x2

.Lrow_pair:
    ldp     x3, x4, [x0], #16       // a0, a1 for this row pair
    mov     x5, xzr                 // c0
    mov     x6, xzr                 // c1
    mov     x8, x16                 // write pointer starts at row base
    ldr     x9, [x8]                // chain A accumulator = out[row_base]

    // ── Inner body fully unrolled: 16 MAC steps at fixed offsets ──
    HYDRA_MAC_STEP 0
    HYDRA_MAC_STEP 8
    HYDRA_MAC_STEP 16
    HYDRA_MAC_STEP 24
    HYDRA_MAC_STEP 32
    HYDRA_MAC_STEP 40
    HYDRA_MAC_STEP 48
    HYDRA_MAC_STEP 56
    HYDRA_MAC_STEP 64
    HYDRA_MAC_STEP 72
    HYDRA_MAC_STEP 80
    HYDRA_MAC_STEP 88
    HYDRA_MAC_STEP 96
    HYDRA_MAC_STEP 104
    HYDRA_MAC_STEP 112
    HYDRA_MAC_STEP 120

    // ── Row-pair tail fold ─────────────────────────────────────
    // At end of unroll:
    //   x8  points at row_base + 16 (= out[nb] for this row pair)
    //   x9  holds the virtual chain-B final write for out[nb]
    //   x5 = c0, x6 = c1
    adds    x9, x9, x5              // out[nb] + c0
    str     x9, [x8], #8            // commit; x8 = row_base + 17
    ldr     x10, [x8]
    adcs    x10, x10, x6            // + c1 + carry
    str     x10, [x8], #8           // commit; x8 = row_base + 18
    b.cc    .Lrow_done

.Lprop:
    // Rare: carry propagated past out[nb+1].  Walk upward through
    // out[] until the carry dies.
    ldr     x9, [x8]
    adds    x9, x9, #1
    str     x9, [x8], #8
    b.cs    .Lprop

.Lrow_done:
    add     x16, x16, #16           // next row base: +2 limbs
    subs    w7, w7, #1
    b.ne    .Lrow_pair

    ret

#if !defined(__APPLE__)
    .size   HY_NAME(hydra_mul_limbs_16x16_aarch64), .-HY_NAME(hydra_mul_limbs_16x16_aarch64)
#endif

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif

#endif  // __aarch64__
