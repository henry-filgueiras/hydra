// bench/bench_pow_mod.cpp — comparative modular exponentiation benchmark
//
// Apples-to-apples comparison of pow_mod across bigint libraries:
//   - Hydra       (always)
//   - Boost       (compile with -DHYDRA_POWMOD_BOOST   + Boost headers)
//   - GMP         (compile with -DHYDRA_POWMOD_GMP     + -lgmp)
//   - OpenSSL     (compile with -DHYDRA_POWMOD_OPENSSL + -lcrypto)
//
// Build examples:
//
//   # Hydra only (no external deps):
//   g++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       bench/bench_pow_mod.cpp -o build-rel/bench_pow_mod
//
//   # Hydra + Boost:
//   g++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       -DHYDRA_POWMOD_BOOST -I/opt/homebrew/include \
//       bench/bench_pow_mod.cpp -o build-rel/bench_pow_mod
//
//   # All four (macOS Homebrew example):
//   g++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//       -DHYDRA_POWMOD_BOOST -I/opt/homebrew/include \
//       -DHYDRA_POWMOD_GMP -I/opt/homebrew/include -L/opt/homebrew/lib -lgmp \
//       -DHYDRA_POWMOD_OPENSSL -I/opt/homebrew/include -L/opt/homebrew/lib -lcrypto \
//       bench/bench_pow_mod.cpp -o build-rel/bench_pow_mod
//
// Output: JSON to stdout (pipe to bench/pow_mod_report.py for tables/charts).
//         Pass --markdown for inline Markdown table.
//         Pass --csv for CSV.

#include "../hydra.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// Conditional backend includes
// ─────────────────────────────────────────────────────────────────────────────

#ifdef HYDRA_POWMOD_BOOST
#  include <boost/multiprecision/cpp_int.hpp>
namespace bmp = boost::multiprecision;
#endif

#ifdef HYDRA_POWMOD_GMP
#  include <gmp.h>
#endif

#ifdef HYDRA_POWMOD_OPENSSL
#  include <openssl/bn.h>
#endif

using namespace hydra;
using clk = std::chrono::steady_clock;

// ─────────────────────────────────────────────────────────────────────────────
// § 0  Deterministic operand generation
// ─────────────────────────────────────────────────────────────────────────────

// Build a deterministic n-bit value from a fixed seed.
// Top bit is always set → value is in [2^(n-1), 2^n).
static std::vector<uint64_t> make_limb_array(uint32_t n_bits, uint64_t seed) {
    const uint32_t n_limbs = (n_bits + 63) / 64;
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> limbs(n_limbs);
    for (auto& l : limbs) l = rng();
    // Mask the top limb to exactly n_bits.
    const uint32_t top_bits = n_bits % 64;
    if (top_bits != 0) {
        limbs.back() &= (1ull << top_bits) - 1;
    }
    // Set the top bit so the value is exactly n_bits wide.
    if (top_bits != 0)
        limbs.back() |= (1ull << (top_bits - 1));
    else
        limbs.back() |= (1ull << 63);
    // Ensure modulus is odd (good for modexp, avoids trivial structure).
    limbs[0] |= 1u;
    return limbs;
}

