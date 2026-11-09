# Rebuild everything from clean and confirm the result.
#
#   .\rebuild-confirm.ps1              clean build, then the checks below
#   .\rebuild-confirm.ps1 -Gen         also recompile the game's code (--gen: long, needs the game's files set up)
#   .\rebuild-confirm.ps1 -Install     afterwards copy the new runtime into the launcher's build folder (old one kept as .bak)
#   .\rebuild-confirm.ps1 -Install -Target "<path to smm2-dx_data>"
#
# Checks (exit code 0 only when every one passes):
#   1. build-helper.cmd --clean exits 0
#   2. the compiler rebuilt every file ("compiling N of M" with N = M: nothing was reused from an earlier build)
#   3. the runtime exe and the launcher exe exist and are newer than this script's start
#   4. text your uncommitted source changes added (string literals in the git diff) is in the runtime exe: the binary
#      really contains your edits, not an older build
param(
    [switch]$Gen,
    [switch]$Install,
    [string]$Target = '',
    [string]$Version = ''   # the version the app shows (default: git describe); e.g. v0.4.0+test
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
$start = Get-Date
$fail = New-Object System.Collections.Generic.List[string]
function Pass($m) { Write-Host ("  PASS  " + $m) -ForegroundColor Green }
function Fail($m) { Write-Host ("  FAIL  " + $m) -ForegroundColor Red; $script:fail.Add($m) }

Write-Host "== source state"
$head = (git rev-parse --short HEAD).Trim()
$dirty = @(git status --short)
Write-Host ("  HEAD " + $head + ", " + $dirty.Count + " changed file(s)")
$dirty | ForEach-Object { Write-Host ("    " + $_) }

Write-Host "== clean build"
$bargs = @('--clean', '--no-banner', '--show-warnings')
if ($Gen) { $bargs += '--gen' }
if ($Version) { $bargs += @('--version', $Version) }
$log = Join-Path $root 'rebuild-confirm.build.log'
# (the helper asks "Launch smm2-dx now? [Y/n]" at the end: with stdin closed it ends there)
$out = & cmd /c ('"' + (Join-Path $root 'build-helper.cmd') + '" ' + ($bargs -join ' ') + ' < NUL 2>&1')
$code = $LASTEXITCODE
$out | Out-File -Encoding utf8 $log
Write-Host ("  full output: " + $log)
if ($code -eq 0) { Pass "build-helper exited 0" } else { Fail ("build-helper exited " + $code) }

$m = $out | Select-String -Pattern 'compiling (\d+) of (\d+) files' | Select-Object -Last 1
if ($m) {
    $n = [int]$m.Matches[0].Groups[1].Value; $t = [int]$m.Matches[0].Groups[2].Value
    if ($n -eq $t) { Pass ("all $t files were compiled (nothing reused)") } else { Fail ("only $n of $t files were compiled") }
} else { Fail "no 'compiling N of M files' line in the output" }
$warn = @($out | Select-String -Pattern 'warning:|error:')
if ($warn.Count) { Write-Host ("  note  " + $warn.Count + " compiler warning/error line(s): see the log") -ForegroundColor Yellow }

Write-Host "== outputs"
$rt = Join-Path $root 'dist\smm2-dx_data\smm2-dx-runtime.exe'
$ln = Join-Path $root 'dist\smm2-dx-windows-x64.exe'
foreach ($f in @($rt, $ln)) {
    if (-not (Test-Path $f)) { Fail ("missing: " + $f); continue }
    $i = Get-Item $f
    $h = (Get-FileHash $f -Algorithm SHA256).Hash.Substring(0, 16)
    if ($i.LastWriteTime -ge $start) { Pass ($i.Name + "  " + $i.Length + " bytes  sha256 " + $h + "...  built " + $i.LastWriteTime.ToString('HH:mm:ss')) }
    else { Fail ($i.Name + " was not rewritten by this build (" + $i.LastWriteTime + ")") }
}

Write-Host "== your changes are in the binary"
if (Test-Path $rt) {
    $bytes = [System.IO.File]::ReadAllBytes($rt)
    $text = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)   # (Latin-1: one char per byte, for a plain search)
    $lits = New-Object System.Collections.Generic.List[string]
    foreach ($line in (git diff -U0 -- 'recomp/*.c' 'recomp/*.h')) {
        if ($line -notmatch '^\+[^+]') { continue }
        $code = $line -replace '//.*$', ''   # (a comment is not in the binary)
        # string literals paired left to right; only plain ones (no format codes or escapes: the binary spells those differently)
        foreach ($mm in [regex]::Matches($code, '"(?:[^"\\]|\\.)*"')) {
            $v = $mm.Value.Substring(1, $mm.Value.Length - 2)
            if ($v.Length -ge 14 -and $v -notmatch '[%\\]') { $lits.Add($v) }
        }
    }
    $lits = @($lits | Select-Object -Unique)
    if (-not $lits.Count) { Write-Host "  (no new string literals in the uncommitted changes to look for)" }
    else {
        $miss = 0
        foreach ($l in $lits) { if (-not $text.Contains($l)) { $miss++; Fail ("not in the runtime: """ + $l + """") } }
        if (-not $miss) { Pass ("all " + $lits.Count + " new string(s) from your changes are in the runtime exe") }
    }
}

if ($Install -and -not $fail.Count) {
    Write-Host "== install"
    if (-not $Target) { $Target = Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $root))) 'builds\v0.4.0+base\smm2-dx_data' }
    $dst = Join-Path $Target 'smm2-dx-runtime.exe'
    try {
        if (Test-Path $dst) { Copy-Item $dst ($dst + '.bak') -Force }
        Copy-Item $rt $dst -Force
        if ((Get-FileHash $rt).Hash -eq (Get-FileHash $dst).Hash) { Pass ("copied to " + $dst + " (identical; old one kept as .bak)") }
        else { Fail "the copy differs from the build" }
    } catch { Fail ("could not copy (is the game running?): " + $_.Exception.Message) }
}

Write-Host ""
if ($fail.Count) { Write-Host ("REBUILD NOT CONFIRMED: " + $fail.Count + " check(s) failed") -ForegroundColor Red; exit 1 }
Write-Host "REBUILD CONFIRMED" -ForegroundColor Green
exit 0
