# What the build and the app need, fetched on demand: zig, the libraries, Python, hactool. Every download is pinned to a
# sha256; a piece that is missing, half fetched or broken is fetched again. Dot-sourced by build-helper.ps1 after dx-common.ps1.
# Everything goes into <repo>\build\smm2-dx_data\work.

$script:ZigVer = '0.17.0'
$script:ZigSha = 'b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428'
$script:ZigDir = Join-Path $Work 'zig'                       # the full compiler (builds the exe)
$script:ZigSlimDir = Join-Path $Work 'zig-x86_64-windows'    # what ships: only what compiling C for Windows needs
$script:DepsDir = Join-Path $Work 'deps-windows-x86_64'
$script:PyDir = Join-Path $Work 'python-windows-x86_64'
$script:HactoolDir = Join-Path $Work 'hactool-windows-x86_64'

function Get-Zig { Join-Path $ZigDir 'zig.exe' }
function Get-Py { Join-Path $PyDir 'python.exe' }

# ---- zig (MIT; its clang / lld: Apache-2.0 with LLVM exception)
function Install-Zig {
    if (Test-Path (Join-Path $ZigDir 'lib')) { if (Test-Path (Get-Zig)) { return } }
    Write-Step "zig $ZigVer"
    $name = "zig-x86_64-windows-$ZigVer"
    $zip = Join-Path $Dl "zig-dl\$name.zip"
    Get-Verified "https://ziglang.org/download/$ZigVer/$name.zip" $zip $ZigSha
    $tmp = Join-Path $Dl 'zig-unpack'
    Remove-Tree $tmp
    Expand-Archive2 $zip $tmp
    Remove-Tree $ZigDir
    New-Item -ItemType Directory -Force (Split-Path $ZigDir) | Out-Null
    Move-Retry (Join-Path $tmp $name) $ZigDir
    Remove-Tree $tmp
    if (-not (Test-Path (Get-Zig))) { throw 'zig did not unpack' }
}

# The copy that ships: the game code is compiled on the player's machine for Windows, so only the Windows C headers and
# libc sources stay (no C++ runtimes, sanitizers or docs). lib\std stays: zig cc builds compiler_rt with it.
function Install-ZigSlim {
    $marker = Join-Path $ZigSlimDir '.slim'
    if (Test-Path $marker) { return }
    Install-Zig
    Write-Step 'zig (the copy that ships)'
    Remove-Tree $ZigSlimDir
    Copy-Tree $ZigDir $ZigSlimDir
    $lib = Join-Path $ZigSlimDir 'lib'
    Get-ChildItem (Join-Path $lib 'libc\include') -Directory | Where-Object { $_.Name -notmatch 'windows|mingw' } | ForEach-Object { Remove-Tree $_.FullName }
    Get-ChildItem (Join-Path $lib 'libc') | Where-Object { $_.Name -ne 'include' -and $_.Name -ne 'mingw' } | ForEach-Object { Remove-Tree $_.FullName }
    foreach ($d in 'libcxx', 'libcxxabi', 'libtsan', 'docs') { Remove-Tree (Join-Path $lib $d) }
    Remove-Tree (Join-Path $ZigSlimDir 'doc')
    Write-TextFile $marker 'slim'
}

