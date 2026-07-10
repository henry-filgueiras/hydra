#!/usr/bin/env bash
#
# scripts/wasm_demo.sh — build the in-browser live benchmark demo.
#
# Produces build-wasm/demo/{index.html, bench_web.js}: a fully
# self-contained page (SINGLE_FILE embeds the wasm as base64 inside
# the JS) that runs the Hydra vs mini-gmp vs Boost pow_mod shootout
# live in the visitor's browser.  No server-side anything; file:// or
# any static host works.
#
# Usage:
#   scripts/wasm_demo.sh          # build
#   scripts/wasm_demo.sh --serve  # build + python http server on :8123
#
# Prereqs: scripts/wasm_bootstrap.sh (emcc), Boost headers.  mini-gmp
# is fetched exactly as in scripts/wasm_bench.sh (shared cache in
# build-wasm/minigmp, sha256-pinned, never committed).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

WASM_DIR="$HERE/build-wasm"
DEMO_OUT="$WASM_DIR/demo"
MINIGMP_DIR="$WASM_DIR/minigmp"
BOOST_INC="/opt/homebrew/include"

say()  { printf '\033[1;36m[wasm-demo]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[wasm-demo] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

command -v emcc >/dev/null 2>&1 || fail "emcc not found — run scripts/wasm_bootstrap.sh first"
[[ -f "$BOOST_INC/boost/multiprecision/cpp_int.hpp" ]] \
    || fail "Boost headers not found at $BOOST_INC — 'brew install boost'"

mkdir -p "$MINIGMP_DIR" "$DEMO_OUT"

# ── mini-gmp (shared fetcher, same cache as wasm_bench.sh) ───
"$HERE/scripts/fetch_minigmp.sh"
if [[ ! -f "$MINIGMP_DIR/mini-gmp.o" || "$MINIGMP_DIR/mini-gmp.c" -nt "$MINIGMP_DIR/mini-gmp.o" ]]; then
    say "compiling mini-gmp.c → wasm object…"
    emcc -O2 -DNDEBUG -c "$MINIGMP_DIR/mini-gmp.c" -o "$MINIGMP_DIR/mini-gmp.o"
fi

# ── Demo module ──────────────────────────────────────────────
# -O2 (not -Os): the SINGLE_FILE module is ~120-160 KB either way,
# and the demo's numbers should match scripts/wasm_bench.sh, which
# uses -O2.  EH-free (see wasm_bench.sh — nothing throws in the
# timed path, and wasm EH costs Hydra ~15%).
say "compiling demo module (SINGLE_FILE, MODULARIZE)…"
emcc -std=c++20 -O2 -DNDEBUG -I. \
    -DHYDRA_POWMOD_GMP -DHYDRA_POWMOD_BOOST \
    -I"$MINIGMP_DIR" -I"$BOOST_INC" \
    demo/bench_web.cpp hydra.cpp "$MINIGMP_DIR/mini-gmp.o" \
    -sSINGLE_FILE=1 \
    -sMODULARIZE=1 \
    -sEXPORT_NAME=HydraBench \
    -sENVIRONMENT=web,node \
    -sALLOW_MEMORY_GROWTH=1 \
    -sSTACK_SIZE=8388608 \
    -sEXPORTED_FUNCTIONS=_hydra_bench_cell,_hydra_validate,_hydra_rsa_keygen,_hydra_next_prime_str,_hydra_powmod_str \
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap \
    -o "$DEMO_OUT/bench_web.js"

cp demo/index.html "$DEMO_OUT/index.html"
say "built: $DEMO_OUT  ($(du -h "$DEMO_OUT/bench_web.js" | cut -f1 | tr -d ' ') module)"

if [[ "${1:-}" == "--serve" ]]; then
    say "serving on http://localhost:8123 (Ctrl-C to stop)"
    exec python3 -m http.server 8123 -d "$DEMO_OUT"
else
    say "open $DEMO_OUT/index.html directly, or: scripts/wasm_demo.sh --serve"
fi
