<#
  PushMPC capture mode: the fallback for players PushMPC cannot plug into (Windows).

  Capture mode runs PushFPS's own window capture on the player's window: frame generation, super resolution and the
  rest of PushFPS's picture on any player. It is a fallback, not the normal route:
    - it adds about 50-60 ms between the player and the screen (sound stays in the player, so lip sync may need
      PushFPS's audio offset), and it shows a second window on top of the player;
    - it cannot capture DRM-protected video (Netflix-style streaming apps and browsers show a black picture);
    - a native integration (MPC-HC, MPC-BE, PotPlayer, the mpv family, VLC, Kodi, Stremio) always gives a better picture.
  PushMPC's Setup turns it on only when it finds a player without a native integration, as the Start menu entry
  "PushMPC Capture (fallback)".

  Usage:
    pushmpc-capture.ps1 launch   [-Player <process name>] [-Yes]   start PushFPS's window capture (installs PushFPS first
                                                                  when it is missing, after asking; -Yes answers ahead)
    pushmpc-capture.ps1 enable   [-Players <names>]                the Start menu entry (Setup calls this)
    pushmpc-capture.ps1 disable                                    removes the entry, and PushFPS if capture mode installed it
    pushmpc-capture.ps1 status
  -InstallDir: PushMPC's folder (default %LOCALAPPDATA%\PushMPC). -StartMenu: the Start menu folder (tests).
  -DryRun: say what would happen. -Machine: "@@kind|text" lines for PushMPC-Setup.exe.
  PushFPS is reused when it is installed (its own settings are never touched: capture mode keeps its settings in
  <InstallDir>\capture\data). Otherwise PushFPS's official release zip is downloaded from PushFPS's GitHub release,
  checked against the SHA-256 pinned below, and its own installer runs (per user, no administrator rights; it fetches
  Veyra's runtime package as PushFPS always does).
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

# PushFPS's release that capture mode installs when PushFPS is missing (the same release PushMPC's engine is built from)
$PushFps = @{
    Version = '4.5.3'
    Asset   = 'PushFPS-4.5.3-win64.zip'
    Url     = 'https://github.com/DosukaSOL/PushFPS/releases/download/v4.5.3/PushFPS-4.5.3-win64.zip'
    Sha256  = '8466AB7187D9AD7F8DB7AC54582E3482FFA4E75D3FF97B28E434816B8FA7FF31'
    Size    = 138569477
}
$CaptureDir = Join-Path $InstallDir 'capture'
$StateFile = Join-Path $CaptureDir 'capture.json'
$DataDir = Join-Path $CaptureDir 'data'
$Shortcut = Join-Path $StartMenu 'PushMPC Capture (fallback).lnk'

function M([string]$t) { [Console]::Out.WriteLine("@@$t"); [Console]::Out.Flush() }
function Info([string]$t) { if ($Machine) { M "info|$t" } else { Write-Host "    $t" } }
function Warn([string]$t) { if ($Machine) { M "warn|$t" } else { Write-Host "    $t" -ForegroundColor Yellow } }
function Fail([string]$t) { if ($Machine) { M "fail|$t" } else { Write-Host "  $t" -ForegroundColor Red }; exit 1 }

function Read-CaptureState {
    if (Test-Path -LiteralPath $StateFile) { try { return Get-Content -LiteralPath $StateFile -Raw | ConvertFrom-Json } catch { } }
    return [pscustomobject]@{ enabled = $false; players = @(); pushfpsInstalledBy = ''; pushfpsDir = '' }
}
function Write-CaptureState($s) {
    if ($DryRun) { return }
    New-Item -ItemType Directory -Force $CaptureDir | Out-Null
    $s | ConvertTo-Json | Set-Content -LiteralPath $StateFile -Encoding UTF8
}

# PushFPS.exe: -PushFpsDir, the folder capture mode installed, an installed PushFPS (its Apps entry or the default folder)
function Find-PushFps {
    $s = Read-CaptureState
    $dirs = @($PushFpsDir, $s.pushfpsDir)
    foreach ($root in 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall') {
        Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
            $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
            if ($p -and "$($p.DisplayName)" -like 'PushFPS*' -and $p.InstallLocation) { $dirs += $p.InstallLocation }
        }
    }
    $dirs += (Join-Path $env:LOCALAPPDATA 'Programs\PushFPS')
    foreach ($d in $dirs) { if ($d -and (Test-Path -LiteralPath (Join-Path $d 'PushFPS.exe'))) { return (Join-Path $d 'PushFPS.exe') } }
    return ''
}

