<#
  anime-models-fetch.ps1: the anime upscalers' weights for pmml (PushMPC's "Anime upscaler"), fetched from their
  authors' own release places and pinned by size and SHA-256 in anime-models-pin.json. PushMPC does not redistribute them.
    ArtCNN v1.6.2 (MIT)          GitHub release assets, used as released (ONNX)
    CuNNy (LGPL-3.0)             the pretrained weights in the authors' repository at a pinned commit; a separate,
                                 replaceable component: pmml's graph reads the weights straight from this file
    AnimeSR v2 (Apache-2.0)      the authors' Google Drive (their only download place), pinned by hash
  and each project's licence text. A file whose size or SHA-256 differs from its pin is refused and deleted; nothing is
  executed. The model graphs (sr-cunny-*.onnx, sr-animesr-v2.onnx) and the wrappers ship with PushMPC and go next to the
  weights (-Graphs).

    anime-models-fetch.ps1 -Pin <anime-models-pin.json> -Dest <pmml>\graphs\anime-sr [-Graphs <folder with the shipped graphs>]
                           [-Cache <folder>] [-Only artcnn,cunny,animesr] [-DryRun]
  Exit codes: 0 ok, 1 a download or a check failed (files already in -Dest that match their pins are kept).
  Works with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Pin,
    [Parameter(Mandatory = $true)][string]$Dest,
    [string]$Graphs = '',
    [string]$Cache = '',
    [string[]]$Only = @(),
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
function Fail([string]$t) { Write-Host "anime-models: $t"; exit 1 }
$pins = Get-Content -LiteralPath $Pin -Raw | ConvertFrom-Json
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Cache) { $Cache = "$Dest.download" }
$files = @($pins.files | Where-Object { -not $Only.Count -or $Only -contains $_.component })
if ($DryRun) { foreach ($f in $files) { Write-Host "[dry run] $($f.name) $($f.size) $($f.sha256) <- $($f.url)" }; exit 0 }
New-Item -ItemType Directory -Force $Dest, $Cache | Out-Null
$curl = Join-Path $env:SystemRoot 'System32\curl.exe'
function Test-Pin([string]$p, $f) { (Test-Path -LiteralPath $p) -and (Get-Item -LiteralPath $p).Length -eq [int64]$f.size -and (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash -eq $f.sha256.ToUpperInvariant() }
$failed = @()
foreach ($f in $files) {
    $target = Join-Path $Dest $f.name
    if (Test-Pin $target $f) { Write-Host "ok $($f.name) (already there)"; continue }
    $p = Join-Path $Cache $f.name
    if (-not (Test-Pin $p $f)) {
        Write-Host "fetch $($f.name) ($($f.component), $($f.licence))"
        Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue
        try {
            if (Test-Path $curl) { & $curl -L --fail --retry 3 --retry-delay 5 -s -S -o $p $f.url; if ($LASTEXITCODE -ne 0) { throw "curl $LASTEXITCODE" } }
            else { Invoke-WebRequest -Uri $f.url -OutFile $p -UseBasicParsing }
        } catch { $failed += "$($f.name): download failed ($($_.Exception.Message))"; continue }
    }
    if (-not (Test-Pin $p $f)) {
        $len = if (Test-Path -LiteralPath $p) { (Get-Item -LiteralPath $p).Length } else { 0 }
        Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue
        $failed += "$($f.name): refused, size $len / SHA-256 differ from the pin ($($f.size), $($f.sha256))"
        continue
    }
    Copy-Item -LiteralPath $p -Destination $target -Force
    Write-Host "ok $($f.name)"
}
if ($Graphs) {
    $src = Join-Path $Graphs 'anime-sr'
    foreach ($m in @($pins.models | Where-Object { $_.graph -ne $_.file })) {
        $g = Join-Path $src $m.graph
        if (Test-Path -LiteralPath $g) { Copy-Item -LiteralPath $g -Destination (Join-Path $Dest $m.graph) -Force } else { $failed += "$($m.graph): not in $src" }
    }
    $wrap = @(Get-ChildItem -LiteralPath $Graphs -Filter 'sr_*.onnx' -File -ErrorAction SilentlyContinue)
    if (-not $wrap.Count) { $failed += "sr_*.onnx: not in $Graphs" }
    foreach ($w in $wrap) { Copy-Item -LiteralPath $w.FullName -Destination (Split-Path -Parent $Dest) -Force }
}
Remove-Item -LiteralPath $Cache -Recurse -Force -ErrorAction SilentlyContinue
if ($failed.Count) { foreach ($x in $failed) { Write-Host "anime-models: $x" }; exit 1 }
Write-Host "anime-models: $($files.Count) files in $Dest"
exit 0
