// bench/mont_interleave_kernels.hpp — fused L-lane Montgomery kernels
// shared by probe_mont_interleave.cpp and probe_pow_mod_batch.cpp.
//
// The e2e experiment funded pow_mod_batch (2026-07-10): the fused
// mul and squaring-run kernels now live in hydra.hpp detail:: and
// are aliased here — these probes are their A/B harness.  The fused
// halved-squaring below stays probe-only: under fusion, sqr-as-mul
// beats it at every k, so production never dispatches it.
//
// Probe finding (2026-07-10): under 2-lane fusion, sqr-as-fused-MUL
// beats the fused halved squaring at every k — the halved kernel's
// shrunken product chain starves the dual-issue slots that fusion
// feeds.  The batch ladder therefore uses the mul kernel for
// everything; the fused halved-sqr is kept here for the record.

#pragma once

#include "../hydra.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace mont_interleave {

// The fused L-lane mul and the squaring-run primitive shipped to
// production (pow_mod_batch engine) — alias, do not duplicate.
using hydra::detail::montgomery_mul_fios_xL;
using hydra::detail::montgomery_sqr_run_xL;

template <int L>
static void montgomery_sqr_fios_halved_xL(
    const uint64_t* const* a,
    uint32_t k,
    const uint64_t* const* mod,
    const uint64_t* n0inv,
    uint64_t* const* out,
    uint64_t* const* work) noexcept
{
    const uint32_t tlen = k + 2;
    for (int l = 0; l < L; ++l) std::memset(work[l], 0, tlen * sizeof(uint64_t));

    for (uint32_t i = 0; i < k; ++i) {
        std::array<uint64_t, L> ai, m, ca, caH, cb;

        for (int l = 0; l < L; ++l) {
            ai[l] = a[l][i];
            uint64_t T0;
            ca[l] = 0; caH[l] = 0;
            if (i == 0) {
                const unsigned __int128 t0 =
                    static_cast<unsigned __int128>(ai[l]) * ai[l] + work[l][0];
                T0    = static_cast<uint64_t>(t0);
                ca[l] = static_cast<uint64_t>(t0 >> 64);
            } else {
                T0 = work[l][0];
            }
            m[l] = T0 * n0inv[l];
            const unsigned __int128 t1 =
                static_cast<unsigned __int128>(m[l]) * mod[l][0] + T0;
            cb[l] = static_cast<uint64_t>(t1 >> 64);
        }

        uint32_t j = 1;

        // Phase 1 (j = 1..i-1): reduce chains only, L-wide.
        for (; j < i; ++j) {
            for (int l = 0; l < L; ++l) {
                const unsigned __int128 tB =
                    static_cast<unsigned __int128>(m[l]) * mod[l][j]
                    + work[l][j]
                    + cb[l];
                work[l][j - 1] = static_cast<uint64_t>(tB);
                cb[l]          = static_cast<uint64_t>(tB >> 64);
            }
        }

        // Phase 2 (j = i, rows i >= 1): undoubled diagonal.
        if (i >= 1) {
            for (int l = 0; l < L; ++l) {
                const unsigned __int128 tA =
                    static_cast<unsigned __int128>(ai[l]) * ai[l] + work[l][i];
                const uint64_t Tj = static_cast<uint64_t>(tA);
                ca[l]             = static_cast<uint64_t>(tA >> 64);

                const unsigned __int128 tB =
                    static_cast<unsigned __int128>(m[l]) * mod[l][i]
                    + Tj
                    + cb[l];
                work[l][i - 1] = static_cast<uint64_t>(tB);
                cb[l]          = static_cast<uint64_t>(tB >> 64);
            }
            j = i + 1;
        }

        // Phase 3 (j = i+1..k-1): doubled cross terms, dual chain, L-wide.
        for (; j < k; ++j) {
            for (int l = 0; l < L; ++l) {
                const unsigned __int128 p =
                    static_cast<unsigned __int128>(ai[l]) * a[l][j];
                const uint64_t p_lo = static_cast<uint64_t>(p);
                const uint64_t p_hi = static_cast<uint64_t>(p >> 64);
                const uint64_t d0 = p_lo << 1;
                const uint64_t d1 = (p_hi << 1) | (p_lo >> 63);
                const uint64_t d2 = p_hi >> 63;

                const unsigned __int128 s =
                    static_cast<unsigned __int128>(d0) + work[l][j] + ca[l];
                const uint64_t Tj = static_cast<uint64_t>(s);
                const uint64_t c0 = static_cast<uint64_t>(s >> 64);

                const unsigned __int128 cnext =
                    static_cast<unsigned __int128>(d1) + c0 + caH[l];
                ca[l]  = static_cast<uint64_t>(cnext);
                caH[l] = static_cast<uint64_t>(cnext >> 64) + d2;

                const unsigned __int128 tB =
                    static_cast<unsigned __int128>(m[l]) * mod[l][j]
                    + Tj
                    + cb[l];
                work[l][j - 1] = static_cast<uint64_t>(tB);
                cb[l]          = static_cast<uint64_t>(tB >> 64);
            }
        }

        // End-of-row fold, all lanes.
        for (int l = 0; l < L; ++l) {
            const unsigned __int128 tkA =
                static_cast<unsigned __int128>(work[l][k]) + ca[l];
            const uint64_t Wp_k = static_cast<uint64_t>(tkA);
            const uint64_t sp1  = static_cast<uint64_t>(tkA >> 64);

            const unsigned __int128 tkB =
                static_cast<unsigned __int128>(Wp_k) + cb[l];
            work[l][k - 1]      = static_cast<uint64_t>(tkB);
            const uint64_t sp2  = static_cast<uint64_t>(tkB >> 64);

            const unsigned __int128 top =
                static_cast<unsigned __int128>(work[l][k + 1]) + caH[l] + sp1 + sp2;
            work[l][k]     = static_cast<uint64_t>(top);
            work[l][k + 1] = static_cast<uint64_t>(top >> 64);
        }
    }

    // Conditional final subtraction, per lane (identical to production).
    for (int l = 0; l < L; ++l) {
        const uint64_t* T = work[l];
        bool need_sub = false;
        if (T[k] != 0) {
            need_sub = true;
        } else {
            for (uint32_t i = k; i-- > 0;) {
                if (T[i] > mod[l][i]) { need_sub = true; break; }
                if (T[i] < mod[l][i]) { need_sub = false; break; }
            }
            if (!need_sub) {
                bool all_equal = true;
                for (uint32_t i = 0; i < k; ++i) {
                    if (T[i] != mod[l][i]) { all_equal = false; break; }
                }
                if (all_equal) need_sub = true;
            }
        }
        if (need_sub) {
            uint64_t borrow = 0;
            for (uint32_t i = 0; i < k; ++i) {
                uint64_t wi = T[i];
                uint64_t mi = mod[l][i];
                uint64_t d1 = wi - mi;
                uint64_t b1 = (d1 > wi) ? 1u : 0u;
                uint64_t d2 = d1 - borrow;
                uint64_t b2 = (d2 > d1) ? 1u : 0u;
                out[l][i] = d2;
                borrow = b1 + b2;
            }
        } else {
            std::memcpy(out[l], T, k * sizeof(uint64_t));
        }
    }
}


} // namespace mont_interleave
