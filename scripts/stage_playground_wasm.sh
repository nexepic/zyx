#!/usr/bin/env bash
# Copy the matching WASM engine files into the Playground's public assets.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build_wasm}"
destination="$root/docs/apps/docs/public/wasm"
assets=(zyx.js zyx-filesystem.mjs zyx.wasm)

for asset in "${assets[@]}"; do
    if [[ ! -s "$build/$asset" ]]; then
        printf 'Missing WASM asset: %s\nRun bash scripts/build_wasm.sh first.\n' "$build/$asset" >&2
        exit 1
    fi
done

mkdir -p -- "$destination"
for asset in "${assets[@]}"; do
    install -m 0644 -- "$build/$asset" "$destination/$asset"
done
