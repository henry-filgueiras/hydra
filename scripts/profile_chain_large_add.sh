#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════════
# profile_chain_large_add.sh — xctrace profiling for chained in-place addition
# ═══════════════════════════════════════════════════════════════════════════════
#
# What it profiles
# ────────────────
#   Chained in-place accumulation (operator+=) for both Hydra and
#   Boost.Multiprecision.  This exercises the hot path where the accumulator
#   is Large and (for Hydra) has sufficient capacity to avoid reallocation.
#
#   Hydra benchmark:  chain/large_add   (parameterised by limb count)
#   Boost benchmark:  boost/chain_large_add  (same limb counts)
#
# Where traces are written
# ────────────────────────
#   traces/YYYYMMDD_HHMMSS/
#     hydra_chain_large_add_time.trace    — Time Profiler (CPU sampling)
#     hydra_chain_large_add_alloc.trace   — Allocations (malloc tracking)
#     boost_chain_large_add_time.trace    — Time Profiler (CPU sampling)
#     boost_chain_large_add_alloc.trace   — Allocations (malloc tracking)
#
# How to open in Instruments
# ──────────────────────────
#   Double-click any .trace bundle in Finder, or:
#     open traces/<stamp>/hydra_chain_large_add_time.trace
#   Instruments will launch and display the recorded session.
#
# Usage
# ─────
#   ./scripts/profile_chain_large_add.sh              # defaults (16 limbs)
#   LIMBS=64 ./scripts/profile_chain_large_add.sh     # 64-limb variant
#   MIN_TIME=5s ./scripts/profile_chain_large_add.sh  # longer recording
#
# ═══════════════════════════════════════════════════════════════════════════════
set -euo pipefail
set -x trace

# Memory profiling environment — enables full stack logging for Allocations
# template and disables the nano zone (which can interfere with tracing).
export MallocStackLogging=1
export MallocNanoZone=0
export DYLD_INSERT_LIBRARIES=

# --------------------------------------------------
# Config  (override via environment variables)
# --------------------------------------------------
BENCH="${BENCH:-./build-rel/hydra_bench}"
LIMBS="${LIMBS:-16}"
TIME_LIMIT="${TIME_LIMIT:-5s}"
MIN_TIME="${MIN_TIME:-3s}"

# Benchmark filter names — must match the registered Google Benchmark names
# exactly.  The /N suffix selects a single parameterised instance.
HYDRA_FILTER="chain/large_add/${LIMBS}"
BOOST_FILTER="boost/chain_large_add/${LIMBS}"

# --------------------------------------------------
# Output folder
# --------------------------------------------------
STAMP=$(date +"%Y%m%d_%H%M%S")
TRACE_DIR="traces/${STAMP}"

mkdir -p "${TRACE_DIR}"

echo "=================================================="
echo " Chain Large-Add Profiling  (${LIMBS} limbs)"
echo " Output: ${TRACE_DIR}"
echo "=================================================="

# --------------------------------------------------
# Helper — run a single xctrace recording
# --------------------------------------------------
run_trace() {
    local label="$1"
    local template="$2"
    local filter="$3"
    local outfile="$4"

    echo ""
    echo ">>> ${label}"
    echo "    template : ${template}"
    echo "    filter   : ${filter}"
    echo "    output   : ${outfile}"

    xctrace record \
        --template "${template}" \
        --output "${outfile}" \
        --time-limit "${TIME_LIMIT}" \
        --launch "${BENCH}" \
        -- \
        --benchmark_filter="${filter}" \
        --benchmark_min_time="${MIN_TIME}"
}

# --------------------------------------------------
# Hydra traces
# --------------------------------------------------
run_trace \
    "Hydra Time Profiler" \
    "Time Profiler" \
    "${HYDRA_FILTER}" \
    "${TRACE_DIR}/hydra_chain_large_add_time.trace"

run_trace \
    "Hydra Allocations" \
    "Allocations" \
    "${HYDRA_FILTER}" \
    "${TRACE_DIR}/hydra_chain_large_add_alloc.trace"

# --------------------------------------------------
# Boost traces
# --------------------------------------------------
run_trace \
    "Boost Time Profiler" \
    "Time Profiler" \
    "${BOOST_FILTER}" \
    "${TRACE_DIR}/boost_chain_large_add_time.trace"

run_trace \
    "Boost Allocations" \
    "Allocations" \
    "${BOOST_FILTER}" \
    "${TRACE_DIR}/boost_chain_large_add_alloc.trace"

# --------------------------------------------------
# Summary
# --------------------------------------------------
echo ""
echo "=================================================="
echo " Done.  4 traces recorded."
echo " Traces written to:"
echo "   ${TRACE_DIR}/"
echo ""
echo " Files:"
for f in "${TRACE_DIR}"/*.trace; do
    echo "   $(basename "$f")"
done
echo ""
echo " Open in Instruments:"
echo "   open ${TRACE_DIR}/hydra_chain_large_add_time.trace"
echo "=================================================="

# Open folder in Finder so you can double-click traces.
open "${TRACE_DIR}"
