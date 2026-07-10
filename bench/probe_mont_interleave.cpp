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
#include "mont_interleave_kernels.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;
using hydra::detail::montgomery_mul_fios;

// Fused L-lane kernels: see mont_interleave_kernels.hpp.
using mont_interleave::montgomery_mul_fios_xL;
using mont_interleave::montgomery_sqr_fios_halved_xL;


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

// One chained halved-squaring per lane per rep (x = sqr(x)); ns per op.
template <int L>
static double run_call_level_sqr(std::array<Lane, 4>& lanes, uint32_t k,
                                 int samples, int reps, int warmup) {
    return best_ns([&]() {
        for (int l = 0; l < L; ++l) {
            Lane& ln = lanes[l];
            hydra::detail::montgomery_sqr_fios_halved(
                ln.x.data(), k, ln.mod.data(), ln.n0inv,
                ln.x.data(), ln.work.data());
        }
        asm volatile("" : : "r"(lanes[0].x.data()) : "memory");
    }, samples, reps, warmup) / L;
}

template <int L>
static double run_fused_sqr(std::array<Lane, 4>& lanes, uint32_t k,
                            int samples, int reps, int warmup) {
    const uint64_t* aptr[L]; const uint64_t* mptr[L];
    uint64_t* optr[L]; uint64_t* wptr[L]; uint64_t n0[L];
    for (int l = 0; l < L; ++l) {
        aptr[l] = lanes[l].x.data(); mptr[l] = lanes[l].mod.data();
        optr[l] = lanes[l].x.data(); wptr[l] = lanes[l].work.data();
        n0[l] = lanes[l].n0inv;
    }
    return best_ns([&]() {
        montgomery_sqr_fios_halved_xL<L>(aptr, k, mptr, n0, optr, wptr);
        asm volatile("" : : "r"(optr[0]) : "memory");
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

        // Squaring: production halved kernel is the oracle.
        for (int l = 0; l < 4; ++l) {
            std::vector<uint64_t> w(k + 2);
            hydra::detail::montgomery_sqr_fios_halved(
                lanes[l].x.data(), k, lanes[l].mod.data(), lanes[l].n0inv,
                ref[l].data(), w.data());
        }
        montgomery_sqr_fios_halved_xL<2>(a4, k, m4, n04, o4, w4);
        for (int l = 0; l < 2; ++l)
            if (std::memcmp(got[l].data(), ref[l].data(), k * 8) != 0) {
                std::printf("MISMATCH sqr×2 k=%u lane=%d\n", k, l); return 1;
            }
        montgomery_sqr_fios_halved_xL<4>(a4, k, m4, n04, o4, w4);
        for (int l = 0; l < 4; ++l)
            if (std::memcmp(got[l].data(), ref[l].data(), k * 8) != 0) {
                std::printf("MISMATCH sqr×4 k=%u lane=%d\n", k, l); return 1;
            }
    }
    std::printf("correctness: fused×2/×4 mul and halved-sqr bit-identical to production\n\n");

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

    // Squaring table — halved kernel is production-dispatched at k<=32
    // (HALVED_SQR_MAX_K); k=64 squares as-mul, covered by the table above.
    std::printf("\nhalved SQUARING, ns per mont-sqr (production dispatch band k<=32)\n");
    std::printf("k    bits   serial      call×2        fused×2       fused×4\n");
    std::printf("--   ----   ---------   -----------   -----------   -----------\n");

    for (uint32_t k : {4u, 8u, 16u, 32u}) {
        std::array<Lane, 4> lanes = {
            make_lane(k, 0x9EED0 + k), make_lane(k, 0x9EED1 + k),
            make_lane(k, 0x9EED2 + k), make_lane(k, 0x9EED3 + k)};

        const int reps    = (k <= 8) ? 200000 : (k <= 16) ? 80000 : 30000;
        const int warmup  = reps / 10;
        const int samples = 5;

        const double s1 = run_call_level_sqr<1>(lanes, k, samples, reps, warmup);
        const double c2 = run_call_level_sqr<2>(lanes, k, samples, reps, warmup);
        const double f2 = run_fused_sqr<2>(lanes, k, samples, reps, warmup);
        const double f4 = run_fused_sqr<4>(lanes, k, samples, reps, warmup);

        std::printf("%-4u %-6u %8.1f   %8.1f %.2fx  %8.1f %.2fx  %8.1f %.2fx\n",
                    k, k * 64, s1, c2, s1 / c2, f2, s1 / f2, f4, s1 / f4);
    }
    return 0;
}
