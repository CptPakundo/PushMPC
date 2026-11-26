<#
  helixsr-fetch.ps1: HelixSR (github.com/lonewolf0622/HelixSR, release 3.4.1, HelixSR Freeware License) for GPUs that
  NVIDIA's and AMD's own runtimes leave out. PushMPC does not redistribute it: Setup downloads the official release on this
  PC, checks it, and keeps only what pushcore uses, unmodified, with HelixSR's licence and notices:
    anr\nvngx_dlssg.dll, anr\helixsr.ini            HelixSR frame generation (2x-6x), part Fg
    fsr4-full\amd_fidelityfx_upscaler_dx12.dll, hlx_fsr4.ini   HLX FSR 4 Full (RX 5000), part Fsr4Full
    fsr4\amd_fidelityfx_upscaler_dx12.dll           HLX FSR 4 (GTX 16 / GTX 10), part Fsr4Lite
    LICENSE, THIRD_PARTY_NOTICES.md, SOURCE.md
  AMD's FidelityFX loader (-Loader, the runtime's pinned copy) goes next to each HLX FSR 4 DLL. HLX DLSS (NVIDIA's DLSS
  network rebuilt from NVIDIA's DLL) is never set up: PushMPC does not copy or convert NVIDIA's models.
  pushcore runs each part only after its self-test on the card (unlocks HelixFg / HlxFsr4).

  Checks before anything is placed: the zip's size and SHA-256 (the release's SHA256SUMS.txt and GitHub's asset digest),
  then each kept file's size and SHA-256. The destination is replaced only when all pass.

    helixsr-fetch.ps1 -Dest <runtime_local>\community\helixsr -Parts Fg,Fsr4Full,Fsr4Lite [-Loader <dll>] [-Cache <folder>] [-DryRun]
  Exit codes: 0 ok, 1 failure (the destination is left as it was).
#>
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [Parameter(Mandatory = $true)][string[]]$Parts,
    [string]$Loader = '',
    [string]$Cache = '',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$Version = '3.4.1'
$Url = "https://github.com/lonewolf0622/HelixSR/releases/download/v$Version/HelixSR-$Version.zip"
$ZipSize = 294792182
$ZipSha = '31AB3FCEE0AA385712C123C6717DC579C60AEAEFF40F705DBE91753F96AC46DA'
$Top = "HelixSR-$Version/"
$Common = @(
    @{ Entry = 'LICENSE';                 To = 'LICENSE';                 Size = 2531;  Sha = '8898AB9D1AE5E59F5E222ACA87E6A30BB073E0B927FE0971DD20C49B79FB81E6' },
    @{ Entry = 'THIRD_PARTY_NOTICES.md';  To = 'THIRD_PARTY_NOTICES.md';  Size = 14665; Sha = '9B6481E2A7AE58F2D54C7015ECB253C6EC21F2F9E10AD59E9DE279323A7F8F7B' },
    @{ Entry = 'SOURCE.md';               To = 'SOURCE.md';               Size = 1110;  Sha = '49D56C50AB03F2396C35B209E8AD9D98704AEE0C0ECB5AF83D05DFE5D5EFDE9F' }
)
$PartFiles = @{
    Fg = @(
        @{ Entry = 'helixsr-scaler/OptiScaler/custom/HelixSR-FG/dlssg/nvngx_dlssg.dll'; To = 'anr\nvngx_dlssg.dll'; Size = 1256448; Sha = '81FBB36D5D318F2B2FDA254E87F69E641D67A2A2CC026F23031912B609B9EC8A' },
        @{ Entry = 'helixsr-scaler/OptiScaler/custom/HelixSR-FG/dlssg/helixsr.ini';     To = 'anr\helixsr.ini';     Size = 618;     Sha = 'A386592DAA86FC05040A5E1F233896D06D62BF7227BCC2DF954228E9683834D8' }
    )
    Fsr4Full = @(
        @{ Entry = 'helixsr-scaler/OptiScaler/custom/HLX FSR 4 Full/ffx/amd_fidelityfx_upscaler_dx12.dll'; To = 'fsr4-full\amd_fidelityfx_upscaler_dx12.dll'; Size = 22876979; Sha = '31D59A0F95BCDAF268D2F37ED6885C05037AD07B4D07131BB29835365D70FC57' },
        @{ Entry = 'helixsr-scaler/OptiScaler/custom/HLX FSR 4 Full/ffx/hlx_fsr4.ini';                     To = 'fsr4-full\hlx_fsr4.ini';                     Size = 1104;     Sha = 'D78DC1D929ED10B0D3FB2C9A9C5F8E420A9339A0F4FD5C261720EB49B392CAB7' }
    )
    Fsr4Lite = @(
        @{ Entry = 'HLX-FSR4/amd_fidelityfx_upscaler_dx12.dll'; To = 'fsr4\amd_fidelityfx_upscaler_dx12.dll'; Size = 58185808; Sha = '8B3DEF5F363E3221C484EF6D196360C4695AC0EA66C02D8A88C26F51FC63FC94' }
    )
}
function Die([string]$m) { Write-Host "helixsr: $m" -ForegroundColor Red; exit 1 }
$Parts = @($Parts | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })   # -File passes one string
foreach ($p in $Parts) { if (-not $PartFiles.ContainsKey($p)) { Die "unknown part $p (Fg, Fsr4Full, Fsr4Lite)" } }
$files = @($Common) + @($Parts | ForEach-Object { $PartFiles[$_] })
if (-not $Cache) { $Cache = Join-Path $env:LOCALAPPDATA "PushMPC\download\helixsr-$Version" }
Write-Host "HelixSR $Version ($($Parts -join ', ')) -> $Dest"
if ($DryRun) { Write-Host "[dry run] $Url $ZipSize $ZipSha"; $files | ForEach-Object { Write-Host "[dry run]   $($_.Entry) -> $($_.To) $($_.Size) $($_.Sha)" }; exit 0 }
if (($Parts -contains 'Fsr4Full' -or $Parts -contains 'Fsr4Lite') -and -not ($Loader -and (Test-Path -LiteralPath $Loader))) { Die 'HLX FSR 4 needs AMD''s FidelityFX loader (-Loader)' }
New-Item -ItemType Directory -Force $Cache | Out-Null
$zip = Join-Path $Cache "HelixSR-$Version.zip"
$okZip = { (Test-Path -LiteralPath $zip) -and (Get-Item -LiteralPath $zip).Length -eq $ZipSize -and (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -eq $ZipSha }
if (-not (& $okZip)) {
    Write-Host "downloading $Url ($([math]::Round($ZipSize / 1MB)) MB)"
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    if (Test-Path $curl) { & $curl -L --fail --retry 3 --retry-delay 5 -s -S -o $zip $Url; if ($LASTEXITCODE -ne 0) { Die "download failed (curl $LASTEXITCODE)" } }
    else { Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing }
    if (-not (& $okZip)) { Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue; Die "the download does not match its pin (size $ZipSize, SHA-256 $ZipSha)" }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force (Split-Path -Parent $Dest) | Out-Null
$tmp = "$Dest.new"
if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
New-Item -ItemType Directory -Force $tmp | Out-Null
$z = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    foreach ($f in $files) {
        $e = $z.GetEntry($Top + $f.Entry)
        if (-not $e) { Die "the release has no $($f.Entry)" }
        $out = Join-Path $tmp $f.To
        New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
    }
} finally { $z.Dispose() }
foreach ($f in $files) {
    $p = Join-Path $tmp $f.To
    if ((Get-Item -LiteralPath $p).Length -ne $f.Size -or (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash -ne $f.Sha) { Remove-Item -LiteralPath $tmp -Recurse -Force; Die "$($f.Entry) does not match its pin" }
}
foreach ($d in 'fsr4-full', 'fsr4') { if (Test-Path -LiteralPath (Join-Path $tmp $d)) { Copy-Item -LiteralPath $Loader (Join-Path $tmp "$d\amd_fidelityfx_loader_dx12.dll") -Force } }
if (Test-Path -LiteralPath $Dest) { Remove-Item -LiteralPath $Dest -Recurse -Force }
Move-Item -LiteralPath $tmp $Dest
Write-Host "helixsr: $Version ($($Parts -join ', ')) installed in $Dest (pinned, unmodified, with HelixSR's licence)"
exit 0
