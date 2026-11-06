# What the Build Helper shows: the picture (tools\banner.txt) pinned on the left of the window, and the log in a panel
# beside it that redraws as lines arrive, so the text scrolls around the picture. In a console without VT support, one
# that is too small, or when the output goes to a file, it falls back to plain lines. Dot-sourced by dx-common.ps1.
#
# Everything the build says goes through Write-Step, Write-Note, Write-Out and Write-Bar.

$script:T0 = Get-Date
$script:Live = -not [Console]::IsOutputRedirected    # a console that can redraw (otherwise: plain lines, for logs)
$script:Panel = $false                               # the picture-and-panel layout is on
$script:LogLines = New-Object Collections.ArrayList  # the panel's wrapped lines: @{ T = text; C = SGR color code }
$script:Status = ''                                  # the panel's bottom line: the progress bar
$script:Esc = [string][char]27
$script:P = @{ Left = 0; Width = 0; Top = 0; Rows = 0; StatusRow = 0; Height = 0 }

# SGR color codes: 35 magenta, 36 cyan, 90 gray, 32 green, 31 red, 33 yellow, 97 white
function Enable-Vt {
    try {
        if (-not ('DxWin.Con' -as [type])) {
            Add-Type -Namespace DxWin -Name Con -ErrorAction Stop -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern System.IntPtr GetStdHandle(int h);
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern bool GetConsoleMode(System.IntPtr h, out uint m);
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern bool SetConsoleMode(System.IntPtr h, uint m);
'@
        }
        $h = [DxWin.Con]::GetStdHandle(-11)
        [uint32]$m = 0
        if (-not [DxWin.Con]::GetConsoleMode($h, [ref]$m)) { return $false }
        if (($m -band 4) -eq 0) { if (-not [DxWin.Con]::SetConsoleMode($h, ($m -bor 4))) { return $false } }
        return $true
    } catch { return $false }
}

# ---- the picture: braille characters are a 2x4 dot grid each, so it can be shrunk to fit a small window
function Get-ArtLines([int]$MaxRows, [int]$MaxCols) {
    $path = Join-Path $PSScriptRoot 'banner.txt'
    if (-not (Test-Path $path)) { return @() }
    $lines = @([IO.File]::ReadAllText($path, [Text.Encoding]::UTF8).TrimEnd() -split "`r?`n")
    $nRows = $lines.Count
    $nCols = 0; foreach ($l in $lines) { if ($l.Length -gt $nCols) { $nCols = $l.Length } }
    $s = 1
    while ($s -lt 8 -and ([Math]::Ceiling($nRows / $s) -gt $MaxRows -or [Math]::Ceiling($nCols / $s) -gt $MaxCols)) { $s++ }
    if ($s -eq 1) { return $lines }
    $pxH = $nRows * 4; $pxW = $nCols * 2
    $bm = New-Object 'bool[,]' $pxH, $pxW
    $dx = 0, 0, 0, 1, 1, 1, 0, 1          # the dot for each bit of a braille character: x
    $dy = 0, 1, 2, 0, 1, 2, 3, 3          #                                              y
    for ($r = 0; $r -lt $nRows; $r++) {
        $l = $lines[$r]
        for ($c = 0; $c -lt $l.Length; $c++) {
            $bits = [int]$l[$c] - 0x2800
            if ($bits -le 0 -or $bits -gt 255) { continue }
            for ($b = 0; $b -lt 8; $b++) { if ($bits -band (1 -shl $b)) { $bm[($r * 4 + $dy[$b]), ($c * 2 + $dx[$b])] = $true } }
        }
    }
    $pxH2 = [int][Math]::Ceiling($pxH / $s); $pxW2 = [int][Math]::Ceiling($pxW / $s)
    $need = if ($s -le 2) { 1 } else { [Math]::Max(1, [int]($s * $s * 0.3)) }   # (thin lines survive a small shrink; larger ones need some weight)
    $bm2 = New-Object 'bool[,]' $pxH2, $pxW2
    for ($y = 0; $y -lt $pxH2; $y++) {
        for ($x = 0; $x -lt $pxW2; $x++) {
            $n = 0
            for ($j = 0; $j -lt $s; $j++) { for ($i = 0; $i -lt $s; $i++) {
                $yy = $y * $s + $j; $xx = $x * $s + $i
                if ($yy -lt $pxH -and $xx -lt $pxW -and $bm[$yy, $xx]) { $n++ }
            } }
            if ($n -ge $need) { $bm2[$y, $x] = $true }
        }
    }
    $nRows2 = [int][Math]::Ceiling($pxH2 / 4); $nCols2 = [int][Math]::Ceiling($pxW2 / 2)
    $outLines = @()
    for ($r = 0; $r -lt $nRows2; $r++) {
        $sb = New-Object Text.StringBuilder
        for ($c = 0; $c -lt $nCols2; $c++) {
            $bits = 0
            for ($b = 0; $b -lt 8; $b++) {
                $yy = $r * 4 + $dy[$b]; $xx = $c * 2 + $dx[$b]
                if ($yy -lt $pxH2 -and $xx -lt $pxW2 -and $bm2[$yy, $xx]) { $bits = $bits -bor (1 -shl $b) }
            }
            [void]$sb.Append([char](0x2800 + $bits))
        }
        $outLines += $sb.ToString()
    }
    $outLines
}

