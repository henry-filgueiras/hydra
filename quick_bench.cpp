// quick_bench.cpp — standalone micro-benchmark for mul kernels
// Compile: g++ -std=c++20 -O3 -march=native quick_bench.cpp -o quick_bench
//
// Measures wall-clock time for the key multiplication paths.

#include "hydra.hpp"
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <vector>

using hydra::Hydra;

// Prevent dead-code elimination
template<typename T>
static void do_not_optimize(T const& val) {
    asm volatile("" : : "r,m"(val) : "memory");
}
static void clobber_memory() {
    asm volatile("" : : : "memory");
}

struct XorShift64 {
    uint64_t s;
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
};

static Hydra make_large(uint32_t n, uint64_t seed = 0xDEAD'BEEFull) {
    std::vector<uint64_t> limbs(n);
    XorShift64 rng{seed};
    for (auto& l : limbs) l = rng.next() | 1u;
    limbs.back() |= (1ull << 63);
    return Hydra::from_limbs(limbs.data(), n);
}

static double bench(const char* name, auto fn, int iters = 2000000) {
    // Warmup
    for (int i = 0; i < iters / 10; ++i) fn();

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iters; ++i) fn();
    auto t1 = std::chrono::high_resolution_clock::now();

    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
    std::printf("  %-30s %8.2f ns/op\n", name, ns);
    return ns;
}

int main() {
    std::printf("=== Multiplication micro-benchmark ===\n\n");

    // Medium × Medium (2-limb × 2-limb → up to 4 limbs)
    {
        Hydra a = Hydra::make_medium(0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, 0, 2);
        Hydra b = Hydra::make_medium(0x1111111111111111ull, 0x2222222222222222ull, 0, 2);
        bench("medium_mul (2x2)", [&]() {
            do_not_optimize(a); do_not_optimize(b);
            Hydra c = a * b;
            do_not_optimize(c);
            clobber_memory();
        });
    }

    // 3-limb × 3-limb
    {
        Hydra a = Hydra::make_medium(0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, 0xCCCCCCCCCCCCCCCCull, 3);
        Hydra b = Hydra::make_medium(0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull, 3);
        bench("medium_mul (3x3)", [&]() {
            do_not_optimize(a); do_not_optimize(b);
            Hydra c = a * b;
            do_not_optimize(c);
            clobber_memory();
        });
    }

    // 4-limb × 4-limb (256-bit)
    {
        Hydra a = make_large(4, 0xAAAA);
        Hydra b = make_large(4, 0xBBBB);
        bench("large_mul_256 (4x4)", [&]() {
            do_not_optimize(a); do_not_optimize(b);
            Hydra c = a * b;
            do_not_optimize(c);
            clobber_memory();
        });
    }

    // 8-limb × 8-limb (512-bit)
    {
        Hydra a = make_large(8, 0x1234);
        Hydra b = make_large(8, 0x5678);
        bench("large_mul_512 (8x8)", [&]() {
            do_not_optimize(a); do_not_optimize(b);
            Hydra c = a * b;
            do_not_optimize(c);
            clobber_memory();
        });
    }

    // Generic fallback: 5-limb × 5-limb
    {
        Hydra a = make_large(5, 0xAAAA);
        Hydra b = make_large(5, 0xBBBB);
        bench("generic_mul (5x5)", [&]() {
            do_not_optimize(a); do_not_optimize(b);
            Hydra c = a * b;
            do_not_optimize(c);
            clobber_memory();
        });
    }

    // Generic fallback: 16-limb × 16-limb
    {
        Hydra a = make_large(16, 0xAAAA);
        Hydra b = make_large(16, 0xBBBB);
        bench("generic_mul (16x16)", [&]() {
            do_not_optimize(a); do_not_optimize(b);
            Hydra c = a * b;
            do_not_optimize(c);
            clobber_memory();
        });
    }

    // Also test the raw kernel performance (no from_limbs overhead)
    std::printf("\n--- Raw kernel times (no result construction) ---\n\n");

    {
        uint64_t a[4] = {0xAAAA'AAAA'AAAA'AAAAull, 0xBBBB'BBBB'BBBB'BBBBull,
                         0xCCCC'CCCC'CCCC'CCCCull, 0xDDDD'DDDD'DDDD'DDDDull};
        uint64_t b[4] = {0x1111'1111'1111'1111ull, 0x2222'2222'2222'2222ull,
                         0x3333'3333'3333'3333ull, 0x4444'4444'4444'4444ull};
        uint64_t out[8];
        bench("raw mul_4x4", [&]() {
            do_not_optimize(a); do_not_optimize(b);
            hydra::detail::mul_4x4(a, b, out);
            do_not_optimize(out);
            clobber_memory();
        });
        bench("raw mul_limbs 4x4", [&]() {
            do_not_optimize(a); do_not_optimize(b);
            hydra::detail::mul_limbs(a, 4, b, 4, out);
            do_not_optimize(out);
            clobber_memory();
        });
    }

    {
        uint64_t a[8], b[8], out[16];
        XorShift64 rng{0x1234};
        for (auto& l : a) l = rng.next() | 1u;
        for (auto& l : b) l = rng.next() | 1u;
        bench("raw mul_8x8", [&]() {
            do_not_optimize(a); do_not_optimize(b);
            hydra::detail::mul_8x8(a, b, out);
            do_not_optimize(out);
            clobber_memory();
        });
        bench("raw mul_limbs 8x8", [&]() {
            do_not_optimize(a); do_not_optimize(b);
            hydra::detail::mul_limbs(a, 8, b, 8, out);
            do_not_optimize(out);
            clobber_memory();
        });
    }

    std::printf("\nDone.\n");
    return 0;
}