# ---- SDL3, libiconv, shaderc, the Vulkan headers and loader, VMA: MSYS2's UCRT64 packages, pinned by sha256
function Install-Deps {
    if ((Test-Path (Join-Path $DepsDir 'include\SDL3')) -and (Test-Path (Join-Path $DepsDir 'bin\libshaderc_shared.dll')) -and
        (Test-Path (Join-Path $DepsDir 'lib\SDL3.lib'))) { return }
    Write-Step 'SDL3, shaderc and the Vulkan headers'
    $repo = 'https://repo.msys2.org/mingw/ucrt64'
    $pkgs = @(
        @('f9f8e2fe9858868e7653b24c973bcc8c19dea1096afc8c905bf1cd1a87960442', 'sdl3-3.4.18-1'),
        @('9a500f38c2b91808741c62fae746b3e9110b33a1ecf5c30fa0c66dbedddf7e16', 'libiconv-1.19-1'),
        @('a4ad271f76acd2307ed67738770b30d6486bd3c7acff63ba83dd13e56ce9ab82', 'shaderc-2026.4-1'),
        @('be602b2657cace531316e78b1eda4087e8f7f84ebb877f08fba198a2db06406a', 'vulkan-headers-1~1.4.363.0-1'),
        @('065076fe13d39ab97eea4740596426d5ae741b5044e48e1c9495d04835c92dbe', 'vulkan-loader-1~1.4.363.0-1'),
        @('44c81162337915afecda3185f66fd98268a54454580b2717080c9dfef57fe781', 'vulkan-memory-allocator-3.4.0-1'))
    $x = Join-Path $Dl 'msys2-unpack'
    Remove-Tree $x
    foreach ($p in $pkgs) {
        $f = "mingw-w64-ucrt-x86_64-$($p[1])-any.pkg.tar.zst"
        Get-Verified "$repo/$f" (Join-Path $Dl "msys2\$f") $p[0]
        Expand-Archive2 (Join-Path $Dl "msys2\$f") $x
    }
    $u = Join-Path $x 'ucrt64'
    Remove-Tree $DepsDir
    foreach ($d in 'include', 'lib', 'bin') { New-Item -ItemType Directory -Force (Join-Path $DepsDir $d) | Out-Null }
    foreach ($d in 'SDL3', 'vulkan', 'vk_video', 'shaderc') { Copy-Item (Join-Path $u "include\$d") (Join-Path $DepsDir 'include') -Recurse }
    Copy-Item (Join-Path $u 'include\vma\vk_mem_alloc.h') (Join-Path $DepsDir 'include')
    foreach ($l in 'libSDL3.dll.a', 'libvulkan-1.dll.a', 'libshaderc_shared.dll.a') { Copy-Item (Join-Path $u "lib\$l") (Join-Path $DepsDir 'lib') }
    # (zig looks for these names)
    Copy-Item (Join-Path $u 'lib\libSDL3.dll.a') (Join-Path $DepsDir 'lib\SDL3.lib')
    Copy-Item (Join-Path $u 'lib\libvulkan-1.dll.a') (Join-Path $DepsDir 'lib\vulkan-1.lib')
    Copy-Item (Join-Path $u 'lib\libshaderc_shared.dll.a') (Join-Path $DepsDir 'lib\shaderc_shared.lib')
    foreach ($b in 'SDL3.dll', 'libiconv-2.dll', 'libshaderc_shared.dll') { Copy-Item (Join-Path $u "bin\$b") (Join-Path $DepsDir 'bin') }
    Remove-Tree $x
}

# ---- Python 3.12 for the recompiler and the setup scripts (python-build-standalone) plus capstone, unicorn and lz4
function Test-Python {
    if (-not (Test-Path (Join-Path $PyDir 'README-LICENSES.txt'))) { return $false }
    try {
        [void](Invoke-Proc (Get-Py) @('-c', 'import threading, subprocess, json, hashlib, capstone, unicorn, lz4.frame') -Quiet)
        return $true
    } catch { return $false }
}

