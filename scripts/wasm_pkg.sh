#!/usr/bin/env bash
#
# scripts/wasm_pkg.sh — build the wasm core of the hydra-bignum npm
# package.
#
# Produces pkg/dist/hydra_core.mjs + hydra_core.wasm (ES module glue +
# module; the glue locates the .wasm via import.meta.url, which node
# and bundlers both resolve).  pkg/index.mjs is the user-facing BigInt
# wrapper.
#
# Usage:
#   scripts/wasm_pkg.sh          # build
#   scripts/wasm_pkg.sh --test   # build + run pkg/test/test.mjs under node
#
# Prereqs: scripts/wasm_bootstrap.sh (emcc), node >= 18.
#
# ── Why -g + wasm-opt --strip-dwarf instead of plain -O2 ────────────
# Binaryen's post-link `wasm-opt -O2` (emcc's default at -O2/-O3)
# PESSIMIZES Hydra's kernels: measured 2026-07-10, it costs +39% at
# 256-bit, +43% at 2048-bit, +60% at 4096-bit pow_mod vs LLVM's own
# -O2 output, and the damage grows when multiple exported functions
# share the kernels (extra roots stop binaryen re-inlining them).
# Building with -g makes emcc run only "limited" binaryen passes; we
# then strip the DWARF ourselves with a passes-free wasm-opt run
# (1.2 MB → ~130 KB).  See DIRECTORS_NOTES "Dragon — binaryen
# wasm-opt pessimizes the Montgomery kernels".

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

say()  { printf '\033[1;36m[wasm-pkg]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[wasm-pkg] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

command -v emcc >/dev/null 2>&1 || fail "emcc not found — run scripts/wasm_bootstrap.sh first"

WASM_OPT="$(command -v wasm-opt || true)"
if [[ -z "$WASM_OPT" ]]; then
    WASM_OPT="$(em-config BINARYEN_ROOT 2>/dev/null)/bin/wasm-opt"
fi
[[ -x "$WASM_OPT" ]] || fail "wasm-opt not found (looked in PATH and em-config BINARYEN_ROOT)"

mkdir -p pkg/dist

say "compiling pkg/hydra_binding.cpp → pkg/dist/hydra_core.{mjs,wasm} (LLVM -O2, binaryen limited)…"
emcc -std=c++20 -O2 -g -DNDEBUG -I. \
    pkg/hydra_binding.cpp \
    -sMODULARIZE=1 \
    -sEXPORT_ES6=1 \
    -sEXPORT_NAME=createHydraModule \
    -sENVIRONMENT=web,node,worker \
    -sALLOW_MEMORY_GROWTH=1 \
    -sSTACK_SIZE=8388608 \
    -sWASM_BIGINT=1 \
    -sEXPORTED_FUNCTIONS=_hydra_pow_mod,_hydra_is_probable_prime,_hydra_next_prime,_hydra_gcd,_hydra_mod_inverse,_hydra_isqrt,_hydra_is_perfect_square \
    -sEXPORTED_RUNTIME_METHODS=HEAPU64,stackSave,stackRestore,stackAlloc \
    -o pkg/dist/hydra_core.mjs 2> >(grep -v 'limited binaryen' >&2 || true)

say "stripping DWARF (passes-free wasm-opt — do NOT let it optimize)…"
"$WASM_OPT" --all-features --strip-dwarf --strip-producers \
    pkg/dist/hydra_core.wasm -o pkg/dist/hydra_core.wasm

say "built: pkg/dist/hydra_core.mjs ($(du -h pkg/dist/hydra_core.mjs | cut -f1 | tr -d ' ')) + hydra_core.wasm ($(du -h pkg/dist/hydra_core.wasm | cut -f1 | tr -d ' '))"

if [[ "${1:-}" == "--test" ]]; then
    say "running pkg/test/test.mjs …"
    node pkg/test/test.mjs
fi
