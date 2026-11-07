# Building on Windows

This builds the player app: `smm2-dx-windows-x86_64.exe`, the runtime with its setup screen. Nothing from the game is in it.
On first launch the app asks the player for their own Super Mario Maker 2 v3.0.3 dump and `prod.keys`, recompiles
the game code on their machine, and runs it.

The build uses [zig](https://ziglang.org) (no Visual Studio, no clang install). Everything is fetched by pinned version
and sha256.

## What you need

Windows 10 or 11 (the `curl` and `tar` that come with Windows). No MSYS2, no Visual Studio, no compiler.

You do **not** need the game dump to build the app. The dump is only needed to run it.

## Build

From the repo root, run the Build Helper:

```
git clone https://github.com/sometimes-steve/smm2-dx
cd smm2-dx
build-helper.cmd
```

It fetches zig and the libraries first, then builds. The result is in the repo root `dist\` folder:
`smm2-dx-windows-x86_64.exe` and one folder, `smm2-dx_data\`. The exe runs `smm2-dx_data\smm2-dx-runtime.exe`, which sits
beside its DLLs (`SDL3.dll`, `libiconv-2.dll`, `libshaderc_shared.dll`), Python, zig, hactool and the import library, and
keeps its saves, settings and logs there. The build's objects and the fetched compiler and dependencies are in
`smm2-dx_data\work\`. Keep the exe and its folder together. The README lists the Build Helper's options and where each
fetched piece comes from.

## Running

Unzip, keep the exe and `smm2-dx_data/` together, run `smm2-dx-windows-x86_64.exe`, and follow the setup screen. You need:

1. Super Mario Maker 2 with the 3.0.3 update (NSP / XCI, or extracted ExeFS and RomFS folders).
2. Your own `prod.keys` and, for the system fonts, the console firmware.
3. A Vulkan-capable graphics driver.

To redo setup later, start with `--setup` or use Quit > Quit to Start Screen in the overlay menu.

Never commit `private/`, `generated/` or `build/`. They are ignored by git.
