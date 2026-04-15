#!/usr/bin/env bash
# bench/run.sh — one-shot benchmark runner + comparison report
#
# Usage:
#   ./bench/run.sh                    # build Release, run, print terminal report
#   ./bench/run.sh --markdown         # emit Markdown table
#   ./bench/run.sh --boost            # build with Boost comparison enabled
#   ./bench/run.sh --output FILE      # save report to FILE
#   ./bench/run.sh --json-out FILE    # save delta JSON to FILE
#   ./bench/run.sh --filter PATTERN   # benchmark filter (passed to both binary and compare.py)
#   ./bench/run.sh --no-build         # skip cmake build step
#
# Environment overrides:
#   BUILD_DIR=path    (default: build-rel)
#   BENCH_BINARY=path (default: $BUILD_DIR/hydra_bench)
#   RESULTS_JSON=path (default: $BUILD_DIR/bench_results.json)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Defaults ──────────────────────────────────────────────────────────────────
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-rel}"
BENCH_BINARY="${BENCH_BINARY:-${BUILD_DIR}/hydra_bench}"
RESULTS_JSON="${RESULTS_JSON:-${BUILD_DIR}/bench_results.json}"
BOOST=0
NO_BUILD=0
COMPARE_ARGS=()
BENCH_FILTER=""

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --boost)       BOOST=1; shift ;;
        --no-build)    NO_BUILD=1; shift ;;
        --filter)      BENCH_FILTER="$2"; shift 2 ;;
        --markdown|--output|--json-out|--threshold|--real-time)
                       COMPARE_ARGS+=("$1");
                       [[ "$1" != "--markdown" && "$1" != "--real-time" ]] && \
                           COMPARE_ARGS+=("$2") && shift
                       shift ;;
        *)             echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# ── Build ─────────────────────────────────────────────────────────────────────
if [[ $NO_BUILD -eq 0 ]]; then
    echo "── Building hydra_bench (Release) ──────────────────────────────────────"
    CMAKE_OPTS=(-DCMAKE_BUILD_TYPE=Release)
    [[ $BOOST -eq 1 ]] && CMAKE_OPTS+=(-DHYDRA_BENCH_BOOST=ON)

    cmake -B "$BUILD_DIR" "${CMAKE_OPTS[@]}" -S "$REPO_ROOT" \
        -DBENCHMARK_ENABLE_TESTING=OFF \
        -DBENCHMARK_ENABLE_GTEST_TESTS=OFF \
        -DBENCHMARK_ENABLE_INSTALL=OFF \
        -Wno-dev 2>&1 | grep -E "(Hydra|Boost|error|warning:)" || true

    cmake --build "$BUILD_DIR" --target hydra_bench -j "$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
    echo ""
fi

# ── Run benchmarks ────────────────────────────────────────────────────────────
echo "── Running benchmarks → ${RESULTS_JSON} ────────────────────────────────"
BENCH_ARGS=(
    --benchmark_format=json
    --benchmark_out="$RESULTS_JSON"
    --benchmark_repetitions=3          # 3 runs; compare.py uses per-run rows
    --benchmark_min_time=0.2s
)
[[ -n "$BENCH_FILTER" ]] && BENCH_ARGS+=(--benchmark_filter="$BENCH_FILTER")

"$BENCH_BINARY" "${BENCH_ARGS[@]}"
echo ""

# ── Compare ───────────────────────────────────────────────────────────────────
echo "── Comparison report ───────────────────────────────────────────────────"
python3 "$SCRIPT_DIR/compare.py" "$RESULTS_JSON" "${COMPARE_ARGS[@]}"
