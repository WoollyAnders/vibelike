#!/usr/bin/env bash
# build.sh [win-debug|win-release] [run]
# Cross-builds from WSL, deploys exe+data to the Windows temp dir, optionally launches.
set -euo pipefail
cd "$(dirname "$0")"

preset="${1:-win-debug}"
action="${2:-}"

cmake --preset "$preset"
cmake --build --preset "$preset"

deploy="/mnt/c/Users/andres.ruiz/AppData/Local/Temp/dustfall"
mkdir -p "$deploy"
cp "build/$preset/dustfall.exe" "$deploy/"
rm -rf "$deploy/data"
cp -r "build/$preset/data" "$deploy/data" 2>/dev/null || true

echo "Deployed to $deploy"

if [ "$action" = "run" ]; then
  (cd "$deploy" && ./dustfall.exe)
fi
