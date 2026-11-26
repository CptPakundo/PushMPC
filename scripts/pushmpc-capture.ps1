<#
  PushMPC capture mode: the fallback for players PushMPC cannot plug into (Windows).

  Capture mode runs PushFPS's own window capture on the player's window: frame generation, super resolution and the
  rest of PushFPS's picture on any player. It is a fallback, not the normal route:
    - it adds about 50-60 ms between the player and the screen (sound stays in the player, so lip sync may need
      PushFPS's audio offset), and it shows a second window on top of the player;
    - it cannot capture DRM-protected video (Netflix-style streaming apps and browsers show a black picture);
    - a native integration (MPC-HC, MPC-BE, PotPlayer, the mpv family, VLC, Kodi, Stremio) always gives a better picture.
  PushMPC's Setup turns it on only when it finds a player without a native integration, as the Start menu entry
  "PushMPC Capture".

  Usage:
    pushmpc-capture.ps1 launch   [-Player <process name>] [-Yes]   start PushFPS's window capture (installs capture
                                                                  mode's PushFPS first, after asking; -Yes answers ahead)
    pushmpc-capture.ps1 enable   [-Players <names>]                the Start menu entry (Setup calls this)
    pushmpc-capture.ps1 disable                                    removes the entry and capture mode's PushFPS
    pushmpc-capture.ps1 status
  -InstallDir: PushMPC's folder (default %LOCALAPPDATA%\PushMPC). -StartMenu: the Start menu folder (tests).
  -PushFpsDir: a PushFPS folder to use instead (tests). -DryRun: say what would happen. -Machine: "@@kind|text" lines.

  Isolation: capture mode never runs, reads settings from, or writes to a PushFPS the user installed. It has a PushFPS
  of its own in <InstallDir>\capture\PushFPS (PushFPS's official release zip, SHA-256 pinned below, put there by
  PushFPS's own installer with -InstallDir, -NoShortcuts and a TEMP of its own, so the installer writes no shortcut,
  no Apps entry and nothing into the user's %TEMP%\PushFPS-download). PushFPS keeps its state next to its program
  (logs\, crash dumps, runtime_local\: caches, colour LUTs, PushFlow's start marker, the AMD worker cache), so all of
  that stays in capture mode's folder too. On top: --data-dir <InstallDir>\capture\data (preferences, presets,
  sessions), QML_DISK_CACHE_PATH (Qt's compiled pages, otherwise %LOCALAPPDATA%\PushFPS\cache), TEMP/TMP (probe and
  card-test files, update staging) and --no-update-check. Uninstall deletes the whole folder.
  Works with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('launch', 'enable', 'disable', 'status')]
    [string]$Action = 'status',
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'PushMPC'),
    [string]$Player = '',
    [string[]]$Players = @(),
    [string]$StartMenu = (Join-Path ([Environment]::GetFolderPath('Programs')) 'PushMPC'),
    [string]$PushFpsDir = '',
    [switch]$Yes,
    [switch]$DryRun,
    [switch]$Machine
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# capture mode's PushFPS (the release PushMPC's engine is built from)
$PushFps = @{
    Version = '4.5.4'
    Asset   = 'PushFPS-4.5.4-win64.zip'
    Url     = 'https://github.com/DosukaSOL/PushFPS/releases/download/v4.5.4/PushFPS-4.5.4-win64.zip'
    Sha256  = '4BF3FE869EB8A2CA579DD7ADE849C0C6CB2BF959F1A434FD53849F8E715F0AF5'
    Size    = 138574882
}
$CaptureDir = Join-Path $InstallDir 'capture'
$AppDir = Join-Path $CaptureDir 'PushFPS'
$StateFile = Join-Path $CaptureDir 'capture.json'
$DataDir = Join-Path $CaptureDir 'data'
$TempDir = Join-Path $CaptureDir 'temp'
$Shortcut = Join-Path $StartMenu 'PushMPC Capture.lnk'