# ---- the panel
function Start-Panel([string]$Version, [int]$Jobs) {
    if (-not $script:Live) { return $false }
    try {
        $size = $Host.UI.RawUI.WindowSize
        $W = $size.Width; $H = $size.Height
        if ($W -lt 90 -or $H -lt 22) { return $false }
        if (-not (Enable-Vt)) { return $false }
        [Console]::OutputEncoding = [Text.Encoding]::UTF8
        $art = @(Get-ArtLines ($H - 2) ([int]($W * 0.42)))
        if ($art.Count -eq 0) { return $false }
        $artCols = 0; foreach ($l in $art) { if ($l.Length -gt $artCols) { $artCols = $l.Length } }
        $left = $artCols + 3
        $width = $W - $left - 1
        if ($width -lt 44) { return $false }
    } catch { return $false }
    $e = $script:Esc
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append("$e[2J$e[H$e[?25l")
    for ($i = 0; $i -lt $art.Count; $i++) { [void]$sb.Append("$e[$($i + 1);1H$e[35m$($art[$i])$e[0m") }
    $script:P = @{ Left = $left; Width = $width; Top = 6; Rows = ($H - 8); StatusRow = ($H - 1); Height = $H }
    [void]$sb.Append("$e[1;${left}H$e[33msmm2-dx$e[97m  Build Helper$e[0m")
    [void]$sb.Append("$e[2;${left}H$e[90m$Version  -  $Jobs jobs$e[0m")
    [void]$sb.Append("$e[3;${left}H$e[33mby sometimes_steve$e[0m")
    [void]$sb.Append("$e[4;${left}H$e[35m$('-' * [Math]::Min($width, 36))$e[0m")
    Write-Host $sb.ToString() -NoNewline
    $script:Panel = $true
    $true
}

function Stop-Panel {
    if (-not $script:Panel) { return }
    $script:Panel = $false
    $e = $script:Esc
    Write-Host "$e[?25h$e[$($script:P.Height);1H" -NoNewline
    Write-Host ''
}

function Update-Panel {
    $p = $script:P; $e = $script:Esc
    $sb = New-Object Text.StringBuilder
    $n = $script:LogLines.Count
    $first = [Math]::Max(0, $n - $p.Rows)
    for ($i = 0; $i -lt $p.Rows; $i++) {
        $row = $p.Top + $i
        $idx = $first + $i
        if ($idx -lt $n) { $ln = $script:LogLines[$idx]; $t = $ln.T.PadRight($p.Width); $c = $ln.C } else { $t = ' ' * $p.Width; $c = 90 }
        [void]$sb.Append("$e[$row;$($p.Left)H$e[${c}m$t$e[0m")
    }
    Write-Host $sb.ToString() -NoNewline
    Update-Status
}

function Update-Status {
    $p = $script:P; $e = $script:Esc
    $t = $script:Status
    if ($t.Length -gt $p.Width) { $t = $t.Substring(0, $p.Width) }
    Write-Host "$e[$($p.StatusRow);$($p.Left)H$e[36m$($t.PadRight($p.Width))$e[0m" -NoNewline
}

