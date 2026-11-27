<#
  pushmpc-vlc.ps1: PushMPC for VLC 3.0.x on Windows, per user, no administrator rights.
    pushmpc-vlc.ps1 -Action Install   [-Plugin <libpushmpc_plugin.dll>] [-PluginDir <dir>] [-VlcRc <file>]
    pushmpc-vlc.ps1 -Action Uninstall [-PluginDir <dir>] [-VlcRc <file>]
    pushmpc-vlc.ps1 -Action Status
  Install:
    1. finds VLC (HKLM/HKCU\Software\VideoLAN\VLC, Program Files) and reads its version and bitness: the plugin is
       64-bit and for VLC 3.0.x; a 32-bit VLC or VLC 4 gets a clear message and nothing is changed;
    2. copies the plugin to %LOCALAPPDATA%\PushMPC\vlc (VLC's own folder stays untouched, so VLC updates keep it),
       with what ships next to it: pm_pushcore.dll (the bridge to PushMPC's pushcore: NVIDIA VFG, DLSS-G, PushFlow,
       FSR FG, zero copy on VLC's D3D11 device) and libpushmpc_d3d11_plugin.dll (PushMPC's Direct3D 11 output: the
       GPU's own Super Resolution where the driver has it, else NVIDIA Image Scaling on any GPU) and pushmpc_content.dll
       (PushMPC's content library: what the picture is, drawn animation or not, measured in the frames);
    3. adds that folder to the user environment variable VLC_PLUGIN_PATH (HKCU\Environment; VLC 3 scans it after its
       own plugins; no plugins cache to rebuild) and broadcasts the change, so VLC started afterwards sees it;
    4. edits %APPDATA%\vlc\vlcrc (backup vlcrc.pushmpc-backup, and the values it replaces in pushmpc-vlc.state.json):
         [core]        video-filter gains postproc:pushmpc_out (the static-chain slot, then the output stage)
         [core]        vout=pushmpc_d3d11 when that output ships and VLC's output is the default (or direct3d11);
                       an output the user picked stays; VLC falls back to its own outputs if it cannot open
         [direct3d11]  d3d11-upscale-mode=super (VLC's own RTX / Intel Video Super Resolution when the window is
                       larger than the video; VLC uses its normal scaler on other GPUs)
         [pushmpc]     pushmpc-smoother=0 (Auto)
  Uninstall puts vlcrc back byte for byte (or removes it when install created it) as long as nothing else changed it
  since; otherwise it restores the replaced values. It removes the files and the folder from VLC_PLUGIN_PATH.
  Switch off inside VLC: Tools > Effects and Filters > Video Effects > Advanced > Post processing, or
  Tools > Preferences > All > Video > Filters > PushMPC > Smoother: Off.
  -VlcRc and -NoEnvironment exist for tests (a fake profile).
#>
param(
    [ValidateSet('Install', 'Uninstall', 'Status')][string]$Action = 'Status',
    [string]$Plugin = '',
    [string]$PluginDir = (Join-Path $env:LOCALAPPDATA 'PushMPC\vlc'),
    [string]$VlcRc = (Join-Path $env:APPDATA 'vlc\vlcrc'),
    [switch]$NoEnvironment,
    [switch]$SkipVlcCheck,
    # tests: print whether a version text counts as VLC 3.0.x ("3.0.24", "3,0,24,0", "3.0.24.0") and exit
    [string]$TestVersion = ''
)
$ErrorActionPreference = 'Stop'
$stateFile = Join-Path $PluginDir 'pushmpc-vlc.state.json'

# major and minor of a version text: "3.0.24", "3.0.24.0" or a file version written with commas ("3,0,24,0")
function Get-VlcMajorMinor([string]$text) {
    $m = [regex]::Match("$text", '(\d+)\s*[.,]\s*(\d+)')
    if (-not $m.Success) { return $null }
    return @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value)
}
function Test-Vlc3([string]$text) { $mm = Get-VlcMajorMinor $text; return [bool]($mm -and $mm[0] -eq 3 -and $mm[1] -eq 0) }
if ($TestVersion) { if (Test-Vlc3 $TestVersion) { 'VLC 3.0' } else { 'not VLC 3.0' }; exit 0 }

function Find-Vlc {
    $dirs = @()
    foreach ($k in 'HKLM:\SOFTWARE\VideoLAN\VLC', 'HKLM:\SOFTWARE\WOW6432Node\VideoLAN\VLC', 'HKCU:\SOFTWARE\VideoLAN\VLC') {
        try { $d = (Get-ItemProperty -Path $k -ErrorAction Stop).InstallDir; if ($d) { $dirs += $d } } catch {}
    }
    $dirs += (Join-Path $env:ProgramFiles 'VideoLAN\VLC'), (Join-Path ${env:ProgramFiles(x86)} 'VideoLAN\VLC')
    foreach ($d in $dirs | Select-Object -Unique) {
        $exe = Join-Path $d 'vlc.exe'
        if (Test-Path $exe) {
            $ver = (Get-Item $exe).VersionInfo.ProductVersion
            $bytes = [IO.File]::ReadAllBytes((Join-Path $d 'libvlccore.dll'))
            $pe = [BitConverter]::ToInt32($bytes, 0x3C)
            $machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
            return [pscustomobject]@{ Dir = $d; Version = $ver; Is64 = ($machine -eq 0x8664) }
        }
    }
    return $null
}

# the files that ship next to the plugin (optional: an older build has only the plugin)
$Extras = @('pm_pushcore.dll', 'libpushmpc_d3d11_plugin.dll', 'pushmpc_content.dll')
function Copy-IfOther([string]$from, [string]$to) {
    # a repair that runs from the installed folder must not copy a file onto itself
    if ([IO.Path]::GetFullPath($from).TrimEnd('\') -ieq [IO.Path]::GetFullPath($to).TrimEnd('\')) { return }
    Copy-Item -Force -LiteralPath $from $to
}
function Get-Sha([string]$f) { if (Test-Path -LiteralPath $f) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash } else { return '' } }

function Read-Rc([string]$f) { if (Test-Path $f) { return [IO.File]::ReadAllLines($f) } else { return @() } }
function Write-Rc([string]$f, [string[]]$lines) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $f) | Out-Null
    [IO.File]::WriteAllLines($f, $lines, (New-Object Text.UTF8Encoding($false)))
}
# a section header as VLC writes it: "[core] # core program" (the name between the first [ and ]), a BOM tolerated
# (a whole-line match missed every key of a VLC-written vlcrc)
$rcHeader = '^\uFEFF?\s*\[([^\]]+)\]'
function Get-RcValue([string[]]$lines, [string]$sec, [string]$key) {
    $in = $false
    foreach ($l in $lines) {
        if ($l -match $rcHeader) { $in = ($Matches[1] -eq $sec); continue }
        if ($in -and $l.StartsWith("$key=")) { return $l.Substring($key.Length + 1) }
    }
    return $null
}
# value $null = comment the key out (VLC's default applies)
function Set-RcValue([string[]]$lines, [string]$sec, [string]$key, $value) {
    $out = New-Object Collections.Generic.List[string]
    $in = $false; $done = $false; $seen = $false
    $newLine = if ($null -eq $value) { $null } else { "$key=$value" }
    foreach ($l in $lines) {
        if ($l -match $rcHeader) {
            if ($in -and -not $done -and $newLine) { $out.Add($newLine); $done = $true }
            $in = ($Matches[1] -eq $sec); if ($in) { $seen = $true }
            $out.Add($l); continue
        }
        if ($in -and -not $done -and ($l -match ('^#?' + [regex]::Escape($key) + '='))) {
            if ($newLine) { $out.Add($newLine) } elseif (-not $l.StartsWith('#')) { $out.Add("#$l") } else { $out.Add($l) }
            $done = $true; continue
        }
        $out.Add($l)
    }
    if (-not $done -and $newLine) { if (-not $seen) { $out.Add("[$sec]") }; $out.Add($newLine) }
    return , $out.ToArray()
}
function Read-State {
    if (-not (Test-Path $stateFile)) { return $null }
    $o = Get-Content $stateFile -Raw | ConvertFrom-Json   # Windows PowerShell 5.1 has no -AsHashtable
    $h = @{}; foreach ($p in $o.PSObject.Properties) { $h[$p.Name] = $p.Value }
    return $h
}
function Join-Filters([string]$cur, [bool]$add) {
    $list = @(); if ($cur) { $list = @($cur.Split(':') | Where-Object { $_ -and $_ -ne 'postproc' -and $_ -ne 'pushmpc_out' }) }
    if ($add) { $list += 'postproc', 'pushmpc_out' }
    return ($list -join ':')
}
# the pre-install entries, then those added since install that are not PushMPC's; an entry of $ours counts as
# PushMPC's only when it was not in the pre-install list (a user's own postproc stays)
function Merge-List($pre, $cur, [string[]]$ours) {
    $preList = @(); if ($pre) { $preList = @("$pre".Split(':') | Where-Object { $_ }) }
    $mine = @($ours | Where-Object { $preList -notcontains $_ })
    $out = New-Object Collections.Generic.List[string]
    foreach ($e in $preList) { if (-not $out.Contains($e)) { $out.Add($e) } }
    if ($cur) { foreach ($e in "$cur".Split(':')) { if ($e -and -not $out.Contains($e) -and $mine -notcontains $e) { $out.Add($e) } } }
    return ($out -join ':')
}
# a key install set: while it still holds one of PushMPC's values ('*': any), back to the pre-install value ($null:
# the key was absent, so it goes); a value the user set since install stays
function Restore-Value([string[]]$lines, [string]$sec, [string]$key, [string[]]$ours, $pre) {
    $cur = Get-RcValue $lines $sec $key
    if ($null -eq $cur) { return , $lines }
    if ($ours -notcontains '*' -and $ours -notcontains $cur) { return , $lines }
    return , (Set-RcValue $lines $sec $key $pre)
}
function Set-UserPluginPath([string]$dir, [bool]$add) {
    if ($NoEnvironment) { return }
    $cur = [Environment]::GetEnvironmentVariable('VLC_PLUGIN_PATH', 'User')
    $parts = @(); if ($cur) { $parts = $cur.Split(';') | Where-Object { $_ -and ($_.TrimEnd('\') -ne $dir.TrimEnd('\')) } }
    if ($add) { $parts += $dir }
    $new = ($parts -join ';')
    [Environment]::SetEnvironmentVariable('VLC_PLUGIN_PATH', $(if ($new) { $new } else { $null }), 'User')
    # tell Explorer (and so newly started programs) about the change
    Add-Type -Namespace PushMpc -Name Native -MemberDefinition @'
[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
'@ -ErrorAction SilentlyContinue
    $r = [UIntPtr]::Zero
    [void][PushMpc.Native]::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 2000, [ref]$r)
}

switch ($Action) {
    'Status' {
        $v = Find-Vlc
        Write-Host ("VLC: " + $(if ($v) { "$($v.Version) in $($v.Dir) ($(if ($v.Is64) {'64-bit'} else {'32-bit'}))" } else { 'not found' }))
        Write-Host "plugin: $(if (Test-Path (Join-Path $PluginDir 'libpushmpc_plugin.dll')) { $PluginDir } else { 'not installed' })"
        Write-Host "VLC_PLUGIN_PATH (user): $([Environment]::GetEnvironmentVariable('VLC_PLUGIN_PATH', 'User'))"
        Write-Host "video-filter: $(Get-RcValue (Read-Rc $VlcRc) 'core' 'video-filter'); vout: $(Get-RcValue (Read-Rc $VlcRc) 'core' 'vout')"
        foreach ($x in $Extras) { Write-Host "${x}: $(if (Test-Path (Join-Path $PluginDir $x)) { 'installed' } else { 'not installed' })" }
    }
    'Install' {
        if (-not $SkipVlcCheck) {
            $v = Find-Vlc
            if (-not $v) { throw 'VLC was not found. Install VLC 3.0 (64-bit) from videolan.org first.' }
            if (-not $v.Is64) { throw "VLC $($v.Version) in $($v.Dir) is 32-bit; PushMPC needs the 64-bit VLC." }
            if (-not (Test-Vlc3 $v.Version)) { throw "VLC $($v.Version) is not VLC 3.0; this PushMPC module is for VLC 3.0.x." }
        }
        if (-not $Plugin) { $Plugin = Join-Path $PSScriptRoot 'libpushmpc_plugin.dll' }
        if (-not (Test-Path $Plugin)) { throw "plugin not found: $Plugin" }
        New-Item -ItemType Directory -Force -Path $PluginDir | Out-Null
        Copy-IfOther $Plugin (Join-Path $PluginDir 'libpushmpc_plugin.dll')
        $have = @{}
        foreach ($x in $Extras) {
            $sx = Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Plugin).Path) $x
            if (Test-Path -LiteralPath $sx) { Copy-IfOther $sx (Join-Path $PluginDir $x); $have[$x] = $true }
            else { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $PluginDir $x) }   # not in this build: no stale copy
        }
        Set-UserPluginPath $PluginDir $true
        $lines = Read-Rc $VlcRc
        # the backup is of the user's own vlcrc: a repeated install (a repair) keeps the first one
        if ((Test-Path $VlcRc) -and -not (Read-State) -and -not (Test-Path "$VlcRc.pushmpc-backup")) { Copy-Item $VlcRc "$VlcRc.pushmpc-backup" }
        $state = @{
            'video-filter' = Get-RcValue $lines 'core' 'video-filter'
            'd3d11-upscale-mode' = Get-RcValue $lines 'direct3d11' 'd3d11-upscale-mode'
            'vout' = Get-RcValue $lines 'core' 'vout'
            'pushmpc-smoother' = Get-RcValue $lines 'pushmpc' 'pushmpc-smoother'
            # no vlcrc yet (VLC never ran): uninstall removes the one this install wrote
            'vlcrc-absent' = $(if (Test-Path $VlcRc) { 'no' } else { 'yes' })
            'vlcdir-absent' = $(if (Test-Path (Split-Path $VlcRc)) { 'no' } else { 'yes' })
        }
        $prev = Read-State; if ($prev) { $state = $prev }  # a repeated install keeps the first originals
        $lines = Set-RcValue $lines 'core' 'video-filter' (Join-Filters (Get-RcValue $lines 'core' 'video-filter') $true)
        $lines = Set-RcValue $lines 'direct3d11' 'd3d11-upscale-mode' 'super'
        $lines = Set-RcValue $lines 'pushmpc' 'pushmpc-smoother' '0'
        # PushMPC's Direct3D 11 output (VLC's own, plus an upscaler for every GPU) in place of VLC's default output
        $vout = Get-RcValue $lines 'core' 'vout'
        if ($have['libpushmpc_d3d11_plugin.dll']) {
            if (-not $vout -or $vout -in 'any', 'direct3d11', 'pushmpc_d3d11') { $lines = Set-RcValue $lines 'core' 'vout' 'pushmpc_d3d11' }
            else { Write-Host "VLC's video output stays $vout (your choice); vout=pushmpc_d3d11 adds PushMPC's upscaling" }
        } elseif ($vout -eq 'pushmpc_d3d11') { $lines = Set-RcValue $lines 'core' 'vout' $state['vout'] }
        Write-Rc $VlcRc $lines
        $state['vlcrc-installed'] = Get-Sha $VlcRc
        $state | ConvertTo-Json | Set-Content -Encoding UTF8 $stateFile
        Write-Host "PushMPC for VLC installed: plugin in $PluginDir$(if ($have.Count) { ' with ' + (@($have.Keys | Sort-Object) -join ', ') }), settings in $VlcRc"
    }
    'Uninstall' {
        $state = Read-State
        $exact = $state -and "$($state['vlcrc-installed'])" -and (Get-Sha $VlcRc) -eq "$($state['vlcrc-installed'])"
        if ($exact -and "$($state['vlcrc-absent'])" -ne 'yes' -and (Test-Path "$VlcRc.pushmpc-backup")) {
            # nothing changed vlcrc since install: the file as it was, byte for byte
            Copy-Item -Force "$VlcRc.pushmpc-backup" $VlcRc; Remove-Item -Force "$VlcRc.pushmpc-backup"
        } elseif ($exact -and "$($state['vlcrc-absent'])" -eq 'yes') {
            Remove-Item -Force $VlcRc
            $dir = Split-Path $VlcRc
            if ("$($state['vlcdir-absent'])" -eq 'yes' -and (Test-Path $dir) -and -not (Get-ChildItem $dir -Force)) { Remove-Item $dir }
        } elseif (Test-Path $VlcRc) {
            # VLC rewrote vlcrc since install (or the user changed it): each key install changed goes back by its rule:
            # the pre-install value (the state file), plus what the user added since, minus PushMPC's own entries; a
            # key absent before with only PushMPC's entries left goes (pushmpc-vlc-test.ps1 checks the cases)
            $lines = Read-Rc $VlcRc
            $pre = if ($state) { $state } else { @{} }
            $vf = Get-RcValue $lines 'core' 'video-filter'
            if ($null -ne $vf) {
                $final = Merge-List $pre['video-filter'] $vf @('postproc', 'pushmpc_out')
                $lines = Set-RcValue $lines 'core' 'video-filter' $(if ($final) { $final } elseif ($null -ne $pre['video-filter']) { '' } else { $null })
            }
            $lines = Restore-Value $lines 'direct3d11' 'd3d11-upscale-mode' @('super') $pre['d3d11-upscale-mode']
            $lines = Restore-Value $lines 'pushmpc' 'pushmpc-smoother' @('*') $pre['pushmpc-smoother']
            $lines = Restore-Value $lines 'core' 'vout' @('pushmpc_d3d11') $pre['vout']
            Write-Rc $VlcRc $lines
            Remove-Item -Force -ErrorAction SilentlyContinue "$VlcRc.pushmpc-backup"
            # a vlcrc this install created and VLC never filled holds no setting now: removed again (and VLC's folder)
            if ($state -and "$($state.'vlcrc-absent')" -eq 'yes' -and -not @(Get-Content $VlcRc | Where-Object { $_.Trim() -and $_ -notmatch '^\s*(\[|#)' }).Count) {
                Remove-Item -Force $VlcRc
                $dir = Split-Path $VlcRc
                if ("$($state.'vlcdir-absent')" -eq 'yes' -and (Test-Path $dir) -and -not (Get-ChildItem $dir -Force)) { Remove-Item $dir }
            }
        }
        Set-UserPluginPath $PluginDir $false
        Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $PluginDir 'libpushmpc_plugin.dll'), $stateFile
        foreach ($x in $Extras) { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $PluginDir $x) }
        if ((Test-Path $PluginDir) -and -not (Get-ChildItem $PluginDir)) { Remove-Item $PluginDir }
        Write-Host 'PushMPC for VLC removed.'
    }
}