function M([string]$t) { [Console]::Out.WriteLine("@@$t"); [Console]::Out.Flush() }
function Info([string]$t) { if ($Machine) { M "info|$t" } else { Write-Host "    $t" } }
function Warn([string]$t) { if ($Machine) { M "warn|$t" } else { Write-Host "    $t" -ForegroundColor Yellow } }
function Fail([string]$t) { if ($Machine) { M "fail|$t" } else { Write-Host "  $t" -ForegroundColor Red }; exit 1 }

function Read-CaptureState {
    if (Test-Path -LiteralPath $StateFile) { try { return Get-Content -LiteralPath $StateFile -Raw | ConvertFrom-Json } catch { } }
    return [pscustomobject]@{ enabled = $false; players = @() }
}
function Write-CaptureState($s) {
    if ($DryRun) { return }
    New-Item -ItemType Directory -Force $CaptureDir | Out-Null
    $s | ConvertTo-Json | Set-Content -LiteralPath $StateFile -Encoding UTF8
}

# capture mode's own PushFPS.exe (never a PushFPS the user installed)
function Find-PushFps {
    foreach ($d in @($PushFpsDir, $AppDir)) { if ($d -and (Test-Path -LiteralPath (Join-Path $d 'PushFPS.exe'))) { return (Join-Path $d 'PushFPS.exe') } }
    return ''
}
function Get-Sha256([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToUpperInvariant() }
function Get-OwnProcesses { @(Get-Process -Name 'PushFPS' -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($AppDir.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) }) }

# PushFPS's release zip (pinned), its own installer into capture mode's folder
function Install-PushFps {
    $dl = Join-Path $CaptureDir 'download'
    $zip = Join-Path $dl $PushFps.Asset
    if ($DryRun) { Info "[dry run] would download $($PushFps.Url) (SHA-256 $($PushFps.Sha256)) and run its Install-PushFPS.ps1 -InstallDir $AppDir -NoShortcuts -NoLaunch"; return '' }
    New-Item -ItemType Directory -Force $dl, $TempDir | Out-Null
    if (-not ((Test-Path -LiteralPath $zip) -and (Get-Sha256 $zip) -eq $PushFps.Sha256)) {
        Info "Downloading PushFPS $($PushFps.Version) ($([math]::Round($PushFps.Size / 1MB)) MB) from its GitHub release"
        & (Join-Path $env:SystemRoot 'System32\curl.exe') -L --fail --retry 3 -C - -s -S -o $zip $PushFps.Url
        if ($LASTEXITCODE -ne 0) { Fail "The download of $($PushFps.Url) failed (curl exit $LASTEXITCODE); run capture mode again to continue it." }
        if ((Get-Sha256 $zip) -ne $PushFps.Sha256) { Remove-Item -LiteralPath $zip -Force; Fail 'The PushFPS download does not match its pinned SHA-256: deleted, nothing installed.' }
    }
    $x = Join-Path $CaptureDir 'installer'
    if (Test-Path -LiteralPath $x) { Remove-Item -LiteralPath $x -Recurse -Force }
    Expand-Archive -LiteralPath $zip -DestinationPath $x -Force
    $inst = Get-ChildItem -LiteralPath $x -Recurse -Filter 'Install-PushFPS.ps1' | Select-Object -First 1
    if (-not $inst) { Fail "PushFPS's release zip has no Install-PushFPS.ps1: not installed." }
    # PushFPS's installer keeps Veyra's runtime package in %TEMP%\PushFPS-download: with a TEMP of its own it neither
    # reads nor writes the user's. A package PushMPC already holds (its own download folder) is linked in, read-only
    # use; the installer checks its SHA-256 either way.
    $cache = Join-Path $TempDir 'PushFPS-download'
    New-Item -ItemType Directory -Force $cache | Out-Null
    foreach ($z in @(Get-ChildItem -LiteralPath (Join-Path $InstallDir 'download') -Filter 'Veyra-*-win64-portable.zip' -ErrorAction SilentlyContinue)) {
        $t = Join-Path $cache $z.Name
        if (-not (Test-Path -LiteralPath $t)) { try { New-Item -ItemType HardLink -Path $t -Target $z.FullName | Out-Null } catch { Copy-Item -LiteralPath $z.FullName $t } }
    }
    Info "Running PushFPS's installer into $AppDir (it downloads Veyra's runtime package for your GPU unless PushMPC has it)"
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $keep = @{ TEMP = $env:TEMP; TMP = $env:TMP }
    $env:TEMP = $TempDir; $env:TMP = $TempDir
    try {
        $p = Start-Process -FilePath $ps -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($inst.FullName)`"", '-NoShortcuts', '-NoLaunch', '-ExperimentalExtras', 'Yes', '-InstallDir', "`"$AppDir`"") -Wait -PassThru -WindowStyle $(if ($Machine) { 'Hidden' } else { 'Normal' })
    } finally { $env:TEMP = $keep.TEMP; $env:TMP = $keep.TMP }
    if ($p.ExitCode -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $AppDir 'PushFPS.exe'))) { Fail "PushFPS's installer did not finish (exit $($p.ExitCode))." }
    foreach ($d in @($dl, $x, $cache)) { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue }
    Info "PushFPS $($PushFps.Version) installed for capture mode ($AppDir)"
    return (Join-Path $AppDir 'PushFPS.exe')
}

