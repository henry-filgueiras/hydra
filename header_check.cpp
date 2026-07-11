// Header self-sufficiency check: this TU includes ONLY hydra.hpp.
// It must compile without any prior includes — guards against the
// header silently depending on transitive standard-library includes
// (e.g. <concepts> for std::unsigned_integral, which some standard
// libraries happen to drag in via <memory> and others don't).
#include "hydra.hpp"

int main() {
    hydra::Hydra x = 1;
    return x.is_zero() ? 1 : 0;
}
