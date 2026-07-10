// demo/bench_web.cpp — wasm module behind the in-browser shootout demo.
//
// Exposes a per-(backend, width) benchmark cell to JavaScript so the
// page can run cells one at a time and update the UI between them.
// Operand generation matches bench/bench_pow_mod.cpp exactly (same
// seeds, same top-bit/odd-modulus shaping) so in-browser numbers are
// directly comparable to the scripts/wasm_bench.sh shootout.
//
// Build via scripts/wasm_demo.sh — emcc SINGLE_FILE+MODULARIZE, with
// -DHYDRA_POWMOD_GMP (mini-gmp shim) and -DHYDRA_POWMOD_BOOST.

#include "../hydra.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>

#ifdef HYDRA_POWMOD_BOOST
#  include <boost/multiprecision/cpp_int.hpp>
namespace bmp = boost::multiprecision;
#endif

#ifdef HYDRA_POWMOD_GMP
#  include <gmp.h>
#endif

using hydra::Hydra;

// ── Operand generation (mirrors bench_pow_mod.cpp) ──────────
static std::vector<uint64_t> make_limb_array(uint32_t n_bits, uint64_t seed) {
    const uint32_t n_limbs = (n_bits + 63) / 64;
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> limbs(n_limbs);
    for (auto& l : limbs) l = rng();
    const uint32_t top_bits = n_bits % 64;
    if (top_bits != 0) {
        limbs.back() &= (1ull << top_bits) - 1;
        limbs.back() |= (1ull << (top_bits - 1));
    } else {
        limbs.back() |= (1ull << 63);
    }
    limbs[0] |= 1u;
    return limbs;
}

static Hydra make_hydra(uint32_t n_bits, uint64_t seed) {
    auto limbs = make_limb_array(n_bits, seed);
    return Hydra::from_limbs(limbs.data(), static_cast<uint32_t>(limbs.size()));
}

