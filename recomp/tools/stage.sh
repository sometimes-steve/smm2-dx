#!/bin/bash
# usage: tools/stage.sh [target] [out dir]   (make all does this; defaults: windows-x86_64, ../build)
# Puts what the exe looks for into <out dir>/smm2-dx_data, the same layout as a player package: python/, tools/, gamelib-src/,
# zig/, hactool/, licenses/ and README.txt (tools/package_lib.sh). Nothing from the game. Fetches Python and zig and
# builds hactool the first time (into ../build/work).
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/package_lib.sh
TARGET=${1:-windows-x86_64}
OUT=${2:-../build}
[ "$TARGET" = windows-x86_64 ] || { echo "stage.sh: only windows-x86_64 is supported"; exit 1; }
S=$OUT/smm2-dx_data
mkdir -p "$S"
rm -rf "$S/python" "$S/zig" "$S/hactool" "$S/tools" "$S/gamelib-src" "$S/licenses"
stage_common "$S" "$TARGET" x86_64-windows
write_readme "$S/README.txt" "$TARGET" "${RECOMP_VERSION:-dev}"
echo "$S: staged ($(du -sh "$S" | cut -f1))"
