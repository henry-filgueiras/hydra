#!/usr/bin/env bash
#
# scripts/pgo_run.sh — reproducible profile-guided optimization for Hydra.
#
# Usage:
#   scripts/pgo_run.sh          # full cycle: instrument → train → merge → use
#   scripts/pgo_run.sh gen      # only build instrumented
#   scripts/pgo_run.sh train    # only run training against build-pgo-gen
#   scripts/pgo_run.sh merge    # only merge *.profraw → merged.profdata
#   scripts/pgo_run.sh use      # only build the PGO-use tree
#   scripts/pgo_run.sh clean    # wipe build-pgo-gen, build-pgo-use, pgo-profiles
#
# Output trees (all .gitignore-d):
#   build-pgo-gen/     instrumented build (same targets as build-rel)
#   pgo-profiles/      raw profiles (default_*.profraw) + merged.profdata
#   build-pgo-use/     final PGO-use release build
#
# Toolchain requirements (hard-stop if missing):
#   - Clang-family C++ compiler (Apple clang or brew llvm both work)
#   - llvm-profdata (via `xcrun -f llvm-profdata` on macOS, or on $PATH)
#
# The normal `build-rel/` Release build is *never* touched; run
# `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j`
# to keep the baseline comparison clean.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

GEN_DIR="$HERE/build-pgo-gen"
USE_DIR="$HERE/build-pgo-use"
PROFILE_DIR="$HERE/pgo-profiles"
PROFDATA="$PROFILE_DIR/merged.profdata"

# Resolve llvm-profdata: prefer xcrun on macOS, fall back to $PATH.
if command -v xcrun >/dev/null 2>&1 && xcrun -f llvm-profdata >/dev/null 2>&1; then
    LLVM_PROFDATA="$(xcrun -f llvm-profdata)"
elif command -v llvm-profdata >/dev/null 2>&1; then
    LLVM_PROFDATA="$(command -v llvm-profdata)"
else
    echo "error: llvm-profdata not found (tried xcrun and PATH)" >&2
    exit 1
fi

step_gen() {
    echo "── [1/4] configure + build instrumented tree (build-pgo-gen) ──"
    mkdir -p "$PROFILE_DIR"
    # Wipe old raw profiles; stale ones from a prior source state will
    # confuse the merge step.
    rm -f "$PROFILE_DIR"/*.profraw
    cmake -S . -B "$GEN_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DHYDRA_BENCH_BOOST=OFF \
        -DHYDRA_PGO_GEN_DIR="$PROFILE_DIR" \
        -DHYDRA_PGO_USE_FILE=""
    cmake --build "$GEN_DIR" --target hydra_bench bench_pow_mod -j
}

step_train() {
    echo "── [2/4] run training corpus against build-pgo-gen ──"
    if [ ! -x "$GEN_DIR/hydra_bench" ] || [ ! -x "$GEN_DIR/bench_pow_mod" ]; then
        echo "error: instrumented binaries missing; run 'pgo_run.sh gen' first" >&2
        exit 1
    fi

    # Every forked process writes its own .profraw thanks to %m (module
    # hash) + %p (pid). One default_*.profraw per run is the norm.
    export LLVM_PROFILE_FILE="$PROFILE_DIR/default_%m_%p.profraw"

    # --- Training workload 1: pow_mod across all widths ---
    # This is the headline end-to-end workload: 7 widths × 50 samples =
    # 350 real pow_mod calls exercising Montgomery, Karatsuba, mul_limbs,
    # REDC, mac_row_2, and the small-add/sub/shift hot paths.
    echo "  [train] bench_pow_mod (7 widths, 50 samples each)"
    "$GEN_DIR/bench_pow_mod" --json >/dev/null

    # --- Training workload 2: Google Benchmark suite, short min_time ---
    # We don't care about wall-clock accuracy here, only code coverage.
    # --benchmark_min_time=0.05s keeps the whole run ~10 s while still
    # hitting every BENCHMARK() at least a few thousand iterations —
    # plenty for PGO edge weighting.
    #
    # --benchmark_filter excludes the `boost/*` comparators: they
    # exercise Boost's code, not ours, and a stale-profile warning is
    # harmless but pollutes the log.
    echo "  [train] hydra_bench (min_time=0.05s, excluding boost/*)"
    "$GEN_DIR/hydra_bench" \
        --benchmark_min_time=0.05s \
        --benchmark_filter='^(?!boost/).*' \
        >/dev/null

    n_raw="$(ls -1 "$PROFILE_DIR"/*.profraw 2>/dev/null | wc -l | tr -d ' ')"
    if [ "$n_raw" = "0" ]; then
        echo "error: no .profraw emitted — instrumentation broken" >&2
        exit 1
    fi
    echo "  [train] collected $n_raw raw profile file(s)"
}

step_merge() {
    echo "── [3/4] merge raw profiles → merged.profdata ──"
    if ! ls "$PROFILE_DIR"/*.profraw >/dev/null 2>&1; then
        echo "error: no .profraw files in $PROFILE_DIR" >&2
        exit 1
    fi
    "$LLVM_PROFDATA" merge -output="$PROFDATA" "$PROFILE_DIR"/*.profraw
    du -h "$PROFDATA" | awk '{printf "  [merge] %s → %s\n", $2, $1}'
}

step_use() {
    echo "── [4/4] configure + build PGO-use tree (build-pgo-use) ──"
    if [ ! -f "$PROFDATA" ]; then
        echo "error: $PROFDATA missing; run 'pgo_run.sh merge' first" >&2
        exit 1
    fi
    cmake -S . -B "$USE_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DHYDRA_BENCH_BOOST=ON \
        -DHYDRA_PGO_GEN_DIR="" \
        -DHYDRA_PGO_USE_FILE="$PROFDATA"
    cmake --build "$USE_DIR" --target hydra_bench bench_pow_mod -j
    echo
    echo "  PGO build ready:"
    echo "    $USE_DIR/hydra_bench"
    echo "    $USE_DIR/bench_pow_mod"
}

step_clean() {
    rm -rf "$GEN_DIR" "$USE_DIR" "$PROFILE_DIR"
    echo "  cleaned: $GEN_DIR $USE_DIR $PROFILE_DIR"
}

case "${1:-all}" in
    gen)   step_gen ;;
    train) step_train ;;
    merge) step_merge ;;
    use)   step_use ;;
    clean) step_clean ;;
    all)
        step_gen
        step_train
        step_merge
        step_use
        ;;
    *)
        echo "usage: $0 [gen|train|merge|use|clean|all]" >&2
        exit 2
        ;;
esac
