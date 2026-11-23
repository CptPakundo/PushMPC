<#
  check-public.ps1: scans the files that will be published (public-files.txt) for personal data: user and machine
  names, personal paths, e-mail domains, the names of local test files. The patterns live in a local file that is
  never published (check-public.patterns.local, git-ignored); the current user name, machine name and profile path
  are always added. The only identity allowed is the public repository github.com/CptPakundo/PushMPC.
  Exit code 0 = clean, 1 = findings (listed), 2 = no patterns file.
    check-public.ps1 [-Root <repo>] [-Patterns <file>] [-Extra <files or folders>]
  -Extra also scans built release files (PushMPC-Setup.exe, an unpacked release folder) before they are uploaded.
#>
param([string]$Root = (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)), [string]$Patterns = '', [string[]]$Extra = @())
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Patterns) { $Patterns = Join-Path $here 'check-public.patterns.local' }
if (-not (Test-Path $Patterns)) { Write-Host "No patterns file $Patterns (create it locally; it is never published)."; exit 2 }
$allowed = 'github.com/CptPakundo/PushMPC'
$rules = @()
foreach ($l in Get-Content $Patterns) {
    if (-not $l.Trim() -or $l.TrimStart().StartsWith('#')) { continue }
    if ($l.StartsWith('re:')) { $rules += [regex]::new($l.Substring(3), 'IgnoreCase') } else { $rules += [regex]::new([regex]::Escape($l), 'IgnoreCase') }
}
foreach ($auto in @($env:USERNAME, $env:COMPUTERNAME, $env:USERPROFILE)) { if ($auto) { $rules += [regex]::new([regex]::Escape($auto) + '(?!/PushMPC)', 'IgnoreCase') } }
# the publishable set
$files = @()
foreach ($g in Get-Content (Join-Path $here 'public-files.txt')) {
    if (-not $g.Trim() -or $g.StartsWith('#')) { continue }
    $p = Join-Path $Root ($g.Replace('/', '\'))
    if ($g.EndsWith('/**')) { $d = $p.Substring(0, $p.Length - 3); if (Test-Path $d) { $files += Get-ChildItem $d -Recurse -File } }
    elseif (Test-Path $p) { $files += Get-Item $p }
}
foreach ($x in $Extra) { if (Test-Path $x -PathType Container) { $files += Get-ChildItem $x -Recurse -File } elseif (Test-Path $x) { $files += Get-Item $x } }
$findings = 0
foreach ($f in $files) {
    $rel = if ($f.FullName.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { $f.FullName.Substring($Root.TrimEnd('\').Length + 1) } else { $f.Name }
    # names of files and folders count too
    foreach ($r in $rules) { if ($r.IsMatch($rel.Replace($allowed, ''))) { Write-Host "$rel : file name matches $r"; $findings++ } }
    if ($f.Length -gt 20MB) { continue }
    $text = [IO.File]::ReadAllText($f.FullName)
    if ($f.Extension -match '^\.(dll|ax|exe|pdb|dxil|png|ico|bin)$') { $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($f.FullName)) + [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($f.FullName)) }
    $lines = $text.Replace($allowed, '<repo>') -split "`n"
    for ($i = 0; $i -lt $lines.Count; $i++) {
        foreach ($r in $rules) { if ($r.IsMatch($lines[$i])) { Write-Host ("{0}:{1}: {2}" -f $rel, ($i + 1), $lines[$i].Trim().Substring(0, [Math]::Min(140, $lines[$i].Trim().Length))); $findings++ } }
    }
}
Write-Host ("{0} files scanned, {1} findings" -f $files.Count, $findings)
if ($findings) { exit 1 } else { exit 0 }
