<#
  nvidia-vfx-fetch.ps1: NVIDIA's Video Effects runtime files (VFG, RTX VSR's nvngx_vsr, VideoSuperRes, NVVideoEffects,
  NVCVImage, nvngxruntime, cudart) from NVIDIA's own PyPI index, for Windows x64 / ARM64. The Windows counterpart of
  scripts/nvidia-vfx-fetch.sh (Linux); both read scripts/nvidia-vfx-pin.json.

  PushMPC never redistributes these files. This script downloads the exact pinned wheel, checks its SHA-256 (the index
  publishes the same hash), extracts only the listed files and checks each one against its own SHA-256 and size.
  NVIDIA's licence (NvidiaProprietary; texts in licenses\ next to the files) is accepted with -AcceptNvidiaLicense.

  On Windows x64 the seven runtime\nvidia-vfg files are byte-identical to the ones in Veyra's runtime package (checked on
  GitHub's runners, same SHA-256 as pushmpc-install.ps1's pins), so either source gives the same install;
  pushmpc-install.ps1 uses this script when the package lacks them, or always with -VfxSource Wheel, and on ARM64.

    nvidia-vfx-fetch.ps1 -Dest <folder> -AcceptNvidiaLicense [-Platform win_amd64|win_arm64] [-Cache <folder>]
                         [-Layout pushmpc|wheel] [-Only <file names>] [-DryRun]
  -Layout pushmpc (default): the files flat in <Dest> (runtime\nvidia-vfg layout); wheel: the wheel's own paths.
  Exit codes: 0 ok, 1 failure (message), 4 licence not accepted.
#>
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [ValidateSet('win_amd64', 'win_arm64', '')][string]$Platform = '',
    [string]$Cache = '',
    [string]$PinFile = (Join-Path $PSScriptRoot 'nvidia-vfx-pin.json'),
    [ValidateSet('pushmpc', 'wheel')][string]$Layout = 'pushmpc',
    [string[]]$Only = @(),
    [switch]$AcceptNvidiaLicense,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
function Die([string]$m, [int]$code = 1) { Write-Host "nvidia-vfx: $m" -ForegroundColor Red; exit $code }
if (-not $AcceptNvidiaLicense) { Die "NVIDIA's licence must be accepted (-AcceptNvidiaLicense; texts: https://pypi.nvidia.com/nvidia-vfx/)" 4 }
if (-not $Platform) { $Platform = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'win_arm64' } else { 'win_amd64' } }
$pin = Get-Content $PinFile -Raw | ConvertFrom-Json
$w = $pin.wheels.$Platform
if (-not $w) { Die "no pin for $Platform in $PinFile" }
if ($w.url -notmatch '^https://pypi\.nvidia\.com/') { Die "unexpected URL $($w.url)" }
if (-not $Cache) { $Cache = Join-Path $env:LOCALAPPDATA 'PushMPC\download' }
$whl = Join-Path $Cache $w.wheel
$files = @($w.files)
if ($Only.Count) { $files = @($files | Where-Object { $Only -contains (Split-Path $_.name -Leaf) -or $_.name -like 'nvidia_vfx-*.dist-info/licenses/*' }) }
Write-Host "nvidia-vfx $($pin.version) (SDK $($pin.sdkVersion)) ${Platform}: $($files.Count) files -> $Dest"
if ($DryRun) { Write-Host "[dry run] would download $($w.url) to $whl and check SHA-256 $($w.sha256)"; $files | ForEach-Object { Write-Host "  $($_.name) $($_.size) $($_.sha256)" }; exit 0 }
function Sha([string]$p) { (Get-FileHash -Algorithm SHA256 $p).Hash }
if (-not (Test-Path $whl) -or (Sha $whl) -ine $w.sha256) {
    New-Item -ItemType Directory -Force $Cache | Out-Null
    Write-Host "downloading $($w.url)"
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    if (Test-Path $curl) { & $curl -L --fail --retry 3 --retry-delay 5 -C - -s -S -o $whl $w.url; if ($LASTEXITCODE -ne 0) { Die "download failed (curl $LASTEXITCODE)" } }
    else { Invoke-WebRequest -Uri $w.url -OutFile $whl -UseBasicParsing }
    if ((Sha $whl) -ine $w.sha256) { Remove-Item $whl -Force; Die 'the wheel does not match its pinned SHA-256 (deleted)' }
}
Write-Host "wheel SHA-256 verified: $($w.sha256)"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$tmp = "$Dest.part"
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
New-Item -ItemType Directory -Force $tmp | Out-Null
$z = [IO.Compression.ZipFile]::OpenRead($whl)
try {
    $byName = @{}; foreach ($e in $z.Entries) { $byName[$e.FullName] = $e }
    foreach ($f in $files) {
        $e = $byName[$f.name]
        if (-not $e) { Die "the wheel lacks $($f.name)" }
        $rel = if ($Layout -eq 'wheel') { $f.to } elseif ($f.to -like 'licenses/*') { $f.to } else { Split-Path $f.name -Leaf }
        $out = Join-Path $tmp ($rel.Replace('/', '\'))
        New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
        if ((Get-Item $out).Length -ne [int64]$f.size -or (Sha $out) -ine $f.sha256) { Die "$($f.name) does not match its pin" }
    }
} finally { $z.Dispose() }
New-Item -ItemType Directory -Force $Dest | Out-Null
Get-ChildItem $tmp -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($tmp.Length + 1)
    $to = Join-Path $Dest $rel
    New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
    Move-Item $_.FullName $to -Force
}
Remove-Item $tmp -Recurse -Force
Write-Host "nvidia-vfx: $($files.Count) files verified and installed in $Dest"
exit 0
