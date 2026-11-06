# Compiles the runtime, links the exe, and builds the launcher. Dot-sourced by build-helper.ps1 after dx-common.ps1 and
# dx-toolchain.ps1. Everything is compiled with zig (zig cc / zig c++, target x86_64-windows-gnu) from the fetched
# toolchain; nothing else is installed on the machine.

$script:Target = 'x86_64-windows-gnu'

function Get-Version {
    try {
        $v = (& git -C $Recomp describe --tags --match 'v[0-9]*' 2>$null)
        if ($LASTEXITCODE -eq 0 -and $v) { return ([string]$v).Trim() }
    } catch { }
    'dev'
}

function Get-Newest([string[]]$Files) {
    $t = [DateTime]::MinValue
    foreach ($f in $Files) { if (Test-Path $f) { $m = (Get-Item $f).LastWriteTimeUtc; if ($m -gt $t) { $t = $m } } }
    $t
}

function Get-Files([string]$Dir, [string]$Filter) {
    $d = Join-Path $Recomp $Dir
    if (-not (Test-Path $d)) { return @() }
    @(Get-ChildItem $d -File -Filter $Filter | Sort-Object Name | ForEach-Object { $_.FullName })
}

# sha256 of the files' bytes, in order, then a line of text: what the game code library is built from. A library with
# another id is never loaded (runtime/gamelib.c).
function Get-LibId([string[]]$Files, [string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    foreach ($f in $Files) { $b = [IO.File]::ReadAllBytes($f); [void]$sha.TransformBlock($b, 0, $b.Length, $null, 0) }
    $t = [Text.Encoding]::UTF8.GetBytes($Text + "`n")
    [void]$sha.TransformFinalBlock($t, 0, $t.Length)
    (($sha.Hash | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 16)
}

function Get-FlagsHash([string]$Text) {
    $sha = [Security.Cryptography.SHA1]::Create()
    (($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)) | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Read-FlagStore {
    $h = @{}
    $f = Join-Path $Work '.flags'
    if (Test-Path $f) { foreach ($l in Get-Content $f) { $i = $l.IndexOf("`t"); if ($i -gt 0) { $h[$l.Substring(0, $i)] = $l.Substring($i + 1) } } }
    $h
}
function Save-FlagStore($h) { Write-TextFile (Join-Path $Work '.flags') ((($h.Keys | Sort-Object | ForEach-Object { "$_`t$($h[$_])" }) -join "`n") + "`n") }

# The units to compile, with the flags the Makefile used to give each
function Get-Units($Cfg, [string]$Version, [string]$LibId) {
    $B = $Work
    $depsInc = '-I' + (ConvertTo-Fwd (Join-Path $DepsDir 'include'))
    $os = @('-D_FILE_OFFSET_BITS=64', '-D_POSIX_THREAD_SAFE_FUNCTIONS', '-D_CRT_RAND_S', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX')
    $gameDefs = @("-DGAME_TITLE_ID=$($Cfg.TitleId)ull", "-DGAME_ID=`"$($Cfg.GameId)`"", "-DGAME_TITLE=`"$($Cfg.Title)`"",
        "-DGAME_BUNDLE_ID=`"$($Cfg.BundleId)`"", "-DGAME_DISPLAY_VERSION=`"$($Cfg.DisplayVersion)`"")
    $rtFlags = @('-O2', '-g', '-fno-strict-aliasing', '-Wall', '-Wno-unused-function', '-Iruntime', '-Iruntime/nvn', $depsInc) + $os + $gameDefs
    $cxxFlags = @('-O2', '-g', '-std=c++17', '-Ithird_party/imgui', '-Ithird_party/imgui/backends', '-Iruntime', '-Iruntime/nvn', $depsInc) + $gameDefs
    $mbedFlags = @('-Ithird_party/mbedtls/include', '-Iruntime/net', '-DMBEDTLS_USER_CONFIG_FILE="mbedtls_user_config.h"')
    $genFlagsText = $script:GenFlags
    $libFlags = @("-DGAMELIB_ID=`"$LibId`"")
    $buildFlags = $libFlags + @("-DGAMELIB_CFLAGS=`"$(($genFlagsText -split ' ' | Where-Object { $_ -ne '-Iruntime' }) -join ' ')`"",
        '-DGAMELIB_SHARED="-shared"', "-DGAMEBUILD_HLE=`"$($Cfg.Hle)`"", "-DGAMEBUILD_PATCHES=`"$($Cfg.Patches)`"")

    # headers every runtime object depends on (and the game's own, for its glue)
    $hdrs = @()
    foreach ($d in 'runtime', 'runtime/platform', 'runtime/nvn', 'runtime/nvn/maxwell', 'runtime/nvn/vulkan', 'runtime/ui', 'runtime/net') { $hdrs += Get-Files $d '*.h' }
    $hdrs += (Join-Path $Recomp "games\$($Cfg.GameId)\game.psd1")
    $rtDep = Get-Newest $hdrs
    $gameHdrs = @(Get-ChildItem (Join-Path $Recomp "games\$($Cfg.GameId)") -Recurse -File -Filter *.h | ForEach-Object { $_.FullName })
    $gameDep = Get-Newest ($hdrs + $gameHdrs)
    $libSrcs = $script:LibSrcs
    $libDep = Get-Newest $libSrcs
    $mbedDep = Get-Newest @((Join-Path $Recomp 'runtime\net\mbedtls_user_config.h'))
    $smaaInc = Join-Path $B 'inc\smaa_hlsl.inc'

    $units = New-Object Collections.ArrayList
    function Add-Unit([string]$Src, [string]$Obj, [string]$Kind, [string[]]$Flags, [DateTime]$Dep, [string]$Group) {
        [void]$units.Add(@{ Src = $Src; Obj = $Obj; Kind = $Kind; Flags = $Flags; Dep = $Dep; Group = $Group })
    }
    $rel = { param($p) (ConvertTo-Fwd $p.Substring($Recomp.Length + 1)) }

    # runtime C, in the Makefile's order
    $rtSrc = @()
    foreach ($d in 'runtime', 'runtime/hle', 'runtime/services', 'runtime/nvn', 'runtime/nvn/maxwell') { $rtSrc += Get-Files $d '*.c' }
    $rtSrc += Join-Path $Recomp 'runtime\platform\windows.c'
    $rtSrc += Get-Files 'runtime/net' '*.c'
    $gameSrc = @()
    foreach ($g in $Cfg.SourceGlobs) { $gd = Split-Path $g; $gf = Split-Path $g -Leaf; $gameSrc += Get-Files $gd $gf }
    $rtSrc += $gameSrc
    $rtSrc += Join-Path $Recomp 'runtime\platform\windows_exports.c'
    if (-not $Cfg.Movies) { $rtSrc = @($rtSrc | Where-Object { (& $rel $_) -ne 'runtime/hle/movie.c' }) }
    foreach ($s in $rtSrc) {
        $r = & $rel $s
        $obj = Join-Path $B ('rt\' + ($r -replace '^runtime/', '').Replace('/', '\') -replace '\.c$', '.o')
        $extra = @()
        $dep = $rtDep
        if ($r -like 'games/*') { $dep = $gameDep }
        if ($r -in 'runtime/services/ssl.c', 'runtime/net/http.c', 'runtime/net/roots.c') { $extra += $mbedFlags; if ($mbedDep -gt $dep) { $dep = $mbedDep } }
        if ($r -eq 'runtime/nvn/smaa_data.c') { $extra += @("-I$(ConvertTo-Fwd (Join-Path $B 'inc'))", '-Ithird_party/smaa/Textures'); if ((Test-Path $smaaInc) -and (Get-Item $smaaInc).LastWriteTimeUtc -gt $dep) { $dep = (Get-Item $smaaInc).LastWriteTimeUtc } }
        if ($r -eq 'runtime/gamelib.c') { $extra += $libFlags; if ($libDep -gt $dep) { $dep = $libDep } }
        if ($r -eq 'runtime/gamebuild.c') { $extra += $buildFlags; if ($libDep -gt $dep) { $dep = $libDep } }
        Add-Unit $s $obj 'cc' ($rtFlags + $extra) $dep 'rt'
    }
    # the Vulkan loader's forwarders (generated)
    Add-Unit (Join-Path $B 'vk_shim.c') (Join-Path $B 'rt\vk_shim.o') 'cc' ($rtFlags + '-Wno-unused-parameter') ([DateTime]::MinValue) 'rt'

    # Mbed TLS (the game's TLS client)
    foreach ($s in Get-Files 'third_party/mbedtls/library' '*.c') {
        $obj = Join-Path $B ('mbedtls\' + [IO.Path]::GetFileNameWithoutExtension($s) + '.o')
        Add-Unit $s $obj 'cc' (@('-O2', '-g') + $mbedFlags + ($os | Where-Object { $_ -like '-D*' })) $mbedDep 'mbedtls'
    }
    # the Vulkan backend
    foreach ($s in (Get-Files 'runtime/nvn/vulkan' '*.c')) {
        Add-Unit $s (Join-Path $B ('rt\nvn\vulkan\' + [IO.Path]::GetFileNameWithoutExtension($s) + '.o')) 'cc' $rtFlags $rtDep 'backend'
    }
    foreach ($s in (Get-Files 'runtime/nvn/vulkan' '*.cpp')) {
        Add-Unit $s (Join-Path $B ('rt\nvn\vulkan\' + [IO.Path]::GetFileNameWithoutExtension($s) + '.o')) 'cxx' ($cxxFlags + $os) $rtDep 'backend'
    }
    # the setup screen and the overlay menu
    foreach ($s in (Get-Files 'runtime/ui' '*.cpp')) {
        $n = [IO.Path]::GetFileNameWithoutExtension($s)
        $extra = @('-x', 'c++') + $os
        if ($n -in 'launcher', 'menu') { $extra += "-DRECOMP_VERSION=`"$Version`"" }
        Add-Unit $s (Join-Path $B "rt\ui\$n.o") 'cxx' ($cxxFlags + $extra) $rtDep 'ui'
    }
    # Dear ImGui
    foreach ($n in 'imgui', 'imgui_draw', 'imgui_tables', 'imgui_widgets') {
        Add-Unit (Join-Path $Recomp "third_party\imgui\$n.cpp") (Join-Path $B "imgui\$n.o") 'cxx' $cxxFlags ([DateTime]::MinValue) 'imgui'
    }
    foreach ($n in 'imgui_impl_sdl3', 'imgui_impl_vulkan') {
        Add-Unit (Join-Path $Recomp "third_party\imgui\backends\$n.cpp") (Join-Path $B "imgui\$n.o") 'cxx' $cxxFlags ([DateTime]::MinValue) 'imgui'
    }
    $units
}

# SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c
function Build-SmaaInclude {
    $src = Join-Path $Recomp 'third_party\smaa\SMAA.hlsl'
    $dst = Join-Path $Work 'inc\smaa_hlsl.inc'
    if ((Test-Path $dst) -and (Get-Item $dst).LastWriteTimeUtc -gt (Get-Item $src).LastWriteTimeUtc) { return }
    $bytes = [IO.File]::ReadAllBytes($src)
    $sb = New-Object Text.StringBuilder
    foreach ($b in $bytes) { if ($b -eq 13) { continue }; if ($b -ge 128) { [void]$sb.Append('?') } else { [void]$sb.Append([char]$b) } }
    $lines = $sb.ToString().Split("`n")
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines = $lines[0..($lines.Count - 2)] }
    $outText = ($lines | ForEach-Object { '"' + $_.Replace('\', '\\').Replace('"', '\"') + '\n"' }) -join "`n"
    Write-TextFile $dst ($outText + "`n")
}

# Windows: the Vulkan loader's entry points, forwarded to vulkan-1.dll loaded at run time (tools/gen_vk_shim.py)
function Build-VkShim {
    $dst = Join-Path $Work 'vk_shim.c'
    $srcs = @((Join-Path $Recomp 'tools\gen_vk_shim.py'), (Join-Path $Recomp 'runtime\platform\vulkan-1.exports'))
    if ((Test-Path $dst) -and (Get-Item $dst).LastWriteTimeUtc -gt (Get-Newest $srcs)) { return }
    [void](Invoke-Proc (Get-Py) @($srcs[0], (Join-Path $DepsDir 'include'), $srcs[1], $dst) -Quiet)
}

function Build-Resource {
    $res = Join-Path $Work 'windows.res'
    $in = @((Join-Path $Recomp 'runtime\platform\windows.rc'), (Join-Path $Recomp 'branding\app-icon.ico'))
    if ((Test-Path $res) -and (Get-Item $res).LastWriteTimeUtc -gt (Get-Newest $in)) { return }
    [void](Invoke-Proc (Get-Zig) @('rc', '-fo', $res, '-i', 'branding', 'runtime/platform/windows.rc') -Quiet)
}

function Invoke-Compile($Units, [int]$Jobs) {
    $store = Read-FlagStore
    $todo = @(); $jobsList = @()
    foreach ($u in $Units) {
        $argText = $u.Kind + ' ' + ($u.Flags -join ' ')
        $hash = Get-FlagsHash $argText
        $key = $u.Obj.Substring($Work.Length + 1)
        $stale = -not (Test-Path $u.Obj)
        if (-not $stale) {
            $ot = (Get-Item $u.Obj).LastWriteTimeUtc
            if ((Get-Item $u.Src).LastWriteTimeUtc -gt $ot -or $u.Dep -gt $ot -or $store[$key] -ne $hash) { $stale = $true }
        }
        if ($stale) {
            New-Item -ItemType Directory -Force (Split-Path $u.Obj) | Out-Null
            $drv = $(if ($u.Kind -eq 'cxx') { 'c++' } else { 'cc' })
            $jobsList += @{ Name = (Split-Path $u.Src -Leaf); Exe = (Get-Zig); Dir = $Recomp
                Args = (@($drv, '-target', $script:Target) + $u.Flags + @('-c', (ConvertTo-Fwd $u.Src), '-o', (ConvertTo-Fwd $u.Obj))) }
            $todo += @{ Key = $key; Hash = $hash }
        }
    }
    if ($jobsList.Count -eq 0) { Write-Note 'objects are up to date'; return }
    Write-Step "compiling $($jobsList.Count) of $(@($Units).Count) files"
    Invoke-Parallel $jobsList $Jobs 'compiled'
    foreach ($t in $todo) { $store[$t.Key] = $t.Hash }
    Save-FlagStore $store
}

function Invoke-Link($Cfg, $Units, [string]$RuntimeExe, [string]$ImpLib) {
    $objs = @($Units | ForEach-Object { $_.Obj })
    $stale = -not (Test-Path $RuntimeExe) -or -not (Test-Path $ImpLib)
    $res = Join-Path $Work 'windows.res'
    $all = $objs + @($res)
    if (-not $stale) { $t = (Get-Item $RuntimeExe).LastWriteTimeUtc; foreach ($o in $all) { if ((Get-Item $o).LastWriteTimeUtc -gt $t) { $stale = $true; break } } }
    if (-not $stale) { Write-Note "$([IO.Path]::GetFileName($RuntimeExe)) is up to date"; return }
    Write-Step "linking $([IO.Path]::GetFileName($RuntimeExe))"
    # the Makefile's order: runtime, Mbed TLS, the Vulkan backend, the setup screen, ImGui (then the icon and the shim)
    $order = @(); foreach ($g in 'rt', 'mbedtls', 'backend', 'ui', 'imgui') { $order += @($Units | Where-Object { $_.Group -eq $g } | ForEach-Object { $_.Obj }) }
    $ld = @('-Wl,--stack,16777216', "-L$(ConvertTo-Fwd (Join-Path $DepsDir 'lib'))", '-lSDL3', '-lwinmm', '-lole32', '-lshell32', '-lcomdlg32', '-luuid',
        '-lshaderc_shared', '-lws2_32', '-lbcrypt', '-Wl,--export-all-symbols', "-Wl,--out-implib,$(ConvertTo-Fwd $ImpLib)")
    $linkDir = Join-Path $Work 'link'
    New-Item -ItemType Directory -Force $linkDir, (Split-Path $RuntimeExe) | Out-Null
    $tmp = Join-Path $linkDir (Split-Path $RuntimeExe -Leaf)
    $rsp = Join-Path $linkDir 'link.rsp'
    # (a response file: ~400 objects don't fit on a Windows command line)
    $lines = @('-target', $script:Target, '-o', (ConvertTo-Fwd $tmp)) + ($order | ForEach-Object { ConvertTo-Fwd $_ }) + @((ConvertTo-Fwd $res)) + $ld
    Write-TextFile $rsp ((($lines | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join "`n") + "`n")
    [void](Invoke-Proc (Get-Zig) @('c++', "@$(ConvertTo-Fwd $rsp)") $Recomp -Quiet)
    Move-Retry $tmp $RuntimeExe
    # the import library the game code DLL links against: the exe's name and what it exports
    $defText = "LIBRARY $(Split-Path $RuntimeExe -Leaf)`n" + (Get-Content (Join-Path $Recomp 'runtime\platform\windows_exports.def') -Raw)
    Write-TextFile (Join-Path $Work 'exports.def') $defText
    [void](Invoke-Proc (Get-Zig) @('dlltool', '-m', 'i386:x86-64', '-d', (Join-Path $Work 'exports.def'), '-l', $ImpLib) -Quiet)
}

# The exe players start: a small launcher that runs the runtime in smm2-dx_data (runtime/platform/launcher_stub.c)
function Build-Launcher([string]$StubExe) {
    $src = Join-Path $Recomp 'runtime\platform\launcher_stub.c'
    $res = Join-Path $Work 'windows.res'
    if ((Test-Path $StubExe) -and (Get-Item $StubExe).LastWriteTimeUtc -gt (Get-Newest @($src, $res))) { return }
    Write-Step "linking $([IO.Path]::GetFileName($StubExe))"
    New-Item -ItemType Directory -Force (Split-Path $StubExe) | Out-Null
    [void](Invoke-Proc (Get-Zig) @('cc', '-target', $script:Target, '-O2', '-s', '-Wl,--subsystem,windows', '-o', $StubExe, $src, $res) $Recomp -Quiet)
}

function Copy-RuntimeDlls {
    New-Item -ItemType Directory -Force $Dx | Out-Null
    foreach ($d in 'SDL3.dll', 'libiconv-2.dll', 'libshaderc_shared.dll') {
        $s = Join-Path $DepsDir "bin\$d"; $t = Join-Path $Dx $d
        if (-not (Test-Path $t) -or (Get-Item $t).LastWriteTimeUtc -lt (Get-Item $s).LastWriteTimeUtc) { Copy-Item $s $t -Force }
    }
}

# ---- the dump-based pipeline: the game's code recompiled ahead of time into a game library next to the runtime
# (the setup screen normally does this on the player's machine; this is for developing the recompiler)
function Invoke-DumpSetup($Cfg, [string]$DumpRoot) {
    if (-not $DumpRoot) { $DumpRoot = $Cfg.DumpRoot }
    if ($DumpRoot.StartsWith('~')) { $DumpRoot = Join-Path $env:USERPROFILE $DumpRoot.Substring(1).TrimStart('/', '\') }
    Write-Step "setting up the game files from $DumpRoot"
    $g = $Cfg.GameId
    [void](Invoke-Proc (Get-Py) @((Join-Path $Recomp "games\$g\setup.py"),
        '--base-romfs', (Join-Path $DumpRoot $Cfg.DumpBaseRomfs), '--update-exefs', (Join-Path $DumpRoot $Cfg.DumpUpdateExefs),
        '--update-romfs', (Join-Path $DumpRoot $Cfg.DumpUpdateRomfs), '--out', (Join-Path $Recomp "private\$g\extracted")))
}

# Recompiles every module to C (AArch64 -> C, on this machine, from your own files)
function Invoke-DumpGen($Cfg, [string]$LibId) {
    $g = $Cfg.GameId
    $files = Join-Path $Recomp "private\$g\extracted"
    $gen = Join-Path $Recomp "generated\$g-c"
    $origin = [UInt64]'0x7000000000'
    Write-Step 'recompiling the game code (sdk, main)'
    $tools = Join-Path $Recomp 'tools'
    $sdkBase = '0x{0:x}' -f ($origin + [UInt64]'0x4000000')
    [void](Invoke-Proc (Get-Py) @('gen.py', 'sdk', (Join-Path $files 'exefs\sdk'), $sdkBase, (Join-Path $gen 'sdk'), '--hle', (Join-Path $Recomp $Cfg.Hle), '--simd', 'c') $tools)
    $a = @('gen.py', 'main', (Join-Path $files 'exefs\main'), '0x7000000000', (Join-Path $gen 'main'))
    if (Test-Path (Join-Path $Recomp $Cfg.Patches)) { $a += @('--patches', (Join-Path $Recomp $Cfg.Patches)) }
    [void](Invoke-Proc (Get-Py) ($a + @('--simd', 'c')) $tools)
    Get-ChildItem $gen -Filter '.gen-*' -Force -ErrorAction SilentlyContinue | Remove-Item -Force
    Write-TextFile (Join-Path $gen ".gen-$LibId") ''
}

function Build-GameLib($Cfg, [string]$LibId, [string]$ImpLib, [string]$LibFile, [int]$Jobs) {
    $g = $Cfg.GameId
    $gen = Join-Path $Recomp "generated\$g-c"
    $srcs = @(Get-ChildItem (Join-Path $gen 'main') -Filter *.c -ErrorAction SilentlyContinue) + @(Get-ChildItem (Join-Path $gen 'sdk') -Filter *.c -ErrorAction SilentlyContinue)
    if ($srcs.Count -eq 0) { return }
    if (-not (Test-Path (Join-Path $gen ".gen-$LibId"))) { throw "the generated game code in $gen is older than the generator and patches (library id $LibId): run build-helper.ps1 -Gen first" }
    $objDir = Join-Path $Work 'obj'
    $genFlags = $script:GenFlags -split ' '
    $gameDefs = @("-DGAME_TITLE_ID=$($Cfg.TitleId)ull", "-DGAME_ID=`"$($Cfg.GameId)`"", "-DGAME_TITLE=`"$($Cfg.Title)`"", "-DGAME_BUNDLE_ID=`"$($Cfg.BundleId)`"", "-DGAME_DISPLAY_VERSION=`"$($Cfg.DisplayVersion)`"")
    $cpuH = Join-Path $Recomp 'runtime\cpu.h'
    $jobs = @(); $objs = @()
    foreach ($s in $srcs) {
        $o = Join-Path $objDir ("$($s.Directory.Name)\$($s.BaseName).o"); $objs += $o
        if ((Test-Path $o) -and (Get-Item $o).LastWriteTimeUtc -gt (Get-Newest @($s.FullName, $cpuH))) { continue }
        New-Item -ItemType Directory -Force (Split-Path $o) | Out-Null
        $jobs += @{ Name = $s.Name; Exe = (Get-Zig); Dir = $Recomp; Args = (@('cc', '-target', $script:Target) + $genFlags + @('-c', (ConvertTo-Fwd $s.FullName), '-o', (ConvertTo-Fwd $o))) }
    }
    $entry = Join-Path $objDir 'tools\gamelib_entry.o'; $objs += $entry
    New-Item -ItemType Directory -Force (Split-Path $entry) | Out-Null
    $jobs += @{ Name = 'gamelib_entry.c'; Exe = (Get-Zig); Dir = $Recomp; Args = (@('cc', '-target', $script:Target) + $genFlags + @("-DGAMELIB_ID=`"$LibId`"") + $gameDefs + @('-c', 'tools/gamelib_entry.c', '-o', (ConvertTo-Fwd $entry))) }
    Write-Step "compiling the game code ($($jobs.Count) files)"
    Invoke-Parallel $jobs $Jobs 'compiled'
    $tmp = "$LibFile.tmp"
    $rsp = Join-Path $Work 'link\gamelib.rsp'
    Write-TextFile $rsp ((@('-target', $script:Target, '-shared', (ConvertTo-Fwd $ImpLib), '-o', (ConvertTo-Fwd $tmp)) + ($objs | ForEach-Object { ConvertTo-Fwd $_ }) | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join "`n")
    [void](Invoke-Proc (Get-Zig) @('cc', "@$(ConvertTo-Fwd $rsp)") $Recomp -Quiet)
    Move-Retry $tmp $LibFile
    Write-TextFile "$LibFile.id" "$LibId`n"
}

