<#
.SYNOPSIS
  Manual check of RTX Video Super Resolution on an AMD RX 7000 / RX 9000 card through ZLUDA (unlock ZludaVsr).

.DESCRIPTION
  NVIDIA's VSR library (nvngx_vsr.dll) carries its kernels as PTX as well as GPU binaries, and exports NGX's CUDA entry
  points. ZLUDA (a CUDA driver that runs on AMD's HIP) translates PTX for Radeon. This script runs vsr_gate_probe.exe
  with ZLUDA's nvcuda.dll on a test picture and checks the result (picture kept, sharper than bicubic, no crash), and
  times it at 1080p -> 4K and 540p -> 1080p.

  You need: an RX 7000 or RX 9000, AMD's HIP SDK / runtime 6.x or newer (amdhip64_6.dll or amdhip64_7.dll), and a ZLUDA
  build for Windows (its folder with nvcuda.dll). PushMPC does not download ZLUDA; point -Zluda at your copy.

  With -Record the result is stored (HKCU\Software\PushMPC\SelfTest\ZludaVsr, keyed by this card and driver) for the
  integration that follows a PASS; the unlock stays off by default until such results exist. Off switch:
  HKCU\Software\PushMPC\Unlocks ZludaVsr (DWORD) = 0, or PUSHMPC_DISABLE_ZLUDAVSR=1.

  Paste the whole output into the issue (adapter name, device ID and driver version only).

.EXAMPLE
  .\zluda-vsr-manual.ps1 -Tool C:\temp\vsr_gate_probe.exe -Zluda C:\tools\zluda -Runtime C:\temp\nvidia-vfx\libs -Record
#>
param(
    [Parameter(Mandatory = $true)][string]$Tool,
    [Parameter(Mandatory = $true)][string]$Zluda,
    [Parameter(Mandatory = $true)][string]$Runtime,
    [string]$OutDir = (Join-Path (Get-Location) 'zluda-vsr-manual-out'),
    [switch]$Record
)
$ErrorActionPreference = 'Stop'
foreach ($p in @($Tool, (Join-Path $Zluda 'nvcuda.dll'), (Join-Path $Runtime 'nvngx_vsr.dll'))) { if (-not (Test-Path $p)) { throw "not found: $p" } }
$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.PNPDeviceID -match 'VEN_1002' } | Select-Object -First 1
Write-Host ("GPU: {0}  driver {1}" -f $gpu.Name, $gpu.DriverVersion)
if ($gpu.Name -notmatch 'RX\s*[79]\d{3}') { Write-Host 'Note: the route targets RX 7000 / RX 9000; other Radeons are reported but not enabled.' -ForegroundColor Yellow }
$hip = @('amdhip64_7.dll', 'amdhip64_6.dll') | Where-Object { Test-Path (Join-Path "$env:WINDIR\System32" $_) } | Select-Object -First 1
Write-Host ("HIP runtime: {0}" -f $(if ($hip) { $hip } else { 'NOT FOUND (install AMD HIP SDK / a driver that ships it)' }))
New-Item -ItemType Directory -Force $OutDir | Out-Null
$env:PATH = "$Zluda;$env:PATH"   # nvngx_vsr.dll loads "nvcuda.dll": ZLUDA's comes first
$results = @()
foreach ($r in @(@{ N = '540p -> 1080p'; A = @('--width', '960', '--height', '540') }, @{ N = '1080p -> 4K'; A = @('--width', '1920', '--height', '1080') })) {
    Write-Host "`n=== $($r.N) ==="
    $extra = @(); if ($Record -and $r.N -eq '1080p -> 4K') { $extra = @('--record', 'ZludaVsr') }
    $out = & $Tool --runtime $Runtime --route cuda --cuda-dll (Join-Path $Zluda 'nvcuda.dll') --scale 2 --quality 1 --out (Join-Path $OutDir ($r.N -replace '[^0-9A-Za-z]+', '-')) @($r.A) @extra 2>&1
    $out | ForEach-Object { Write-Host $_ }
    $results += @($out | Select-String '^RESULT route=cuda ran=(\d) pass=(\d) psnr_vs_bicubic=([\d.]+) sharpness=([\d.]+) ms_median=(-?[\d.]+)' | ForEach-Object {
        [pscustomobject]@{ Run = $r.N; Pass = $_.Matches[0].Groups[2].Value -eq '1'; Psnr = [double]$_.Matches[0].Groups[3].Value; Ms = [double]$_.Matches[0].Groups[5].Value } })
}
$results | Format-Table -AutoSize | Out-String | Write-Host
$k = $results | Where-Object { $_.Run -eq '1080p -> 4K' -and $_.Pass } | Select-Object -First 1
if ($k) {
    Write-Host ("PASS: {0:N2} ms per frame at 1080p -> 4K (24p budget {1:N1} ms; PushMPC can use it with FSR frame generation when it fits)" -f $k.Ms, (1000 / 23.976 * 0.85)) -ForegroundColor Green
    exit 0
}
Write-Host 'FAIL: RTX VSR did not run through ZLUDA on this card (the log above says at which step).' -ForegroundColor Red
exit 1