# PushFPS versions whose positional screen: URI start was checked; others get the window picker (--dialog screen)
$TestedPushFps = @('4.5.3', '4.5.4')   # 4.5.4: no change to the URI start (v4.5.3..v4.5.4 diff)
function Get-PushFpsVersion([string]$exe) {
    $vf = Join-Path (Split-Path $exe) 'PushFPS-VERSION.txt'
    if (Test-Path -LiteralPath $vf) { $m = [regex]::Match((Get-Content -LiteralPath $vf -Raw), '\d+\.\d+\.\d+'); if ($m.Success) { return $m.Value } }
    return "$((Get-Item -LiteralPath $exe).VersionInfo.ProductVersion)" -replace '^(\d+\.\d+\.\d+).*$', '$1'
}
# screen:<kind 0 = window>:<HWND>:<method 0 = WGC>:<fps 0 = the display's>:<bits 1 cursor, 2 borderless>:<crop L:T:R:B>
function New-WindowUri($hwnd) { "screen:0:$([uint64]$hwnd.ToInt64()):0:0:2:0:0:0:0" }
# the process names of the players capture mode is for (Setup's names), and a player's mpv.conf when it reads one
$PlayerProcesses = [ordered]@{ 'KMPlayer 64X' = 'KMPlayer64'; 'KMPlayer' = 'KMPlayer'; 'GOM Player' = 'GOM'; 'Emby Theater' = 'Emby.Theater'
    'Media Player' = 'Microsoft.Media.Player'; 'VLC' = 'vlc'; 'Plex HTPC' = 'Plex HTPC'; 'Plex for Windows' = 'Plex'; 'Stremio' = 'stremio'; 'Kodi' = 'kodi'
    'Jellyfin MPV Shim' = 'jellyfin-mpv-shim'; 'Plex-MPV-Shim' = 'plex-mpv-shim' }
