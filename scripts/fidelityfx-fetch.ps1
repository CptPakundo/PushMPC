<#
  fidelityfx-fetch.ps1: AMD's signed FidelityFX DLLs from the AMD FSR SDK 2.4.0 release (FSR upscaling 4.1.2, which adds
  FSR 4.1 on Radeon RX 6000 and RDNA2/3.5 APUs; frame generation 4.0.2; loader 2.3.0.4555), downloaded from AMD's own
  repository (GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag v2.4.0, Kits/FidelityFX/signedbin) and pinned by SHA-256 and
  size. PushMPC does not redistribute them. Unlock name Fsr4Int8 (pushmpc/include/pushmpc/unlocks.h).

  The runtime package (Veyra's / PushFPS's) carries SDK 2.3.0 (upscaler 4.1.1: FSR 4.1 INT8 for RX 7000, not RX 6000).
  pushmpc-install.ps1 calls this script to replace those three files with the 2.4.0 ones (-FidelityFx Sdk240; Auto = on
  an AMD GPU unless switched off). The upscaler DLL itself picks FSR 4.1 FP8 / INT8 or FSR 3.1 per GPU and driver, so
  a GPU without FSR 4 support keeps FSR 3.1 through the same file.

  Self-test (always, before anything is replaced): the SHA-256 and size pins, an Authenticode signature from Advanced
  Micro Devices, and the file versions below; the result goes to HKCU\Software\PushMPC\SelfTest\Fsr4Int8 (-Record).

    fidelityfx-fetch.ps1 -Dest <folder> [-Cache <folder>] [-Record] [-DryRun]
  Exit codes: 0 ok, 1 failure (the destination is left as it was).
#>
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [string]$Cache = '',
    [switch]$Record,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$Tag = 'v2.4.0'
$Base = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$Tag/Kits/FidelityFX/signedbin"
# pins from .github/workflows/vendor-paths.yml (job fsr-sdk, run 45611733275), the files in AMD's tree at the tag
$Files = @(
    @{ Name = 'amd_fidelityfx_upscaler_dx12.dll';        Size = 57664272; Sha = 'AAD4FCCCCB29084A655F9A4F9F613BD22C8C03FB4A0F8840148CC6543B4CCACE'; Version = '4.1.2.4555' },
    @{ Name = 'amd_fidelityfx_framegeneration_dx12.dll'; Size = 29679376; Sha = 'E6843252F58B0341A0D80F6A20BC0D840643EE95BC48C22CAC08745344577BB6'; Version = '4.0.2.4555' },
    @{ Name = 'amd_fidelityfx_loader_dx12.dll';          Size = 39184;    Sha = 'E2191D3C0828BE45EB8AFFD4E3EC0378F2293ADC345B772A7170EDCF3B021ECB'; Version = '2.3.0.4555' }
)
function Die([string]$m) { Write-Host "fidelityfx: $m" -ForegroundColor Red; if ($Record) { Save-Result $false $m }; exit 1 }
function Save-Result([bool]$pass, [string]$why) {
    $k = 'HKCU:\Software\PushMPC\SelfTest\Fsr4Int8'
    New-Item -Force $k | Out-Null
    Set-ItemProperty $k Key "fidelityfx-$Tag"; Set-ItemProperty $k Result $(if ($pass) { 'pass' } else { 'fail' })
    Set-ItemProperty $k Reason $why; Set-ItemProperty $k When ([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))
}
if (-not $Cache) { $Cache = Join-Path $env:LOCALAPPDATA "PushMPC\download\fidelityfx-$Tag" }
Write-Host "AMD FSR SDK $Tag signed DLLs -> $Dest"
if ($DryRun) { $Files | ForEach-Object { Write-Host "[dry run] $Base/$($_.Name) $($_.Size) $($_.Sha)" }; exit 0 }
New-Item -ItemType Directory -Force $Cache | Out-Null
$curl = Join-Path $env:SystemRoot 'System32\curl.exe'
foreach ($f in $Files) {
    $p = Join-Path $Cache $f.Name
    if (-not (Test-Path $p) -or (Get-FileHash $p -Algorithm SHA256).Hash -ne $f.Sha) {
        Write-Host "downloading $($f.Name)"
        if (Test-Path $curl) { & $curl -L --fail --retry 3 --retry-delay 5 -s -S -o $p "$Base/$($f.Name)"; if ($LASTEXITCODE -ne 0) { Die "download of $($f.Name) failed (curl $LASTEXITCODE)" } }
        else { Invoke-WebRequest -Uri "$Base/$($f.Name)" -OutFile $p -UseBasicParsing }
    }
    $i = Get-Item $p
    if ($i.Length -ne $f.Size -or (Get-FileHash $p -Algorithm SHA256).Hash -ne $f.Sha) { Remove-Item $p -Force; Die "$($f.Name) does not match its pin" }
    if ($i.VersionInfo.FileVersion -ne $f.Version) { Die "$($f.Name) has version $($i.VersionInfo.FileVersion), expected $($f.Version)" }
    $sig = Get-AuthenticodeSignature -FilePath $p
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Advanced Micro Devices') { Die "$($f.Name) is not validly signed by AMD ($($sig.Status))" }
}
New-Item -ItemType Directory -Force $Dest | Out-Null
foreach ($f in $Files) { Copy-Item (Join-Path $Cache $f.Name) (Join-Path $Dest $f.Name) -Force }
$why = "AMD FSR SDK ${Tag}: upscaler 4.1.2.4555, frame generation 4.0.2.4555, loader 2.3.0.4555 (pinned, AMD-signed)"
if ($Record) { Save-Result $true $why }
Write-Host "fidelityfx: $why installed in $Dest"
exit 0
