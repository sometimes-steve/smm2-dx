# Shared helpers for the PowerShell build (build-helper.ps1). Windows PowerShell 5.1 or newer; nothing else to install.
# Dot-sourced by build-helper.ps1; the other dx-*.ps1 files use what is defined here.
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # (Invoke-WebRequest and Expand-Archive crawl with the progress bar on)

# recomp/, the repo's dist folder (the finished app), and the one folder beside the exe that holds everything the app keeps
$script:Recomp = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script:Out    = [IO.Path]::GetFullPath((Join-Path $Recomp '..\dist'))
$script:DxName = 'smm2-dx_data'
$script:Dx     = Join-Path $Out $DxName
$script:Work   = Join-Path $Dx 'work'          # objects, and the fetched compiler, libraries and tools
$script:Dl     = Join-Path $Work 'dl'          # downloads, kept so a fetch isn't repeated
$script:SysTar = Join-Path $env:SystemRoot 'System32\tar.exe'    # bsdtar: zip, tar.gz and tar.zst (not Git's GNU tar)
$script:SysCurl = Join-Path $env:SystemRoot 'System32\curl.exe'

. (Join-Path $PSScriptRoot 'dx-console.ps1')   # Write-Step, Write-Note, Write-Out, Write-Bar, Show-Banner

# One argument for a Windows command line (CommandLineToArgvW rules), so flags such as -DX="a b" arrive as written.
function ConvertTo-Arg([string]$a) {
    if ($a -eq '') { return '""' }
    if ($a -notmatch '[\s"]') { return $a }
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append('"')
    $bs = 0
    foreach ($c in $a.ToCharArray()) {
        if ($c -eq [char]92) { $bs++ }
        elseif ($c -eq [char]34) { [void]$sb.Append([char]92, $bs * 2 + 1); [void]$sb.Append([char]34); $bs = 0 }
        else { if ($bs) { [void]$sb.Append([char]92, $bs); $bs = 0 }; [void]$sb.Append($c) }
    }
    if ($bs) { [void]$sb.Append([char]92, $bs * 2) }
    [void]$sb.Append('"')
    $sb.ToString()
}

function Start-Proc($job) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $job.Exe
    $psi.Arguments = (@($job.Args) | ForEach-Object { ConvertTo-Arg ([string]$_) }) -join ' '
    $psi.WorkingDirectory = $(if ($job.ContainsKey('Dir') -and $job.Dir) { $job.Dir } else { $script:Recomp })
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = [Diagnostics.Process]::Start($psi)
    $job.Proc = $p
    $job.OutTask = $p.StandardOutput.ReadToEndAsync()
    $job.ErrTask = $p.StandardError.ReadToEndAsync()
}

# Runs a program and waits; throws with its output when it fails. Returns its output.
function Invoke-Proc([string]$Exe, [object[]]$Arguments, [string]$Dir = '', [switch]$Quiet) {
    $job = @{ Exe = $Exe; Args = $Arguments; Dir = $Dir }
    Start-Proc $job
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $label = [IO.Path]::GetFileNameWithoutExtension($Exe)
    while (-not $job.Proc.WaitForExit(200)) { if ($sw.Elapsed.TotalSeconds -ge 1.5) { Show-Busy "working ($label)" $sw } }
    if ($sw.Elapsed.TotalSeconds -ge 1.5) { Clear-Bar }
    $job.Proc.WaitForExit()
    $text = $job.OutTask.Result + $job.ErrTask.Result
    if ($job.Proc.ExitCode -ne 0) { throw "$([IO.Path]::GetFileName($Exe)) failed (exit $($job.Proc.ExitCode)):`n$text" }
    if (-not $Quiet -and $text.Trim()) { Write-Out $text.TrimEnd() }
    $text
}

