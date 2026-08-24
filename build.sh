#!/usr/bin/env bash
# Configure + build (+ install) the CopyToPoints plugins on Linux / macOS.
#   ./build.sh                                # classic (Nuke 14) using $NUKE_DIR or the default below
#   ./build.sh --usd /usr/local/Nuke17.0v3    # CopyToPointsUSD against a Nuke 15+ install
#   ./build.sh --install                      # + cmake --install into ~/.nuke/CopyToPoints
# The Nuke install must contain <install>/cmake/NukeConfig.cmake.  Use the compiler
# Foundry documents for that Nuke version (gcc 9 for Nuke 14, gcc 11 for 15-17 on
# Linux; Xcode clang on macOS).  Not tested by the author on these platforms - the
# platform-specific code is only the viewer brush's mouse polling (X11 / CoreGraphics).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
nuke="${NUKE_DIR:-/usr/local/Nuke14.1v8}"
usd=0; install=0
while [ $# -gt 0 ]; do
  case "$1" in
    --usd) usd=1; nuke="$2"; shift 2 ;;
    --install) install=1; shift ;;
    --nuke) nuke="$2"; shift 2 ;;
    *) echo "unknown arg $1"; exit 1 ;;
  esac
done
major="$(basename "$nuke" | sed -E 's/^Nuke([0-9]+).*/\1/')"
if [ "$usd" = 1 ]; then bd="$here/build-usd$major"; opts="-DBUILD_CLASSIC=OFF -DBUILD_USD=ON"; else bd="$here/build"; opts="-DBUILD_CLASSIC=ON -DBUILD_USD=OFF"; fi
cmake -S "$here" -B "$bd" -DCMAKE_BUILD_TYPE=Release -DNuke_DIR="$nuke/cmake" $opts
cmake --build "$bd" --config Release -j
if [ "$install" = 1 ]; then cmake --install "$bd" --config Release --prefix "$HOME/.nuke"; fi
echo "done: $bd"
