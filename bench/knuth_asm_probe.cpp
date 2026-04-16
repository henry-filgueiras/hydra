// Generate assembly for the Knuth kernel in isolation.
// Build: g++ -S -O3 -DNDEBUG -I. bench/knuth_asm_probe.cpp -o knuth.s

#include "../hydra.hpp"
#include <cstdint>

extern "C" void knuth_probe(
    const uint64_t* u_in, uint32_t nu,
    const uint64_t* v_in, uint32_t nv,
    uint64_t* q, uint64_t* r, uint64_t* work)
{
    hydra::detail::divmod_knuth_limbs(u_in, nu, v_in, nv, q, r, work);
}
