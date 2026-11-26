<#
.SYNOPSIS
  Manual check of DLSS frame generation on a GeForce RTX 20 card (unlock TuringDlssg, the community port
  sdli1995/dlssg_for_sm86), for RTX 2060 / 2070 / 2080 / TITAN RTX / Quadro RTX owners.

.DESCRIPTION
  Setup downloads the port on RTX 20 cards (scripts\dlssg-port-fetch.ps1). pushcore runs its self-test the first time
  a video plays with DLSS-G on this card and driver: the port installs, DLSS-G starts and 240 generated frames come out
  valid = pass; anything else = fail, and PushMPC falls back to PushFlow / NVIDIA optical flow / FSR frame generation.

  Steps:
    1. Run this script once with -Before: it shows the card, the port's files and the current self-test state.
    2. In MPC-HC, open a 24 or 30 fps video, PushMPC's settings: frame generation method DLSS-G (or Auto), multiplier
       2x or more; play at least 30 seconds.
    3. Run this script again (no switch): it prints the self-test result, what pushcore chose, and the port's log.
  Paste the whole output into the issue (no personal data: adapter name, device ID, driver version, log lines only).

  Undo / switch off: HKCU\Software\PushMPC\Unlocks  TuringDlssg (DWORD) = 0, or PUSHMPC_DISABLE_TURINGDLSSG=1. To test
  again, delete HKCU\Software\PushMPC\SelfTest\TuringDlssg.

.PARAMETER Root
  PushMPC's install folder (default %LOCALAPPDATA%\PushMPC).
#>
param(
    [string]$Root = (Join-Path $env:LOCALAPPDATA 'PushMPC'),
    [switch]$Before
)
$ErrorActionPreference = 'Stop'
$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.PNPDeviceID -match 'VEN_10DE' } | Select-Object -First 1
if (-not $gpu) { Write-Host 'No NVIDIA GPU found.' -ForegroundColor Red; exit 1 }
$dev = if ("$($gpu.PNPDeviceID)" -match 'DEV_([0-9A-Fa-f]{4})') { $Matches[1] } else { '?' }
Write-Host ("GPU: {0}  device {1}  driver {2}" -f $gpu.Name, $dev, $gpu.DriverVersion)
if ($gpu.Name -notmatch 'RTX 20|TITAN RTX|Quadro RTX') { Write-Host 'Note: this is not an RTX 20 card; the port is used on RTX 20 only.' -ForegroundColor Yellow }

$backend = @(Get-ChildItem -LiteralPath $Root -Recurse -Filter 'sm86_backend.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match '\\community\\dlssg-sm75\\dlssg_sm86\\backend\\' })
if (-not $backend.Count) { Write-Host "The port is not installed under $Root (Setup fetches it on RTX 20 cards; check Setup's output)." -ForegroundColor Red; exit 1 }
$port = $backend[0].Directory.Parent.Parent.FullName
Write-Host "Port folder: $port"
$pins = @{ 'dlssg_sm86\runtime\nvngx_dlssg.dll' = 'CADEE1AA6DB3907BABDC3285DA2AEBC4553D92B89C38BA3EAE8ABD8BBA1BEA72'
           'dlssg_sm86\backend\sm86_backend.dll' = '93A1B4AC1AE0DD0853C45B59B16C3698035CC8E3A749E34C232EB579E602B8E2'
           'dlssg_sm86\kernels\fg_kernels.dll' = '9882A01F30DD65F36EA1FC6223827E6F86679B99EB09E108EACC51556EEA4E18' }
foreach ($k in $pins.Keys) {
    $p = Join-Path $port $k
    $ok = (Test-Path -LiteralPath $p) -and (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash -eq $pins[$k]
    Write-Host ("  {0} {1}" -f $(if ($ok) { 'ok  ' } else { 'BAD ' }), $k)
}

$key = 'HKCU:\Software\PushMPC\SelfTest\TuringDlssg'
$st = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
$mode = (Get-ItemProperty 'HKCU:\Software\PushMPC\Unlocks' -Name TuringDlssg -ErrorAction SilentlyContinue).TuringDlssg
Write-Host ("Unlock TuringDlssg: {0}" -f $(if ($null -eq $mode) { 'Auto' } elseif ($mode -eq 0) { 'off' } else { 'forced on' }))
if ($st) { Write-Host ("Self-test: {0}  key {1}  {2}  {3}{4}" -f $st.Result, $st.Key, $st.When, $st.Reason, $(if ($st.Running) { "  (running: $($st.Running))" } else { '' })) }
else { Write-Host 'Self-test: not run yet' }
if ($Before) { Write-Host "`nNow play a video with DLSS-G (or Auto) at 2x or more for 30 seconds, then run this script again without -Before."; exit 0 }

$logs = @(Get-ChildItem -LiteralPath $Root -Recurse -Include '*.log' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 20)
Write-Host "`n=== pushcore lines ==="
foreach ($l in $logs) {
    Select-String -LiteralPath $l.FullName -Pattern 'TuringDlssg|dlssg-port|RTX 20 DLSS-G|frame generation fallback|DLSS-G did not start' -ErrorAction SilentlyContinue |
        Select-Object -Last 15 | ForEach-Object { Write-Host "$($l.Name): $($_.Line)" }
}
$portLog = $logs | Where-Object { $_.Name -eq 'dlssg-port.log' } | Select-Object -First 1
if ($portLog) { Write-Host "`n=== the port's log (last 40 lines) ==="; Get-Content -LiteralPath $portLog.FullName -Tail 40 | ForEach-Object { Write-Host $_ } }
if ($st -and $st.Result -eq 'pass') { Write-Host "`nPASS: DLSS-G runs on this RTX 20 card; Auto uses it where it fits the frame budget." -ForegroundColor Green; exit 0 }
if ($st -and $st.Result -eq 'fail') { Write-Host "`nFAIL: $($st.Reason). PushMPC uses the next method on this card." -ForegroundColor Red; exit 1 }
Write-Host "`nNo result yet: play a video with DLSS-G at 2x or more for at least 30 seconds." -ForegroundColor Yellow
exit 2