# adds text to the panel's log, wrapped to its width
function Add-Log([string]$Text, [int]$Color = 90, [string]$Indent = '') {
    $w = $script:P.Width
    foreach ($line in ($Text -split "`r?`n")) {
        $line = $line.TrimEnd()
        $pre = $Indent
        do {
            $room = [Math]::Max(10, $w - $pre.Length)
            if ($line.Length -le $room) { [void]$script:LogLines.Add(@{ T = $pre + $line; C = $Color }); $line = '' }
            else { [void]$script:LogLines.Add(@{ T = $pre + $line.Substring(0, $room); C = $Color }); $line = $line.Substring($room); $pre = $Indent + '  ' }
        } while ($line.Length -gt 0)
    }
    Update-Panel
}

# ---- what the build says
function Write-Step([string]$Text) {
    $el = (Get-Date) - $script:T0
    $stamp = '[{0:00}:{1:00}] ' -f [int][Math]::Floor($el.TotalMinutes), $el.Seconds
    if ($script:Panel) { Add-Log ($stamp + $Text) 36; return }
    Write-Host ('[smm2-dx ' + $stamp.TrimStart('[')) -ForegroundColor Magenta -NoNewline
    Write-Host $Text -ForegroundColor Cyan
}

function Write-Note([string]$Text) {
    if ($script:Panel) { Add-Log $Text 90 '  '; return }
    Write-Host "    $Text" -ForegroundColor DarkGray
}

# output of a program, or any other text: -Color is an SGR code
function Write-Out([string]$Text, [int]$Color = 90) {
    if ($script:Panel) { Add-Log $Text $Color '  '; return }
    $map = @{ 31 = 'Red'; 32 = 'Green'; 33 = 'Yellow'; 36 = 'Cyan'; 90 = 'Gray'; 97 = 'White' }
    foreach ($l in ($Text -split "`r?`n")) { Write-Host $l -ForegroundColor $map[$Color] }
}

# One line that redraws itself: [#######-------------] 35%  text
function Write-Bar([long]$Done, [long]$Total, [string]$Text) {
    $pct = if ($Total -gt 0) { [int](100 * $Done / $Total) } else { 0 }
    $fill = [int]($pct / 5)
    $bar = ('#' * $fill) + ('-' * (20 - $fill))
    $line = "[{0}] {1,3}%  {2}" -f $bar, $pct, $Text
    if ($script:Panel) { $script:Status = $line; Update-Status; return }
    Write-Host ("`r    " + $line + '   ') -NoNewline -ForegroundColor DarkCyan
}

# a spinner with the time a step has taken: for steps that print nothing while they work
$script:Spin = 0
function Show-Busy([string]$Label, [Diagnostics.Stopwatch]$Watch) {
    if (-not $script:Live) { return }
    $script:Spin++
    $frame = '|', '/', '-', '\'
    $line = '[{0}] {1}  {2:N0}s' -f $frame[$script:Spin % 4], $Label, $Watch.Elapsed.TotalSeconds
    if ($script:Panel) { $script:Status = $line; Update-Status } else { Write-Host ("`r    " + $line + '   ') -NoNewline -ForegroundColor DarkCyan }
}

# the bar goes away when a step ends
function Clear-Bar {
    if ($script:Panel) { $script:Status = ''; Update-Status } elseif ($script:Live) { Write-Host '' }
}

function Show-Banner([string]$Version, [int]$Jobs, [switch]$NoArt) {
    try { $Host.UI.RawUI.WindowTitle = "smm2-dx Build Helper $Version" } catch { }
    if (-not $NoArt -and (Start-Panel $Version $Jobs)) { return }
    # plain layout: the title block (and, in a console, the picture shrunk to fit above it)
    Write-Host ''
    if (-not $NoArt -and $script:Live) {
        try {
            [Console]::OutputEncoding = [Text.Encoding]::UTF8
            $size = $Host.UI.RawUI.WindowSize
            foreach ($l in (Get-ArtLines ([Math]::Max(8, $size.Height - 8)) ($size.Width - 2))) { Write-Host $l -ForegroundColor Magenta }
            Write-Host ''
        } catch { }
    }
    Write-Host '  ====================================' -ForegroundColor Magenta
    Write-Host '   smm2-dx  ' -ForegroundColor Yellow -NoNewline
    Write-Host "Build Helper  ($Version, $Jobs jobs)" -ForegroundColor White
    Write-Host '   by sometimes_steve' -ForegroundColor DarkYellow
    Write-Host '  ====================================' -ForegroundColor Magenta
    Write-Host ''
}
