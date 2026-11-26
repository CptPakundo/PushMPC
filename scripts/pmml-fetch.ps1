<#
  pmml-fetch.ps1: pmml's runtime for Windows, fetched by the pins in ml-runtime-pin.json, as pmml/scripts/fetch_runtime.py
  fetches it (same pin file, same layout, same checks), for Setup on PCs without Python.

    pmml-fetch.ps1 -Pin <ml-runtime-pin.json> -Out <dir> [-Platform win-x64] [-Flavors core] [-Eps trt-rtx,openvino,qnn]
                   [-Cache <dir>] [-WinMl] [-DryRun]

  Everything comes from its official source (nuget.org, pypi.org wheels used as archives) and is checked against the pinned
  SHA-256 before anything is extracted; an entry pinned as TODO is refused. Only the native libraries, the side-by-side
  manifests the pins name and the licence files are extracted; nothing is executed.
  Layout: <Out>\ort\<flavor>\ (onnxruntime.dll), <Out>\eps\<ep>\ (the plugin EP and ep.json {name, epName, library,
  version}), <Out>\licenses\, <Out>\fetched.json. Exit 0 = done, 1 = a download or a check failed (nothing half-written
  is left in <Out>: the result is built in <Out>.part and moved into place).
  -WinMl: Windows ML (the winmlCatalog pin) into <Out>\winml, for pmml_winml_ep.exe, which asks Windows ML's EP catalog
  for the EPs that exist only there (AMD's AMDGPUExecutionProvider); Setup runs that tool after this script.
  Works with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Pin,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Platform = 'win-x64',
    [string[]]$Flavors = @('core'),
    [string[]]$Eps = @(),
    [string]$Cache = '',
    [switch]$WinMl,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Fail([string]$t) { Write-Host "pmml-fetch: $t"; exit 1 }
$pinData = Get-Content -LiteralPath $Pin -Raw | ConvertFrom-Json
$Flavors = @($Flavors | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Eps = @($Eps | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Cache) { $Cache = "$Out.download" }

# fnmatch as fetch_runtime.py uses it: '*' and '?' match across '/', and a '**/' prefix may also match nothing
function Test-Glob([string]$name, [string[]]$patterns) {
    foreach ($p in $patterns) {
        foreach ($q in @($p, $p.Replace('**/', ''))) {
            $rx = '^' + ([regex]::Escape($q) -replace '\\\*', '.*' -replace '\\\?', '.') + '$'
            if ($name -match $rx) { return $true }
        }
    }
    return $false
}
function Get-Pinned([string]$url, [string]$sha) {
    if (-not $sha -or $sha -eq 'TODO') { Fail "$url has no pinned SHA-256 (TODO): refused" }
    $file = Join-Path $Cache ([IO.Path]::GetFileName(($url -split '\?')[0]))
    if ((Test-Path -LiteralPath $file) -and (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ieq $sha) { Write-Host "cached  $(Split-Path $file -Leaf)"; return $file }
    if ($DryRun) { Write-Host "would fetch $url (SHA-256 $sha)"; return '' }
    Write-Host "fetch   $url"
    & (Join-Path $env:SystemRoot 'System32\curl.exe') -L --fail --retry 3 -C - -s -S -A 'pmml-fetch/1' -o $file $url
    if ($LASTEXITCODE -ne 0) { Fail "download failed (curl exit $LASTEXITCODE): $url" }
    $got = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    if ($got -ine $sha) { Remove-Item -LiteralPath $file -Force; Fail "$(Split-Path $file -Leaf): SHA-256 $got does not match the pinned $sha" }
    Write-Host "sha256  $($got.ToLowerInvariant())  $(Split-Path $file -Leaf)"
    return $file
}
# the members matching the patterns, flattened into $dest (unsafe names refused); licence files into $licDir
function Expand-Pinned([string]$archive, [string[]]$patterns, [string]$dest, [string]$licDir) {
    New-Item -ItemType Directory -Force $dest, $licDir | Out-Null
    $got = @()
    $z = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($e in $z.Entries) {
            $n = $e.FullName
            if (-not $e.Name) { continue }   # a folder
            if ($n.StartsWith('/') -or $n -match '(^|/)\.\.(/|$)' -or $n -match '^[A-Za-z]:') { Fail "unsafe entry $n in $(Split-Path $archive -Leaf)" }
            $base = $e.Name.ToLowerInvariant()
            if ($base.StartsWith('license') -or $base -like '*thirdpartynotices*' -or $base.StartsWith('notice')) {
                $stem = [IO.Path]::GetFileNameWithoutExtension($archive); if ($stem.Length -gt 40) { $stem = $stem.Substring(0, 40) }
                [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $licDir "$stem-$($e.Name)"), $true)
            }
            if (-not (Test-Glob $n $patterns)) { continue }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $dest $e.Name), $true)
            $got += $e.Name
        }
    } finally { $z.Dispose() }
    return $got
}