static Hydra make_hydra(uint32_t n_bits, uint64_t seed) {
    auto limbs = make_limb_array(n_bits, seed);
    return Hydra::from_limbs(limbs.data(), static_cast<uint32_t>(limbs.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// § 1  Backend-agnostic operand holder
// ─────────────────────────────────────────────────────────────────────────────

struct Operands {
    uint32_t bits;

    // --- Hydra ---
    Hydra h_base, h_exp, h_mod;

#ifdef HYDRA_POWMOD_BOOST
    bmp::cpp_int b_base, b_exp, b_mod;
#endif

#ifdef HYDRA_POWMOD_GMP
    mpz_t g_base, g_exp, g_mod, g_result;
    bool gmp_inited = false;
#endif

#ifdef HYDRA_POWMOD_OPENSSL
    BN_CTX* bn_ctx = nullptr;
    BIGNUM *o_base = nullptr, *o_exp = nullptr, *o_mod = nullptr, *o_result = nullptr;
#endif

    explicit Operands(uint32_t n_bits)
        : bits(n_bits),
          // Seeds are chosen for reproducibility.  base/exp/mod each get
          // a different seed so they're statistically independent.
          h_base(make_hydra(n_bits, 0xCAFE'0000ull + n_bits)),
          h_exp (make_hydra(n_bits, 0xBEEF'0000ull + n_bits)),
          h_mod (make_hydra(n_bits, 0xDEAD'0000ull + n_bits))
    {
        // ── Boost ──
#ifdef HYDRA_POWMOD_BOOST
        {
            auto to_boost = [](const Hydra& h) -> bmp::cpp_int {
                std::string s = h.to_string();
                return bmp::cpp_int(s);
            };
            b_base = to_boost(h_base);
            b_exp  = to_boost(h_exp);
            b_mod  = to_boost(h_mod);
        }
#endif

        // ── GMP ──
#ifdef HYDRA_POWMOD_GMP
        {
            mpz_init(g_base); mpz_init(g_exp);
            mpz_init(g_mod);  mpz_init(g_result);
            gmp_inited = true;
            auto to_gmp = [](mpz_t out, const Hydra& h) {
                std::string s = h.to_string();
                mpz_set_str(out, s.c_str(), 10);
            };
            to_gmp(g_base, h_base);
            to_gmp(g_exp,  h_exp);
            to_gmp(g_mod,  h_mod);
        }
#endif

        // ── OpenSSL ──
#ifdef HYDRA_POWMOD_OPENSSL
        {
            bn_ctx   = BN_CTX_new();
            o_base   = BN_new(); o_exp = BN_new();
            o_mod    = BN_new(); o_result = BN_new();
            auto to_bn = [](BIGNUM* out, const Hydra& h) {
                std::string s = h.to_string();
                BN_dec2bn(&out, s.c_str());
            };
            to_bn(o_base, h_base);
            to_bn(o_exp,  h_exp);
            to_bn(o_mod,  h_mod);
        }
#endif
    }

    ~Operands() {
#ifdef HYDRA_POWMOD_GMP
        if (gmp_inited) {
            mpz_clear(g_base); mpz_clear(g_exp);
            mpz_clear(g_mod);  mpz_clear(g_result);
        }
#endif
#ifdef HYDRA_POWMOD_OPENSSL
        if (bn_ctx) BN_CTX_free(bn_ctx);
        if (o_base) BN_free(o_base);
        if (o_exp)  BN_free(o_exp);
        if (o_mod)  BN_free(o_mod);
        if (o_result) BN_free(o_result);
#endif
    }

    // Non-copyable.
    Operands(const Operands&) = delete;
    Operands& operator=(const Operands&) = delete;
};

// ─────────────────────────────────────────────────────────────────────────────
// § 2  Timing harness
// ─────────────────────────────────────────────────────────────────────────────

// Warm up for `warmup_ms`, then collect `n_samples` individual timings.
// Each sample is a single pow_mod call.
struct TimingResult {
    std::vector<double> samples_ns;  // per-call latency in nanoseconds
    double median_ns   = 0;
    double p95_ns      = 0;
    double mean_ns     = 0;
    double ops_per_sec = 0;

    void compute_stats() {
        if (samples_ns.empty()) return;
        std::sort(samples_ns.begin(), samples_ns.end());
        const size_t n = samples_ns.size();

        // Median
        if (n % 2 == 0)
            median_ns = (samples_ns[n/2 - 1] + samples_ns[n/2]) / 2.0;
        else
            median_ns = samples_ns[n/2];

        // P95
        const size_t p95_idx = static_cast<size_t>(std::ceil(0.95 * n)) - 1;
        p95_ns = samples_ns[std::min(p95_idx, n - 1)];

        // Mean
        double sum = 0;
        for (double s : samples_ns) sum += s;
        mean_ns = sum / n;

        // Ops/sec from median
        ops_per_sec = 1e9 / median_ns;
    }
};

static constexpr int WARMUP_ITERS  = 3;
static constexpr int SAMPLE_COUNT  = 50;  // per-width, per-backend

// ─── Hydra ──────────────────────────────────────────────────────────────

static TimingResult bench_hydra(Operands& ops) {
    TimingResult r;
    r.samples_ns.reserve(SAMPLE_COUNT);

    // Warmup
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        auto result = pow_mod(ops.h_base, ops.h_exp, ops.h_mod);
        asm volatile("" : : "r"(result.meta) : "memory");
    }

    // Collect
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        auto t0 = clk::now();
        auto result = pow_mod(ops.h_base, ops.h_exp, ops.h_mod);
        auto t1 = clk::now();
        asm volatile("" : : "r"(result.meta) : "memory");
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        r.samples_ns.push_back(ns);
    }
    r.compute_stats();
    return r;
}

// ─── Boost ──────────────────────────────────────────────────────────────

#ifdef HYDRA_POWMOD_BOOST
static TimingResult bench_boost(Operands& ops) {
    TimingResult r;
    r.samples_ns.reserve(SAMPLE_COUNT);

    for (int i = 0; i < WARMUP_ITERS; ++i) {
        auto result = bmp::powm(ops.b_base, ops.b_exp, ops.b_mod);
        asm volatile("" : : "g"(&result) : "memory");
    }

    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        auto t0 = clk::now();
        auto result = bmp::powm(ops.b_base, ops.b_exp, ops.b_mod);
        auto t1 = clk::now();
        asm volatile("" : : "g"(&result) : "memory");
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        r.samples_ns.push_back(ns);
    }
    r.compute_stats();
    return r;
}
#endif

// ─── GMP ────────────────────────────────────────────────────────────────

#ifdef HYDRA_POWMOD_GMP
static TimingResult bench_gmp(Operands& ops) {
    TimingResult r;
    r.samples_ns.reserve(SAMPLE_COUNT);

    for (int i = 0; i < WARMUP_ITERS; ++i) {
        mpz_powm(ops.g_result, ops.g_base, ops.g_exp, ops.g_mod);
    }

    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        auto t0 = clk::now();
        mpz_powm(ops.g_result, ops.g_base, ops.g_exp, ops.g_mod);
        auto t1 = clk::now();
        asm volatile("" : : "g"(ops.g_result) : "memory");
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        r.samples_ns.push_back(ns);
    }
    r.compute_stats();
    return r;
}
#endif

// ─── OpenSSL ────────────────────────────────────────────────────────────

#ifdef HYDRA_POWMOD_OPENSSL
static TimingResult bench_openssl(Operands& ops) {
    TimingResult r;
    r.samples_ns.reserve(SAMPLE_COUNT);

    for (int i = 0; i < WARMUP_ITERS; ++i) {
        BN_mod_exp(ops.o_result, ops.o_base, ops.o_exp, ops.o_mod, ops.bn_ctx);
    }

    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        auto t0 = clk::now();
        BN_mod_exp(ops.o_result, ops.o_base, ops.o_exp, ops.o_mod, ops.bn_ctx);
        auto t1 = clk::now();
        asm volatile("" : : "g"(ops.o_result) : "memory");
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        r.samples_ns.push_back(ns);
    }
    r.compute_stats();
    return r;
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// § 3  Cross-validation
// ─────────────────────────────────────────────────────────────────────────────

// Verify all enabled backends produce the same result.
static bool cross_validate(Operands& ops) {
    Hydra h_result = pow_mod(ops.h_base, ops.h_exp, ops.h_mod);
    std::string h_str = h_result.to_string();
    bool ok = true;

#ifdef HYDRA_POWMOD_BOOST
    {
        auto b_result = bmp::powm(ops.b_base, ops.b_exp, ops.b_mod);
        std::string b_str = b_result.str();
        if (h_str != b_str) {
            fprintf(stderr, "MISMATCH at %u bits: Hydra=%s  Boost=%s\n",
                    ops.bits, h_str.c_str(), b_str.c_str());
            ok = false;
        }
    }
#endif

#ifdef HYDRA_POWMOD_GMP
    {
        mpz_powm(ops.g_result, ops.g_base, ops.g_exp, ops.g_mod);
        char* g_str = mpz_get_str(nullptr, 10, ops.g_result);
        if (h_str != std::string(g_str)) {
            fprintf(stderr, "MISMATCH at %u bits: Hydra=%s  GMP=%s\n",
                    ops.bits, h_str.c_str(), g_str);
            ok = false;
        }
        free(g_str);
    }
#endif

#ifdef HYDRA_POWMOD_OPENSSL
    {
        BN_mod_exp(ops.o_result, ops.o_base, ops.o_exp, ops.o_mod, ops.bn_ctx);
        char* o_str = BN_bn2dec(ops.o_result);
        if (h_str != std::string(o_str)) {
            fprintf(stderr, "MISMATCH at %u bits: Hydra=%s  OpenSSL=%s\n",
                    ops.bits, h_str.c_str(), o_str);
            ok = false;
        }
        OPENSSL_free(o_str);
    }
#endif

    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// § 4  Output formatting
// ─────────────────────────────────────────────────────────────────────────────

struct BenchRow {
    uint32_t bits;
    TimingResult hydra;
#ifdef HYDRA_POWMOD_BOOST
    TimingResult boost;
#endif
#ifdef HYDRA_POWMOD_GMP
    TimingResult gmp;
#endif
#ifdef HYDRA_POWMOD_OPENSSL
    TimingResult openssl;
#endif
};

static std::string format_ns(double ns) {
    char buf[32];
    if (ns < 1000.0)
        snprintf(buf, sizeof(buf), "%.1f ns", ns);
    else if (ns < 1e6)
        snprintf(buf, sizeof(buf), "%.2f us", ns / 1e3);
    else if (ns < 1e9)
        snprintf(buf, sizeof(buf), "%.2f ms", ns / 1e6);
    else
        snprintf(buf, sizeof(buf), "%.3f s", ns / 1e9);
    return buf;
}

static std::string format_ops(double ops) {
    char buf[32];
    if (ops >= 1e6)
        snprintf(buf, sizeof(buf), "%.2fM", ops / 1e6);
    else if (ops >= 1e3)
        snprintf(buf, sizeof(buf), "%.2fK", ops / 1e3);
    else
        snprintf(buf, sizeof(buf), "%.1f", ops);
    return buf;
}

static void print_json(const std::vector<BenchRow>& rows) {
    printf("{\n  \"benchmark\": \"pow_mod\",\n");
    printf("  \"description\": \"Modular exponentiation: pow_mod(base, exp, mod) where base, exp, mod are all N-bit\",\n");
    printf("  \"backends\": [\"hydra\"");
#ifdef HYDRA_POWMOD_BOOST
    printf(", \"boost_cpp_int\"");
#endif
#ifdef HYDRA_POWMOD_GMP
    printf(", \"gmp\"");
#endif
#ifdef HYDRA_POWMOD_OPENSSL
    printf(", \"openssl\"");
#endif
    printf("],\n");
    printf("  \"samples_per_width\": %d,\n", SAMPLE_COUNT);
    printf("  \"results\": [\n");

    for (size_t ri = 0; ri < rows.size(); ++ri) {
        const auto& row = rows[ri];
        printf("    {\n");
        printf("      \"bits\": %u,\n", row.bits);

        auto print_backend = [](const char* name, const TimingResult& t, bool last) {
            printf("      \"%s\": {\n", name);
            printf("        \"median_ns\": %.1f,\n", t.median_ns);
            printf("        \"p95_ns\": %.1f,\n", t.p95_ns);
            printf("        \"mean_ns\": %.1f,\n", t.mean_ns);
            printf("        \"ops_per_sec\": %.1f\n", t.ops_per_sec);
            printf("      }%s\n", last ? "" : ",");
        };

        bool last_is_hydra = true;
#ifdef HYDRA_POWMOD_BOOST
        last_is_hydra = false;
#endif
#ifdef HYDRA_POWMOD_GMP
        last_is_hydra = false;
#endif
#ifdef HYDRA_POWMOD_OPENSSL
        last_is_hydra = false;
#endif

#ifdef HYDRA_POWMOD_OPENSSL
        print_backend("hydra", row.hydra, false);
#elif defined(HYDRA_POWMOD_GMP)
        print_backend("hydra", row.hydra, false);
#elif defined(HYDRA_POWMOD_BOOST)
        print_backend("hydra", row.hydra, false);
#else
        print_backend("hydra", row.hydra, true);
#endif

#ifdef HYDRA_POWMOD_BOOST
        {
            bool is_last = true;
#ifdef HYDRA_POWMOD_GMP
            is_last = false;
#endif
#ifdef HYDRA_POWMOD_OPENSSL
            is_last = false;
#endif
            print_backend("boost_cpp_int", row.boost, is_last);
        }
#endif

#ifdef HYDRA_POWMOD_GMP
        {
            bool is_last = true;
#ifdef HYDRA_POWMOD_OPENSSL
            is_last = false;
#endif
            print_backend("gmp", row.gmp, is_last);
        }
#endif

#ifdef HYDRA_POWMOD_OPENSSL
        print_backend("openssl", row.openssl, true);
#endif

        printf("    }%s\n", (ri + 1 < rows.size()) ? "," : "");
    }
    printf("  ]\n}\n");
}

static void print_markdown(const std::vector<BenchRow>& rows) {
    // Header
    printf("## pow_mod Benchmark — Big Integer Modular Arithmetic Comparison\n\n");
    printf("**Operation:** `pow_mod(base, exp, mod)` where base, exp, mod are all N-bit\n\n");
    printf("**Samples:** %d per width per backend | **Metric:** median latency\n\n", SAMPLE_COUNT);

    // Determine active backends
    bool has_boost = false, has_gmp = false, has_openssl = false;
#ifdef HYDRA_POWMOD_BOOST
    has_boost = true;
#endif
#ifdef HYDRA_POWMOD_GMP
    has_gmp = true;
#endif
#ifdef HYDRA_POWMOD_OPENSSL
    has_openssl = true;
#endif

    // ── Median Latency Table ──
    printf("### Median Latency\n\n");
    printf("| Width |    Hydra   ");
    if (has_boost)   printf("|  Boost cpp_int ");
    if (has_gmp)     printf("|     GMP     ");
    if (has_openssl) printf("|   OpenSSL   ");
    printf("|\n");

    printf("|------:|:----------:");
    if (has_boost)   printf("|:--------------:");
    if (has_gmp)     printf("|:-----------:");
    if (has_openssl) printf("|:-----------:");
    printf("|\n");

    for (const auto& row : rows) {
        printf("| %4u  | %10s ", row.bits, format_ns(row.hydra.median_ns).c_str());
#ifdef HYDRA_POWMOD_BOOST
        printf("| %14s ", format_ns(row.boost.median_ns).c_str());
#endif
#ifdef HYDRA_POWMOD_GMP
        printf("| %11s ", format_ns(row.gmp.median_ns).c_str());
#endif
#ifdef HYDRA_POWMOD_OPENSSL
        printf("| %11s ", format_ns(row.openssl.median_ns).c_str());
#endif
        printf("|\n");
    }

    // ── P95 Latency Table ──
    printf("\n### P95 Latency\n\n");
    printf("| Width |    Hydra   ");
    if (has_boost)   printf("|  Boost cpp_int ");
    if (has_gmp)     printf("|     GMP     ");
    if (has_openssl) printf("|   OpenSSL   ");
    printf("|\n");

    printf("|------:|:----------:");
    if (has_boost)   printf("|:--------------:");
    if (has_gmp)     printf("|:-----------:");
    if (has_openssl) printf("|:-----------:");
    printf("|\n");

    for (const auto& row : rows) {
        printf("| %4u  | %10s ", row.bits, format_ns(row.hydra.p95_ns).c_str());
#ifdef HYDRA_POWMOD_BOOST
        printf("| %14s ", format_ns(row.boost.p95_ns).c_str());
#endif
#ifdef HYDRA_POWMOD_GMP
        printf("| %11s ", format_ns(row.gmp.p95_ns).c_str());
#endif
#ifdef HYDRA_POWMOD_OPENSSL
        printf("| %11s ", format_ns(row.openssl.p95_ns).c_str());
#endif
        printf("|\n");
    }

    // ── Ops/sec Table ──
    printf("\n### Throughput (ops/sec)\n\n");
    printf("| Width |    Hydra   ");
    if (has_boost)   printf("|  Boost cpp_int ");
    if (has_gmp)     printf("|     GMP     ");
    if (has_openssl) printf("|   OpenSSL   ");
    printf("|\n");

    printf("|------:|:----------:");
    if (has_boost)   printf("|:--------------:");
    if (has_gmp)     printf("|:-----------:");
    if (has_openssl) printf("|:-----------:");
    printf("|\n");

    for (const auto& row : rows) {
        printf("| %4u  | %10s ", row.bits, format_ops(row.hydra.ops_per_sec).c_str());
#ifdef HYDRA_POWMOD_BOOST
        printf("| %14s ", format_ops(row.boost.ops_per_sec).c_str());
#endif
#ifdef HYDRA_POWMOD_GMP
        printf("| %11s ", format_ops(row.gmp.ops_per_sec).c_str());
#endif
#ifdef HYDRA_POWMOD_OPENSSL
        printf("| %11s ", format_ops(row.openssl.ops_per_sec).c_str());
#endif
        printf("|\n");
    }

    // ── Relative performance (if any comparison backend) ──
    if (has_boost || has_gmp || has_openssl) {
        printf("\n### Relative Performance (vs Hydra median, negative = Hydra faster)\n\n");
        printf("| Width ");
        if (has_boost)   printf("| Boost delta ");
        if (has_gmp)     printf("| GMP delta ");
        if (has_openssl) printf("| OpenSSL delta ");
        printf("|\n");

        printf("|------:");
        if (has_boost)   printf("|:-----------:");
        if (has_gmp)     printf("|:---------:");
        if (has_openssl) printf("|:-------------:");
        printf("|\n");

        for (const auto& row : rows) {
            printf("| %4u  ", row.bits);
#ifdef HYDRA_POWMOD_BOOST
            {
                double delta = ((row.hydra.median_ns / row.boost.median_ns) - 1.0) * 100.0;
                printf("| %+.1f%%      ", delta);
            }
#endif
#ifdef HYDRA_POWMOD_GMP
            {
                double delta = ((row.hydra.median_ns / row.gmp.median_ns) - 1.0) * 100.0;
                printf("| %+.1f%%    ", delta);
            }
#endif
#ifdef HYDRA_POWMOD_OPENSSL
            {
                double delta = ((row.hydra.median_ns / row.openssl.median_ns) - 1.0) * 100.0;
                printf("| %+.1f%%          ", delta);
            }
#endif
            printf("|\n");
        }
    }

    printf("\n> **Note:** This benchmark compares big integer modular arithmetic performance,\n");
    printf("> not production cryptographic suitability.\n");
}

static void print_csv(const std::vector<BenchRow>& rows) {
    printf("bits,backend,median_ns,p95_ns,mean_ns,ops_per_sec\n");
    for (const auto& row : rows) {
        printf("%u,hydra,%.1f,%.1f,%.1f,%.1f\n",
               row.bits, row.hydra.median_ns, row.hydra.p95_ns,
               row.hydra.mean_ns, row.hydra.ops_per_sec);
#ifdef HYDRA_POWMOD_BOOST
        printf("%u,boost_cpp_int,%.1f,%.1f,%.1f,%.1f\n",
               row.bits, row.boost.median_ns, row.boost.p95_ns,
               row.boost.mean_ns, row.boost.ops_per_sec);
#endif
#ifdef HYDRA_POWMOD_GMP
        printf("%u,gmp,%.1f,%.1f,%.1f,%.1f\n",
               row.bits, row.gmp.median_ns, row.gmp.p95_ns,
               row.gmp.mean_ns, row.gmp.ops_per_sec);
#endif
#ifdef HYDRA_POWMOD_OPENSSL
        printf("%u,openssl,%.1f,%.1f,%.1f,%.1f\n",
               row.bits, row.openssl.median_ns, row.openssl.p95_ns,
               row.openssl.mean_ns, row.openssl.ops_per_sec);
#endif
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// § 5  Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // Parse args
    enum class OutputMode { json, markdown, csv } mode = OutputMode::json;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--markdown" || std::string(argv[i]) == "--md")
            mode = OutputMode::markdown;
        else if (std::string(argv[i]) == "--csv")
            mode = OutputMode::csv;
        else if (std::string(argv[i]) == "--json")
            mode = OutputMode::json;
    }

    // Target widths
    const uint32_t widths[] = { 256, 512, 1024, 2048, 4096 };
    const int n_widths = sizeof(widths) / sizeof(widths[0]);

    // Banner to stderr (doesn't pollute stdout data)
    fprintf(stderr, "pow_mod benchmark — backends:");
    fprintf(stderr, " Hydra");
#ifdef HYDRA_POWMOD_BOOST
    fprintf(stderr, " | Boost.Multiprecision");
#endif
#ifdef HYDRA_POWMOD_GMP
    fprintf(stderr, " | GMP");
#endif
#ifdef HYDRA_POWMOD_OPENSSL
    fprintf(stderr, " | OpenSSL");
#endif
    fprintf(stderr, "\n");
    fprintf(stderr, "Widths: 256, 512, 1024, 2048, 4096 bits\n");
    fprintf(stderr, "Samples per width: %d (+ %d warmup)\n\n", SAMPLE_COUNT, WARMUP_ITERS);

    std::vector<BenchRow> rows;

    for (int wi = 0; wi < n_widths; ++wi) {
        uint32_t bits = widths[wi];
        fprintf(stderr, "  %4u bits ... ", bits);

        Operands ops(bits);

        // Cross-validate before timing
        if (!cross_validate(ops)) {
            fprintf(stderr, "CROSS-VALIDATION FAILED — aborting\n");
            return 1;
        }

        BenchRow row;
        row.bits = bits;

        row.hydra = bench_hydra(ops);
        fprintf(stderr, "hydra=%s", format_ns(row.hydra.median_ns).c_str());

#ifdef HYDRA_POWMOD_BOOST
        row.boost = bench_boost(ops);
        fprintf(stderr, "  boost=%s", format_ns(row.boost.median_ns).c_str());
#endif

#ifdef HYDRA_POWMOD_GMP
        row.gmp = bench_gmp(ops);
        fprintf(stderr, "  gmp=%s", format_ns(row.gmp.median_ns).c_str());
#endif

#ifdef HYDRA_POWMOD_OPENSSL
        row.openssl = bench_openssl(ops);
        fprintf(stderr, "  openssl=%s", format_ns(row.openssl.median_ns).c_str());
#endif

        fprintf(stderr, "\n");
        rows.push_back(std::move(row));
    }

    fprintf(stderr, "\nDone.\n\n");

    // Output
    switch (mode) {
        case OutputMode::json:     print_json(rows);     break;
        case OutputMode::markdown: print_markdown(rows);  break;
        case OutputMode::csv:      print_csv(rows);       break;
    }

    return 0;
}
