set -e

  # Debug build (tests with sanitizers):
  cmake -B build -DCMAKE_BUILD_TYPE=Debug
  cmake --build build -j
  ./build/hydra_test

  # Release benchmarks:
  cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
  cmake --build build-rel -j
  ./build-rel/hydra_bench

  # pow_mod comparative benchmark (all backends):
  cmake -B build-rel -DCMAKE_BUILD_TYPE=Release \
      -DHYDRA_POWMOD_GMP=ON -DHYDRA_POWMOD_OPENSSL=ON
  cmake --build build-rel --target bench_pow_mod -j
  ./build-rel/bench_pow_mod --markdown
