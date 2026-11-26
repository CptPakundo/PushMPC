<#
  test-plex-conf.ps1 [-Script <pushmpc-plex-native.ps1>] [-Work <dir>]: the mpv.conf edits of pushmpc-plex-native.ps1
  (EnsureConf twice, RemoveConf) on files with and without a BOM, CRLF / LF, no last newline, empty and absent: one block,
  -Opts replaced, the original bytes back. Runs under Windows PowerShell 5.1 and PowerShell 7. Exit: number of failures.
#>
param([string]$Script = (Join-Path $PSScriptRoot 'pushmpc-plex-native.ps1'), [string]$Work = (Join-Path ([IO.Path]::GetTempPath()) 'pushmpc-plex-conf-test'))
$ErrorActionPreference = 'Stop'
$ast = [Management.Automation.Language.Parser]::ParseFile($Script, [ref]$null, [ref]$null)
$src = $ast.Extent.Text
# the shared definitions between "$begin =" and "# ---- selftest"
$i = $src.IndexOf('$begin = '); $j = $src.IndexOf('# ---- selftest')
$NoConf = $false; $Opts = ''
. ([scriptblock]::Create($src.Substring($i, $j - $i)))
New-Item -ItemType Directory -Force $Work | Out-Null
$fails = 0
$cases = @(
    @{ n = 'utf8-nobom-crlf'; b = [Text.Encoding]::UTF8.GetBytes("volume=50`r`n[p]`r`nvolume=80`r`n") },
    @{ n = 'utf8-bom-lf'; b = [byte[]](0xEF,0xBB,0xBF) + [Text.Encoding]::UTF8.GetBytes("hwdec=auto`n# caf" + [char]0xE9 + "`n") },
    @{ n = 'no-trailing-nl'; b = [Text.Encoding]::UTF8.GetBytes("a=1`nb=2") },
    @{ n = 'empty'; b = [byte[]]@() },
    @{ n = 'absent'; b = $null }
)
foreach ($c in $cases) {
    $conf = Join-Path $Work "$($c.n).conf"; Remove-Item "$conf*" -Force -ErrorAction SilentlyContinue
    if ($null -ne $c.b) { [IO.File]::WriteAllBytes($conf, [byte[]]$c.b) }
    $a = [pscustomobject]@{ Conf = $conf }
    EnsureConf $a; $Opts = 'smoother=4'; EnsureConf $a; $Opts = ''   # twice: idempotent, opts replaced
    $t = [IO.File]::ReadAllText($conf)
    $nblocks = ([regex]::Matches($t, [regex]::Escape($begin))).Count
    $hasOpt = $t -match 'pushmpc=smoother=4'
    RemoveConf $a
    if ($null -eq $c.b) { $ok = -not (Test-Path $conf) } else { $now = [IO.File]::ReadAllBytes($conf); $ok = ([Convert]::ToBase64String($now) -eq [Convert]::ToBase64String([byte[]]$c.b)) }
    $left = @(Get-ChildItem "$conf.*" -ErrorAction SilentlyContinue).Count
    $r = $ok -and $nblocks -eq 1 -and $hasOpt -and $left -eq 0
    if (-not $r) { $fails++ }
    "$($PSVersionTable.PSVersion.Major) $($c.n): blocks=$nblocks opt=$hasOpt restored=$ok leftovers=$left -> $(if ($r) { 'ok' } else { 'FAIL' })"
}
exit $fails
