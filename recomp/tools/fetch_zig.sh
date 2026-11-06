#!/bin/bash
# The compiler the start screen builds the game code with on the player's machine (runtime/gamebuild.c): zig (MIT,
# its clang / lld: Apache-2.0 with LLVM exception), as `zig cc`. Fetches the pinned release for this OS / CPU (or
# ZIG_TARGET=<arch>-<os>, e.g. x86_64-windows) into ../build/smm2-dx_data/work/zig/ and checks its hash. ../build/smm2-dx_data/work/zig/ is the build's own
# compiler and stays whole: it cross-builds the runtime for every target (make TARGET=...).
# ZIG_OUT=<dir>: a copy to ship instead (tools/package_lib.sh), cut down to what compiling C for that same machine
# needs. Shipped as zig/ next to the exe.
set -euo pipefail
cd "$(dirname "$0")/.."
VER=0.17.0
OS=windows
ARCH=x86_64
TARGET=${ZIG_TARGET:-$ARCH-$OS}
EXT=tar.xz; [ "${TARGET#*-}" = windows ] && EXT=zip
NAME=zig-$TARGET-$VER
# sha256 of each release archive (ziglang.org/download/index.json; checked against it when pinned)
case "$TARGET" in
    x86_64-windows) SHA=b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428 ;;
    *) echo "no zig for $TARGET"; exit 1;;
esac
[ -n "$SHA" ] || { echo "tools/fetch_zig.sh: pin the sha256 of $NAME.$EXT first"; exit 1; }

DL=../build/smm2-dx_data/work/zig-dl
mkdir -p "$DL"
[ -f "$DL/$NAME.$EXT" ] || curl -fL --proto '=https' -o "$DL/$NAME.$EXT" "https://ziglang.org/download/$VER/$NAME.$EXT"
GOT=$( (shasum -a 256 2>/dev/null || sha256sum) < "$DL/$NAME.$EXT" | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || { echo "$NAME.$EXT: sha256 $GOT, expected $SHA"; exit 1; }
OUT=${ZIG_OUT:-../build/smm2-dx_data/work/zig}
rm -rf "$DL/$NAME" "$OUT"
if [ "$EXT" = zip ]; then unzip -q "$DL/$NAME.$EXT" -d "$DL"; else tar -xJf "$DL/$NAME.$EXT" -C "$DL"; fi
mkdir -p "$(dirname "$OUT")"
mv "$DL/$NAME" "$OUT"

# a shipped copy: only what compiling C for this same OS needs (the game code is built for the machine it runs on):
# its libc headers and libc sources; no C++ runtimes (libcxx, libcxxabi) or sanitizers
[ -n "${ZIG_OUT:-}" ] || { echo "$OUT: zig $VER for $TARGET, $(du -sh "$OUT" | cut -f1)"; exit 0; }
L=$OUT/lib
case "${TARGET#*-}" in
    windows) inc='windows|mingw'; src='mingw' ;;
esac
for d in "$L"/libc/include/*; do basename "$d" | grep -qE "$inc" || rm -rf "$d"; done
for d in "$L"/libc/*; do n=$(basename "$d"); [ "$n" = include ] || echo "$n" | grep -qE "^($src)$" || rm -rf "$d"; done
rm -rf "$L/libcxx" "$L/libcxxabi" "$L/libtsan" "$L/docs" "$OUT/doc"  # (lib/std stays: zig cc builds compiler_rt with it)
echo "$OUT: zig $VER for $TARGET, $(du -sh "$OUT" | cut -f1)"
