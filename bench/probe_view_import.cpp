// bench/probe_view_import.cpp — borrowed-view import probe (the
// wasm-boundary experiment behind detail::MagnitudeView).
//
// Question: for a read-heavy predicate whose operand already sits in
// memory in Hydra's native limb shape (the wasm interop case), does
// skipping Hydra::from_limbs' alloc+copy — importing via a borrowed
// MagnitudeView instead — matter?
//
// The two paths are compute-identical BY CONSTRUCTION: hydra::isqrt
// delegates to detail::isqrt_magnitude, which is exactly what the view
// path calls, so the A/B isolates the import step alone:
//   owned : Hydra::from_limbs(buf, k)   → is_perfect_square(H)
//   view  : MagnitudeView::trimmed(...) → is_perfect_square_view(v)
//
// Measured per width (min-of-medians over --runs, cross-run CV):
//   import_owned  — from_limbs only (alloc+copy for k >= 4)
//   import_view   — trimmed() only (a leading-zero scan)
//   compute       — is_perfect_square_view on pre-built views
//   e2e owned/view — import + predicate, the wasm-call shape
//   allocs/op     — global operator-new count per predicate call
//
// Cadence: operands ROTATE across iterations (8 buffers per width) —
// fixed-operand probes overstate L1 locality (house rule).
// Correctness gate runs before anything is timed.
//
// Build:
//   clang++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       bench/probe_view_import.cpp -o build-rel/probe_view_import
// Run:
//   ./build-rel/probe_view_import --runs 6

#include "../hydra.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <vector>

using clk = std::chrono::steady_clock;
using hydra::Hydra;
using hydra::detail::MagnitudeView;

// ── allocation counter ───────────────────────────────────────────────
// Program-wide operator-new replacement; counts every heap allocation
// (LargeRep::create and std::vector scratch both land here).
static std::atomic<uint64_t> g_allocs{0};

void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// ── measurement protocol ─────────────────────────────────────────────
// Each sample is a batch mean; each run is the median of SAMPLES
// samples; the reported value is the min across runs (min-of-medians).
// Batch reps are calibrated so one sample is ~4 ms.

static constexpr int SAMPLES = 5;

struct Measurement { double ns; double cv; };

template <class F>
static double batch_ns(F&& f, uint32_t reps) {
    const auto t0 = clk::now();
    for (uint32_t i = 0; i < reps; ++i) f(i);
    const auto t1 = clk::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
}

template <class F>
static Measurement measure(F&& f, int runs) {
    // calibrate: target ~4 ms per sample
    uint32_t reps = 1;
    for (;;) {
        const double t = batch_ns(f, reps) * reps;
        if (t >= 1e6 || reps >= (1u << 24)) {
            reps = std::max<uint32_t>(1, (uint32_t)(reps * 4e6 / std::max(t, 1.0)));
            break;
        }
        reps *= 8;
    }
    std::vector<double> medians;
    for (int r = 0; r < runs; ++r) {
        std::vector<double> s;
        for (int i = 0; i < SAMPLES; ++i) s.push_back(batch_ns(f, reps));
        std::nth_element(s.begin(), s.begin() + SAMPLES / 2, s.end());
        medians.push_back(s[SAMPLES / 2]);
    }
    double mn = 1e300, mean = 0;
    for (double m : medians) { mn = std::min(mn, m); mean += m; }
    mean /= (double)medians.size();
    double var = 0;
    for (double m : medians) var += (m - mean) * (m - mean);
    const double cv = medians.size() > 1
        ? std::sqrt(var / (double)(medians.size() - 1)) / mean : 0.0;
    return { mn, cv };
}

// DCE sink
static volatile uint64_t g_sink = 0;

