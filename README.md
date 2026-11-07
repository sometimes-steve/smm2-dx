# smm2-dx

Super Mario Maker 2 recompiled for Windows, built from your own dump. Nothing from the game is in this repo.

- `recomp/`: the player app (runtime, setup screen, recompiler). This is what produces `smm2-dx-windows-x86_64.exe`.

## Download

Prebuilt zips (the exe plus its `smm2-dx_data` folder) are on the [Releases](../../releases) page. Unzip, run `smm2-dx-windows-x86_64.exe`, and follow the setup screen.
You need all of these, from your own console:

- Super Mario Maker 2 with the v3.0.3 update (the decrypted, extracted ExeFS and RomFS folders)
- `prod.keys`
- The console firmware (the system NCAs). The setup screen copies the console's button-glyph font from it, and the game crashes without that one font. (The text font is [BIZ UDPGothic](https://github.com/googlefonts/morisawa-biz-ud-gothic), SIL Open Font License 1.1, which comes with the program.)
- An [OpenCourse World](https://opencourse.world) account. Export your `ocw-config.json` (your server domain and personal token) and choose it on the setup screen. It is read where it is and never copied, and the token is never shown or written to a log. Course World and every other online feature of the game go through OpenCourse World, and it is the only online service this app talks to.
- A Vulkan-capable graphics driver

## Building on Windows

The **Build Helper** builds everything on Windows with nothing to install first: no MSYS2, no Visual Studio, no
compiler. It needs only Windows 10 or 11 (the `curl` and `tar` that come with Windows).

```
cmd /c "git clone https://github.com/sometimes-steve/smm2-dx smm2-dx && cd smm2-dx && build-helper.cmd"
```

That one line works from cmd or PowerShell and clones into `smm2-dx\` in the current folder. To rebuild later, run `build-helper.cmd` inside it, with options such as `--clean` after it. It fetches what it needs, builds, and
leaves the finished app in `dist\`: `smm2-dx-windows-x86_64.exe` and one folder, `smm2-dx_data\`, which holds the runtime, its
DLLs, Python, zig, hactool, your saves and settings, and `work\` (the build's intermediates and fetched tools). Keep the
exe and its folder together. No game dump is needed to build.

| Option | What it does |
|---|---|
| `--run` | starts the app when the build is done |
| `--no-banner` | prints a one-line header instead of the title banner |
| `--clean` | deletes the compiled objects first (the fetched tools are kept) |
| `--jobs n` | files compiled at once (default: one per logical processor) |

### Where everything comes from

Nothing below is stored in this repo. **Everything can be pulled by the Build Helper, or downloaded by hand.** The Build
Helper downloads each piece from the source below and checks it against a pinned sha256 (listed in
`recomp/tools/build.py`; the Python one also in `build-helper.cmd`). A piece that is missing, half fetched or broken is fetched again on the next run.

To download a piece yourself instead, save the file under the name shown into `dist\smm2-dx_data\work\dl\` (in the
folder shown). If the file's sha256 matches, the Build Helper uses it and doesn't download anything.

| Piece | Version | Download from | Save as, in `work\dl\` |
|---|---|---|---|
| zig compiler (builds the exe, and compiles the game code on the player's machine) | 0.17.0 | [ziglang.org/download/0.17.0](https://ziglang.org/download/0.17.0/zig-x86_64-windows-0.17.0.zip) | `zig-dl\zig-x86_64-windows-0.17.0.zip` |
| SDL3, libiconv (SDL3's dependency), shaderc | 3.4.18, 1.19, 2026.4 | [repo.msys2.org/mingw/ucrt64](https://repo.msys2.org/mingw/ucrt64/) | `msys2\mingw-w64-ucrt-x86_64-sdl3-3.4.18-1-any.pkg.tar.zst`, `...-libiconv-1.19-1-any.pkg.tar.zst`, `...-shaderc-2026.4-1-any.pkg.tar.zst` |
| Vulkan headers, Vulkan loader, Vulkan Memory Allocator (build time only) | 1.4.363, 1.4.363, 3.4.0 | [repo.msys2.org/mingw/ucrt64](https://repo.msys2.org/mingw/ucrt64/) | `msys2\mingw-w64-ucrt-x86_64-vulkan-headers-1~1.4.363.0-1-any.pkg.tar.zst`, `...-vulkan-loader-1~1.4.363.0-1-any.pkg.tar.zst`, `...-vulkan-memory-allocator-3.4.0-1-any.pkg.tar.zst` |
| Python (runs the recompiler and the setup scripts) | 3.12.14 | [python-build-standalone 20260924](https://github.com/astral-sh/python-build-standalone/releases/tag/20260924) | `cpython-3.12.14+20260924-x86_64-pc-windows-msvc-install_only_stripped.tar.gz` |
| capstone, unicorn, lz4 (the recompiler's Python packages) | 5.0.9, 2.1.4, 4.4.5 | PyPI, as pinned by hash in `recomp/tools/requirements-gen.txt` | fetched with pip by the Build Helper |
| hactool (unpacks the firmware; built from source with zig) | 1.4.0 | [github.com/SciresM/hactool](https://github.com/SciresM/hactool/archive/refs/tags/1.4.0.tar.gz) | `hactool-1.4.0.tar.gz` |

#### Pinned sha256

A file is used only if its sha256 matches. Check one by hand with `certutil -hashfile <file> SHA256`. The same values are
in `recomp/tools/build.py` (the Python one also in `build-helper.cmd`).

| File | sha256 |
|---|---|
| `zig-x86_64-windows-0.17.0.zip` | `b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428` |
| `mingw-w64-ucrt-x86_64-sdl3-3.4.18-1-any.pkg.tar.zst` | `f9f8e2fe9858868e7653b24c973bcc8c19dea1096afc8c905bf1cd1a87960442` |
| `mingw-w64-ucrt-x86_64-libiconv-1.19-1-any.pkg.tar.zst` | `9a500f38c2b91808741c62fae746b3e9110b33a1ecf5c30fa0c66dbedddf7e16` |
| `mingw-w64-ucrt-x86_64-shaderc-2026.4-1-any.pkg.tar.zst` | `a4ad271f76acd2307ed67738770b30d6486bd3c7acff63ba83dd13e56ce9ab82` |
| `mingw-w64-ucrt-x86_64-vulkan-headers-1~1.4.363.0-1-any.pkg.tar.zst` | `be602b2657cace531316e78b1eda4087e8f7f84ebb877f08fba198a2db06406a` |
| `mingw-w64-ucrt-x86_64-vulkan-loader-1~1.4.363.0-1-any.pkg.tar.zst` | `065076fe13d39ab97eea4740596426d5ae741b5044e48e1c9495d04835c92dbe` |
| `mingw-w64-ucrt-x86_64-vulkan-memory-allocator-3.4.0-1-any.pkg.tar.zst` | `44c81162337915afecda3185f66fd98268a54454580b2717080c9dfef57fe781` |
| `cpython-3.12.14+20260924-x86_64-pc-windows-msvc-install_only_stripped.tar.gz` | `c5bf8edfe858c1df9891be498b5bbc8761d383df5b9790658b088fea4870433a` |
| `hactool-1.4.0.tar.gz` | `1373c98ef8c58da42cb1e81b361283955caad0e1b8b5ad389b0ee49b7a21da43` |

The Python packages (capstone, unicorn, lz4) are checked by pip against the hashes in `recomp/tools/requirements-gen.txt`.

Then `recomp/tools/build.py` copies the recompiler scripts, the headers the game code is built against, and the
licenses into `smm2-dx_data`, so the app finds everything beside itself.

The game files, keys and firmware are never downloaded. You supply those through the setup screen.

Logs are written to `smm2-dx_log\`, next to the exe.
