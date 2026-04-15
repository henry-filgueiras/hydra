set -x trace

if ! DevToolsSecurity -status 2>/dev/null | grep -q "enabled"; then
    echo "Enabling developer mode..."
    sudo DevToolsSecurity -enable
fi

codesign -s - -f --entitlements scripts/debug.entitlements build-rel/hydra_bench
