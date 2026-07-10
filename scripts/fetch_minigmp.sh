#!/usr/bin/env bash
#
# scripts/fetch_minigmp.sh — fetch + verify mini-gmp into build-wasm/minigmp.
#
# Shared by scripts/wasm_bench.sh, scripts/wasm_demo.sh, and the wasm CI
# job.  Portable across macOS (shasum) and Linux (sha256sum).  mini-gmp
# (LGPLv3+/GPLv2+ dual) is downloaded at build time into a gitignored
# tree and used only as a benchmark comparator — never vendored into
# this MIT repo, never linked into the Hydra library.
#
# Usage:  scripts/fetch_minigmp.sh
# Output: build-wasm/minigmp/{mini-gmp.c, mini-gmp.h, gmp.h}
#         (gmp.h is a shim so code that includes <gmp.h> compiles
#          against mini-gmp's mpz_* subset unchanged)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

WASM_DIR="$HERE/build-wasm"
GMP_VER="6.3.0"
GMP_TARBALL="gmp-$GMP_VER.tar.xz"
GMP_URL="https://ftp.gnu.org/gnu/gmp/$GMP_TARBALL"
GMP_SHA256="a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898"
MINIGMP_DIR="$WASM_DIR/minigmp"

say()  { printf '\033[1;36m[fetch-minigmp]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[fetch-minigmp] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

if [[ -f "$MINIGMP_DIR/mini-gmp.c" && -f "$MINIGMP_DIR/gmp.h" ]]; then
    say "already present in $MINIGMP_DIR (cached)"
    exit 0
fi

mkdir -p "$MINIGMP_DIR"

say "downloading $GMP_TARBALL from ftp.gnu.org (mini-gmp is bundled inside)…"
curl -sL --max-time 120 -o "$WASM_DIR/$GMP_TARBALL" "$GMP_URL"

if command -v shasum >/dev/null 2>&1; then
    echo "$GMP_SHA256  $WASM_DIR/$GMP_TARBALL" | shasum -a 256 -c - >/dev/null \
        || fail "checksum mismatch on $GMP_TARBALL"
elif command -v sha256sum >/dev/null 2>&1; then
    echo "$GMP_SHA256  $WASM_DIR/$GMP_TARBALL" | sha256sum -c - >/dev/null \
        || fail "checksum mismatch on $GMP_TARBALL"
else
    fail "neither shasum nor sha256sum available"
fi

tar -xf "$WASM_DIR/$GMP_TARBALL" -C "$WASM_DIR" \
    "gmp-$GMP_VER/mini-gmp/mini-gmp.c" "gmp-$GMP_VER/mini-gmp/mini-gmp.h"
cp "$WASM_DIR/gmp-$GMP_VER/mini-gmp/mini-gmp.c" \
   "$WASM_DIR/gmp-$GMP_VER/mini-gmp/mini-gmp.h" "$MINIGMP_DIR/"
printf '// gmp.h shim: route the GMP backend through mini-gmp.\n#include "mini-gmp.h"\n' \
    > "$MINIGMP_DIR/gmp.h"

say "mini-gmp $GMP_VER ready (checksum verified) → $MINIGMP_DIR"
