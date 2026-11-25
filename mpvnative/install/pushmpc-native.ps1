<#
  pushmpc-native.ps1: installs PushMPC's native route on Windows for Setup (scripts/pushmpc-install.ps1) or by hand:
  PushMPC's own mpv build (FFmpeg 9.1 + vf_pushmpc + the engine module; the CI artifact "PushMPC-mpv-windows"), with
  its portable_config/mpv.conf set up. Windows mpv builds link FFmpeg statically, so an existing mpv cannot take the
  filter at run time; players that load FFmpeg as separate DLLs are a later step.

    pushmpc-native.ps1 install|uninstall|status [-From <portable mpv folder>] [-Dest <folder>] [-Opts 'smoother=4:...']
                       [-Hwdec d3d11va] [-DryRun] [-Json]

    -From    the portable folder (mpv.exe, avfilter-*.dll, pushmpc_engine.dll, ...); default: this script's folder
    -Dest    where it goes; default %LOCALAPPDATA%\PushMPC\mpv-native
    -Opts    vf_pushmpc options (default none: the engine's Auto); written as vf-append=@pushmpc:pushmpc=<opts>
    -Hwdec   hwdec for the build's mpv.conf (default d3d11va: decoder textures straight into pushcore)
    -Json    one JSON object on stdout (for Setup), the log on stderr
  install copies the folder, writes Dest\portable_config\mpv.conf (a marked block; an existing file is kept as
  mpv.conf.pushmpc-native-backup) and checks that `mpv --vf=help` lists pushmpc. uninstall removes Dest (only the files
  this installer put there, plus portable_config\mpv.conf when it holds nothing else). status reports both.
  Exit: 0 done, 1 error, 2 usage, 3 not supported (the folder lacks the filter).
#>
param(
    [Parameter(Position = 0)][string]$Action = '',
    [string]$From = '',
    [string]$Dest = '',
    [string]$Opts = '',
    [string]$Hwdec = 'd3d11va',
    [switch]$DryRun,
    [switch]$Json
)
$ErrorActionPreference = 'Stop'
$begin = '# >>> PushMPC native: do not edit between these lines (PushMPC uninstall removes them)'
$end = '# <<< PushMPC native'
if ($Action -notin 'install', 'uninstall', 'status') { Get-Help $MyInvocation.MyCommand.Path | Out-String | Write-Host; exit 2 }
if (-not $From) { $From = $PSScriptRoot }
if (-not $Dest) { $Dest = Join-Path $env:LOCALAPPDATA 'PushMPC\mpv-native' }
$conf = Join-Path $Dest 'portable_config\mpv.conf'
$manifest = Join-Path $Dest '.pushmpc-native-files'

function Emit([string]$state, [string]$detail) {
    if ($Json) { [ordered]@{ target = 'windows'; action = $Action; state = $state; detail = $detail; dest = $Dest } | ConvertTo-Json -Compress }
    else { [Console]::Error.WriteLine("windows: $state" + $(if ($detail) { " ($detail)" } else { '' })) }
}
function Strip([string[]]$lines) {
    $out = New-Object System.Collections.Generic.List[string]; $skip = $false
    foreach ($l in $lines) {
        if ($l -eq $begin) { $skip = $true; continue }
        if ($skip -and $l -eq $end) { $skip = $false; continue }
        if (-not $skip) { $out.Add($l) }
    }
    return , $out.ToArray()
}
function Probe {
    $mpv = Join-Path $Dest 'mpv.com'
    if (-not (Test-Path $mpv)) { return 'mpv.com missing' }
    $o = & $mpv --no-config --vf=help 2>&1 | Out-String
    if ($o -match '(?m)^\s*pushmpc\s') { return 'vf_pushmpc present' }
    return 'vf_pushmpc missing from this mpv'
}

switch ($Action) {
    'status' {
        $inst = (Test-Path $conf) -and ((Get-Content $conf) -contains $begin)
        Emit $(if ($inst) { 'installed' } else { 'not-installed' }) $(if (Test-Path $Dest) { Probe } else { '' })
        exit 0
    }
    'install' {
        foreach ($f in 'mpv.exe', 'mpv.com', 'pushmpc_engine.dll') {
            if (-not (Test-Path (Join-Path $From $f))) { Emit 'error' "$f is not in $From"; exit 1 }
        }
        if (-not (Get-ChildItem $From -Filter 'avfilter-*.dll')) { Emit 'error' "no avfilter-*.dll in $From"; exit 1 }
        if ($DryRun) { Emit 'would-install' $Dest; exit 0 }
        New-Item -ItemType Directory -Force (Join-Path $Dest 'portable_config') | Out-Null
        $files = Get-ChildItem $From -File | Where-Object { $_.Name -ne (Split-Path $PSCommandPath -Leaf) }
        foreach ($f in $files) { Copy-Item $f.FullName (Join-Path $Dest $f.Name) -Force }
        $files.Name | Set-Content $manifest -Encoding utf8
        $old = @()
        if (Test-Path $conf) {
            if (-not (Test-Path "$conf.pushmpc-native-backup")) { Copy-Item $conf "$conf.pushmpc-native-backup" }
            $old = Strip (Get-Content $conf)
        }
        $vf = if ($Opts) { "vf-append=@pushmpc:pushmpc=$Opts" } else { 'vf-append=@pushmpc:pushmpc' }
        $block = @($begin, $vf, "hwdec=$Hwdec", 'video-sync=display-resample', 'interpolation=no', 'hr-seek-framedrop=no', $end)
        Set-Content $conf ($block + $old) -Encoding utf8
        $p = Probe
        if ($p -ne 'vf_pushmpc present') { Emit 'installed-inactive' $p; exit 3 }
        Emit 'installed' $p
        exit 0
    }
    'uninstall' {
        if ($DryRun) { Emit 'would-uninstall' $Dest; exit 0 }
        if (Test-Path $conf) {
            $rest = Strip (Get-Content $conf)
            $bk = "$conf.pushmpc-native-backup"
            if ((Test-Path $bk) -and ((Compare-Object @(Get-Content $bk) @($rest)) -eq $null)) { Move-Item $bk $conf -Force }   # unchanged since: the original bytes
            elseif (($rest | Where-Object { $_.Trim() }).Count -eq 0 -and -not (Test-Path $bk)) { Remove-Item $conf -Force }
            else { Set-Content $conf $rest -Encoding utf8; if (Test-Path $bk) { Remove-Item $bk -Force } }
        }
        if (Test-Path $manifest) {
            foreach ($n in Get-Content $manifest) { $p = Join-Path $Dest $n; if (Test-Path $p) { Remove-Item $p -Force } }
            Remove-Item $manifest -Force
        }
        foreach ($d in (Join-Path $Dest 'portable_config'), $Dest) {
            if ((Test-Path $d) -and -not (Get-ChildItem $d -Force)) { Remove-Item $d -Force }
        }
        Emit 'uninstalled' $Dest
        exit 0
    }
}
