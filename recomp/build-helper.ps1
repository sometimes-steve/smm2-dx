<#
.SYNOPSIS
    smm2-dx Build Helper: builds smm2-dx for Windows, the launcher exe and the smm2-dx_data folder, in <repo>\dist.

.DESCRIPTION
    Needs only Windows (PowerShell 5.1 or newer, and the curl and tar that come with Windows 10 and later). The first run
    downloads the compiler (zig), the libraries (SDL3, shaderc, the Vulkan headers), Python and hactool, each pinned to a
    sha256, into dist\smm2-dx_data\work; a piece that is missing or broken is fetched again. Nothing is installed on the
    machine. The game's files are never downloaded: you supply them in the setup screen.

.PARAMETER Package
    Also write dist\SMM2Recomp-windows-x86_64.zip: the launcher exe and smm2-dx_data, with nothing from the game.
.PARAMETER Clean
    Delete the compiled objects first (the fetched tools are kept).
.PARAMETER Jobs
    Compiles at once (default: one per logical processor).
.PARAMETER Setup
    From your own dump: lay out the game files in private\ (see games\smm2\game.psd1 for the paths; -DumpRoot to change).
.PARAMETER Gen
    Recompile the game's code ahead of time into generated\ and build the game library beside the runtime (for developing
    the recompiler; the setup screen does this on a player's machine).
.PARAMETER Run
    Start the app when the build is done.
.PARAMETER NoBanner
    Skip the picture at the start.
.PARAMETER Version
    The version the app shows (default: git describe, else dev).
.EXAMPLE
    .\build-helper.ps1
.EXAMPLE
    .\build-helper.ps1 -Package
#>
[CmdletBinding()]
param(
    [switch]$Package,
    [switch]$Clean,
    [int]$Jobs = 0,
    [switch]$Setup,
    [switch]$Gen,
    [switch]$Run,
    [switch]$ShowWarnings,
    [switch]$NoBanner,
    [string]$DumpRoot = '',
    [string]$Game = 'smm2',
    [string]$Version = ''
)
$ErrorActionPreference = 'Stop'
$t0 = Get-Date

. (Join-Path $PSScriptRoot 'tools\dx-common.ps1')
. (Join-Path $PSScriptRoot 'tools\dx-toolchain.ps1')
. (Join-Path $PSScriptRoot 'tools\dx-build.ps1')
. (Join-Path $PSScriptRoot 'tools\dx-stage.ps1')
$script:ShowWarnings = [bool]$ShowWarnings

$cfg = Import-PowerShellDataFile (Join-Path $Recomp "games\$Game\game.psd1")
if ($Jobs -lt 1) { $Jobs = [Environment]::ProcessorCount }
if (-not $Version) { $Version = Get-Version }
try {
    Show-Banner $Version $Jobs -NoArt:$NoBanner
    $stubExe = Join-Path $Out "$($cfg.GameId)-dx-windows-x86_64.exe"       # the exe players start
    $runtimeExe = Join-Path $Dx "$($cfg.GameId)-dx-runtime.exe"           # the runtime, beside its DLLs
    $impLib = Join-Path $Dx "$($cfg.GameId)-dx.dll.a"                      # what the game code library links against
    $gameLib = Join-Path $Dx "$($cfg.GameId)game.dll"

    if ($Clean) {
        Write-Step 'cleaning'
        foreach ($d in 'rt', 'imgui', 'mbedtls', 'inc', 'link', 'obj') { Remove-Tree (Join-Path $Work $d) }
        foreach ($f in 'vk_shim.c', 'windows.res', 'exports.def', '.flags', '.staged') { Remove-Item (Join-Path $Work $f) -Force -ErrorAction SilentlyContinue }
    }

    # the toolchain
    Install-Zig
    Install-Deps
    Install-Python

    # what the game code library is built from: the generator, the headers it is compiled against, the flags
    $script:GenFlags = '-O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime -D_FILE_OFFSET_BITS=64 -mcpu=x86_64_v2'
    $libSrcs = @('tools/gen.py', 'tools/lifter.py', 'tools/analyze.py', 'tools/nso.py', 'tools/passes.py', 'tools/patches.py', 'tools/simd_c.py', 'tools/gamelib_entry.c', $cfg.Hle)
    if (Test-Path (Join-Path $Recomp $cfg.Patches)) { $libSrcs += $cfg.Patches }
    $libSrcs = @($libSrcs | ForEach-Object { Join-Path $Recomp $_ })
    $libSrcs += @(Get-Files (Split-Path $cfg.PatchDeps) (Split-Path $cfg.PatchDeps -Leaf))
    $libSrcs += @('runtime/cpu.h', 'runtime/simd.h', 'runtime/simd_fp.h', 'runtime/gamelib.h' | ForEach-Object { Join-Path $Recomp $_ })
    $script:LibSrcs = $libSrcs
    $libId = Get-LibId $libSrcs $script:GenFlags

    if ($Setup) { Invoke-DumpSetup $cfg $DumpRoot }
    if ($Gen) { Invoke-DumpGen $cfg $libId }


    Write-Step "building smm2-dx $Version"
    Build-SmaaInclude
    Build-VkShim
    Build-Resource
    $units = Get-Units $cfg $Version $libId
    Invoke-Compile $units $Jobs
    Invoke-Link $cfg $units $runtimeExe $impLib
    Build-Launcher $stubExe
    Copy-RuntimeDlls
    Build-GameLib $cfg $libId $impLib $gameLib $Jobs
    Update-Staging $cfg $Version ($libSrcs + @((Join-Path $Recomp 'games\smm2\setup.py'), (Join-Path $Recomp 'games\smm2\sysdata.py'), (Join-Path $Recomp 'games\smm2\mii_stub.py'), (Join-Path $Recomp 'games\smm2\mini_head.json'), (Join-Path $Recomp 'games\smm2\BIZUDPGothic-Regular.ttf'), (Join-Path $Recomp 'games\smm2\BIZUDPGothic-OFL.txt'), (Join-Path $Recomp 'games\smm2\versions.py')))

    if ($Package) { New-Package $cfg $Version $stubExe $runtimeExe $impLib }
} finally {
    Stop-Panel
}

Write-Host ("Done in {0:N0}s: {1}" -f ((Get-Date) - $t0).TotalSeconds, $stubExe) -ForegroundColor Green
Write-Host ''
Write-Host 'If you enjoy my work, consider buying me a coffee! https://buymeacoffee.com/sometimes_steve' -ForegroundColor Yellow
if (-not $Run -and -not [Console]::IsInputRedirected) {
    Write-Host ''
    Write-Host 'Launch smm2-dx now? [Y/n] ' -NoNewline
    $answer = [Console]::ReadLine()
    if ($answer -notmatch '^\s*n') { $Run = $true }
}
if ($Run) { Start-Process $stubExe -WorkingDirectory (Split-Path $stubExe) }