$part = "$Out.part"
if (-not $DryRun) {
    New-Item -ItemType Directory -Force $Cache | Out-Null
    if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Recurse -Force }
    New-Item -ItemType Directory -Force $part | Out-Null
}
$record = [ordered]@{ platform = $Platform; ort = [ordered]@{}; eps = [ordered]@{} }
foreach ($flavor in $Flavors) {
    $entry = $pinData.onnxruntime.$flavor
    if (-not $entry) { Fail "unknown flavor $flavor" }
    $spec = $entry.platforms.$Platform
    if (-not $spec) { Write-Host "skip    ort/$flavor`: no $Platform build"; continue }
    if ($spec -is [string]) { $arc = Get-Pinned $entry.url $entry.sha256; $pat = @($spec) }
    else { $arc = Get-Pinned $spec.url $spec.sha256; $pat = @($spec.extract) }
    if ($DryRun) { continue }
    $files = @(Expand-Pinned $arc $pat (Join-Path $part "ort\$flavor") (Join-Path $part 'licenses'))
    if ($flavor -eq 'directml') {
        $d = $pinData.onnxruntime.directmlRuntime
        $rarc = Get-Pinned $d.url $d.sha256
        $files += @(Expand-Pinned $rarc @($d.platforms.$Platform) (Join-Path $part "ort\$flavor") (Join-Path $part 'licenses'))
    }
    if (-not $files.Count) { Fail "ort/$flavor`: nothing matched $($pat -join ', ')" }
    $record.ort[$flavor] = [ordered]@{ version = $entry.version; files = $files }
    Write-Host "ok      ort/$flavor $($entry.version): $($files -join ', ')"
}
foreach ($ep in $Eps) {
    $entry = $pinData.eps.$ep
    if (-not $entry) { Fail "unknown EP $ep" }
    $spec = $entry.platforms.$Platform
    if (-not $spec) { Write-Host "skip    eps/$ep`: no $Platform package"; continue }
    $arc = Get-Pinned $spec.url $spec.sha256
    if ($DryRun) { continue }
    $pat = if ($spec.extract) { @($spec.extract) } else { @($entry.extract) }
    $dest = Join-Path $part "eps\$ep"
    $files = @(Expand-Pinned $arc $pat $dest (Join-Path $part 'licenses'))
    $lib = $files | Where-Object { $f = $_; ($f -match '\.(dll|so|dylib)$') -and @($entry.library | Where-Object { $f -like $_ }).Count } | Select-Object -First 1
    if (-not $lib) { Fail "eps/$ep`: no file matched $($entry.library -join ', ')" }
    [ordered]@{ name = $ep; epName = $entry.epName; library = $lib; version = $entry.version } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dest 'ep.json') -Encoding ASCII
    $record.eps[$ep] = [ordered]@{ version = $entry.version; library = $lib; files = $files.Count }
    Write-Host "ok      eps/$ep $($entry.version): library $lib; $($files.Count) files"
}
if ($WinMl) {
    # Windows ML's EP catalog C API (pmml_winml_ep.exe): AMD's AMDGPUExecutionProvider exists only through it
    $wm = $pinData.winmlCatalog
    $spec = if ($wm) { $wm.platforms.$Platform } else { $null }
    if (-not $spec) { Write-Host "skip    winml: no $Platform package" }
    else {
        $arc = Get-Pinned $wm.url $wm.sha256
        if (-not $DryRun) {
            $files = @(Expand-Pinned $arc @($spec) (Join-Path $part 'winml') (Join-Path $part 'licenses'))
            if (-not $files.Count) { Fail "winml: nothing matched $spec" }
            $record['winml'] = [ordered]@{ version = $wm.version; files = $files }
            Write-Host "ok      winml $($wm.version): $($files -join ', ')"
        }
    }
}
if ($DryRun) { exit 0 }
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $part 'fetched.json') -Encoding ASCII
if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Recurse -Force }
Move-Item -LiteralPath $part $Out
exit 0
