set -x trace

sudo DevToolsSecurity -enable

codesign -s - -f --entitlements scripts/debug.entitlements build/hydra_bench