function Get-Sha256([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToUpperInvariant() }

# PushFPS's release zip (pinned), its installer run per user without shortcuts (the Start menu entry is ours)
function Install-PushFps {
    $dl = Join-Path $CaptureDir 'download'
    $zip = Join-Path $dl $PushFps.Asset
    if ($DryRun) { Info "[dry run] would download $($PushFps.Url) (SHA-256 $($PushFps.Sha256)) and run its Install-PushFPS.ps1 -NoShortcuts -NoLaunch"; return '' }
    New-Item -ItemType Directory -Force $dl | Out-Null
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
    $target = Join-Path $env:LOCALAPPDATA 'Programs\PushFPS'
    Info "Running PushFPS's installer (per user, into $target; it downloads Veyra's runtime package for your GPU)"
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $p = Start-Process -FilePath $ps -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($inst.FullName)`"", '-NoShortcuts', '-NoLaunch', '-ExperimentalExtras', 'Yes', '-InstallDir', "`"$target`"") -Wait -PassThru -WindowStyle $(if ($Machine) { 'Hidden' } else { 'Normal' })
    if ($p.ExitCode -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $target 'PushFPS.exe'))) { Fail "PushFPS's installer did not finish (exit $($p.ExitCode))." }
    Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
    $s = Read-CaptureState
    $s | Add-Member -NotePropertyName pushfpsInstalledBy -NotePropertyValue 'pushmpc-capture' -Force
    $s | Add-Member -NotePropertyName pushfpsDir -NotePropertyValue $target -Force
    $s | Add-Member -NotePropertyName pushfpsInstaller -NotePropertyValue $inst.FullName -Force
    Write-CaptureState $s
    Info "PushFPS $($PushFps.Version) installed for capture mode"
    return (Join-Path $target 'PushFPS.exe')
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
        Info "Capture mode (fallback) is on for: $(if ($Players.Count) { $Players -join ', ' } else { 'any player' }). Start menu: PushMPC Capture (fallback)"
    }
    'disable' {
        $s = Read-CaptureState
        if ($DryRun) { Info "[dry run] would remove $Shortcut and $CaptureDir"; break }
        if (Test-Path -LiteralPath $Shortcut) { Remove-Item -LiteralPath $Shortcut -Force; Info 'Start menu entry PushMPC Capture (fallback) removed' }
        if ((Test-Path -LiteralPath $StartMenu) -and -not @(Get-ChildItem -LiteralPath $StartMenu -Force).Count) { Remove-Item -LiteralPath $StartMenu -Force }
        # PushFPS only when capture mode installed it (a PushFPS the user installed stays as it is)
        if ($s.pushfpsInstalledBy -eq 'pushmpc-capture' -and $s.pushfpsInstaller -and (Test-Path -LiteralPath $s.pushfpsInstaller)) {
            $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
            $p = Start-Process -FilePath $ps -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($s.pushfpsInstaller)`"", '-Uninstall', '-InstallDir', "`"$($s.pushfpsDir)`"") -Wait -PassThru -WindowStyle Hidden
            if ($p.ExitCode -eq 0) { Info 'PushFPS (installed by capture mode) removed' } else { Warn "PushFPS's uninstall returned $($p.ExitCode): remove it from Windows Settings > Apps" }
        }
        if (Test-Path -LiteralPath $CaptureDir) { Remove-Item -LiteralPath $CaptureDir -Recurse -Force }
        Info 'Capture mode is off'
    }
    'status' {
        $s = Read-CaptureState
        $exe = Find-PushFps
        Info ("capture mode: {0}; Start menu entry: {1}; PushFPS: {2}{3}" -f $(if ($s.enabled) { 'on (fallback)' } else { 'off' }), (Test-Path -LiteralPath $Shortcut),
              $(if ($exe) { $exe } else { 'not installed (installed on first use, after asking)' }), $(if ($s.pushfpsInstalledBy) { ' (installed by capture mode)' } else { '' }))
        if (@($s.players).Count) { Info "for: $(@($s.players) -join ', ')" }
    }
    'launch' {
        $exe = Find-PushFps
        if (-not $exe -and $DryRun) { Info "[dry run] PushFPS is not installed: would ask, then install PushFPS $($PushFps.Version) from $($PushFps.Url)"; break }
        if (-not $exe) {
            $q ="Capture mode runs PushFPS's window capture on your player's window (frame generation and super resolution for any player; about 50-60 ms delay; DRM-protected video stays black).`r`n`r`n" +
                 "PushFPS is not installed. Install PushFPS $($PushFps.Version) now? It comes from PushFPS's GitHub release ($([math]::Round($PushFps.Size / 1MB)) MB, checked by SHA-256), installs for your account only, and downloads Veyra's runtime package for your graphics card."
            if (-not (Ask $q)) { Info 'Capture mode needs PushFPS: not installed, nothing changed.'; exit 0 }
            $exe = Install-PushFps
            if (-not $exe) { exit 0 }
        }
        # the player window to bring to the front (PushFPS lists every window; the player's is then on top)
        if ($Player) {
            $proc = Get-Process -Name $Player -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
            if ($proc) { Info "player window: $($proc.MainWindowTitle) ($($proc.ProcessName))" } else { Warn "$Player is not running: start it, then pick its window in PushFPS" }
        }
        $argList = @('--data-dir', "`"$DataDir`"", '--dialog', 'screen')
        if ($DryRun) { Info "[dry run] would start $exe $($argList -join ' ')"; break }
        New-Item -ItemType Directory -Force $DataDir | Out-Null
        Start-Process -FilePath $exe -ArgumentList $argList | Out-Null
        Info "PushFPS started with its window capture: pick the player's window (capture mode keeps its own settings in $DataDir)"
    }
}