function Install-Python {
    if (Test-Python) { return }
    Write-Step 'Python 3.12 with capstone, unicorn and lz4'
    $rel = '20260924'; $ver = '3.12.14'
    $name = "cpython-$ver+$rel-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"
    $sha = 'c5bf8edfe858c1df9891be498b5bbc8761d383df5b9790658b088fea4870433a'
    Get-Verified "https://github.com/astral-sh/python-build-standalone/releases/download/$rel/$($name.Replace('+', '%2B'))" (Join-Path $Dl $name) $sha
    Remove-Tree $PyDir
    Expand-Archive2 (Join-Path $Dl $name) $PyDir 1
    $py = Get-Py
    Write-Note 'installing capstone, unicorn and lz4 (pip)'
    # the generator's packages: this platform's wheels (hash-checked), unpacked into site-packages
    $w = Join-Path $Dl 'wheels-win_amd64'
    [void](Invoke-Proc $py @('-m', 'pip', 'download', '-q', '--disable-pip-version-check', '--only-binary=:all:', '--no-deps',
        '--platform', 'win_amd64', '--python-version', '3.12', '--implementation', 'cp', '--require-hashes',
        '-r', (Join-Path $Recomp 'tools\requirements-gen.txt'), '-d', $w) -Quiet)
    $site = Join-Path $PyDir 'Lib\site-packages'
    foreach ($whl in Get-ChildItem $w -Filter *.whl) {
        [void](Invoke-Proc $py @('-c', 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])', $whl.FullName, $site) -Quiet)
    }
    Write-Note 'trimming what the scripts do not need'
    # what running the scripts doesn't need: GUI toolkits, tests, the package installer, headers, docs
    $lib = Join-Path $PyDir 'Lib'
    foreach ($d in 'test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'lib2to3', 'pydoc_data') { Remove-Tree (Join-Path $lib $d) }
    Get-ChildItem $site -Filter 'pip*' -ErrorAction SilentlyContinue | ForEach-Object { Remove-Tree $_.FullName }
    foreach ($d in 'include', 'share', 'tcl', 'Scripts', 'libs') { Remove-Tree (Join-Path $PyDir $d) }
    foreach ($pat in 'tcl*', 'tk*', '_tkinter*') { Get-ChildItem (Join-Path $PyDir 'DLLs') -Filter $pat -ErrorAction SilentlyContinue | Remove-Item -Force }
    Get-ChildItem $PyDir -Recurse -Directory -Filter '__pycache__' | ForEach-Object { Remove-Tree $_.FullName }
    Get-ChildItem $PyDir -Recurse -File -Filter '*.a' | Remove-Item -Force
    foreach ($d in 'capstone', 'unicorn') { Remove-Tree (Join-Path $site "$d\include") }
    Write-TextFile (Join-Path $PyDir 'README-LICENSES.txt') @"
Python $ver (python-build-standalone $rel, x86_64-pc-windows-msvc): https://github.com/astral-sh/python-build-standalone
Python: PSF License (LICENSE.txt in this folder or in the Python distribution); the components it includes
keep their own licenses (python-build-standalone documents them).
Packages for the game code generator: capstone 5.0.9 (BSD-3-Clause), unicorn 2.1.4 (GPL-2.0, unmodified
library, run as a separate program's module), lz4 4.4.5 (BSD-3-Clause); their licenses are in site-packages/*.dist-info.
"@
    if (-not (Test-Python)) { throw 'the fetched Python does not import capstone, unicorn and lz4' }
}

# ---- hactool 1.4.0 (SciresM, ISC; its mbedtls: Apache-2.0), built from the release's source with zig cc
function Install-Hactool {
    if (Test-Path (Join-Path $HactoolDir 'hactool.exe')) { return }
    Install-Zig; Install-Python
    Write-Step 'hactool 1.4.0'
    $ver = '1.4.0'
    $tgz = Join-Path $Dl "hactool-$ver.tar.gz"
    Get-Verified "https://github.com/SciresM/hactool/archive/refs/tags/$ver.tar.gz" $tgz '1373c98ef8c58da42cb1e81b361283955caad0e1b8b5ad389b0ee49b7a21da43'
    $src = Join-Path $Work "hactool-src-$ver"
    Remove-Tree $src
    Expand-Archive2 $tgz $src 1
    [void](Invoke-Proc (Get-Py) @((Join-Path $Recomp 'tools\hactool_patch.py'), $src) -Quiet)
    Remove-Tree $HactoolDir
    $objDir = Join-Path $HactoolDir 'obj'
    New-Item -ItemType Directory -Force $objDir | Out-Null
    $cf = @('-target', 'x86_64-windows-gnu', '-O2', '-std=gnu11', '-w', "-I$(ConvertTo-Fwd $src)/mbedtls/include",
        '-D_BSD_SOURCE', '-D_POSIX_SOURCE', '-D_POSIX_C_SOURCE=200112L', '-D_DEFAULT_SOURCE', '-D__USE_MINGW_ANSI_STDIO=1', '-D_FILE_OFFSET_BITS=64')
    $jobs = @(); $objs = @()
    $files = @(Get-ChildItem (Join-Path $src 'mbedtls\library') -Filter *.c) + @(Get-ChildItem $src -Filter *.c)
    foreach ($c in $files) {
        if ($c.Name -eq 'net_sockets.c') { continue }   # (no networking: Windows would need ws2_32)
        $o = Join-Path $objDir ("$($c.Directory.Name)_$($c.BaseName).o")
        $objs += $o
        $jobs += @{ Name = $c.Name; Exe = (Get-Zig); Args = (@('cc') + $cf + @('-c', $c.FullName, '-o', $o)) }
    }
    Invoke-Parallel $jobs ([Environment]::ProcessorCount) 'hactool'
    [void](Invoke-Proc (Get-Zig) (@('cc', '-target', 'x86_64-windows-gnu', '-o', (Join-Path $HactoolDir 'hactool.exe')) + $objs + @('-s', '-ladvapi32')) -Quiet)
    Remove-Tree $objDir
    Get-ChildItem $HactoolDir -Include '*.lib', '*.pdb' -Recurse | Remove-Item -Force   # (zig leaves an import library next to a Windows exe)
    Write-TextFile (Join-Path $HactoolDir 'LICENSE.txt') ((Get-Content (Join-Path $src 'LICENSE') -Raw) + "`nhactool $ver`: https://github.com/SciresM/hactool (built from the release's source).`nIts bundled mbedtls is Apache-2.0: see mbedtls-LICENSE.txt.`n")
    Copy-Item (Join-Path $src 'mbedtls\apache-2.0.txt') (Join-Path $HactoolDir 'mbedtls-LICENSE.txt')
}
