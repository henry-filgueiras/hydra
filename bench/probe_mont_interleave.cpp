// bench/probe_mont_interleave.cpp — B1 opening probe: does interleaving
// N *independent* Montgomery multiplications beat N serial ones?
//
// The single-op FIOS kernel is bound by its serial carry chains; the
// multiplier's latency (3-5 cy) vs throughput (~1/cy) gap is idle time
// no asm can recover (measured null, see Resolved Dragons).  Batching
// independent exponentiations is the structural change: this probe
// measures the ceiling of that idea at the kernel layer, three ways —
//
//   serial   one lane, chained (x = mont(x, b)) — the ladder shape.
//   call×L   L independent chained lanes, interleaved at CALL level.
//            Tests whether the OoO window alone overlaps adjacent
//            kernel calls (if yes, a batch API needs no fused kernel).
//   fused×L  L lanes interleaved *inside* the row loops — per-j, the
//            2L MAC chains of L lanes are adjacent instructions, so
//            lane B's multiplies can fill lane A's carry bubbles.
//            Register-pressure risk: L=2 needs ~2× live state, L=4 ~4×
//            (31 GPRs on aarch64 — spill can eat the win).
//
// All fused outputs are checked bit-identical to the production
// montgomery_mul_fios per lane before anything is timed.
//
// Kernel-probe wins can invert e2e (house rule) — a win here FUNDS the
// e2e pow_mod_batch experiment, it does not conclude it.
//
// Build (native):
//   clang++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       bench/probe_mont_interleave.cpp -o build-rel/probe_mont_interleave
// Build (wasm — LLVM-only pipeline, see the binaryen dragon):
//   emcc -std=c++20 -O2 -g -DNDEBUG -I. bench/probe_mont_interleave.cpp \
//       -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608 \
//       -o build-wasm/probe_mont_interleave.js
//   (then strip or not — timing is unaffected by the DWARF section)

#include "../hydra.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;
using hydra::detail::montgomery_mul_fios;