# Runs many programs, a few at a time. Each job: @{ Name; Exe; Args; Dir }. Throws after all have finished if any failed.
function Invoke-Parallel([object[]]$Jobs, [int]$Max, [string]$What) {
    $Jobs = @($Jobs)
    if ($Jobs.Count -eq 0) { return }
    if ($Max -lt 1) { $Max = 1 }
    $total = $Jobs.Count; $next = 0; $done = 0; $running = @(); $failed = @()
    $step = [Math]::Max(1, [int]($total / 10)); $shown = 0
    while ($done -lt $total) {
        while ($running.Count -lt $Max -and $next -lt $total) { $j = $Jobs[$next]; $next++; Start-Proc $j; $running += $j }
        Start-Sleep -Milliseconds 10
        $still = @()
        foreach ($j in $running) {
            if ($j.Proc.HasExited -and $j.OutTask.IsCompleted -and $j.ErrTask.IsCompleted) {
                $done++
                $text = ($j.ErrTask.Result + $j.OutTask.Result).Trim()
                if ($j.Proc.ExitCode -ne 0) { $failed += ,@($j.Name, $text) }
                elseif ($text -and $script:ShowWarnings) { Write-Host "$($j.Name):`n$text" -ForegroundColor Yellow }
                if ($script:Live) { Write-Bar $done $total "$What $done/$total" }
                elseif ($done - $shown -ge $step -or $done -eq $total) { $shown = $done; Write-Note "$What $done/$total" }
            } else { $still += $j }
        }
        $running = $still
    }
    Clear-Bar
    if ($failed.Count) {
        Stop-Panel   # (the compiler's messages are long: they go below the layout)
        foreach ($f in $failed) { Write-Host "FAILED $($f[0])`n$($f[1])" -ForegroundColor Red }
        throw "$($failed.Count) of $total failed ($What)"
    }
}

# A file or folder that was just written may be held for a moment by antivirus: move it again a few times
function Move-Retry([string]$From, [string]$To) {
    for ($i = 1; ; $i++) {
        try { Move-Item -LiteralPath $From -Destination $To -Force; return }
        catch { if ($i -ge 20) { throw }; Start-Sleep -Milliseconds 500 }
    }
}

function Get-Sha256([string]$File) { (Get-FileHash -Algorithm SHA256 -LiteralPath $File).Hash.ToLowerInvariant() }

# Downloads a file (curl.exe comes with Windows 10 and later) and checks it against its pinned sha256; kept in
# work/dl so it is fetched once.
function Get-RemoteSize([string]$Url) {
    try {
        $t = Invoke-Proc $SysCurl @('-sIL', '--proto', '=https', '--retry', '2', $Url) -Quiet
        $m = [regex]::Matches($t, '(?im)^content-length:\s*(\d+)')
        if ($m.Count) { return [long]$m[$m.Count - 1].Groups[1].Value }
    } catch { }
    0
}

