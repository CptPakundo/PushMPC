<#
.SYNOPSIS
  PushMPC's Intel checks on your own Intel Arc / Xe GPU: XeSS frame generation, XeSS super resolution, Intel Video Super
  Resolution, Intel VPL frame interpolation and the D3D11 bridge.

.DESCRIPTION
  Runs pushcore_selftest.exe (from the intel-video CI artifact, next to a pushcore.dll of the same build) against the
  installed PushMPC runtime folder and prints one line per check with PASS / FAIL and its numbers. With -Record the
  results are stored in HKCU\Software\PushMPC\SelfTest (as PushMPC itself does once per driver), so the installed
  PushMPC uses them right away. Nothing else on the PC is changed.

  Paste the whole output into the issue.

.PARAMETER Tool
  Path to pushcore_selftest.exe. pushcore.dll must be next to it.

.PARAMETER Root
  PushMPC's runtime folder (the one with runtime_local\). Default: %LOCALAPPDATA%\PushMPC\runtime.

.EXAMPLE
  .\intel-selftest-manual.ps1 -Tool C:\temp\intel-selftest\pushcore_selftest.exe
#>
param(
    [Parameter(Mandatory = $true)][string]$Tool,
    [string]$Root = (Join-Path $env:LOCALAPPDATA 'PushMPC\runtime'),
    [switch]$Record,
    [int[]]$XefgMultipliers = @(2, 3, 4),
    [string]$OutDir = (Join-Path (Get-Location) 'intel-selftest-out')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Tool)) { throw "Tool not found: $Tool" }
if (-not (Test-Path -LiteralPath (Join-Path (Split-Path $Tool) 'pushcore.dll'))) { throw 'pushcore.dll is not next to the tool' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.Name -match 'Intel' } | Select-Object -First 1
Write-Host "GPU: $(if ($gpu) { "$($gpu.Name), driver $($gpu.DriverVersion)" } else { 'no Intel GPU found (the checks still run on the first GPU)' })"
Write-Host "Runtime folder: $Root"

function Invoke-Check([string]$name, [string[]]$extra) {
    $json = Join-Path $OutDir "$name.json"
    $argv = @('--core', (Join-Path (Split-Path $Tool) 'pushcore.dll'), '--root', $Root, '--json', $json) + $extra
    if ($Record) { $argv += '--record' }
    & $Tool @argv | Out-Null
    $code = $LASTEXITCODE
    if (-not (Test-Path $json)) { Write-Host "$name : FAIL (no report, exit $code)"; return $null }
    return (Get-Content $json -Raw | ConvertFrom-Json)
}
function Show([string]$label, $j, [string]$key) {
    if (-not $j) { return }
    $v = $j.$key
    $state = if ($v -match '^pass') { 'PASS' } elseif ($v -match '^unavailable') { 'NOT AVAILABLE' } else { 'FAIL' }
    Write-Host ('{0,-34} {1,-14} {2}' -f $label, $state, $v)
    $j.PSObject.Properties | Where-Object { $_.Name -like "$key.*" } | ForEach-Object { Write-Host ('    {0} = {1}' -f $_.Name.Substring($key.Length + 1), $_.Value) }
}

$all = Invoke-Check 'all' @('--bridge', '--ivsr', '--vpl', '--xess')
if ($all) {
    Write-Host "Adapter: $($all.adapter), driver $($all.driver)"
    Show 'D3D11 bridge' $all 'bridge'
    Show 'Intel VSR' $all 'ivsr'
    Show 'Intel VPL frame interpolation' $all 'vplfi'
    Show 'XeSS SR' $all 'xesssr'
}
foreach ($m in $XefgMultipliers) {
    $j = Invoke-Check "xefg-$m" @('--xefg', '--xefg-multiplier', "$m", '--no-caps')
    Show "XeSS FG ${m}x" $j 'xefg'
}
if ($all) {
    Write-Host ''
    Write-Host 'What PushMPC can use on this GPU now:'
    foreach ($p in 'capsXefg', 'capsVplFi', 'capsXessSr', 'capsIvsr') { Write-Host ('  {0,-12} {1}' -f $p.Substring(4), $all.$p) }
}
Write-Host "Reports: $OutDir"