$PlayerMpvConf = @{ 'Plex HTPC' = (Join-Path $env:LOCALAPPDATA 'Plex HTPC\mpv.conf'); 'Plex' = (Join-Path $env:LOCALAPPDATA 'Plex\mpv.conf') }
function Find-PlayerWindow {
    $names = @()
    if ($Player) { $names = @($Player) }
    else { foreach ($p in @((Read-CaptureState).players)) { foreach ($k in $PlayerProcesses.Keys) { if ("$p" -like "$k*") { $names += $PlayerProcesses[$k] } } } }
    foreach ($n in $names) {
        $proc = Get-Process -Name $n -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
        if ($proc) { return $proc }
    }
    return $null
}
# Lip sync: the captured picture reaches the screen about 60-100 ms after the player's own sound (the capture plus one
# source frame for interpolation). A player built on libmpv that reads an mpv.conf (Plex with its own libmpv) gets
# audio-delay=+0.080 in a marked part while capture mode runs; it applies from the next video the player opens.
# Other players: their own audio delay setting (the guide says where).
$DelayBegin = '# >>> PushMPC capture mode: audio delay while capture mode runs (removed when PushFPS closes)'
$DelayEnd = '# <<< PushMPC capture mode'
function Set-PlayerAudioDelay($proc) {
    $conf = $PlayerMpvConf[$proc.ProcessName]
    if (-not $conf -or -not (Test-Path -LiteralPath (Split-Path $conf))) { Info "lip sync: set about +80 ms audio delay in $($proc.ProcessName)'s own settings if the sound comes early"; return '' }
    $text = if (Test-Path -LiteralPath $conf) { [IO.File]::ReadAllText($conf) } else { '' }
    if ($text.Contains($DelayBegin)) { return $conf }
    $nl = if ($text.Contains("`r`n") -or -not $text) { "`r`n" } else { "`n" }
    $add = $(if ($text -and -not $text.EndsWith("`n")) { $nl } else { '' }) + $DelayBegin + $nl + 'audio-delay=+0.080' + $nl + $DelayEnd + $nl
    [IO.File]::WriteAllText($conf, $text + $add, (New-Object System.Text.UTF8Encoding($false)))
    Info "lip sync: audio-delay=+0.080 in $conf while capture mode runs (from the next video Plex opens)"
    return $conf
}
function Clear-PlayerAudioDelay([string]$conf) {
    if (-not (Test-Path -LiteralPath $conf)) { return }
    $text = [IO.File]::ReadAllText($conf)
    $i = $text.IndexOf($DelayBegin); if ($i -lt 0) { return }
    $j = $text.IndexOf($DelayEnd, $i); if ($j -lt 0) { return }
    $end = $j + $DelayEnd.Length
    while ($end -lt $text.Length -and ($text[$end] -eq [char]13 -or $text[$end] -eq [char]10)) { $end++ }
    $new = $text.Remove($i, $end - $i)
    if ($new.Length -eq 0) { Remove-Item -LiteralPath $conf -Force } else { [IO.File]::WriteAllText($conf, $new, (New-Object System.Text.UTF8Encoding($false))) }
}

function Ask([string]$text) {
    if ($Yes) { return $true }
    if ($Machine -or -not [Environment]::UserInteractive) { return $false }
    Add-Type -AssemblyName System.Windows.Forms
    return [System.Windows.Forms.MessageBox]::Show($text, 'PushMPC capture mode', 'YesNo', 'Question') -eq 'Yes'
}

