<#
  xess-fetch.ps1: Intel's signed XeSS runtime DLLs from the Intel XeSS SDK 3.0.2 release (XeSS SR libxess.dll 2.0.2.68,
  XeSS frame generation libxess_fg.dll 1.3.1.78, XeLL libxell.dll 1.3.2.10), downloaded file by file from Intel's own
  repository (github.com/intel/xess, tag v3.0.2, bin/) and pinned by SHA-256 and size. PushMPC does not redistribute them.
  pushcore loads them from <runtime>\runtime_local\intel\experimental (the folder PushFPS's own XeSS presenter uses).
  Unlocks XessFg / XessSr (pushmpc/include/pushmpc/unlocks.h): pushcore runs their self-tests once per GPU and driver.

  pushmpc-install.ps1 calls this script (-Xess Auto = on an Intel GPU unless both unlocks are switched off; Yes = any GPU,
  XeSS runs on every Shader Model 6.4 GPU; No = never). The libxess_fg.dll build is also the one the MFG unlock (XeFG above
  4x) is audited for; another build keeps XeFG at the runtime's own multipliers.

  Checks before anything is installed: the SHA-256 and size pins, an Authenticode signature from Intel Corporation, the file
  versions below.

    xess-fetch.ps1 -Dest <folder> [-Cache <folder>] [-DryRun]
  Exit codes: 0 ok, 1 failure (the destination is left as it was).
#>
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [string]$Cache = '',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$Tag = 'v3.0.2'
$Base = "https://raw.githubusercontent.com/intel/xess/$Tag/bin"
# pins: the files of XeSS_SDK_3.0.2.zip (SHA-256 88B8A373F30E33F3558A77A93E634F11B8132FC3047EA1A8EDEEAD32B8471990, as
# xefg-spike.yml checks it) = the repository's bin\ at the tag (same sizes); pushcore's PushCoreXessSr.cpp pins the same
$Files = @(
    @{ Name = 'libxess.dll';    Size = 77795704; Sha = '251659DD84A3E84DE67C886A4186E01F3ECA49B00641906FE38BB6B807E5D5B7'; Version = '2.0.2.68' },
    @{ Name = 'libxess_fg.dll'; Size = 22957432; Sha = 'EC5E0C65E075570C6EDE72618BB666D0BE0C2E10B2EA9762C0FE8CB8E375AB27'; Version = '1.3.1.78' },
    @{ Name = 'libxell.dll';    Size = 415368;   Sha = 'D2030DCD694FDA8F2EC7E044B13E6DB8F0B56D4BA9113A5EFAD334E3F3DED8C7'; Version = '1.3.2.10' }
)
function Die([string]$m) { Write-Host "xess: $m" -ForegroundColor Red; exit 1 }
if (-not $Cache) { $Cache = Join-Path $env:LOCALAPPDATA "PushMPC\download\xess-$Tag" }
Write-Host "Intel XeSS SDK $Tag runtime DLLs -> $Dest"
if ($DryRun) { $Files | ForEach-Object { Write-Host "[dry run] $Base/$($_.Name) $($_.Size) $($_.Sha)" }; exit 0 }
New-Item -ItemType Directory -Force $Cache | Out-Null
$curl = Join-Path $env:SystemRoot 'System32\curl.exe'
foreach ($f in $Files) {
    $p = Join-Path $Cache $f.Name
    if (-not (Test-Path $p) -or (Get-FileHash $p -Algorithm SHA256).Hash -ne $f.Sha) {
        Write-Host "downloading $($f.Name) ($([math]::Round($f.Size / 1MB, 1)) MB)"
        if (Test-Path $curl) { & $curl -L --fail --retry 3 --retry-delay 5 -s -S -o $p "$Base/$($f.Name)"; if ($LASTEXITCODE -ne 0) { Die "download of $($f.Name) failed (curl $LASTEXITCODE)" } }
        else { Invoke-WebRequest -Uri "$Base/$($f.Name)" -OutFile $p -UseBasicParsing }
    }
    $i = Get-Item $p
    if ($i.Length -ne $f.Size -or (Get-FileHash $p -Algorithm SHA256).Hash -ne $f.Sha) { Remove-Item $p -Force; Die "$($f.Name) does not match its pin" }
    if ($i.VersionInfo.FileVersion -notlike "$($f.Version)*") { Die "$($f.Name) has version $($i.VersionInfo.FileVersion), expected $($f.Version)" }
    $sig = Get-AuthenticodeSignature -FilePath $p
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Intel Corporation') { Die "$($f.Name) is not validly signed by Intel ($($sig.Status))" }
}
New-Item -ItemType Directory -Force $Dest | Out-Null
foreach ($f in $Files) { Copy-Item (Join-Path $Cache $f.Name) (Join-Path $Dest $f.Name) -Force }
Write-Host "xess: Intel XeSS SDK ${Tag}: XeSS SR 2.0.2.68, frame generation 1.3.1.78, XeLL 1.3.2.10 (pinned, Intel-signed) installed in $Dest"
exit 0