int main(int argc, char** argv) {
    int runs = 6;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--runs") && i + 1 < argc) runs = std::atoi(argv[++i]);

    constexpr uint32_t NB = 8;      // rotating operand buffers per width
    const uint32_t widths[] = {2, 4, 8, 16, 32, 64};   // 128..4096 bits

    std::mt19937_64 rng(0xB0220ull);        // deterministic operands

    // ── correctness gate ─────────────────────────────────────────────
    for (uint32_t k : widths) {
        for (int t = 0; t < 8; ++t) {
            std::vector<uint64_t> root((k + 1) / 2);
            for (auto& w : root) w = rng();
            root.back() |= (1ull << 62);
            Hydra r = Hydra::from_limbs(root.data(), (uint32_t)root.size());
            for (const Hydra& n : {r * r, r * r + Hydra{1u}, r * r - Hydra{1u}}) {
                auto lv = n.limb_view();
                const bool a = hydra::is_perfect_square(n);
                const bool b = hydra::detail::is_perfect_square_view(
                    MagnitudeView::trimmed(lv.ptr, lv.count));
                if (a != b) {
                    std::fprintf(stderr, "MISMATCH at k=%u\n", k);
                    return 1;
                }
            }
        }
    }
    std::printf("correctness gate: owned == view on squares/±1 at all widths\n\n");
    std::printf("| bits | import_owned | import_view | compute | e2e_owned | e2e_view | Δe2e | allocs/op o→v |\n");
    std::printf("|-----:|-------------:|------------:|--------:|----------:|---------:|-----:|--------------:|\n");

    for (uint32_t k : widths) {
        // rotating operand set: random k-limb values, top bit set
        std::vector<std::vector<uint64_t>> bufs(NB, std::vector<uint64_t>(k));
        for (auto& b : bufs) {
            for (auto& w : b) w = rng();
            b.back() |= (1ull << 63);
        }
        std::vector<MagnitudeView> views;
        for (auto& b : bufs)
            views.push_back(MagnitudeView::trimmed(b.data(), k));

        // Sinks must read the imported LIMB DATA, not just metadata —
        // clang legally elides the whole LargeRep allocation otherwise
        // (measured: a meta-only sink reports 1.5 ns for a 4096-bit
        // from_limbs).
        auto m_imp_o = measure([&](uint32_t i) {
            Hydra h = Hydra::from_limbs(bufs[i % NB].data(), k);
            auto lv = h.limb_view();
            g_sink += lv.ptr[0] + lv.ptr[lv.count - 1];
        }, runs);
        auto m_imp_v = measure([&](uint32_t i) {
            MagnitudeView v = MagnitudeView::trimmed(bufs[i % NB].data(), k);
            g_sink += v.limbs[0] + v.limbs[v.count - 1];
        }, runs);
        auto m_cmp = measure([&](uint32_t i) {
            g_sink += hydra::detail::is_perfect_square_view(views[i % NB]);
        }, runs);
        auto m_e2e_o = measure([&](uint32_t i) {
            Hydra h = Hydra::from_limbs(bufs[i % NB].data(), k);
            g_sink += hydra::is_perfect_square(h);
        }, runs);
        auto m_e2e_v = measure([&](uint32_t i) {
            MagnitudeView v = MagnitudeView::trimmed(bufs[i % NB].data(), k);
            g_sink += hydra::detail::is_perfect_square_view(v);
        }, runs);

        // allocation count per predicate call (100 calls, averaged)
        constexpr uint32_t AC = 100;
        g_allocs = 0;
        for (uint32_t i = 0; i < AC; ++i) {
            Hydra h = Hydra::from_limbs(bufs[i % NB].data(), k);
            g_sink += hydra::is_perfect_square(h);
        }
        const double alloc_o = (double)g_allocs.load() / AC;
        g_allocs = 0;
        for (uint32_t i = 0; i < AC; ++i) {
            MagnitudeView v = MagnitudeView::trimmed(bufs[i % NB].data(), k);
            g_sink += hydra::detail::is_perfect_square_view(v);
        }
        const double alloc_v = (double)g_allocs.load() / AC;

        const double delta = (m_e2e_o.ns - m_e2e_v.ns) / m_e2e_o.ns * 100.0;
        std::printf(
            "| %4u | %9.1f ns | %8.1f ns | %8.0f ns | %8.0f ns | %8.0f ns | %+5.1f%% | %.1f → %.1f |\n",
            k * 64, m_imp_o.ns, m_imp_v.ns, m_cmp.ns,
            m_e2e_o.ns, m_e2e_v.ns, delta, alloc_o, alloc_v);
        std::fflush(stdout);

        // flag noisy rows (house rule: distrust deltas when CV is high)
        const double worst_cv = std::max({m_imp_o.cv, m_imp_v.cv, m_cmp.cv,
                                          m_e2e_o.cv, m_e2e_v.cv});
        if (worst_cv > 0.10)
            std::printf("|      | ^ cross-run CV %.0f%% — treat row as noisy |\n",
                        worst_cv * 100.0);
    }

    std::printf("\ncompiler: %s | runs=%d, %d samples/run, min-of-medians, ~4 ms batches\n",
#if defined(__clang__)
                __clang_version__,
#else
                __VERSION__,
#endif
                runs, SAMPLES);
    return (int)(g_sink & 0);
}