// ── fused L-lane FIOS ────────────────────────────────────────────────
// Same row structure as montgomery_mul_fios; all per-lane state is
// std::array<.., L> with the lane loops fully unrolled (L is a
// template constant), so per-lane scalars can live in registers.
template <int L>
static void montgomery_mul_fios_xL(
    const uint64_t* const* a, const uint64_t* const* b,
    uint32_t k,
    const uint64_t* const* mod,
    const uint64_t* n0inv,
    uint64_t* const* out,
    uint64_t* const* work) noexcept
{
    const uint32_t tlen = k + 2;
    for (int l = 0; l < L; ++l) std::memset(work[l], 0, tlen * sizeof(uint64_t));

    for (uint32_t i = 0; i < k; ++i) {
        std::array<uint64_t, L> ai, m, ca, cb;

        // Bootstrap (j = 0), all lanes.
        for (int l = 0; l < L; ++l) {
            ai[l] = a[l][i];
            const unsigned __int128 t0 =
                static_cast<unsigned __int128>(ai[l]) * b[l][0] + work[l][0];
            const uint64_t T0 = static_cast<uint64_t>(t0);
            ca[l]             = static_cast<uint64_t>(t0 >> 64);
            m[l]              = T0 * n0inv[l];
            const unsigned __int128 t1 =
                static_cast<unsigned __int128>(m[l]) * mod[l][0] + T0;
            cb[l]             = static_cast<uint64_t>(t1 >> 64);
        }

        // Inner loop: per j, the 2L chains are adjacent instructions.
        for (uint32_t j = 1; j < k; ++j) {
            for (int l = 0; l < L; ++l) {
                const unsigned __int128 tA =
                    static_cast<unsigned __int128>(ai[l]) * b[l][j]
                    + work[l][j]
                    + ca[l];
                const uint64_t Tj = static_cast<uint64_t>(tA);
                ca[l]             = static_cast<uint64_t>(tA >> 64);

                const unsigned __int128 tB =
                    static_cast<unsigned __int128>(m[l]) * mod[l][j]
                    + Tj
                    + cb[l];
                work[l][j - 1]    = static_cast<uint64_t>(tB);
                cb[l]             = static_cast<uint64_t>(tB >> 64);
            }
        }

        // End-of-row fold, all lanes.
        for (int l = 0; l < L; ++l) {
            const unsigned __int128 tkA =
                static_cast<unsigned __int128>(work[l][k]) + ca[l];
            const uint64_t Wp_k  = static_cast<uint64_t>(tkA);
            const uint64_t ca_hi = static_cast<uint64_t>(tkA >> 64);

            const unsigned __int128 tkB =
                static_cast<unsigned __int128>(Wp_k) + cb[l];
            work[l][k - 1]       = static_cast<uint64_t>(tkB);
            const uint64_t cb_hi = static_cast<uint64_t>(tkB >> 64);

            const unsigned __int128 top =
                static_cast<unsigned __int128>(work[l][k + 1]) + ca_hi + cb_hi;
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

// ── per-lane operand set ─────────────────────────────────────────────
struct Lane {
    std::vector<uint64_t> mod, b, x, work;
    uint64_t n0inv;
};

static Lane make_lane(uint32_t k, uint64_t seed) {
    std::mt19937_64 rng(seed);
    Lane ln;
    ln.mod.resize(k); ln.b.resize(k); ln.x.resize(k); ln.work.resize(k + 2);
    for (auto& v : ln.mod) v = rng();
    ln.mod[0]     |= 1u;
    ln.mod[k - 1] |= (1ull << 63);
    for (auto& v : ln.b) v = rng();
    for (auto& v : ln.x) v = rng();
    ln.x[k - 1] &= ~(1ull << 63);          // keep x < mod-ish; kernel is total anyway
    ln.n0inv = hydra::detail::montgomery_n0inv(ln.mod[0]);
    return ln;
}

template <typename Fn>
static double best_ns(Fn&& fn, int samples, int reps, int warmup) {
    double best = 1e300;
    for (int s = 0; s < samples; ++s) {
        for (int i = 0; i < warmup; ++i) fn();
        auto t0 = clk::now();
        for (int i = 0; i < reps; ++i) fn();
        auto t1 = clk::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
        if (ns < best) best = ns;
    }
    return best;
}

// One chained mont-mul per lane per rep; returns ns per SINGLE mul.
template <int L>
static double run_call_level(std::array<Lane, 4>& lanes, uint32_t k,
                             int samples, int reps, int warmup) {
    return best_ns([&]() {
        for (int l = 0; l < L; ++l) {
            Lane& ln = lanes[l];
            montgomery_mul_fios(ln.x.data(), ln.b.data(), k,
                                ln.mod.data(), ln.n0inv,
                                ln.x.data(), ln.work.data());
        }
        asm volatile("" : : "r"(lanes[0].x.data()) : "memory");
    }, samples, reps, warmup) / L;
}

template <int L>
static double run_fused(std::array<Lane, 4>& lanes, uint32_t k,
                        int samples, int reps, int warmup) {
    const uint64_t* aptr[L]; const uint64_t* bptr[L]; const uint64_t* mptr[L];
    uint64_t* optr[L]; uint64_t* wptr[L]; uint64_t n0[L];
    for (int l = 0; l < L; ++l) {
        aptr[l] = lanes[l].x.data(); bptr[l] = lanes[l].b.data();
        mptr[l] = lanes[l].mod.data(); optr[l] = lanes[l].x.data();
        wptr[l] = lanes[l].work.data(); n0[l] = lanes[l].n0inv;
    }
    return best_ns([&]() {
        montgomery_mul_fios_xL<L>(aptr, bptr, k, mptr, n0, optr, wptr);
        asm volatile("" : : "r"(optr[0]) : "memory");
    }, samples, reps, warmup) / L;
}

int main() {
    // ── correctness: fused×L must be bit-identical to production ────
    for (uint32_t k : {1u, 2u, 3u, 4u, 7u, 16u, 32u, 64u}) {
        std::array<Lane, 4> lanes = {
            make_lane(k, 0xA110 + k), make_lane(k, 0xB220 + k),
            make_lane(k, 0xC330 + k), make_lane(k, 0xD440 + k)};
        std::array<std::vector<uint64_t>, 4> ref;
        for (int l = 0; l < 4; ++l) {
            ref[l].resize(k);
            std::vector<uint64_t> w(k + 2);
            montgomery_mul_fios(lanes[l].x.data(), lanes[l].b.data(), k,
                                lanes[l].mod.data(), lanes[l].n0inv,
                                ref[l].data(), w.data());
        }
        const uint64_t* a4[4]; const uint64_t* b4[4]; const uint64_t* m4[4];
        uint64_t* o4[4]; uint64_t* w4[4]; uint64_t n04[4];
        std::array<std::vector<uint64_t>, 4> got;
        std::array<std::vector<uint64_t>, 4> wk;
        for (int l = 0; l < 4; ++l) {
            got[l].resize(k); wk[l].resize(k + 2);
            a4[l] = lanes[l].x.data(); b4[l] = lanes[l].b.data();
            m4[l] = lanes[l].mod.data(); o4[l] = got[l].data();
            w4[l] = wk[l].data(); n04[l] = lanes[l].n0inv;
        }
        montgomery_mul_fios_xL<2>(a4, b4, k, m4, n04, o4, w4);
        for (int l = 0; l < 2; ++l)
            if (std::memcmp(got[l].data(), ref[l].data(), k * 8) != 0) {
                std::printf("MISMATCH fused×2 k=%u lane=%d\n", k, l); return 1;
            }
        montgomery_mul_fios_xL<4>(a4, b4, k, m4, n04, o4, w4);
        for (int l = 0; l < 4; ++l)
            if (std::memcmp(got[l].data(), ref[l].data(), k * 8) != 0) {
                std::printf("MISMATCH fused×4 k=%u lane=%d\n", k, l); return 1;
            }
    }
    std::printf("correctness: fused×2 / fused×4 bit-identical to montgomery_mul_fios\n\n");

    std::printf("ns per mont-mul (lower = better; ×N = throughput gain vs serial)\n");
    std::printf("k    bits   serial      call×2        call×4        fused×2       fused×4\n");
    std::printf("--   ----   ---------   -----------   -----------   -----------   -----------\n");

    for (uint32_t k : {4u, 8u, 16u, 32u, 64u}) {
        std::array<Lane, 4> lanes = {
            make_lane(k, 0x5EED0 + k), make_lane(k, 0x5EED1 + k),
            make_lane(k, 0x5EED2 + k), make_lane(k, 0x5EED3 + k)};

        const int reps    = (k <= 8) ? 200000 : (k <= 16) ? 80000 : (k <= 32) ? 30000 : 10000;
        const int warmup  = reps / 10;
        const int samples = 5;

        const double s1 = run_call_level<1>(lanes, k, samples, reps, warmup);
        const double c2 = run_call_level<2>(lanes, k, samples, reps, warmup);
        const double c4 = run_call_level<4>(lanes, k, samples, reps, warmup);
        const double f2 = run_fused<2>(lanes, k, samples, reps, warmup);
        const double f4 = run_fused<4>(lanes, k, samples, reps, warmup);

        std::printf("%-4u %-6u %8.1f   %8.1f %.2fx  %8.1f %.2fx  %8.1f %.2fx  %8.1f %.2fx\n",
                    k, k * 64, s1,
                    c2, s1 / c2, c4, s1 / c4, f2, s1 / f2, f4, s1 / f4);
    }
    return 0;
}
