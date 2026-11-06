# Building on Windows

This builds the player app: `smm2-dx-windows-x86_64.exe`, the runtime with its setup screen. Nothing from the game is in it.
On first launch the app asks the player for their own Super Mario Maker 2 v3.0.3 dump and `prod.keys`, recompiles
the game code on their machine, and runs it.

The build cross-compiles with [zig](https://ziglang.org) (`TARGET=windows-x86_64`), so it needs no Visual Studio and
no clang install. Everything is fetched by pinned version and sha256.

## What you need

Install [MSYS2](https://www.msys2.org) and work in its **UCRT64** shell. Then:

```bash
pacman -S --needed git make zip unzip curl python zstd
```

For the optional course server inside the app you also need Rust ([rustup](https://rustup.rs)). `core/rust-toolchain.toml`
pins the version. Without it the app still builds, just without the embedded server.

You do **not** need the game dump to build the app. The dump is only needed to run it.

## Build

From the repo root:

```bash
git clone https://github.com/sometimes-steve/smm2-dx
cd SMM2-DX/recomp

make TARGET=windows-x86_64 GAME=smm2 embed-server                     # optional: the course server (needs Rust)
make TARGET=windows-x86_64 GAME=smm2 -j all                           # fetches zig and the libraries first, then builds
```

The result is in the repo root `build/` folder: `smm2-dx-windows-x86_64.exe`, a small launcher, and one folder,
`smm2-dx_data/`. The launcher runs `smm2-dx_data/smm2-dx-runtime.exe`, which sits beside its DLLs (`SDL3.dll`,
`libiconv-2.dll`, `libshaderc_shared.dll`), Python, zig, hactool and the import library, and keeps its saves, settings and
logs there. The build's objects and the fetched compiler and dependencies are in `smm2-dx_data/work/`. Keep the exe and
its folder together.

## Package for players

```bash
make TARGET=windows-x86_64 GAME=smm2 package
```

This writes `dist/SMM2Recomp-windows-x86_64.zip`: the launcher exe and `smm2-dx_data/` (the runtime, SDL3 / shaderc DLLs, a
private Python with the recompiler packages, zig, hactool, licenses) and a README. `scripts/check_package.py` fails the build if any game content slipped in.

## Running

Unzip, keep the exe and `smm2-dx_data/` together, run `smm2-dx-windows-x86_64.exe`, and follow the setup screen. You need:

1. Super Mario Maker 2 with the 3.0.3 update (NSP / XCI, or extracted ExeFS and RomFS folders).
2. Your own `prod.keys` and, for the system fonts, the console firmware.
3. A Vulkan-capable graphics driver.

To redo setup later, start with `--setup` or use Quit > Quit to Start Screen in the overlay menu.

## Building from a dump without the launcher

`make smm2` runs the whole pipeline from a dump you extracted yourself (`setup`, `gen`, then `all`). It uses the
paths in `games/smm2/game.mk` (`DUMP_ROOT`, default `~/smm-work/extracted`). Override them on the command line, e.g.
`make smm2 DUMP_ROOT=/c/path/to/dump`. This needs a native MSYS2 UCRT64 toolchain (clang, lld, SDL3, Vulkan, shaderc,
FFmpeg) and `make venv` for the Python packages.

Never commit `private/`, `generated/` or `build/`. They are ignored by git and checked by `scripts/check_no_proprietary.py`.
