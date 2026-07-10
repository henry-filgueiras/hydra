#!/usr/bin/env bash
#
# scripts/wasm_bench.sh — the "fastest bignum you can ship in a
# browser" shootout: Hydra vs mini-gmp vs Boost cpp_int, all compiled
# to WebAssembly and run under node.
#
# Why mini-gmp is the honest GMP comparator here: real GMP's speed
# comes from per-arch hand-written assembly, which does not exist for
# wasm32.  Projects that compile GMP-dependent code to wasm get
# mini-gmp — the portable two-file fallback bundled with GMP — or
# generic-C GMP, which is in the same ballpark.  Boost cpp_int is
# header-only C++ and compiles to wasm unchanged, same as Hydra.
#
# Usage:
#   scripts/wasm_bench.sh              # fetch deps, build, run (--runs 2)
#   scripts/wasm_bench.sh --runs N     # pass through to bench_pow_mod
#
# Prereqs: scripts/wasm_bootstrap.sh (emcc + node), Boost headers in
# /opt/homebrew/include.  mini-gmp (LGPLv3+/GPLv2+ dual) is downloaded
# at bench time into build-wasm/ and never committed to this MIT repo;
# it is used only as a benchmark comparator, never linked into Hydra.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

WASM_DIR="$HERE/build-wasm"
GMP_VER="6.3.0"
GMP_TARBALL="gmp-$GMP_VER.tar.xz"
GMP_URL="https://ftp.gnu.org/gnu/gmp/$GMP_TARBALL"
GMP_SHA256="a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898"
MINIGMP_DIR="$WASM_DIR/minigmp"
BOOST_INC="/opt/homebrew/include"
if [[ "$#" -eq 0 ]]; then RUNS_ARGS=(--runs 2); else RUNS_ARGS=("$@"); fi

say()  { printf '\033[1;36m[wasm-bench]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[wasm-bench] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

command -v emcc >/dev/null 2>&1 || fail "emcc not found — run scripts/wasm_bootstrap.sh first"
command -v node >/dev/null 2>&1 || fail "node not found — run scripts/wasm_bootstrap.sh first"
[[ -f "$BOOST_INC/boost/multiprecision/cpp_int.hpp" ]] \
    || fail "Boost headers not found at $BOOST_INC — 'brew install boost'"

mkdir -p "$MINIGMP_DIR"

# ── Fetch + verify mini-gmp (cached across runs) ─────────────
if [[ ! -f "$MINIGMP_DIR/mini-gmp.c" ]]; then
    say "downloading $GMP_TARBALL from ftp.gnu.org (mini-gmp is bundled inside)…"
    /usr/bin/curl -sL --max-time 120 -o "$WASM_DIR/$GMP_TARBALL" "$GMP_URL"
    echo "$GMP_SHA256  $WASM_DIR/$GMP_TARBALL" | shasum -a 256 -c - >/dev/null \
        || fail "checksum mismatch on $GMP_TARBALL"
    tar -xf "$WASM_DIR/$GMP_TARBALL" -C "$WASM_DIR" \
        "gmp-$GMP_VER/mini-gmp/mini-gmp.c" "gmp-$GMP_VER/mini-gmp/mini-gmp.h"
    cp "$WASM_DIR/gmp-$GMP_VER/mini-gmp/mini-gmp.c" \
       "$WASM_DIR/gmp-$GMP_VER/mini-gmp/mini-gmp.h" "$MINIGMP_DIR/"
    # gmp.h shim: bench_pow_mod's GMP backend includes <gmp.h> and uses
    # only the mpz_* subset mini-gmp implements.
    printf '// gmp.h shim: route the GMP backend through mini-gmp.\n#include "mini-gmp.h"\n' \
        > "$MINIGMP_DIR/gmp.h"
    say "mini-gmp $GMP_VER ready (checksum verified)"
else
    say "mini-gmp already present in $MINIGMP_DIR (cached)"
fi

# ── Build ────────────────────────────────────────────────────
# mini-gmp.c is C — compile separately (no -std=c++20).
if [[ ! -f "$MINIGMP_DIR/mini-gmp.o" || "$MINIGMP_DIR/mini-gmp.c" -nt "$MINIGMP_DIR/mini-gmp.o" ]]; then
    say "compiling mini-gmp.c → wasm object…"
    emcc -O2 -DNDEBUG -c "$MINIGMP_DIR/mini-gmp.c" -o "$MINIGMP_DIR/mini-gmp.o"
fi

say "compiling three-way shootout (Hydra + mini-gmp + Boost cpp_int)…"
# No -fwasm-exceptions here, deliberately: nothing throws in the timed
# path (throw → abort under emcc's default is acceptable for a bench),
# and wasm EH costs Hydra ~35% at 2048-bit (8.55 ms vs 6.3 ms measured
# 2026-07-10).  All three backends get the same EH-free treatment.
# hydra_test, by contrast, NEEDS -fwasm-exceptions (see wasm_bootstrap).
emcc -std=c++20 -O2 -DNDEBUG -I. \
    -DHYDRA_POWMOD_GMP -DHYDRA_POWMOD_BOOST \
    -I"$MINIGMP_DIR" -I"$BOOST_INC" \
    bench/bench_pow_mod.cpp hydra.cpp "$MINIGMP_DIR/mini-gmp.o" \
    -sALLOW_MEMORY_GROWTH=1 \
    -sSTACK_SIZE=8388608 \
    -o "$WASM_DIR/bench_pow_mod_shootout.js"

# ── Run ──────────────────────────────────────────────────────
say "running under node ${RUNS_ARGS[*]} (mini-gmp at 4096-bit is slow — be patient)…"
node "$WASM_DIR/bench_pow_mod_shootout.js" "${RUNS_ARGS[@]}" --md

say "done.  The 'gmp' column above is mini-gmp — GMP's portable wasm fallback."
