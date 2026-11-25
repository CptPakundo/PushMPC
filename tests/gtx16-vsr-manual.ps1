<#
.SYNOPSIS
  Manual check of RTX Video Super Resolution on a GeForce GTX 16 card (unlock Gtx16Vsr), for GTX 1660 / 1650 owners.

.DESCRIPTION
  GTX 16 cards (TU116 / TU117) are compute capability 7.5, the same instruction set as RTX 20. NVIDIA's VSR library
  (nvngx_vsr.dll) ships complete kernels for it; NVIDIA lists VSR for RTX cards only. This script runs
  vsr_gate_probe.exe, which tries three ways to run VSR on a test picture and checks the result:
    core     NVIDIA's normal path (through the driver's NGX core): shows whether the gate exists at all;
    snippet  the VSR library called directly, without the driver's feature table;
  and prints PASS/FAIL per route, the time per frame at 1080p -> 4K and 540p -> 1080p, and the overall verdict.

  With -Record the result is stored for PushMPC (HKCU\Software\PushMPC\SelfTest\Gtx16Vsr, keyed by this card and driver):
  on a PASS PushMPC uses RTX VSR on this card from the next file on (Auto picks it when it fits the frame budget);
  on a FAIL it stays off with the reason. Undo: delete that key, or switch the unlock off with
  HKCU\Software\PushMPC\Unlocks  Gtx16Vsr (DWORD) = 0, or the environment variable PUSHMPC_DISABLE_GTX16VSR=1.

  If the card's driver crashes during a route (the screen flickers, Windows recovers the driver), the run stops there:
  paste the output so far into the issue; that is a FAIL for that route.

  Paste the whole output into the issue (no personal data is printed: adapter name, device ID and driver version only).

.PARAMETER Tool
  Path to vsr_gate_probe.exe (from the release's tools zip, or built with pushmpc\tools\vsr_gate_probe.cmd).

.PARAMETER Runtime
  Folder with nvngx_vsr.dll. Default: PushMPC's runtime (%LOCALAPPDATA%\PushMPC\runtime\nvidia-vfg), else a PushFPS install.

.EXAMPLE
  .\gtx16-vsr-manual.ps1 -Tool C:\temp\vsr_gate_probe.exe -Record
#>
param(
    [Parameter(Mandatory = $true)][string]$Tool,
    [string]$Runtime = '',
    [string]$OutDir = (Join-Path (Get-Location) 'gtx16-vsr-manual-out'),
    [switch]$Record
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Tool)) { throw "Tool not found: $Tool" }
if (-not $Runtime) {
    foreach ($c in @("$env:LOCALAPPDATA\PushMPC\runtime\nvidia-vfg", "$env:LOCALAPPDATA\PushMPC\runtime\experimental", "$env:LOCALAPPDATA\Programs\PushFPS\runtime\nvidia-vfg")) {
        if (Test-Path (Join-Path $c 'nvngx_vsr.dll')) { $Runtime = $c; break }
    }
}
if (-not $Runtime -or -not (Test-Path (Join-Path $Runtime 'nvngx_vsr.dll'))) { throw 'nvngx_vsr.dll not found: pass -Runtime <folder>' }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.PNPDeviceID -match 'VEN_10DE' } | Select-Object -First 1
Write-Host ("GPU: {0}  driver {1}" -f $gpu.Name, $gpu.DriverVersion)
if ($gpu.Name -notmatch 'GTX 16|GTX16|MX450|MX550') { Write-Host 'Note: this is not a GTX 16 card; the result is recorded only for GTX 16 cards.' -ForegroundColor Yellow }

$runs = @(
    @{ Name = '1080p -> 4K (quality 1)'; Args = @('--width', '1920', '--height', '1080', '--scale', '2', '--quality', '1') },
    @{ Name = '540p -> 1080p (quality 1)'; Args = @('--width', '960', '--height', '540', '--scale', '2', '--quality', '1') },
    @{ Name = '1080p -> 4K (quality 4)'; Args = @('--width', '1920', '--height', '1080', '--scale', '2', '--quality', '4') }
)
$results = @()
foreach ($r in $runs) {
    Write-Host "`n=== $($r.Name) ==="
    $sub = Join-Path $OutDir (($r.Name -replace '[^0-9A-Za-z]+', '-').Trim('-'))
    $extra = @(); if ($Record -and $r -eq $runs[0]) { $extra = @('--record', 'Gtx16Vsr') }
    $out = & $Tool --runtime $Runtime --route all --out $sub @($r.Args) @extra 2>&1
    $out | ForEach-Object { Write-Host $_ }
    $code = $LASTEXITCODE
    $routes = @($out | Select-String '^RESULT route=(\w+) ran=(\d) pass=(\d) psnr_vs_bicubic=([\d.]+) sharpness=([\d.]+) ms_median=(-?[\d.]+)' | ForEach-Object {
        [pscustomobject]@{ Run = $r.Name; Route = $_.Matches[0].Groups[1].Value; Pass = $_.Matches[0].Groups[3].Value -eq '1'; Psnr = [double]$_.Matches[0].Groups[4].Value; Ms = [double]$_.Matches[0].Groups[6].Value } })
    $results += $routes
    if ($code -gt 1) { Write-Host "the probe stopped (exit $code)" -ForegroundColor Red }
}
Write-Host "`n=== Summary ==="
$results | Format-Table -AutoSize | Out-String | Write-Host
$pass1080 = $results | Where-Object { $_.Run -like '1080p -> 4K (quality 1)' -and $_.Pass } | Select-Object -First 1
if ($pass1080) {
    $budget = 1000.0 / 23.976 * 0.85
    $fits = $pass1080.Ms -gt 0 -and $pass1080.Ms -le $budget * 0.5
    Write-Host ("PASS via route {0}: {1:N2} ms per frame at 1080p -> 4K ({2})" -f $pass1080.Route, $pass1080.Ms, $(if ($fits) { 'fits half of the 24p budget: default ON' } else { 'over half of the 24p budget: Auto uses it only for smaller sizes / with frame generation off' })) -ForegroundColor Green
    if ($pass1080.Route -ne 'core') { Write-Host 'Only the direct (snippet) route works: PushMPC needs its NGX-core bypass in the graph for this card (report this).' -ForegroundColor Yellow }
    exit 0
}
Write-Host 'FAIL: no route produced a sane picture at 1080p -> 4K. RTX VSR stays off on this card.' -ForegroundColor Red
exit 1
