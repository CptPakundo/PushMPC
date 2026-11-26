<#
.SYNOPSIS
  No-GPU check of the RTX 20 DLSS-G port's pins: the files Setup fetches (scripts\dlssg-port-fetch.ps1) must be the
  ones pushcore verifies before it loads them (pushmpc\src\PushCoreDlssgPort.h), and the folder must match.

.DESCRIPTION
  Compares each pinned file's size and SHA-256 in the two places, and the install folder. With -Zip it also checks a
  downloaded release zip against the pins without extracting it to the runtime (fetch script into a temporary folder).
  Exit 0 = consistent; 1 = a mismatch (printed).

.EXAMPLE
  .\dlssg-port-pins.ps1
  .\dlssg-port-pins.ps1 -Zip C:\temp\dlssg-release-0.4.2-x64.zip
#>
param([string]$Zip = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$h = Get-Content -Raw (Join-Path $root 'pushmpc\src\PushCoreDlssgPort.h')
$f = Get-Content -Raw (Join-Path $root 'scripts\dlssg-port-fetch.ps1')
$bad = 0
$headerRuntime = [regex]::Match($h, 'kRuntimeSha256 = "([0-9a-f]{64})"').Groups[1].Value
foreach ($m in [regex]::Matches($h, '\{ L"(dlssg_sm86\\\\[^"]+)", (\d+)ull, (kRuntimeSha256|"([0-9a-f]{64})") \}')) {
    $rel = $m.Groups[1].Value -replace '\\\\', '/'
    $size = $m.Groups[2].Value
    $sha = if ($m.Groups[3].Value -eq 'kRuntimeSha256') { $headerRuntime } else { $m.Groups[4].Value }
    $p = [regex]::Match($f, "Entry = '$([regex]::Escape($rel))';\s*Size = (\d+);\s*Sha = '([0-9A-F]{64})'")
    if (-not $p.Success) { Write-Host "MISSING in the fetch script: $rel"; $bad++; continue }
    if ($p.Groups[1].Value -ne $size -or $p.Groups[2].Value -ne $sha.ToUpperInvariant()) { Write-Host "MISMATCH ${rel}: header $size $sha, fetch $($p.Groups[1].Value) $($p.Groups[2].Value)"; $bad++ }
    else { Write-Host "ok $rel $size" }
}
if ([regex]::Matches($h, '\{ L"dlssg_sm86').Count -ne 3) { Write-Host 'the header should pin exactly three files'; $bad++ }
# the folder: PortRoot = <runtime>\community\dlssg-sm75, with <runtime> = <root>\runtime\experimental (localRuntimeDirectory)
if ($h -notmatch 'runtimeDir / L"community" / L"dlssg-sm75"') { Write-Host 'PortRoot changed in the header'; $bad++ }
$inst = Get-Content -Raw (Join-Path $root 'scripts\pushmpc-install.ps1')
if ($inst -notmatch [regex]::Escape("'runtime\experimental\community\dlssg-sm75'")) { Write-Host 'the installer does not put the port in runtime\experimental\community\dlssg-sm75'; $bad++ }
if ($Zip) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("dlssg-pins-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force (Join-Path $tmp 'cache') | Out-Null
    Copy-Item -LiteralPath $Zip (Join-Path $tmp 'cache\dlssg-release-0.4.2-x64.zip')
    & (Join-Path $root 'scripts\dlssg-port-fetch.ps1') -Dest (Join-Path $tmp 'out\dlssg-sm75') -Cache (Join-Path $tmp 'cache')
    if ($LASTEXITCODE -ne 0) { Write-Host "the fetch script rejected $Zip"; $bad++ }
    Remove-Item -LiteralPath $tmp -Recurse -Force
}
if ($bad) { Write-Host "$bad problem(s)" -ForegroundColor Red; exit 1 }
Write-Host 'pins consistent' -ForegroundColor Green
exit 0
