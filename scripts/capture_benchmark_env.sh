#!/usr/bin/env bash
#
# scripts/capture_benchmark_env.sh — benchmark provenance capture.
#
# Emits a Markdown block describing the exact environment a benchmark
# ran in: git state, OS/CPU, toolchain versions (compiler, CMake, node,
# emcc, wasm-opt), comparator library versions (GMP, OpenSSL), package
# metadata, and the configure/build flags of the build directory the
# numbers came from.  Paste the output verbatim into benchmark evidence
# (perf_snapshot.md, DIRECTORS_NOTES.md) so results are never again
# recorded without their toolchain.
#
# Every field degrades gracefully: a missing tool or file yields an
# explicit "(unavailable)" instead of aborting the capture.  The script
# makes no performance claims and never runs the benchmark itself.
#
# Usage:
#   scripts/capture_benchmark_env.sh                              # env only
#   scripts/capture_benchmark_env.sh --build-dir build-rel        # + that dir's flags
#   scripts/capture_benchmark_env.sh -- ./build-rel/bench_pow_mod --runs 6 --md
#       # records (does NOT execute) the benchmark command line
#   scripts/capture_benchmark_env.sh > /tmp/env.md   # then paste into evidence

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

BUILD_DIR="build-rel"
BENCH_CMD=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="${2:?--build-dir needs an argument}"; shift 2 ;;
        --)          shift; BENCH_CMD="$*"; break ;;
        *)           echo "usage: $0 [--build-dir DIR] [-- benchmark command...]" >&2; exit 2 ;;
    esac
done

# field LABEL CMD...  — run CMD, print first line of stdout, or "(unavailable)".
field() {
    local label="$1"; shift
    local out=""
    out="$("$@" 2>/dev/null | head -n1)" || out=""
    [[ -n "$out" ]] || out="(unavailable)"
    printf -- '- **%s:** %s\n' "$label" "$out"
}

# first_of LABEL "cmd1" "cmd2"...  — first command (run via sh -c) with output wins.
first_of() {
    local label="$1"; shift
    local out=""
    for cmd in "$@"; do
        out="$(sh -c "$cmd" 2>/dev/null | head -n1)" || out=""
        [[ -n "$out" ]] && break
    done
    [[ -n "$out" ]] || out="(unavailable)"
    printf -- '- **%s:** %s\n' "$label" "$out"
}

echo "### Benchmark environment"
echo

# ── Repository ──
GIT_SHA="$(git rev-parse HEAD 2>/dev/null || true)"
printf -- '- **Git SHA:** %s\n' "${GIT_SHA:-(unavailable)}"
if git rev-parse --git-dir >/dev/null 2>&1; then
    DIRTY_COUNT="$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
    if [[ "$DIRTY_COUNT" == "0" ]]; then
        printf -- '- **Working tree:** clean\n'
    else
        printf -- '- **Working tree:** dirty (%s modified/untracked paths)\n' "$DIRTY_COUNT"
    fi
else
    printf -- '- **Working tree:** (unavailable)\n'
fi

# ── Machine ──
first_of "OS" \
    'echo "$(sw_vers -productName) $(sw_vers -productVersion) (build $(sw_vers -buildVersion))"' \
    '. /etc/os-release && echo "$PRETTY_NAME"' \
    'uname -sr'
field "Kernel" uname -sr
field "Architecture" uname -m
first_of "CPU" \
    'sysctl -n machdep.cpu.brand_string' \
    'grep -m1 "model name" /proc/cpuinfo | cut -d: -f2- | sed "s/^ //"'

# ── Toolchain ──
CXX_FROM_CACHE=""
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    CXX_FROM_CACHE="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt" | head -n1)"
fi
if [[ -n "$CXX_FROM_CACHE" && -x "$CXX_FROM_CACHE" ]]; then
    field "C++ compiler ($CXX_FROM_CACHE)" "$CXX_FROM_CACHE" --version
else
    field "C++ compiler (c++)" c++ --version
fi
field "CMake" cmake --version
field "Node" node --version
field "Emscripten (emcc)" emcc --version
# wasm-opt lives inside emsdk when not on PATH — same lookup as wasm_pkg.sh.
first_of "Binaryen (wasm-opt)" \
    'wasm-opt --version' \
    '"$(em-config BINARYEN_ROOT 2>/dev/null)/bin/wasm-opt" --version'

# ── Comparator libraries ──
first_of "GMP" \
    'pkg-config --modversion gmp' \
    'brew list --versions gmp' \
    'sed -n "s/^#define __GNU_MP_VERSION\([_A-Z]*\) *//p" "$(brew --prefix gmp)/include/gmp.h" | paste -sd. -'
# Label stays plain "OpenSSL": the last fallback (PATH openssl) can be
# the system LibreSSL, which is not the linked comparator — the linked
# libcrypto path is recorded in the build-flags block below.
first_of "OpenSSL" \
    '"$(brew --prefix openssl@3)/bin/openssl" version' \
    'pkg-config --modversion openssl' \
    'openssl version'

# ── Package manager metadata ──
first_of "Package manager" \
    'brew --version' \
    'dpkg-query --version' \
    'apk --version'
first_of "Package versions (gmp / openssl)" \
    'brew list --versions gmp openssl@3 2>/dev/null | sort -u | tr "\n" ";" | sed "s/;\$//; s/;/; /g"' \
    'dpkg-query -W -f "\${Package} \${Version}; " "libgmp*" "libssl*" 2>/dev/null | sed "s/; \$//"'

# ── Build configuration ──
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    printf -- '- **Build dir:** `%s`\n' "$BUILD_DIR"
    echo "- **Configure/build flags** (from \`$BUILD_DIR/CMakeCache.txt\`):"
    echo
    echo '  ```'
    grep -E '^(CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER:|CMAKE_CXX_FLAGS|CMAKE_OSX|HYDRA_[A-Z0-9_]+|GMP_(VERSION|INCLUDE_DIRS|LIBRARY:)|OPENSSL_(CRYPTO_LIBRARY:|INCLUDE_DIR:))' \
        "$BUILD_DIR/CMakeCache.txt" 2>/dev/null \
        | grep -vE -- '-ADVANCED:|=$' | sort | sed 's/^/  /' \
        || echo '  (no matching cache entries)'
    echo '  ```'
else
    printf -- '- **Build dir:** `%s` — CMakeCache.txt not found; configure/build flags (unavailable)\n' "$BUILD_DIR"
fi

# ── Run identity ──
if [[ -n "$BENCH_CMD" ]]; then
    printf -- '- **Benchmark command:** `%s`\n' "$BENCH_CMD"
else
    printf -- '- **Benchmark command:** (not recorded — pass it after `--`)\n'
fi
field "Captured" date '+%Y-%m-%d %H:%M:%S %Z (UTC%z)'
