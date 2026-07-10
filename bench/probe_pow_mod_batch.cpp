// bench/probe_pow_mod_batch.cpp — B1 e2e gate: a full 2-lane batched
// Montgomery ladder (tier-1 shape: shared exponent + shared modulus)
// vs two production pow_mod calls.
//
// The batch ladder is a faithful replica of pow_mod_montgomery's
// front/back matter — same base reduction, same adaptive window
// (W=6 at exp ≥ 512 bits), same from_montgomery — with three
// deliberate differences, each part of what the pow_mod_batch API
// would actually ship:
//   1. MontgomeryContext (n0inv + R² via Knuth D) built ONCE for the
//      pair instead of once per call.
//   2. Every kernel call is the fused 2-lane FIOS mul — including
//      squarings.  The interleave probe showed sqr-as-fused-mul beats
//      fused halved-sqr at every k (the halved kernel's shrunken
//      product chain starves the dual-issue slots fusion feeds).
//   3. Ping-pong result buffers instead of memcpy-back.
//
// Correctness: batch results must equal hydra::pow_mod per lane
// (value-equal as Hydra) across widths and structured exponents
// before anything is timed.
//
// Cadence: operands ROTATE across iterations (8 base pairs per
// width) — fixed-operand kernel probes overstate L1 locality (house
// rule); two lanes' tables (up to 2×32×64 limbs at W=6) compete for
// L1 in a way single-lane doesn't.
//
// Build:
//   clang++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       bench/probe_pow_mod_batch.cpp -o build-rel/probe_pow_mod_batch

#include "../hydra.hpp"
#include "mont_interleave_kernels.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;
using hydra::Hydra;

// The ladder under test IS the shipped implementation — this probe
// is its A/B harness against production single-op pow_mod.
static std::array<Hydra, 2> pow_mod_batch2(
    const Hydra& b0, const Hydra& b1,
    const Hydra& exp, const Hydra& mod)
{
    return hydra::pow_mod_montgomery_x2(b0, b1, exp, mod);
}

// ── operand generation ───────────────────────────────────────────────
static Hydra rnd_hydra(std::mt19937_64& rng, uint32_t k, bool top_set,
                       bool force_odd = false) {
    std::vector<uint64_t> v(k);
    for (auto& w : v) w = rng();
    if (top_set) v[k - 1] |= (1ull << 63);
    if (force_odd) v[0] |= 1u;
    return Hydra::from_limbs(v.data(), k);
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

int main() {
    // ── correctness vs production pow_mod ───────────────────────────
    {
        int checks = 0;
        for (uint32_t k : {1u, 2u, 3u, 4u, 7u, 8u, 16u, 32u, 33u, 64u}) {
            std::mt19937_64 rng(0xBA7C4 + k);
            const Hydra mod = rnd_hydra(rng, k, true, true);
            std::vector<Hydra> exps;
            exps.push_back(rnd_hydra(rng, k, true));            // full width
            exps.push_back(Hydra{1u});                          // exp = 1
            exps.push_back(Hydra{3u});                          // tiny window
            {   // low-popcount exponent (long zero runs)
                std::vector<uint64_t> e(k, 0);
                e[k - 1] = 1ull << 63;
                e[0] |= 1u;
                exps.push_back(Hydra::from_limbs(e.data(), k));
            }
            for (const auto& exp : exps) {
                const Hydra b0 = rnd_hydra(rng, k, false);
                const Hydra b1 = rnd_hydra(rng, k, false);
                auto got = pow_mod_batch2(b0, b1, exp, mod);
                if (got[0] != hydra::pow_mod(b0, exp, mod) ||
                    got[1] != hydra::pow_mod(b1, exp, mod)) {
                    std::printf("MISMATCH k=%u\n", k);
                    return 1;
                }
                checks += 2;
            }
        }
        std::printf("correctness: batch2 == production pow_mod (%d lanes checked)\n\n", checks);
    }

    // ── cadence A/B ──────────────────────────────────────────────────
    std::printf("e2e pow_mod, tier-1 batch (shared exp+mod), rotating operands\n");
    std::printf("k    bits   production ns/op   batch2 ns/op    speedup\n");
    std::printf("--   ----   ----------------   -------------   -------\n");

    for (uint32_t k : {4u, 8u, 16u, 32u, 64u}) {
        std::mt19937_64 rng(0xCADE0 + k);
        const Hydra mod = rnd_hydra(rng, k, true, true);
        const Hydra exp = rnd_hydra(rng, k, true);

        constexpr int PAIRS = 8;
        std::vector<Hydra> B0, B1;
        for (int p = 0; p < PAIRS; ++p) {
            B0.push_back(rnd_hydra(rng, k, false));
            B1.push_back(rnd_hydra(rng, k, false));
        }

        const int reps    = (k <= 8) ? 2000 : (k <= 16) ? 600 : (k <= 32) ? 150 : 30;
        const int warmup  = reps / 5 + 1;
        const int samples = 5;

        int pi = 0;
        volatile uint64_t sink = 0;

        const double prod_ns = best_ns([&]() {
            const int p = pi++ & (PAIRS - 1);
            Hydra r0 = hydra::pow_mod(B0[p], exp, mod);
            Hydra r1 = hydra::pow_mod(B1[p], exp, mod);
            sink += r0.limb_view().count + r1.limb_view().count;
        }, samples, reps, warmup) / 2.0;

        const double batch_ns = best_ns([&]() {
            const int p = pi++ & (PAIRS - 1);
            auto r = pow_mod_batch2(B0[p], B1[p], exp, mod);
            sink += r[0].limb_view().count + r[1].limb_view().count;
        }, samples, reps, warmup) / 2.0;

        std::printf("%-4u %-6u %13.0f      %13.0f      %.2fx\n",
                    k, k * 64, prod_ns, batch_ns, prod_ns / batch_ns);
    }
    return 0;
}
