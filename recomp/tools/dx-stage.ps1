# Staging (what the app looks for beside its runtime) and the player package. Dot-sourced by build-helper.ps1.
# Nothing from the game: the setup screen makes everything game-specific on the player's machine from their own dump.

# Puts what the runtime looks for into <Dest> (build\smm2-dx_data, or the package's smm2-dx_data):
#   python/        a Python 3.12 of its own, with capstone, unicorn and lz4
#   tools/         the recompiler (gen.py and its modules) and the game's setup scripts
#   gamelib-src/   the headers the game code is compiled against, its entry file, the HLE list, the patches
#   zig/           the compiler the game code is built with
#   hactool/       hactool, for the firmware
#   licenses/      third-party licenses; THIRD-PARTY.txt says which is which
function Copy-Support([string]$Dest, $Cfg) {
    $g = $Cfg.GameId
    foreach ($d in 'python', 'zig', 'hactool', 'tools', 'gamelib-src', 'licenses') { Remove-Tree (Join-Path $Dest $d) }
    foreach ($d in "tools\games\$g", 'gamelib-src\mods', 'licenses') { New-Item -ItemType Directory -Force (Join-Path $Dest $d) | Out-Null }

    Install-Python
    Write-Note 'copying python'
    Copy-Tree $PyDir (Join-Path $Dest 'python') @('__pycache__')

    # the recompiler and the setup scripts
    foreach ($f in 'gen.py', 'lifter.py', 'analyze.py', 'nso.py', 'passes.py', 'patches.py', 'simd_c.py') { Copy-Item (Join-Path $Recomp "tools\$f") (Join-Path $Dest 'tools') }
    foreach ($f in 'setup.py', 'sysdata.py', 'mii_stub.py', 'mini_head.json', 'BIZUDPGothic-Regular.ttf', 'BIZUDPGothic-OFL.txt', 'versions.py') { Copy-Item (Join-Path $Recomp "games\$g\$f") (Join-Path $Dest "tools\games\$g") }
    # what the game code is compiled against (the same files the library id hashes)
    foreach ($f in 'cpu.h', 'simd.h', 'simd_fp.h', 'gamelib.h', 'game.h') { Copy-Item (Join-Path $Recomp "runtime\$f") (Join-Path $Dest 'gamelib-src') }
    Copy-Item (Join-Path $Recomp 'tools\gamelib_entry.c') (Join-Path $Dest 'gamelib-src')
    Copy-Item (Join-Path $Recomp $Cfg.Hle) (Join-Path $Dest 'gamelib-src\hle_symbols.txt')
    Copy-Item (Join-Path $Recomp $Cfg.Patches) (Join-Path $Dest 'gamelib-src\patches.py')
    # every mod patch file the game's patches.py loads: the same files the library id hashes
    Copy-Item (Join-Path $Recomp $Cfg.PatchDeps) (Join-Path $Dest 'gamelib-src\mods')

    Install-ZigSlim
    Write-Note 'copying zig'
    Copy-Tree $ZigSlimDir (Join-Path $Dest 'zig') @('.slim')
    Remove-Item (Join-Path $Dest 'zig\.slim') -Force -ErrorAction SilentlyContinue
    Install-Hactool
    Copy-Tree $HactoolDir (Join-Path $Dest 'hactool')

    $lic = Join-Path $Dest 'licenses'
    Copy-Item (Join-Path $Recomp 'LICENSE-LASRecomp') (Join-Path $lic 'LICENSE-LASRecomp.txt')
    Copy-Item (Join-Path $Recomp 'third_party\imgui\LICENSE.txt') (Join-Path $lic 'LICENSE-imgui.txt')
    if (Test-Path (Join-Path $Recomp 'third_party\smaa\LICENSE.txt')) { Copy-Item (Join-Path $Recomp 'third_party\smaa\LICENSE.txt') (Join-Path $lic 'LICENSE-smaa.txt') }
    Copy-Item (Join-Path $Recomp 'third_party\zstd\LICENSE') (Join-Path $lic 'LICENSE-zstd.txt')
    Copy-Item (Join-Path $Recomp 'third_party\bcdec\LICENSE') (Join-Path $lic 'LICENSE-bcdec.txt')
    Copy-Item (Join-Path $Dest 'hactool\mbedtls-LICENSE.txt') (Join-Path $lic 'Apache-2.0.txt')
    Write-TextFile (Join-Path $Dest 'THIRD-PARTY.txt') @'
Third-party components in this package and their licenses (the project's own code: see the source repository)

- LAS Recomp (the recompiler and runtime this is forked from): MIT, licenses/LICENSE-LASRecomp.txt
- Dear ImGui (the setup screen and the overlay menu): MIT, licenses/LICENSE-imgui.txt
- SMAA (built in): MIT-style, licenses/LICENSE-smaa.txt
- Mbed TLS 3.6 (built in: the game's TLS client): Apache-2.0, licenses/Apache-2.0.txt
- Zstandard decoder (built in: the shader cache): BSD-3-Clause, licenses/LICENSE-zstd.txt
- bcdec (built in: BC texture decoding where the GPU lacks it): MIT / Unlicense, licenses/LICENSE-bcdec.txt
- zig (zig/, the compiler the game code is built with): MIT, zig/LICENSE; its clang / lld: Apache-2.0 with
  LLVM exception (licenses/Apache-2.0.txt)
- Python 3.12 (python/, python-build-standalone): PSF License and its components' own: python/README-LICENSES.txt
- capstone (BSD-3-Clause), unicorn (GPL-2.0; source: https://github.com/unicorn-engine/unicorn) and lz4
  (BSD-3-Clause), the game code generator's Python packages: python/.../site-packages/*.dist-info
- hactool (hactool/): ISC, hactool/LICENSE.txt; its mbedtls: Apache-2.0, hactool/mbedtls-LICENSE.txt
- SDL3 (SDL3.dll): zlib license, https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt
- shaderc, glslang, SPIRV-Tools (libshaderc_shared.dll): Apache-2.0 / BSD-3-Clause, licenses/Apache-2.0.txt
- libiconv (libiconv-2.dll, used by SDL3): LGPL-2.1, an unmodified MSYS2 build (source: https://www.gnu.org/software/libiconv/)
'@ -Crlf
}

# The player's README.txt
function Write-PlayerReadme([string]$File, [string]$Version) {
    Write-TextFile $File @"
Super Mario Maker 2 (recompiled) $Version (windows-x86_64)

Super Mario Maker 2 v3.0.3, recompiled for your computer from YOUR OWN game. Nothing of the game is in this
download: you bring your dump, and the setup screen prepares everything from it, on your machine.

1. Start smm2-dx-windows-x86_64.exe (keep it together with its smm2-dx_data folder).
2. The setup screen asks for:
   - your Super Mario Maker 2 with the 3.0.3 update, decrypted: the folders with its extracted ExeFS and RomFS
     (e.g. an emulator's "Extract RomFS / ExeFS" of the updated game). NSP / XCI files are not used;
   - your console's prod.keys and firmware (the folder of its system .nca files, e.g. Ryujinx's
     bis/system/Contents/registered): the game's fonts come from there. They are read where they are, never copied;
     key values are never shown or logged.
3. Press "Set up and play". The first time it checks your files against the known 3.0.3 version, extracts them,
   copies the fonts and recompiles the game code (a few minutes to half an hour), then the game starts.
   Later starts go straight to the game. The overlay menu (F1 / Esc) > Mods > Game files brings the setup back.

Your saves, settings and the set-up game files are in the smm2-dx_data folder beside the program (keep it somewhere
you can write to, e.g. Documents - not Program Files). The logs are in smm2-dx_log, also beside it.

Keys: WASD stick, arrows d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter = +, Backspace = -, F1 menu.
Controllers work through SDL.

Windows 10 / 11, 64-bit, a CPU with x86-64-v2 (AVX2 used when available) and a graphics driver with Vulkan 1.3.

Help, status and the source: https://github.com/sometimes-steve/smm2-dx - the setup screen tells you when a new
release is out (it only links to it; nothing is downloaded by itself).
"@ -Crlf
}

# Stages the build's support files beside the runtime, again when what they are made of changes or any is missing.
function Update-Staging($Cfg, [string]$Version, [string[]]$Sources) {
    $stamp = Join-Path $Work '.staged'
    $need = @('python\python.exe', 'zig\zig.exe', 'hactool\hactool.exe', 'tools\gen.py', 'README.txt')
    $ok = Test-Path $stamp
    foreach ($n in $need) { if (-not (Test-Path (Join-Path $Dx $n))) { $ok = $false } }
    if ($ok) {
        $t = (Get-Item $stamp).LastWriteTimeUtc
        $newest = Get-Newest ($Sources + @((Join-Path $Recomp 'tools\dx-stage.ps1'), (Join-Path $Recomp 'tools\dx-toolchain.ps1')))
        if ($newest -gt $t) { $ok = $false }
        else {   # (the staged Python must still import what the generator needs)
            try { [void](Invoke-Proc (Join-Path $Dx 'python\python.exe') @('-c', 'import threading, subprocess, json, hashlib, capstone, unicorn, lz4.frame') -Quiet) } catch { $ok = $false }
        }
    }
    if ($ok) { Write-Note 'smm2-dx_data is staged'; return }
    Write-Step 'staging python, zig, hactool and the recompiler into smm2-dx_data'
    Copy-Support $Dx $Cfg
    Write-PlayerReadme (Join-Path $Dx 'README.txt') $Version
    Write-TextFile $stamp 'staged'
}

# dist\SMM2Recomp-windows-x86_64.zip: the launcher exe and smm2-dx_data (the runtime, its DLLs, the import library,
# Python, zig, hactool, the recompiler), and a README
function New-Package($Cfg, [string]$Version, [string]$StubExe, [string]$RuntimeExe, [string]$ImpLib) {
    Write-Step 'packaging'
    $name = 'SMM2Recomp-windows-x86_64'
    $dist = $Out
    $pkg = Join-Path $Work "package\$name"   # (staged in work, so dist holds only the app and the zip)
    Remove-Tree $pkg
    Remove-Item (Join-Path $dist "$name.zip") -Force -ErrorAction SilentlyContinue
    $data = Join-Path $pkg $DxName
    New-Item -ItemType Directory -Force $data | Out-Null
    Copy-Item $StubExe $pkg
    Copy-Item $RuntimeExe $data
    Copy-Item $ImpLib $data
    foreach ($d in 'SDL3.dll', 'libiconv-2.dll', 'libshaderc_shared.dll') { Copy-Item (Join-Path $DepsDir "bin\$d") $data }
    Copy-Item (Join-Path $Recomp 'branding\app-icon.ico') (Join-Path $data 'SMM2Recomp.ico')
    Copy-Support $data $Cfg
    Write-PlayerReadme (Join-Path $pkg 'README.txt') $Version
    [void](Invoke-Proc (Get-Py) @((Join-Path $Recomp 'scripts\check_package.py'), $pkg))   # fails on any game content
    [void](Invoke-Proc $SysTar @('-a', '-c', '-f', (Join-Path $dist "$name.zip"), '-C', (Split-Path $pkg), $name) -Quiet)
    $zip = Join-Path $dist "$name.zip"
    Write-Out ("{0}: {1:N0} MB, {2}" -f $zip, ((Get-Item $zip).Length / 1MB), $Version) 32
}
