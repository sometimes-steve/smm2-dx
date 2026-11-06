# Sourced by tools/package.sh: what every player package holds
# besides the runtime itself, in <dest> (the package folder). Nothing from the game:
# the setup screen (runtime/ui/launcher.cpp) makes everything game-specific on the player's machine from their own dump.
#   python/        a Python 3.12 of its own, with capstone, unicorn and lz4 (tools/fetch_python.sh)
#   tools/         the recompiler (gen.py and its modules) and the game's setup scripts (setup.py, sysdata.py,
#                  versions.py: the known v3.0.3 build ids and hashes)
#   gamelib-src/   the headers the game code is compiled against, its entry file, the HLE list, the patches
#   zig/           the compiler the game code is built with (tools/fetch_zig.sh)
#   hactool/       hactool, for NSP / XCI and the firmware (tools/build_hactool.sh; ISC)
#   licenses/      third-party licenses; THIRD-PARTY.txt says which is which
# usage: stage_common <dest> <target> <zig target>   (target: windows-x86_64)
stage_common() {
    local dest=$1 target=$2 zt=$3
    mkdir -p "$dest/tools/games/smm2" "$dest/gamelib-src/mods" "$dest/licenses"
    # Python
    # fetched again when missing, when a fetch stopped half way (README-LICENSES.txt is written last), or when the
    # packages the generator imports don't load from it (a damaged or half-trimmed copy)
    local py="../build/smm2-dx_data/work/python-$target"
    if [ ! -f "$py/README-LICENSES.txt" ] || ! "$py/python.exe" -c 'import threading, subprocess, json, hashlib, capstone, unicorn, lz4.frame' >/dev/null 2>&1; then
        rm -rf "$py"
        tools/fetch_python.sh "$target"
    fi
    cp -R "$py" "$dest/python"
    find "$dest/python" -name '__pycache__' -type d -prune -exec rm -rf {} +
    # the recompiler and the setup scripts
    cp tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py tools/simd_c.py "$dest/tools/"
    cp games/smm2/setup.py games/smm2/sysdata.py games/smm2/mii_stub.py games/smm2/mini_head.json games/smm2/BIZUDPGothic-Regular.ttf games/smm2/BIZUDPGothic-OFL.txt games/smm2/versions.py "$dest/tools/games/smm2/"
    # what the game code is compiled against (the same files GAMELIB_ID hashes)
    cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h runtime/game.h tools/gamelib_entry.c "$dest/gamelib-src/"
    cp games/smm2/hle_symbols.txt "$dest/gamelib-src/hle_symbols.txt"
    cp games/smm2/patches.py "$dest/gamelib-src/patches.py"
    # every mod patch file games/smm2/patches.py loads (patches.py): the same files GAMELIB_ID hashes
    cp games/smm2/mods/*.py "$dest/gamelib-src/mods/"
    # the compiler
    [ -d "../build/smm2-dx_data/work/zig-$zt" ] || ZIG_TARGET=$zt ZIG_OUT="../build/smm2-dx_data/work/zig-$zt" tools/fetch_zig.sh
    cp -R "../build/smm2-dx_data/work/zig-$zt" "$dest/zig"
    # hactool
    [ -x "../build/smm2-dx_data/work/hactool-$target/hactool" ] || [ -f "../build/smm2-dx_data/work/hactool-$target/hactool.exe" ] || tools/build_hactool.sh "$target"
    cp -R "../build/smm2-dx_data/work/hactool-$target" "$dest/hactool"
    # licenses
    cp LICENSE-LASRecomp "$dest/licenses/LICENSE-LASRecomp.txt"
    cp third_party/imgui/LICENSE.txt "$dest/licenses/LICENSE-imgui.txt"
    cp third_party/smaa/LICENSE.txt "$dest/licenses/LICENSE-smaa.txt" 2>/dev/null || true
    cp third_party/zstd/LICENSE "$dest/licenses/LICENSE-zstd.txt"
    cp third_party/bcdec/LICENSE "$dest/licenses/LICENSE-bcdec.txt"
    cp "$dest/hactool/mbedtls-LICENSE.txt" "$dest/licenses/Apache-2.0.txt"
    {
        echo "Third-party components in this package and their licenses (the project's own code: see the source repository)"
        echo
        echo "- LAS Recomp (the recompiler and runtime this is forked from): MIT, licenses/LICENSE-LASRecomp.txt"
        echo "- Dear ImGui (the setup screen and the overlay menu): MIT, licenses/LICENSE-imgui.txt"
        echo "- SMAA (built in): MIT-style, licenses/LICENSE-smaa.txt"
        echo "- Mbed TLS 3.6 (built in: the game's TLS client): Apache-2.0, licenses/Apache-2.0.txt"
        echo "- Zstandard decoder (built in: the shader cache): BSD-3-Clause, licenses/LICENSE-zstd.txt"
        echo "- bcdec (built in: BC texture decoding where the GPU lacks it): MIT / Unlicense, licenses/LICENSE-bcdec.txt"
        echo "- zig (zig/, the compiler the game code is built with): MIT, zig/LICENSE; its clang / lld: Apache-2.0 with"
        echo "  LLVM exception (licenses/Apache-2.0.txt)"
        echo "- Python 3.12 (python/, python-build-standalone): PSF License and its components' own: python/README-LICENSES.txt"
        echo "- capstone (BSD-3-Clause), unicorn (GPL-2.0; source: https://github.com/unicorn-engine/unicorn) and lz4"
        echo "  (BSD-3-Clause), the game code generator's Python packages: python/.../site-packages/*.dist-info"
        echo "- hactool (hactool/): ISC, hactool/LICENSE.txt; its mbedtls: Apache-2.0, hactool/mbedtls-LICENSE.txt"
        case "$target" in
        windows-*)
            echo "- SDL3 (SDL3.dll): zlib license, https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt"
            echo "- shaderc, glslang, SPIRV-Tools (libshaderc_shared.dll): Apache-2.0 / BSD-3-Clause, licenses/Apache-2.0.txt"
            echo "- libiconv (libiconv-2.dll, used by SDL3): LGPL-2.1, an unmodified MSYS2 build (source: https://www.gnu.org/software/libiconv/)" ;;
        esac
    } > "$dest/THIRD-PARTY.txt"
}

# The player's README.txt. usage: write_readme <file> <target> <version>
write_readme() {
    local f=$1 target=$2 ver=$3 run data
    case "$target" in
        windows-*) run='smm2-dx-windows-x86_64.exe' data='this folder (keep it somewhere you can write to, e.g. Documents - not Program Files)' ;;
    esac
    cat > "$f" <<EOF
Super Mario Maker 2 (recompiled) $ver ($target)

Super Mario Maker 2 v3.0.3, recompiled for your computer from YOUR OWN game. Nothing of the game is in this
download: you bring your dump, and the setup screen prepares everything from it, on your machine.

1. Start $run.
2. The setup screen asks for:
   - your Super Mario Maker 2 with the 3.0.3 update: the game's NSP or XCI plus the update's NSP, or folders with
     their extracted ExeFS and RomFS (e.g. an emulator's "Extract RomFS / ExeFS" of the updated game);
   - your console's prod.keys and firmware (the folder of its system .nca files, e.g. Ryujinx's
     bis/system/Contents/registered): the game's fonts come from there. They are read where they are, never copied;
     key values are never shown or logged;
3. Press "Set up and play". The first time it checks your files against the known 3.0.3 version, extracts them,
   copies the fonts and recompiles the game code (a few minutes to half an hour), then the game starts.
   Later starts go straight to the game. The overlay menu (F1 / Esc) > Mods > Game files brings the setup back.

Your saves, settings, the set-up game files and the logs are in $data.

Keys: WASD stick, arrows d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter = +, Backspace = -, F1 menu.
Controllers work through SDL.
EOF
    case "$target" in
        windows-*) cat >> "$f" <<EOF

Windows 10 / 11, 64-bit, a CPU with x86-64-v2 (AVX2 used when available) and a graphics driver with Vulkan 1.3.
EOF
        ;;
    esac
    cat >> "$f" <<EOF

Help, status and the source: https://github.com/smm2recomp/smm2recomp - the setup screen tells you when a new
release is out (it only links to it; nothing is downloaded by itself).
EOF
    case "$target" in windows-*) sed -i.bak 's/$/\r/' "$f" && rm -f "$f.bak" ;; esac
}