struct Ops {
    Hydra h_base, h_exp, h_mod;
    explicit Ops(uint32_t bits)
        : h_base(make_hydra(bits, 0xCAFE'0000ull + bits)),
          h_exp (make_hydra(bits, 0xBEEF'0000ull + bits)),
          h_mod (make_hydra(bits, 0xDEAD'0000ull + bits)) {}
};

// ── Per-backend single-shot runners ─────────────────────────
// Each returns a stable string form of the result for cross-checks.

static std::string run_hydra(const Ops& o) {
    return hydra::pow_mod(o.h_base, o.h_exp, o.h_mod).to_string();
}

#ifdef HYDRA_POWMOD_BOOST
static std::string run_boost(const Ops& o) {
    auto cvt = [](const Hydra& h) { return bmp::cpp_int(h.to_string()); };
    bmp::cpp_int r = bmp::powm(cvt(o.h_base), cvt(o.h_exp), cvt(o.h_mod));
    return r.str();
}
#endif

#ifdef HYDRA_POWMOD_GMP
static std::string run_gmp(const Ops& o) {
    auto set = [](mpz_t out, const Hydra& h) {
        mpz_set_str(out, h.to_string().c_str(), 10);
    };
    mpz_t b, e, m, r;
    mpz_init(b); mpz_init(e); mpz_init(m); mpz_init(r);
    set(b, o.h_base); set(e, o.h_exp); set(m, o.h_mod);
    mpz_powm(r, b, e, m);
    char* s = mpz_get_str(nullptr, 10, r);
    std::string out(s);
    std::free(s);
    mpz_clear(b); mpz_clear(e); mpz_clear(m); mpz_clear(r);
    return out;
}
#endif

// ── Timed cells ──────────────────────────────────────────────
// backend: 0 = hydra, 1 = boost, 2 = mini-gmp
// Returns median per-op latency in milliseconds, or -1 for unknown
// backend.  Conversion cost (decimal string → backend type) is paid
// once, outside the timed region, exactly like bench_pow_mod.
//
// Browsers without cross-origin isolation clamp performance.now()
// (and therefore emscripten_get_now()) to 100 µs — or 1 ms in some
// engines — so timing single sub-millisecond ops reads as zero.
// Each sample therefore times a BATCH sized (by a calibration pass)
// to run ≥ TARGET_BATCH_MS, and reports elapsed/batch_reps.  At
// large widths one op already exceeds the window and the batch
// degenerates to a single op, which is then long enough to time
// directly.  Same technique as Google Benchmark's min-time loop.

namespace {
constexpr double TARGET_BATCH_MS = 15.0;

template <typename Fn>
double median_batched(Fn&& op, int samples) {
    // Calibration: double reps until the batch is measurable.
    // Escape hatch: if the clock reads ~zero even at 64 reps, it is
    // frozen or pathologically coarse (e.g. headless virtual time) —
    // give up on batching rather than spinning toward 2^20 ops.
    int reps = 1;
    double elapsed = 0;
    for (;;) {
        const double t0 = emscripten_get_now();
        for (int i = 0; i < reps; ++i) op();
        elapsed = emscripten_get_now() - t0;
        if (elapsed >= TARGET_BATCH_MS || reps >= (1 << 20)) break;
        if (elapsed <= 0.01) {
            if (reps >= 64) { reps = 1; break; }
            reps *= 8;
        } else {
            reps = static_cast<int>(reps * (TARGET_BATCH_MS / elapsed) + 1);
        }
    }
    std::vector<double> t;
    t.reserve(static_cast<size_t>(samples));
    for (int s = 0; s < samples; ++s) {
        const double t0 = emscripten_get_now();
        for (int i = 0; i < reps; ++i) op();
        t.push_back((emscripten_get_now() - t0) / reps);
    }
    std::sort(t.begin(), t.end());
    const size_t n = t.size();
    return (n % 2 == 0) ? (t[n / 2 - 1] + t[n / 2]) / 2.0 : t[n / 2];
}
} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
double hydra_bench_cell(int backend, int bits, int samples) {
    if (samples < 1) samples = 1;
    Ops o(static_cast<uint32_t>(bits));

    if (backend == 0) {
        volatile uint32_t sink = 0;
        double r = median_batched([&]() {
            Hydra x = hydra::pow_mod(o.h_base, o.h_exp, o.h_mod);
            sink += x.limb_view().count;
        }, samples);
        (void)sink;
        return r;
    }
#ifdef HYDRA_POWMOD_BOOST
    if (backend == 1) {
        auto cvt = [](const Hydra& h) { return bmp::cpp_int(h.to_string()); };
        bmp::cpp_int b = cvt(o.h_base), e = cvt(o.h_exp), m = cvt(o.h_mod), r;
        volatile unsigned sink = 0;
        double out = median_batched([&]() {
            r = bmp::powm(b, e, m);
            sink += static_cast<unsigned>(r.backend().size());
        }, samples);
        (void)sink;
        return out;
    }
#endif
#ifdef HYDRA_POWMOD_GMP
    if (backend == 2) {
        auto set = [](mpz_t out, const Hydra& h) {
            mpz_set_str(out, h.to_string().c_str(), 10);
        };
        mpz_t b, e, m, r;
        mpz_init(b); mpz_init(e); mpz_init(m); mpz_init(r);
        set(b, o.h_base); set(e, o.h_exp); set(m, o.h_mod);
        volatile unsigned long sink = 0;
        double out = median_batched([&]() {
            mpz_powm(r, b, e, m);
            sink += mpz_size(r);
        }, samples);
        (void)sink;
        mpz_clear(b); mpz_clear(e); mpz_clear(m); mpz_clear(r);
        return out;
    }
#endif
    return -1.0;
}

// ── Playground exports ───────────────────────────────────────
// String-based API for the interactive panel: decimal strings in,
// comma-separated decimal strings out (no JSON escaping across the
// boundary — the page does all presentation).  Returned pointers
// stay valid until the next call (static buffer).

static std::string g_out;

// Toy RSA keypair: two (bits/2)-bit primes via next_prime from a
// seeded random start, e = 65537, d = e^-1 mod φ via extended_gcd.
// EDUCATIONAL ONLY — Hydra is variable-time; the page says so too.
// Returns "p,q,n,d,keygen_ms" (decimal).
EMSCRIPTEN_KEEPALIVE
const char* hydra_rsa_keygen(int bits, double seed) {
    const double t0 = emscripten_get_now();
    const uint32_t half = static_cast<uint32_t>(bits) / 2;

    std::mt19937_64 rng(static_cast<uint64_t>(seed));
    auto random_start = [&](uint64_t salt) {
        std::mt19937_64 r(rng() ^ salt);
        auto limbs = make_limb_array(half, r());
        return Hydra::from_limbs(limbs.data(),
                                 static_cast<uint32_t>(limbs.size()));
    };

    const Hydra e{65537u};
    Hydra p = hydra::next_prime(random_start(0x9E3779B97F4A7C15ull));
    Hydra q, phi, d;
    for (uint64_t salt = 1;; ++salt) {
        q = hydra::next_prime(random_start(salt));
        if (q == p) continue;
        phi = (p - Hydra{1u}) * (q - Hydra{1u});
        auto eg = hydra::extended_gcd(e, phi);
        if (!(eg.gcd == Hydra{1u})) continue;   // e | φ — re-roll q (rare)
        d = eg.x % phi;
        if (d.is_negative()) d = d + phi;
        break;
    }
    const double ms = emscripten_get_now() - t0;

    g_out = p.to_string() + "," + q.to_string() + ","
          + (p * q).to_string() + "," + d.to_string() + ","
          + std::to_string(ms);
    return g_out.c_str();
}

// Smallest prime > start (decimal string).  Returns "prime,ms".
EMSCRIPTEN_KEEPALIVE
const char* hydra_next_prime_str(const char* start_dec) {
    const Hydra start{start_dec};
    const double t0 = emscripten_get_now();
    Hydra p = hydra::next_prime(start);
    const double ms = emscripten_get_now() - t0;
    g_out = p.to_string() + "," + std::to_string(ms);
    return g_out.c_str();
}

// (base^exp) mod m on decimal strings.  Returns "result,ms".
EMSCRIPTEN_KEEPALIVE
const char* hydra_powmod_str(const char* base_dec, const char* exp_dec,
                             const char* mod_dec) {
    const Hydra b{base_dec}, e{exp_dec}, m{mod_dec};
    const double t0 = emscripten_get_now();
    Hydra r = hydra::pow_mod(b, e, m);
    const double ms = emscripten_get_now() - t0;
    g_out = r.to_string() + "," + std::to_string(ms);
    return g_out.c_str();
}

// Cross-check all compiled backends agree at the given width.
// Returns 1 on agreement, 0 on mismatch.
EMSCRIPTEN_KEEPALIVE
int hydra_validate(int bits) {
    Ops o(static_cast<uint32_t>(bits));
    const std::string want = run_hydra(o);
#ifdef HYDRA_POWMOD_BOOST
    if (run_boost(o) != want) return 0;
#endif
#ifdef HYDRA_POWMOD_GMP
    if (run_gmp(o) != want) return 0;
#endif
    return 1;
}

} // extern "C"