switch ($Action) {
    'enable' {
        $s = Read-CaptureState
        $s | Add-Member -NotePropertyName enabled -NotePropertyValue $true -Force
        $s | Add-Member -NotePropertyName players -NotePropertyValue @($Players) -Force
        $me = Join-Path $InstallDir 'filter\pushmpc-capture.ps1'
        if (-not (Test-Path -LiteralPath $me)) { $me = $MyInvocation.MyCommand.Path }
        if ($DryRun) { Info "[dry run] would create $Shortcut" }
        else {
            New-Item -ItemType Directory -Force $StartMenu, $DataDir | Out-Null
            $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
            $sh = (New-Object -ComObject WScript.Shell).CreateShortcut($Shortcut)
            $sh.TargetPath = $ps
            $sh.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$me`" launch"
            $sh.Description = 'PushMPC capture mode (fallback for players without a PushMPC integration; about 50-60 ms delay, no DRM video)'
            $ico = Join-Path $InstallDir 'PushMpcSetup.exe'; if (Test-Path -LiteralPath $ico) { $sh.IconLocation = "$ico,0" }
            $sh.Save()
        }
        Write-CaptureState $s
        Info "Capture mode (fallback) is on for: $(if ($Players.Count) { $Players -join ', ' } else { 'any player' }). Start menu: PushMPC Capture"
    }
    'disable' {
        if ($DryRun) { Info "[dry run] would remove $Shortcut and $CaptureDir (capture mode's PushFPS with it)"; break }
        if (Get-OwnProcesses) { Warn "Capture mode's PushFPS is running: close it, then remove capture mode again ($CaptureDir is kept)" }
        if (Test-Path -LiteralPath $Shortcut) { Remove-Item -LiteralPath $Shortcut -Force; Info 'Start menu entry PushMPC Capture removed' }
        if ((Test-Path -LiteralPath $StartMenu) -and -not @(Get-ChildItem -LiteralPath $StartMenu -Force).Count) { Remove-Item -LiteralPath $StartMenu -Force }
        # capture mode's own folder only: PushFPS's -Uninstall is not used (it also deletes PushFPS's Start menu and
        # desktop shortcuts, which belong to a PushFPS the user may have installed)
        if ((Test-Path -LiteralPath $CaptureDir) -and -not (Get-OwnProcesses)) { Remove-Item -LiteralPath $CaptureDir -Recurse -Force; Info "removed $CaptureDir" }
        Info 'Capture mode is off'
    }
    'status' {
        $s = Read-CaptureState
        $exe = Find-PushFps
        Info ("capture mode: {0}; Start menu entry: {1}; its PushFPS: {2}" -f $(if ($s.enabled) { 'on (fallback)' } else { 'off' }), (Test-Path -LiteralPath $Shortcut),
              $(if ($exe) { $exe } else { 'not installed (installed on first use, after asking)' }))
        if (@($s.players).Count) { Info "for: $(@($s.players) -join ', ')" }
    }
    'launch' {
        $exe = Find-PushFps
        if (-not $exe -and $DryRun) { Info "[dry run] capture mode's PushFPS is not installed: would ask, then install PushFPS $($PushFps.Version) from $($PushFps.Url) into $AppDir"; break }
        if (-not $exe) {
            $q = "Capture mode runs PushFPS's window capture on your player's window (frame generation and super resolution for any player; about 50-60 ms delay; DRM-protected video stays black).`r`n`r`n" +
                 "Install capture mode's PushFPS $($PushFps.Version) now? It comes from PushFPS's GitHub release ($([math]::Round($PushFps.Size / 1MB)) MB, checked by SHA-256) and goes into PushMPC's folder; a PushFPS you installed yourself is not used or changed. It downloads Veyra's runtime package for your graphics card unless PushMPC already has it."
            if (-not (Ask $q)) { Info 'Capture mode needs PushFPS: not installed, nothing changed.'; exit 0 }
            $exe = Install-PushFps
            if (-not $exe) { exit 0 }
        }
        # the player's window: -Player, else the first running player capture mode is on for
        $proc = Find-PlayerWindow
        $argList = @('--data-dir', "`"$DataDir`"", '--no-update-check')
        $ver = if (Test-Path -LiteralPath $exe) { Get-PushFpsVersion $exe } else { '' }
        if ($proc -and ($ver -in $TestedPushFps)) {
            # a window capture of the player opened directly (PushFPS's own screen: URI, the last argument): WGC, no cursor,
            # no yellow border (bits 2), the display's rate; the picture-only page
            $argList += @('--page', 'min', (New-WindowUri $proc.MainWindowHandle))
            Info "player window: $($proc.MainWindowTitle) ($($proc.ProcessName)), captured directly"
        } else {
            $argList += @('--dialog', 'screen')
            if (-not $proc) { Info 'no running player window found: pick the player''s window in PushFPS' }
            else { Info "PushFPS $ver is not a version the direct window start was tested with: pick the player's window in PushFPS" }
        }
        if ($DryRun) { Info "[dry run] would start $exe $($argList -join ' ') (TEMP and QML_DISK_CACHE_PATH in $CaptureDir)"; break }
        New-Item -ItemType Directory -Force $DataDir, $TempDir, (Join-Path $DataDir 'qml-cache') | Out-Null
        # the child inherits these: Qt's QML cache, temp files and update staging stay in capture mode's folder
        $env:QML_DISK_CACHE_PATH = Join-Path $DataDir 'qml-cache'; $env:TEMP = $TempDir; $env:TMP = $TempDir
        $delay = if ($proc) { Set-PlayerAudioDelay $proc } else { '' }
        $pf = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory (Split-Path $exe) -PassThru
        Info 'PushFPS started with its window capture'
        # the audio delay lasts while capture mode runs: put back when PushFPS closes
        if ($delay) { try { $pf.WaitForExit() } catch { }; Clear-PlayerAudioDelay $delay }
    }
}
