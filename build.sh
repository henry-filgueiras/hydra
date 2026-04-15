cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./scripts/sign_hydra_test.sh
