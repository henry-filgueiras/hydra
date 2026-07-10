#!/usr/bin/env bash
#
# scripts/wasm_bootstrap.sh — install + verify the WebAssembly toolchain
# for the Phase 3 wasm target (DIRECTORS_NOTES.md, "Phase 3 Roadmap"
# item 1).  macOS + Homebrew only, by design.
#
# Usage:
#   scripts/wasm_bootstrap.sh          # install missing tools, run smoke test
#   scripts/wasm_bootstrap.sh --full   # also compile hydra_test to wasm and
#                                      # run the full 989-test suite under node
#   scripts/wasm_bootstrap.sh --check  # report tool status only, install nothing
#
# What it installs (idempotent — present tools are left alone):
#   emscripten   emcc/em++ C++ → wasm compiler (bundles node + binaryen)
#   wasmtime     standalone WASI runtime (optional runner, used if present)
#
# What it verifies:
#   1. emcc can compile C++20.
#   2. `unsigned __int128` lowers correctly under wasm — the one
#      codegen assumption Hydra's kernels rely on (see the roadmap
#      note: if this were broken, a 32-bit-limb build profile would
#      be the fallback).  The smoke test multiplies two max-u64
#      values and checks both 128-bit halves.
#
# Output tree (gitignore-d via the build-*/ pattern):
#   build-wasm/    smoke test artifacts, and hydra_test.{js,wasm} with --full

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

WASM_DIR="$HERE/build-wasm"
MODE="${1:-install}"

say()  { printf '\033[1;36m[wasm-bootstrap]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[wasm-bootstrap] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ── Preconditions ────────────────────────────────────────────
[[ "$(uname -s)" == "Darwin" ]] || fail "this script is macOS-only (see header)"
command -v brew >/dev/null 2>&1 || fail "Homebrew not found — install from https://brew.sh first"

# ── Tool inventory / install ─────────────────────────────────
# ensure_tool <command> <brew formula>
ensure_tool() {
    local cmd="$1" formula="$2"
    if command -v "$cmd" >/dev/null 2>&1; then
        say "$cmd already present: $("$cmd" --version 2>/dev/null | head -1)"
        return 0
    fi
    if [[ "$MODE" == "--check" ]]; then
        say "$cmd MISSING (would run: brew install $formula)"
        return 1
    fi
    say "$cmd not found — brew install $formula (this can take a few minutes)…"
    brew install "$formula"
    command -v "$cmd" >/dev/null 2>&1 \
        || fail "brew install $formula finished but $cmd still not on PATH — open a new shell or check 'brew doctor'"
    say "$cmd installed: $("$cmd" --version 2>/dev/null | head -1)"
}

MISSING=0
ensure_tool emcc     emscripten || MISSING=1
ensure_tool wasmtime wasmtime   || MISSING=1   # optional runner; emcc output runs under node

if [[ "$MODE" == "--check" ]]; then
    [[ "$MISSING" == 0 ]] && say "all tools present" || say "some tools missing — rerun without --check to install"
    exit "$MISSING"
fi

# node ships inside brew's emscripten dependency tree; emcc finds it via
# its own config, but the --full test invokes it directly, so resolve it.
NODE_BIN="$(command -v node || true)"
[[ -n "$NODE_BIN" ]] || fail "node not found even after emscripten install — 'brew install node'"

# ── Smoke test: C++20 + __int128 lowering under wasm ─────────
mkdir -p "$WASM_DIR"
SMOKE_SRC="$WASM_DIR/int128_smoke.cpp"
SMOKE_JS="$WASM_DIR/int128_smoke.js"

cat > "$SMOKE_SRC" <<'EOF'
// Verifies the codegen assumption Hydra's kernels are built on:
// unsigned __int128 multiply/add/shift must lower correctly to wasm.
#include <cstdio>
#include <cstdint>
int main() {
    const uint64_t m = 0xFFFFFFFFFFFFFFFFull;
    unsigned __int128 p = (unsigned __int128)m * m;      // max 64x64 product
    p += (unsigned __int128)m << 1;                      // carry through bit 128? no — stays in range
    // (2^64-1)^2 + 2(2^64-1) = 2^128 - 1  → hi = 2^64-1, lo = 2^64-1
    std::printf("%016llx %016llx\n",
                (unsigned long long)(p >> 64), (unsigned long long)p);
    return 0;
}
EOF

say "compiling __int128 smoke test with emcc (-std=c++20 -O2)…"
emcc -std=c++20 -O2 "$SMOKE_SRC" -o "$SMOKE_JS"

GOT="$("$NODE_BIN" "$SMOKE_JS")"
WANT="ffffffffffffffff ffffffffffffffff"
[[ "$GOT" == "$WANT" ]] \
    || fail "__int128 smoke test produced '$GOT', expected '$WANT' — wasm lowering is broken; the 32-bit-limb fallback profile is needed (see roadmap)"
say "__int128 lowering verified: $GOT"

# ── Optional: full hydra_test suite under wasm ───────────────
if [[ "$MODE" == "--full" ]]; then
    say "compiling hydra_test to wasm (this is the real portability gate)…"
    # Same TU set as the native hydra_test target (hydra.cpp is the
    # header-only stub TU).  Generous stack: divmod/pow_mod keep
    # multi-KB scratch on the stack by design.  -fwasm-exceptions:
    # emcc aborts on `throw` by default, and both the library
    # (domain_error on div-by-zero etc.) and the tests rely on
    # exceptions; native wasm EH needs node >= 17 (v8 flag on by
    # default there).
    emcc -std=c++20 -O2 -DNDEBUG -I. \
        -fwasm-exceptions \
        hydra_test.cpp hydra.cpp \
        -sALLOW_MEMORY_GROWTH=1 \
        -sSTACK_SIZE=8388608 \
        -o "$WASM_DIR/hydra_test.js"
    say "running the full test suite under node…"
    "$NODE_BIN" "$WASM_DIR/hydra_test.js"
    say "wasm test run complete — see pass/fail line above"
fi

say "done.  Next steps for the wasm sprint (roadmap item 1):"
say "  1. scripts/wasm_bootstrap.sh --full     # full test suite under node"
say "  2. bench_pow_mod compiled the same way, benched vs mini-gmp-in-wasm"
say "  3. CI job (macos or linux runner) wiring these two steps"
