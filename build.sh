# old just in case
#cmake -B build -DCMAKE_BUILD_TYPE=Release
#cmake --build build -j

cmake -S . -B build-rel \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DHYDRA_BENCH_BOOST=ON \
      -DCMAKE_XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT="dwarf-with-dsym"

cmake --build build-rel -j


./scripts/sign_hydra_test.sh
