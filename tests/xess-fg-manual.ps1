<#
.SYNOPSIS
  Manual XeSS frame generation (XeFG) check for PushMPC, intended for Intel Arc owners.

.DESCRIPTION
  Runs xefg_headless_spike.exe (XeFG through a headless swap chain shim: no window, no
  real swap chain) at real video sizes and prints PASS/FAIL, the number of generated
  frames per real frame, the cost per generated frame and the image sanity numbers.

  Per run the tool renders a textured square moving at a known speed, feeds exact motion
  vectors and constant depth, and compares every generated frame with an ideally rendered
  intermediate frame (square position error in pixels, PSNR in the square's region) and
  with a plain 50/50 blend of the two real frames.

  PASS for a run needs: all XeFG init calls succeed, >= 90 % of the expected generated
  frames, median position error <= 2 px, and median PSNR above the blend baseline.

  Paste the whole output (and the report.txt files in -OutDir if asked) into the issue.

.PARAMETER Tool
  Path to xefg_headless_spike.exe. libxess_fg.dll and libxell.dll must be next to it.

.EXAMPLE
  .\xess-fg-manual.ps1 -Tool C:\temp\xefg-spike\xefg_headless_spike.exe
#>
param(
    [Parameter(Mandatory = $true)][string]$Tool,
    [string]$OutDir = (Join-Path (Get-Location) 'xess-fg-manual-out'),
    [string[]]$Sizes = @('1920x1080', '3840x2160'),
    [string[]]$Interp = @('1', '2', '3'),
    [int]$Frames = 40,
    [int]$TimingFrames = 150,
    [switch]$Warp
)

$ErrorActionPreference = 'Stop'
# Accept "1,2,3" as one string too (pwsh -File passes arrays that way).
$interpList = [int[]]@($Interp | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { [int]$_ })
$sizeList = @($Sizes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not (Test-Path $Tool)) { throw "Tool not found: $Tool" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

Write-Host "PushMPC XeSS-FG manual check"
Write-Host ("OS: {0}" -f [Environment]::OSVersion.VersionString)
Get-CimInstance Win32_VideoController | ForEach-Object { Write-Host ("GPU: {0}  driver {1}" -f $_.Name, $_.DriverVersion) }

function Get-Value([string[]]$lines, [string]$prefix) {
    $l = $lines | Where-Object { $_ -like "$prefix*" } | Select-Object -First 1
    if ($l) { return $l.Substring($prefix.Length).Trim() } else { return '' }
}
function Get-Field([string]$line, [string]$name) {
    if ($line -match "(^|\s)$name=([^\s]+)") { return $Matches[2] } else { return '' }
}

$rows = @()
foreach ($size in $sizeList) {
    $w, $h = $size.Split('x') | ForEach-Object { [int]$_ }
    $square = [math]::Max(24, [math]::Round($h * 0.19))
    $vx = [math]::Round($w * 0.0094, 2); $vy = [math]::Round($h * 0.0042, 2)
    foreach ($k in $interpList) {
        $name = "{0}-{1}x" -f $size, ($k + 1)
        $dir = Join-Path $OutDir $name
        $toolArgs = @('--interp', $k, '--width', $w, '--height', $h, '--square', $square, '--speed', "$vx,$vy",
                  '--frames', $Frames, '--timing-frames', $TimingFrames, '--verbose-calls', '2', '--out', $dir)
        if ($Warp) { $toolArgs += '--warp' }
        if ($w -ge 3000) { $toolArgs += @('--max-captures', '400') }
        Write-Host ""
        Write-Host "== $name"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        & $Tool @toolArgs *> (Join-Path $OutDir "$name.console.txt")
        $code = $LASTEXITCODE
        $sw.Stop()
        $report = Join-Path $dir 'report.txt'
        $lines = if (Test-Path $report) { Get-Content $report } else { @() }
        $adapter = Get-Value $lines 'adapter '
        $summary = Get-Value $lines 'SUMMARY '
        $result = Get-Value $lines 'SPIKE_RESULT '
        $ceiling = Get-Value $lines 'runtime maxSupportedInterpolations='
        $timingOn = Get-Value $lines 'timing fgOn '
        $timingOff = Get-Value $lines 'timing fgOff '
        if (-not $result) { $result = "FAIL (no result, exit $code)" }
        $status = if ($result -like 'PASS*') { 'PASS' } elseif ($k -gt 1 -and $result -like '*runtime ceiling*') { 'N/A (no MFG)' } else { 'FAIL' }
        Write-Host ("adapter: {0}" -f $adapter)
        Write-Host ("result : {0}  ({1})  runtime ceiling={2}  wall={3:N1}s" -f $status, $result, $ceiling, $sw.Elapsed.TotalSeconds)
        Write-Host ("summary: {0}" -f $summary)
        Write-Host ("timing : on[{0}]  off[{1}]" -f $timingOn, $timingOff)
        $rows += [pscustomobject]@{
            Run          = $name
            Status       = $status
            GenPerReal   = Get-Field $summary 'genPerReal'
            MsPerGen     = Get-Field $summary 'msPerGenerated'
            OutPerSecOn  = Get-Field $timingOn 'outputsPerSec'
            OutPerSecOff = Get-Field $timingOff 'outputsPerSec'
            PosErrPx     = Get-Field $summary 'medianPosErrPx'
            PsnrRoi      = Get-Field $summary 'medianPsnrRoi'
            PsnrBlend    = Get-Field $summary 'medianPsnrBlendRoi'
        }
    }
}

Write-Host ""
Write-Host "==== XeSS-FG headless results"
$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
Write-Host "MsPerGen = added wall-clock time per generated frame with XeFG on vs off (unthrottled, includes XeFG pacing)."
Write-Host "PsnrRoi/PsnrBlend = median PSNR (dB) of the generated frame / of a 50-50 blend vs the ideal frame, square region."
$required = $rows | Where-Object { $_.Run -like '*-2x' }
$overall = if ($required.Count -gt 0 -and -not ($required | Where-Object { $_.Status -ne 'PASS' })) { 'PASS' } else { 'FAIL' }
Write-Host ("OVERALL (2X runs): {0}" -f $overall)
Write-Host "Reports: $OutDir"
if ($overall -ne 'PASS') { exit 1 }
