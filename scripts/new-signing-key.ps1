<#
  new-signing-key.ps1: creates the RSA key that signs release.json (release.json.sig = base64 RSA-SHA256, PKCS#1 v1.5).
    new-signing-key.ps1 [-OutDir <folder>] [-Bits 3072] [-PublicKey <updater\release-key.xml>] [-Force]
  Writes <OutDir>\release-private.xml (KEEP SECRET: never in the repository; deps\ is git-ignored) and the public half
  to -PublicKey (default: updater\release-key.xml, which is published and embedded in PushMpcUpdater.exe).
  For GitHub Actions put the private XML's whole text into the repository secret PUSHMPC_SIGNING_KEY.
  Default OutDir: <repo>\deps\signing.
#>
param([string]$OutDir = '', [int]$Bits = 3072, [string]$PublicKey = '', [switch]$Force)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $OutDir) { $OutDir = Join-Path $repo 'deps\signing' }
if (-not $PublicKey) { $PublicKey = Join-Path $repo 'updater\release-key.xml' }
$priv = Join-Path $OutDir 'release-private.xml'
if ((Test-Path $priv) -and -not $Force) { throw "$priv exists (use -Force to replace it; the updater in the field only trusts the old key's public half)" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$rsa = New-Object System.Security.Cryptography.RSACryptoServiceProvider($Bits)
try {
    [IO.File]::WriteAllText($priv, $rsa.ToXmlString($true), (New-Object System.Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText($PublicKey, $rsa.ToXmlString($false), (New-Object System.Text.UTF8Encoding($false)))
} finally { $rsa.Dispose() }
Write-Host "private key: $priv (secret)"
Write-Host "public key:  $PublicKey (publish; rebuild the updater so it embeds it)"
