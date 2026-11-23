<#
  export-public.ps1: copies the publishable set (public-files.txt) into a repository tree.
  public\ content goes to the repository root (README, LICENSE, docs, .github); everything else keeps its path.
  The destination is emptied first, except its .git folder.
    export-public.ps1 -Dest <folder> [-Root <repo>]
#>
param([Parameter(Mandatory)][string]$Dest, [string]$Root = "")
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $Root) { $Root = Split-Path -Parent $here }
$Root = (Resolve-Path $Root).Path.TrimEnd('\')
New-Item -ItemType Directory -Force $Dest | Out-Null
$Dest = (Resolve-Path $Dest).Path.TrimEnd('\')
if ($Dest -eq $Root -or $Dest.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Dest must be outside $Root" }
Get-ChildItem $Dest -Force | Where-Object { $_.Name -ne '.git' } | Remove-Item -Recurse -Force
$files = @()
foreach ($g in Get-Content (Join-Path $here 'public-files.txt')) {
    if (-not $g.Trim() -or $g.StartsWith('#')) { continue }
    $p = Join-Path $Root ($g.Replace('/', '\'))
    if ($g.EndsWith('/**')) { $d = $p.Substring(0, $p.Length - 3); if (Test-Path $d) { $files += Get-ChildItem $d -Recurse -File -Force } }
    elseif (Test-Path $p) { $files += Get-Item $p -Force }
    else { Write-Warning "missing: $g" }
}
$n = 0
foreach ($f in ($files | Sort-Object FullName -Unique)) {
    $rel = $f.FullName.Substring($Root.Length + 1)
    if ($rel -match '\.local($|\.)' ) { continue }
    if ($rel.StartsWith('public\', [StringComparison]::OrdinalIgnoreCase)) { $rel = $rel.Substring(7) }
    $out = Join-Path $Dest $rel
    New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null
    Copy-Item $f.FullName $out -Force
    $n++
}
Write-Host "$n files exported to $Dest"
