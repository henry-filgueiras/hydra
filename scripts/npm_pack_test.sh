#!/usr/bin/env bash
# scripts/npm_pack_test.sh — prove the *published tarball* works.
#
# npm pack → install into a fresh temp project outside the repo →
# init + known-answer operations.  Guards the P0 failure mode where
# source-tree tests pass but the tarball ships without
# dist/hydra_core.wasm (the glue locates the .wasm at runtime via
# import.meta.url, so a missing file only fails at the consumer).
#
# Prereq: pkg/dist built (scripts/wasm_pkg.sh).
set -euo pipefail
cd "$(dirname "$0")/.."

say()  { printf '\033[1;36m[pack-test]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[pack-test] FAIL:\033[0m %s\n' "$*" >&2; exit 1; }

[[ -f pkg/dist/hydra_core.wasm && -f pkg/dist/hydra_core.mjs ]] \
    || fail "pkg/dist not built — run scripts/wasm_pkg.sh first"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

say "packing…"
TARBALL="$(cd pkg && npm pack --pack-destination "$TMP" --silent)"

say "checking tarball manifest…"
# Capture the listing once and grep the variable.  Piping tar straight
# into `grep -q` is a trap under `set -o pipefail`: grep exits on the
# first match, tar can catch SIGPIPE writing the rest of the listing
# (exit 141), and the check fails even though the file IS in the
# tarball.  Bit us on CI 2026-07-11 — deterministically on ubuntu
# runners (GNU tar, mid-listing entry), never on macOS (bsdtar).
MANIFEST="$(tar -tzf "$TMP/$TARBALL")"
manifest_has() { grep -q "^package/$1\$" <<< "$MANIFEST"; }
for f in dist/hydra_core.wasm dist/hydra_core.mjs index.mjs index.d.ts; do
    manifest_has "${f//./\\.}" || {
        printf 'tarball manifest (node %s, npm %s):\n%s\n' \
            "$(node --version)" "$(npm --version)" "$MANIFEST" >&2
        fail "$f missing from tarball"
    }
done

say "installing into fresh project at $TMP …"
cd "$TMP"
npm init -y --silent >/dev/null
npm install --no-audit --no-fund --silent "./$TARBALL"

say "importing + known-answer operations…"
node --input-type=module -e '
import { init, powMod, modInverse, isProbablePrime } from "hydra-bignum";
await init();
const p = (1n << 127n) - 1n;                       // Mersenne prime M127
if (powMod(2n, p - 1n, p) !== 1n) throw new Error("powMod Fermat check failed");
if (modInverse(3n, 7n) !== 5n)    throw new Error("modInverse check failed");
if (!isProbablePrime(p))          throw new Error("isProbablePrime check failed");
console.log("tarball install OK: powMod/modInverse/isProbablePrime");
'
say "OK"
