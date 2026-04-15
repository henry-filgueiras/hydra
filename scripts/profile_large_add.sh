#!/usr/bin/env bash
set -euo pipefail

# --------------------------------------------------
# Config
# --------------------------------------------------
BENCH="./build/hydra_bench"

# Benchmark filters
HYDRA_FILTER="hydra/large_add_256"
BOOST_FILTER="boost/large_add_256"

# How long to let benchmark run
MIN_TIME="2.0"

# --------------------------------------------------
# Output folder
# --------------------------------------------------
STAMP=$(date +"%Y%m%d_%H%M%S")
TRACE_DIR="traces/${STAMP}"

mkdir -p "${TRACE_DIR}"

echo "=================================================="
echo " Hydra Perf Hunt"
echo " Output: ${TRACE_DIR}"
echo "=================================================="

# --------------------------------------------------
# Helper
# --------------------------------------------------
run_trace() {
    local label="$1"
    local template="$2"
    local filter="$3"
    local outfile="$4"

    echo ">>> ${label}"

    xctrace record \
        --template "${template}" \
        --time-limit 5s \
        --output "${outfile}" \
        --launch "${BENCH}" \
        -- \
        --benchmark_filter="${filter}" \
        --benchmark_min_time=2.0
}

# --------------------------------------------------
# Hydra traces
# --------------------------------------------------
run_trace \
    "Hydra Time Profiler" \
    "Time Profiler" \
    "${HYDRA_FILTER}" \
    "${TRACE_DIR}/hydra_time.trace"

run_trace \
    "Hydra Allocations" \
    "Allocations" \
    "${HYDRA_FILTER}" \
    "${TRACE_DIR}/hydra_alloc.trace"

# --------------------------------------------------
# Boost traces
# --------------------------------------------------
run_trace \
    "Boost Time Profiler" \
    "Time Profiler" \
    "${BOOST_FILTER}" \
    "${TRACE_DIR}/boost_time.trace"

run_trace \
    "Boost Allocations" \
    "Allocations" \
    "${BOOST_FILTER}" \
    "${TRACE_DIR}/boost_alloc.trace"

echo ""
echo "=================================================="
echo " Done."
echo " Traces written to:"
echo "   ${TRACE_DIR}"
echo "=================================================="

# Open folder in Finder
open "${TRACE_DIR}"
