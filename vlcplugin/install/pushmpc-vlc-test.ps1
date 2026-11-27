<#
pushmpc-vlc-test.ps1: pushmpc-vlc.ps1's vlcrc handling on synthetic files, no VLC needed. Each case: a user's vlcrc
before install (or none), install, then VLC rewrites the file as VLC does ("[section] # description" headers, a
setting of VLC's own) and the user may change it, then uninstall; the case checks the keys the user had and that no
PushMPC key is left. Exit 0 when every case passes.
#>
$ErrorActionPreference = 'Stop'
$installer = Join-Path $PSScriptRoot 'pushmpc-vlc.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) ("pushmpc-vlc-test-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$script:fails = 0

function Get-Value([string]$rc, [string]$sec, [string]$key) {
    if (-not (Test-Path $rc)) { return $null }
    $in = $false
    foreach ($l in [IO.File]::ReadAllLines($rc)) {
        if ($l -match '^\s*\[([^\]]+)\]') { $in = ($Matches[1] -eq $sec); continue }
        if ($in -and $l.StartsWith("$key=")) { return $l.Substring($key.Length + 1) }
    }
    return $null
}
function Set-Value([string]$rc, [string]$sec, [string]$key, [string]$value) {
    $out = New-Object Collections.Generic.List[string]; $in = $false; $done = $false
    foreach ($l in [IO.File]::ReadAllLines($rc)) {
        if ($l -match '^\s*\[([^\]]+)\]') { $in = ($Matches[1] -eq $sec) }
        elseif ($in -and -not $done -and $l -match ('^#?' + [regex]::Escape($key) + '=')) { $out.Add("$key=$value"); $done = $true; continue }
        $out.Add($l)
    }
    [IO.File]::WriteAllLines($rc, $out, (New-Object Text.UTF8Encoding($true)))
}
# VLC writes its vlcrc back with a BOM, "[name] # description" headers and settings of its own
function Write-AsVlc([string]$rc) {
    $out = New-Object Collections.Generic.List[string]
    $out.Add('###'); $out.Add('### vlc 3.0.24'); $out.Add('')
    foreach ($l in [IO.File]::ReadAllLines($rc)) {
        if ($l -match '^\s*\[([^\]]+)\]') { $out.Add("[$($Matches[1])] # $($Matches[1]) module") } else { $out.Add($l) }
    }
    $out.Add(''); $out.Add('[qt] # Qt interface'); $out.Add('qt-privacy-ask=0')
    [IO.File]::WriteAllLines($rc, $out, (New-Object Text.UTF8Encoding($true)))
}
function Check([string]$name, [string]$rc, [string]$sec, [string]$key, $want) {
    $got = Get-Value $rc $sec $key
    if ($got -ceq $want) { Write-Host "ok   ${name}: [$sec] $key=$(if ($null -eq $got) { '(absent)' } else { $got })" }
    else { Write-Host "FAIL ${name}: [$sec] $key=$(if ($null -eq $got) { '(absent)' } else { $got }) (want $(if ($null -eq $want) { 'absent' } else { $want }))"; $script:fails++ }
}
function Check-NoPushmpc([string]$name, [string]$rc) {
    $left = @(if (Test-Path $rc) { [IO.File]::ReadAllLines($rc) | Where-Object { $_ -match '^(pushmpc-|vout=.*pushmpc|video-filter=.*pushmpc_out)' } })
    if ($left.Count) { Write-Host "FAIL ${name}: PushMPC keys left: $($left -join ' ')"; $script:fails++ } else { Write-Host "ok   ${name}: no PushMPC key left" }
}
function Run-Case([string]$name, $pre, [scriptblock]$edit) {
    $d = Join-Path $work $name; $pd = Join-Path $d 'plugins'; $src = Join-Path $d 'src'; $rc = Join-Path $d 'vlc\vlcrc'
    New-Item -ItemType Directory -Force -Path $src | Out-Null
    foreach ($f in 'libpushmpc_plugin.dll', 'libpushmpc_d3d11_plugin.dll') { Set-Content -Path (Join-Path $src $f) -Value '' }
    if ($null -ne $pre) { New-Item -ItemType Directory -Force -Path (Split-Path $rc) | Out-Null; [IO.File]::WriteAllLines($rc, [string[]]$pre) }
    & $installer -Action Install -Plugin (Join-Path $src 'libpushmpc_plugin.dll') -PluginDir $pd -VlcRc $rc -NoEnvironment -SkipVlcCheck | Out-Null
    Write-AsVlc $rc
    if ($edit) { & $edit $rc }
    & $installer -Action Uninstall -PluginDir $pd -VlcRc $rc -NoEnvironment | Out-Null
    return $rc
}

$rc = Run-Case 'postproc' @('[core]', 'video-filter=postproc') $null
Check 'postproc' $rc 'core' 'video-filter' 'postproc'; Check-NoPushmpc 'postproc' $rc
$rc = Run-Case 'xpostproc' @('[core]', 'video-filter=sharpen:postproc') $null
Check 'xpostproc' $rc 'core' 'video-filter' 'sharpen:postproc'; Check-NoPushmpc 'xpostproc' $rc
$rc = Run-Case 'nofilter' @('[core]', 'fullscreen=1') $null
Check 'nofilter' $rc 'core' 'video-filter' $null; Check 'nofilter' $rc 'core' 'fullscreen' '1'; Check-NoPushmpc 'nofilter' $rc
$rc = Run-Case 'added' @('[core]', 'video-filter=sharpen') { param($f) Set-Value $f 'core' 'video-filter' ((Get-Value $f 'core' 'video-filter') + ':gradfun') }
Check 'added' $rc 'core' 'video-filter' 'sharpen:gradfun'; Check-NoPushmpc 'added' $rc
$rc = Run-Case 'vout' @('[core]', 'vout=direct3d11', '[direct3d11]', 'd3d11-upscale-mode=processor') $null
Check 'vout' $rc 'core' 'vout' 'direct3d11'; Check 'vout' $rc 'direct3d11' 'd3d11-upscale-mode' 'processor'; Check-NoPushmpc 'vout' $rc
$rc = Run-Case 'novlcrc' $null $null
Check 'novlcrc' $rc 'core' 'video-filter' $null; Check 'novlcrc' $rc 'qt' 'qt-privacy-ask' '0'; Check-NoPushmpc 'novlcrc' $rc
$rc = Run-Case 'uservout' @('[core]', 'fullscreen=1') { param($f) Set-Value $f 'core' 'vout' 'opengl' }
Check 'uservout' $rc 'core' 'vout' 'opengl'; Check-NoPushmpc 'uservout' $rc

Remove-Item -Recurse -Force $work
if ($script:fails) { Write-Host "pushmpc-vlc-test.ps1: $($script:fails) check(s) failed"; exit 1 }
Write-Host 'pushmpc-vlc-test.ps1: all cases passed'
