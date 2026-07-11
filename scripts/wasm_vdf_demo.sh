#!/usr/bin/env bash
#
# scripts/wasm_vdf_demo.sh — assemble the Wesolowski VDF flagship demo
# (ROADMAP A2).
#
# The demo is plain ES modules on top of the hydra-bignum npm package
# (pkg/) — there is nothing to compile here beyond the package's wasm
# core, which scripts/wasm_pkg.sh builds (LLVM-only pipeline; see that
# script for why emcc's default binaryen pass is avoided).  This script
# just lays out a servable tree:
#
#   build-wasm/demo/vdf/
#     index.html vdf.mjs            (from demo/vdf/)
#     pkg/index.mjs pkg/dist/…      (the hydra-bignum package)
#
# It nests under build-wasm/demo so the GitHub Pages deploy of the
# benchmark demo picks it up at /vdf/ with no extra plumbing.
#
# Usage:
#   scripts/wasm_vdf_demo.sh          # assemble (builds pkg/dist if missing)
#   scripts/wasm_vdf_demo.sh --serve  # assemble + python http server on :8124
#
# NOTE: file:// will NOT work — ES module imports need http(s).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

OUT="$HERE/build-wasm/demo/vdf"

say()  { printf '\033[1;36m[vdf-demo]\033[0m %s\n' "$*"; }

if [[ ! -f pkg/dist/hydra_core.wasm || ! -f pkg/dist/hydra_core.mjs ]]; then
    say "pkg/dist missing — building the hydra-bignum wasm core first…"
    scripts/wasm_pkg.sh
fi

mkdir -p "$OUT/pkg/dist"
cp demo/vdf/index.html demo/vdf/vdf.mjs "$OUT/"
cp pkg/index.mjs "$OUT/pkg/"
cp pkg/dist/hydra_core.mjs pkg/dist/hydra_core.wasm "$OUT/pkg/dist/"

say "assembled: $OUT ($(du -sh "$OUT" | cut -f1 | tr -d ' '))"

if [[ "${1:-}" == "--serve" ]]; then
    say "serving on http://localhost:8124/vdf/ (Ctrl-C to stop)"
    exec python3 -m http.server 8124 -d "$HERE/build-wasm/demo"
else
    say "serve with: scripts/wasm_vdf_demo.sh --serve  (module imports need http, not file://)"
fi
