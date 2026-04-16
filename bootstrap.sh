#!/usr/bin/env bash
# bootstrap.sh — install all prerequisites for building and benchmarking Hydra
#
# Usage:
#   ./bootstrap.sh          # install everything
#   ./bootstrap.sh --minimal  # just the build essentials (no comparison libs)
#
# Requires Homebrew (https://brew.sh).

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
RESET='\033[0m'

info()  { printf "${GREEN}==> %s${RESET}\n" "$*"; }
warn()  { printf "${YELLOW}==> %s${RESET}\n" "$*"; }
error() { printf "${RED}==> %s${RESET}\n" "$*" >&2; }

# ─────────────────────────────────────────────────────────
# Check for Homebrew
# ─────────────────────────────────────────────────────────
if ! command -v brew &>/dev/null; then
    error "Homebrew not found.  Install it from https://brew.sh"
    exit 1
fi

# ─────────────────────────────────────────────────────────
# Parse args
# ─────────────────────────────────────────────────────────
MINIMAL=false
for arg in "$@"; do
    case "$arg" in
        --minimal) MINIMAL=true ;;
        --help|-h)
            echo "Usage: ./bootstrap.sh [--minimal]"
            echo ""
            echo "  --minimal   Install only build essentials (cmake, python)."
            echo "              Skips Boost, GMP, and OpenSSL."
            exit 0
            ;;
        *)
            error "Unknown option: $arg"
            exit 1
            ;;
    esac
done

# ─────────────────────────────────────────────────────────
# Core build tools (always needed)
# ─────────────────────────────────────────────────────────
CORE_PACKAGES=(
    cmake
    python@3
)

# Full benchmark suite (comparison libraries)
BENCH_PACKAGES=(
    boost
    gmp
    openssl
)

# ─────────────────────────────────────────────────────────
# Install
# ─────────────────────────────────────────────────────────

install_if_missing() {
    local pkg="$1"
    if brew list --formula "$pkg" &>/dev/null; then
        warn "$pkg already installed"
    else
        info "Installing $pkg ..."
        brew install "$pkg"
    fi
}

echo ""
printf "${BOLD}Hydra — dependency bootstrap${RESET}\n"
echo ""

info "Installing core build tools ..."
for pkg in "${CORE_PACKAGES[@]}"; do
    install_if_missing "$pkg"
done

if [ "$MINIMAL" = false ]; then
    echo ""
    info "Installing benchmark comparison libraries ..."
    for pkg in "${BENCH_PACKAGES[@]}"; do
        install_if_missing "$pkg"
    done
fi

# ─────────────────────────────────────────────────────────
# Verify compiler
# ─────────────────────────────────────────────────────────
echo ""
if command -v g++ &>/dev/null; then
    info "C++ compiler: $(g++ --version | head -1)"
elif command -v clang++ &>/dev/null; then
    info "C++ compiler: $(clang++ --version | head -1)"
else
    warn "No C++ compiler found.  Install Xcode Command Line Tools:"
    warn "  xcode-select --install"
fi

# ─────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────
echo ""
printf "${BOLD}Done.${RESET}  Next steps:\n"
echo ""
echo "  # Debug build (tests with sanitizers):"
echo "  cmake -B build -DCMAKE_BUILD_TYPE=Debug"
echo "  cmake --build build -j"
echo "  ./build/hydra_test"
echo ""
echo "  # Release benchmarks:"
echo "  cmake -B build-rel -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build-rel -j"
echo "  ./build-rel/hydra_bench"
echo ""

if [ "$MINIMAL" = false ]; then
    echo "  # pow_mod comparative benchmark (all backends):"
    echo "  cmake -B build-rel -DCMAKE_BUILD_TYPE=Release \\"
    echo "      -DHYDRA_POWMOD_GMP=ON -DHYDRA_POWMOD_OPENSSL=ON"
    echo "  cmake --build build-rel --target bench_pow_mod -j"
    echo "  ./build-rel/bench_pow_mod --markdown"
    echo ""
fi
