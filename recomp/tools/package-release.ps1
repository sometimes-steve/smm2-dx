# Packages dist\ into a release zip that contains none of the player's own data.
# Usage: powershell -ExecutionPolicy Bypass -File recomp\tools\package-release.ps1 -Version v0.1.0
# Output: dist\release\smm2-dx-<Version>-windows-x86_64.zip
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'

$root  = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$dist  = Join-Path $root 'dist'
$exe   = Join-Path $dist 'smm2-dx-windows-x86_64.exe'
$data  = Join-Path $dist 'smm2-dx_data'
if (-not (Test-Path $exe) -or -not (Test-Path $data)) { throw "Build first: $exe or $data is missing." }

$stage = Join-Path ([IO.Path]::GetTempPath()) "smm2-dx-release-$([guid]::NewGuid().ToString('N'))"
$out   = Join-Path $dist 'release'
$zip   = Join-Path $out "smm2-dx-$Version-windows-x86_64.zip"
try {
    New-Item -ItemType Directory -Force "$stage\smm2-dx_data", $out | Out-Null
    Copy-Item $exe $stage

    # Left out: the player's extracted game, firmware-derived data, Nintendo fonts, saves, logs, caches, their paths.
    robocopy $data "$stage\smm2-dx_data" /E /NFL /NDL /NJH /NJS /NP `
        /XD extracted cache work logs sysdata save `
        /XF launcher.ini *.bfttf *.log | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

    # Safety net: refuse to ship anything that looks like game or key material.
    $bad = Get-ChildItem $stage -Recurse -File | Where-Object {
        $_.Extension -in '.nca', '.nsp', '.xci', '.nso', '.bfttf', '.keys' -or $_.Name -in 'prod.keys', 'title.keys', 'launcher.ini' }
    if ($bad) { throw "Refusing to package:`n$($bad.FullName -join "`n")" }

    if (Test-Path $zip) { Remove-Item $zip }
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $stage .
    if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
    '{0} ({1:N1} MB)' -f $zip, ((Get-Item $zip).Length / 1MB)
}
finally { if (Test-Path $stage) { Remove-Item -Recurse -Force $stage } }
