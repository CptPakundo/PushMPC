<#
  dlssg-port-fetch.ps1: DLSS frame generation for RTX 20 cards, through the community port sdli1995/dlssg_for_sm86
  (release 0.4.2; its Turing kernels come from Coldwood1026/dlssg_for_sm75). PushMPC does not redistribute it: Setup
  downloads the pinned upstream release from GitHub, checks it, and keeps only what pushcore uses:
    dlssg_sm86\runtime\nvngx_dlssg.dll   NVIDIA's DLSS-G runtime 310.9.16.0, unmodified (NVIDIA-signed)
    dlssg_sm86\backend\sm86_backend.dll  the port's backend (MIT)
    dlssg_sm86\kernels\fg_kernels.dll    the port's kernel pack (SM75 / SM86 images)
    LICENSE.md, THIRD_PARTY_NOTICES.txt  the port's licence texts
  The upstream proxy (version.dll), its INI and the XeSS-FG / DLSS SR extras are not installed: pushcore loads the
  backend itself (pushmpc/src/PushCoreDlssgPort.h). Unlock name TuringDlssg (pushmpc/include/pushmpc/unlocks.h); the
  real self-test is pushcore's first DLSS-G open on the card (HKCU\Software\PushMPC\SelfTest\TuringDlssg).

  Checks (before anything is placed): the zip's size and SHA-256 (GitHub's asset digest), then each kept file's size and
  SHA-256, and NVIDIA's Authenticode signature on the runtime. The destination is replaced only when all pass.

    dlssg-port-fetch.ps1 -Dest <folder> [-Cache <folder>] [-DryRun]
  <folder> is the runtime's community folder: <runtime root>\runtime\experimental\community\dlssg-sm75.
  Exit codes: 0 ok, 1 failure (the destination is left as it was).
#>
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [string]$Cache = '',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$Version = '0.4.2'
$Url = "https://github.com/sdli1995/dlssg_for_sm86/releases/download/$Version/dlssg-release-$Version-x64.zip"
$ZipSize = 89769758
$ZipSha = '492BBD0C8429AF27CF74DABCD51F733C56993D0E2CCF176C503C19EF090B2133'
# the files kept (zip entry = path below the destination), pinned; the same pins as pushmpc/src/PushCoreDlssgPort.h
$Files = @(
    @{ Entry = 'dlssg_sm86/runtime/nvngx_dlssg.dll';   Size = 7506544;  Sha = 'CADEE1AA6DB3907BABDC3285DA2AEBC4553D92B89C38BA3EAE8ABD8BBA1BEA72'; Nvidia = $true },
    @{ Entry = 'dlssg_sm86/backend/sm86_backend.dll';  Size = 2328200;  Sha = '93A1B4AC1AE0DD0853C45B59B16C3698035CC8E3A749E34C232EB579E602B8E2' },
    @{ Entry = 'dlssg_sm86/kernels/fg_kernels.dll';    Size = 11875464; Sha = '9882A01F30DD65F36EA1FC6223827E6F86679B99EB09E108EACC51556EEA4E18' },
    @{ Entry = 'LICENSE.md';                           Size = 1065;     Sha = '7537220F50C6430D953EDA8BFF01F0817014E604CB3607F3B62C786D070D8570' },
    @{ Entry = 'THIRD_PARTY_NOTICES.txt';              Size = 10517;    Sha = '6729E527A07AE224904E824BFDCDDA52B6E1E80A0C5D23246D69B3804F241184' }
)
function Die([string]$m) { Write-Host "dlssg-port: $m" -ForegroundColor Red; exit 1 }
if (-not $Cache) { $Cache = Join-Path $env:LOCALAPPDATA "PushMPC\download\dlssg-port-$Version" }
Write-Host "RTX 20 DLSS-G port $Version (sdli1995/dlssg_for_sm86) -> $Dest"
if ($DryRun) {
    Write-Host "[dry run] $Url $ZipSize $ZipSha"
    $Files | ForEach-Object { Write-Host "[dry run]   $($_.Entry) $($_.Size) $($_.Sha)" }
    exit 0
}
New-Item -ItemType Directory -Force $Cache | Out-Null
$zip = Join-Path $Cache "dlssg-release-$Version-x64.zip"
$okZip = { (Test-Path -LiteralPath $zip) -and (Get-Item -LiteralPath $zip).Length -eq $ZipSize -and (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -eq $ZipSha }
if (-not (& $okZip)) {
    Write-Host "downloading $Url"
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    if (Test-Path $curl) { & $curl -L --fail --retry 3 --retry-delay 5 -s -S -o $zip $Url; if ($LASTEXITCODE -ne 0) { Die "download failed (curl $LASTEXITCODE)" } }
    else { Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing }
    if (-not (& $okZip)) { Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue; Die "the download does not match its pin (size $ZipSize, SHA-256 $ZipSha)" }
}
# extract only the pinned entries into a fresh folder next to the destination, check them, then swap it in
Add-Type -AssemblyName System.IO.Compression.FileSystem
$parent = Split-Path -Parent $Dest
New-Item -ItemType Directory -Force $parent | Out-Null
$tmp = "$Dest.new"
if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
New-Item -ItemType Directory -Force $tmp | Out-Null
$z = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    foreach ($f in $Files) {
        $e = $z.GetEntry($f.Entry)
        if (-not $e) { Die "the release has no $($f.Entry)" }
        $out = Join-Path $tmp ($f.Entry -replace '/', '\')
        New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
    }
} finally { $z.Dispose() }
foreach ($f in $Files) {
    $p = Join-Path $tmp ($f.Entry -replace '/', '\')
    if ((Get-Item -LiteralPath $p).Length -ne $f.Size -or (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash -ne $f.Sha) {
        Remove-Item -LiteralPath $tmp -Recurse -Force; Die "$($f.Entry) does not match its pin"
    }
    if ($f.Nvidia) {
        $sig = Get-AuthenticodeSignature -FilePath $p
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=NVIDIA Corporation') {
            Remove-Item -LiteralPath $tmp -Recurse -Force; Die "$($f.Entry) is not validly signed by NVIDIA ($($sig.Status))"
        }
    }
}
if (Test-Path -LiteralPath $Dest) { Remove-Item -LiteralPath $Dest -Recurse -Force }
Move-Item -LiteralPath $tmp $Dest
Write-Host "dlssg-port: $Version installed in $Dest (pinned; NVIDIA's runtime 310.9.16.0 signed by NVIDIA)"
exit 0
