<#
  pushmpc-vlc.ps1: PushMPC for VLC 3.0.x on Windows, per user, no administrator rights.
    pushmpc-vlc.ps1 -Action Install   [-Plugin <libpushmpc_plugin.dll>] [-PluginDir <dir>] [-VlcRc <file>]
    pushmpc-vlc.ps1 -Action Uninstall [-PluginDir <dir>] [-VlcRc <file>]
    pushmpc-vlc.ps1 -Action Status
  Install:
    1. finds VLC (HKLM/HKCU\Software\VideoLAN\VLC, Program Files) and reads its version and bitness: the plugin is
       64-bit and for VLC 3.0.x; a 32-bit VLC or VLC 4 gets a clear message and nothing is changed;
    2. copies the plugin to %LOCALAPPDATA%\PushMPC\vlc (VLC's own folder stays untouched, so VLC updates keep it);
    3. adds that folder to the user environment variable VLC_PLUGIN_PATH (HKCU\Environment; VLC 3 scans it after its
       own plugins; no plugins cache to rebuild) and broadcasts the change, so VLC started afterwards sees it;
    4. edits %APPDATA%\vlc\vlcrc (backup vlcrc.pushmpc-backup, and the values it replaces in pushmpc-vlc.state.json):
         [core]        video-filter gains postproc:pushmpc_out (the static-chain slot, then the output stage)
         [direct3d11]  d3d11-upscale-mode=super (VLC's own RTX / Intel Video Super Resolution when the window is
                       larger than the video; VLC uses its normal scaler on other GPUs)
         [pushmpc]     pushmpc-smoother=0 (Auto)
  Uninstall restores the replaced values and removes the folder from VLC_PLUGIN_PATH.
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

function Read-Rc([string]$f) { if (Test-Path $f) { return [IO.File]::ReadAllLines($f) } else { return @() } }
function Write-Rc([string]$f, [string[]]$lines) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $f) | Out-Null
    [IO.File]::WriteAllLines($f, $lines, (New-Object Text.UTF8Encoding($false)))
}
function Get-RcValue([string[]]$lines, [string]$sec, [string]$key) {
    $in = $false
    foreach ($l in $lines) {
        if ($l -match '^\[(.+)\]\s*$') { $in = ($Matches[1] -eq $sec); continue }
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
        if ($l -match '^\[(.+)\]\s*$') {
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
    $list = @(); if ($cur) { $list = $cur.Split(':') | Where-Object { $_ -and $_ -ne 'postproc' -and $_ -ne 'pushmpc_out' } }
    if ($add) { $list += 'postproc', 'pushmpc_out' }
    return ($list -join ':')
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
        Write-Host "video-filter: $(Get-RcValue (Read-Rc $VlcRc) 'core' 'video-filter')"
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
        Copy-Item -Force $Plugin (Join-Path $PluginDir 'libpushmpc_plugin.dll')
        Set-UserPluginPath $PluginDir $true
        $lines = Read-Rc $VlcRc
        if ((Test-Path $VlcRc) -and -not (Test-Path "$VlcRc.pushmpc-backup")) { Copy-Item $VlcRc "$VlcRc.pushmpc-backup" }
        $state = @{
            'video-filter' = Get-RcValue $lines 'core' 'video-filter'
            'd3d11-upscale-mode' = Get-RcValue $lines 'direct3d11' 'd3d11-upscale-mode'
        }
        $prev = Read-State; if ($prev) { $state = $prev }  # a repeated install keeps the first originals
        $state | ConvertTo-Json | Set-Content -Encoding UTF8 $stateFile
        $lines = Set-RcValue $lines 'core' 'video-filter' (Join-Filters (Get-RcValue $lines 'core' 'video-filter') $true)
        $lines = Set-RcValue $lines 'direct3d11' 'd3d11-upscale-mode' 'super'
        $lines = Set-RcValue $lines 'pushmpc' 'pushmpc-smoother' '0'
        Write-Rc $VlcRc $lines
        Write-Host "PushMPC for VLC installed: plugin in $PluginDir, settings in $VlcRc"
    }
    'Uninstall' {
        $state = Read-State
        if (Test-Path $VlcRc) {
            $lines = Read-Rc $VlcRc
            $rest = Join-Filters (Get-RcValue $lines 'core' 'video-filter') $false
            $lines = Set-RcValue $lines 'core' 'video-filter' $(if ($rest) { $rest } else { $null })
            $orig = if ($state) { $state['d3d11-upscale-mode'] } else { $null }
            $lines = Set-RcValue $lines 'direct3d11' 'd3d11-upscale-mode' $orig
            $lines = Set-RcValue $lines 'pushmpc' 'pushmpc-smoother' $null
            Write-Rc $VlcRc $lines
        }
        Set-UserPluginPath $PluginDir $false
        Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $PluginDir 'libpushmpc_plugin.dll'), $stateFile
        if ((Test-Path $PluginDir) -and -not (Get-ChildItem $PluginDir)) { Remove-Item $PluginDir }
        Write-Host 'PushMPC for VLC removed.'
    }
}