function Get-Verified([string]$Url, [string]$File, [string]$Sha256) {
    New-Item -ItemType Directory -Force (Split-Path $File) | Out-Null
    if ((Test-Path $File) -and (Get-Sha256 $File) -eq $Sha256) { return }
    Remove-Item $File -Force -ErrorAction SilentlyContinue
    Write-Note "downloading $([IO.Path]::GetFileName($File))"
    $part = "$File.part"
    # retries on any error (a dropped connection included) and continues the partial file where it stopped
    $base = @('-fL', '--proto', '=https', '-C', '-', '--retry', '5', '--retry-delay', '2', '--retry-all-errors')
    $code = 1
    for ($try = 1; $try -le 3 -and $code -ne 0; $try++) {
        if ($try -gt 1) { Write-Note "retrying ($try of 3)"; Start-Sleep -Seconds 3 }
        if ($script:Panel) {
            # the panel draws the bar: curl runs quietly and the partial file's size is watched
            $total = Get-RemoteSize $Url
            $job = @{ Exe = $SysCurl; Args = (@($base) + @('-sS', '-o', $part, $Url)); Dir = '' }
            Start-Proc $job
            $name = [IO.Path]::GetFileName($File)
            while (-not $job.Proc.HasExited) {
                $sz = 0; if (Test-Path $part) { $sz = (Get-Item $part).Length }
                $txt = if ($total -gt 0) { '{0:N0} / {1:N0} MB  {2}' -f ($sz / 1MB), ($total / 1MB), $name } else { '{0:N0} MB  {1}' -f ($sz / 1MB), $name }
                Write-Bar $sz $total $txt
                Start-Sleep -Milliseconds 200
            }
            $code = $job.Proc.ExitCode
            Clear-Bar
            if ($code -ne 0) { Write-Note ($job.ErrTask.Result.Trim()) }
        } elseif ($script:Live) {
            # curl draws its own progress bar on the console
            $p = Start-Process $SysCurl -ArgumentList ((@($base) + @('--progress-bar', '-o', $part, $Url)) | ForEach-Object { ConvertTo-Arg $_ }) -NoNewWindow -Wait -PassThru
            Write-Host ''
            $code = $p.ExitCode
        } else {
            $job = @{ Exe = $SysCurl; Args = (@($base) + @('-sS', '-o', $part, $Url)); Dir = '' }
            Start-Proc $job; $job.Proc.WaitForExit()
            $code = $job.Proc.ExitCode
            if ($code -ne 0) { Write-Note ($job.ErrTask.Result.Trim()) }
        }
        if ($code -in 33, 36) { Remove-Item $part -Force -ErrorAction SilentlyContinue }   # (the server can't continue a download: start again)
    }
    if ($code -ne 0) { throw "download failed (curl exit $code): $Url`nDownload it by hand and save it as $File (see the README)" }
    Move-Retry $part $File
    $got = Get-Sha256 $File
    if ($got -ne $Sha256) { Remove-Item $File -Force; throw "$([IO.Path]::GetFileName($File)): sha256 $got, expected $Sha256" }
}

# bsdtar reads zip, tar.gz and tar.zst alike
function Expand-Archive2([string]$Archive, [string]$Dest, [int]$Strip = 0) {
    New-Item -ItemType Directory -Force $Dest | Out-Null
    $a = @('-xf', $Archive, '-C', $Dest)
    if ($Strip) { $a += "--strip-components=$Strip" }
    Write-Note "unpacking $([IO.Path]::GetFileName($Archive))"
    [void](Invoke-Proc $SysTar $a -Quiet)
}

# robocopy returns 0-7 for success
function Copy-Tree([string]$From, [string]$To, [string[]]$ExcludeDirs = @()) {
    New-Item -ItemType Directory -Force $To | Out-Null
    $a = @($From, $To, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/R:1', '/W:1')
    if ($ExcludeDirs.Count) { $a += '/XD'; $a += $ExcludeDirs }
    $p = Start-Process robocopy -ArgumentList ($a | ForEach-Object { ConvertTo-Arg $_ }) -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $env:TEMP 'dx-robocopy.log')
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (-not $p.WaitForExit(200)) { if ($sw.Elapsed.TotalSeconds -ge 1.5) { Show-Busy 'copying' $sw } }
    if ($sw.Elapsed.TotalSeconds -ge 1.5) { Clear-Bar }
    if ($p.ExitCode -ge 8) { throw "robocopy $From -> $To failed ($($p.ExitCode))" }
}

# Removes a folder; a junction or link inside is unlinked, never followed
function Remove-Tree([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    Get-ChildItem -LiteralPath $Path -Recurse -Force -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } |
        ForEach-Object { cmd /c rmdir "`"$($_.FullName)`"" | Out-Null }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { cmd /c rmdir "`"$Path`"" | Out-Null; return }
    Remove-Item -LiteralPath $Path -Recurse -Force
}

function Write-TextFile([string]$Path, [string]$Text, [switch]$Crlf) {
    New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
    if ($Crlf) { $Text = $Text -replace "`r?`n", "`r`n" }
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

# forward slashes: what the compiler's command line (and its @response files) take without escaping
function ConvertTo-Fwd([string]$p) { $p.Replace('\', '/') }
