<#
  mpv-integration.ps1: puts PushMPC's VapourSynth filter (pushmpc.Enhance through pushmpc.vpy) into mpv-based players and
  takes it out again exactly. The main installer calls it; it also runs on its own:
    mpv-integration.ps1 -Action Install|Uninstall|Status [-Root <fake root>] [-ScriptPath <pushmpc.vpy>] [-MpvExe <mpv.exe>...]
                        [-BufferedFrames 4] [-ConcurrentFrames 2] [-NoSmplayer] [-RuntimeOnPath] [-WhatIf]
  Dot-source it (. .\mpv-integration.ps1) to get the functions without running an action.

  What it changes (Install), each with a backup and an exact restore (Uninstall):
   - mpv.conf of every mpv found: portable_config\mpv.conf next to mpv.exe when that folder exists (shinchiro's portable
     layout), else %APPDATA%\mpv\mpv.conf. A marked block goes at the very top (before any [profile], so it is global):
       # >>> PushMPC managed ...
       vf-append=@pushmpc:vapoursynth=file=[<pushmpc.vpy>]:buffered-frames=4:concurrent-frames=2
       hwdec=auto-copy            (VapourSynth gets CPU frames only: copy-back decoding)
       hr-seek-framedrop=no       (no dropped frames around a seek: the filter sees a continuous stream)
       interpolation=no           (mpv's own frame blending would fight the generated frames)
       # <<< PushMPC managed
     The original file is copied to mpv.conf.pushmpc-backup (or mpv.conf.pushmpc-absent when there was none). Uninstall
     removes the block; when nothing else changed since, the file is byte-identical to the backup and the backup goes.
   - SMPlayer (experimental: key names to verify): [advanced] mplayer_additional_options in smplayer.ini gets the same
     options as mpv command-line flags, appended after the user's own; the old value is kept in smplayer.ini.pushmpc-backup.json.
   - mpv.net: the same block in its own mpv.conf (portable_config\mpv.conf next to mpvnet.exe when that folder exists,
     else %APPDATA%\mpv.net\mpv.conf). Its bundled libmpv is a shinchiro-type build with VapourSynth.
   - MPC-QT (libmpv, ignores mpv.conf): its Options > Tweaks "mpv options file" (settings.json keys
     tweaksMpvOptionsFileChkBox / tweaksMpvOptionsFilePath; %APPDATA%\mpc-qt or <exe>\config with portable.txt). When
     the user already uses an options file, the block goes into that file; else the switch is turned on and pointed at
     %LOCALAPPDATA%\PushMPC\mpc-qt-mpv.conf (the block alone). settings.json is kept in settings.json.pushmpc-backup and
     the old two values in settings.json.pushmpc-backup.json. MPC-QT must be closed (it rewrites settings.json on exit).
   - Jellyfin MPV Shim / Plex-MPV-Shim (%APPDATA%\jellyfin-mpv-shim, %APPDATA%\plex-mpv-shim): the block in the mpv.conf
     beside conf.json, and conf.json's mpv_ext = true, mpv_ext_path = the mpv found (their bundled libmpv has no
     VapourSynth on Windows); conf.json backed up the same way. Without a VapourSynth-capable mpv: not changed.
   - Kodi (%APPDATA%\Kodi\userdata, or portable_data\userdata next to kodi.exe): playercorefactory.xml gets a marked
     <player name="PushMPC mpv"> (ExternalPlayer: the mpv found, fullscreen) inside <players> and a marked
     <rules action="prepend"> sending every video to it. Kodi's own player has no filter hook; mpv then reads its mpv.conf
     with the block. Backup/absent marker and exact restore as for mpv.conf.
   - with -RuntimeOnPath: %LOCALAPPDATA%\PushMPC\vapoursynth on the user's PATH (mpv loads VSScript.dll from the DLL
     search path); removed again only if this script added it.
  Detected but not changed: SVP's own mpv (SVP manages it); Stremio 5 (its mpv sets the vf property itself whenever the
  display state changes, which drops any configured filter: use Stremio's "external player" with mpv, see
  docs\players-mpv.md).
  -Root: every folder (APPDATA, LOCALAPPDATA, Program Files, the user PATH) is taken under this fake root and PATH is
  not searched: the tests use it.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidateSet('Install', 'Uninstall', 'Status', 'None')][string]$Action = 'None',
    [string]$Root = '',
    [string]$ScriptPath = '',
    [string[]]$MpvExe = @(),
    [int]$BufferedFrames = 4,
    [int]$ConcurrentFrames = 2,
    [switch]$NoSmplayer,
    [switch]$RuntimeOnPath
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$script:PmBegin = '# >>> PushMPC managed: do not edit between these lines (PushMPC uninstall removes them)'
$script:PmEnd = '# <<< PushMPC managed'

function Get-PmEnv([string]$Root) {
    if ($Root) {
        $r = $Root.TrimEnd('\')
        return [pscustomobject]@{ Fake = $true; AppData = "$r\AppData\Roaming"; LocalAppData = "$r\AppData\Local"; ProgramFiles = "$r\Program Files"
            ProgramFilesX86 = "$r\Program Files (x86)"; UserProfile = "$r\User"; UserPathFile = "$r\user-path.txt" }
    }
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    return [pscustomobject]@{ Fake = $false; AppData = $env:APPDATA; LocalAppData = $env:LOCALAPPDATA; ProgramFiles = $env:ProgramFiles
        ProgramFilesX86 = $(if ($pf86) { $pf86 } else { $env:ProgramFiles }); UserProfile = $env:USERPROFILE; UserPathFile = '' }
}

# ------------------------------------------------------------------------------------------------ text with exact bytes
function Read-PmText([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $text = [Text.UTF8Encoding]::new($false).GetString($bytes, $(if ($bom) { 3 } else { 0 }), $bytes.Length - $(if ($bom) { 3 } else { 0 }))
    return [pscustomobject]@{ Text = $text; Bom = $bom; Bytes = $bytes }
}
function Write-PmText([string]$Path, [string]$Text, [bool]$Bom) {
    $body = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    if ($Bom) { $body = [byte[]](0xEF, 0xBB, 0xBF) + $body }
    [IO.File]::WriteAllBytes($Path, $body)
}
function Get-PmNewline([string]$Text) { if (-not $Text -or $Text.Contains("`r`n")) { "`r`n" } else { "`n" } }

function Get-PmVfOption([string]$ScriptPath, [int]$Buffered, [int]$Concurrent) {
    $p = $ScriptPath.Replace('\', '/')
    # mpv's [...] quoting keeps ':' and spaces in the path; ']' cannot be quoted that way: %n% (length) quoting then
    $q = if ($p.Contains(']')) { '%' + $p.Length + '%' + $p } else { "[$p]" }
    return "@pushmpc:vapoursynth=file=${q}:buffered-frames=${Buffered}:concurrent-frames=${Concurrent}"
}
function Get-PmBlock([string]$ScriptPath, [int]$Buffered, [int]$Concurrent, [string]$Nl) {
    $lines = @($script:PmBegin, "vf-append=$(Get-PmVfOption $ScriptPath $Buffered $Concurrent)", 'hwdec=auto-copy', 'hr-seek-framedrop=no', 'interpolation=no', $script:PmEnd)
    return ($lines -join $Nl) + $Nl
}
# the block's span in the text: start index and length (including its trailing newline), or $null
function Find-PmBlock([string]$Text) {
    $b = $Text.IndexOf($script:PmBegin)
    if ($b -lt 0) { return $null }
    $e = $Text.IndexOf($script:PmEnd, $b)
    if ($e -lt 0) { return $null }
    $end = $e + $script:PmEnd.Length
    if ($end -lt $Text.Length -and $Text[$end] -eq "`r") { $end++ }
    if ($end -lt $Text.Length -and $Text[$end] -eq "`n") { $end++ }
    return [pscustomobject]@{ Start = $b; Length = $end - $b }
}
function Remove-PmBlockText([string]$Text) {
    $f = Find-PmBlock $Text
    if (-not $f) { return $Text }
    return $Text.Remove($f.Start, $f.Length)
}

# lines outside our block that would fight it (global section only, before the first [profile])
function Get-PmConflicts([string]$Text) {
    $out = @()
    $global = $true
    foreach ($l in ((Remove-PmBlockText $Text) -split "`r?`n")) {
        $t = $l.Trim()
        if ($t.StartsWith('[')) { $global = $false }
        if ($t -match '^(vf|vf-append|vf-add|vf-pre)\s*=.*vapoursynth' -or $t -match '^--?(vf|vf-append|vf-add)=.*vapoursynth') { $out += "another VapourSynth filter is set ($t): two filters would both run (SVP?)" }
        if ($global -and $t -match '^hwdec\s*=\s*(\S+)' -and $Matches[1] -notmatch '(-copy|^no)$') { $out += "a later global '$t' overrides hwdec=auto-copy: VapourSynth needs copy-back decoding" }
        if ($global -and $t -match '^interpolation\s*=\s*yes') { $out += "a later global '$t' turns mpv's frame blending back on" }
    }
    return $out
}

# ------------------------------------------------------------------------------------------------ players
function Find-PmPlayers($E, [string[]]$MpvExe) {
    $found = @()
    $mpvs = @()
    foreach ($m in $MpvExe) { if ($m -and (Test-Path -LiteralPath $m)) { $mpvs += (Resolve-Path -LiteralPath $m).Path } }
    if (-not $E.Fake) {
        foreach ($c in @(Get-Command mpv.exe -All -ErrorAction SilentlyContinue)) { if ($c.Source) { $mpvs += $c.Source } }
        foreach ($c in @(Get-Command mpvnet.exe -All -ErrorAction SilentlyContinue)) { if ($c.Source) { $mpvs += $c.Source } }
    }
    foreach ($d in @("$($E.LocalAppData)\Programs\mpv", "$($E.ProgramFiles)\mpv", "$($E.UserProfile)\scoop\apps\mpv\current", "$($E.UserProfile)\scoop\apps\mpv-git\current")) {
        if (Test-Path -LiteralPath "$d\mpv.exe") { $mpvs += "$d\mpv.exe" }
    }
    foreach ($d in @("$($E.LocalAppData)\Programs\mpv.net", "$($E.ProgramFiles)\mpv.net", "$($E.UserProfile)\scoop\apps\mpv.net\current", "$($E.UserProfile)\scoop\apps\mpvnet\current")) {
        if (Test-Path -LiteralPath "$d\mpvnet.exe") { $mpvs += "$d\mpvnet.exe" }
    }
    $svpDirs = @("$($E.ProgramFilesX86)\SVP 4\mpv64", "$($E.ProgramFiles)\SVP 4\mpv64")
    $smDirs = @("$($E.ProgramFiles)\SMPlayer", "$($E.ProgramFilesX86)\SMPlayer")
    $seen = @{}
    foreach ($m in $mpvs) {
        $key = $m.ToLowerInvariant(); if ($seen[$key]) { continue }; $seen[$key] = $true
        $dir = Split-Path -Parent $m
        if ($svpDirs | Where-Object { $_ -and $dir -ieq $_ }) { continue }
        if ($smDirs | Where-Object { $_ -and $dir -ieq "$_\mpv" }) { continue }
        $portable = Test-Path -LiteralPath "$dir\portable_config" -PathType Container
        if ((Split-Path -Leaf $m) -ieq 'mpvnet.exe') {
            $found += [pscustomobject]@{ Player = 'mpv.net'; Exe = $m; Kind = 'mpvconf'
                Config = $(if ($portable) { "$dir\portable_config\mpv.conf" } else { "$($E.AppData)\mpv.net\mpv.conf" }); Managed = $true
                Note = $(if ($portable) { 'portable_config next to mpvnet.exe' } else { 'mpv.net config folder' }) }
            continue
        }
        $shin = (Test-Path -LiteralPath "$dir\updater.bat") -or (Test-Path -LiteralPath "$dir\installer\mpv-install.bat") -or (Test-Path -LiteralPath "$dir\mpv.com")
        $found += [pscustomobject]@{ Player = $(if ($shin) { 'mpv (shinchiro build)' } else { 'mpv' }); Exe = $m; Kind = 'mpvconf'
            Config = $(if ($portable) { "$dir\portable_config\mpv.conf" } else { "$($E.AppData)\mpv\mpv.conf" }); Managed = $true
            Note = $(if ($portable) { 'portable_config next to mpv.exe' } else { 'user config folder' }) }
    }
    # the mpv that external-player hosts (Kodi, the shims) start: the first plain mpv.exe found (not mpv.net, not SVP's)
    $hostMpv = @($found | Where-Object { $_.Kind -eq 'mpvconf' -and (Split-Path -Leaf $_.Exe) -ieq 'mpv.exe' } | Select-Object -First 1)
    $hostExe = if ($hostMpv.Count) { $hostMpv[0].Exe } else { '' }
    foreach ($d in $svpDirs) {
        if (Test-Path -LiteralPath "$d\mpv.exe") { $found += [pscustomobject]@{ Player = "SVP's mpv"; Exe = "$d\mpv.exe"; Kind = 'none'; Config = ''; Managed = $false; Note = 'left alone: SVP manages its own mpv and filter' } }
    }
    foreach ($d in $smDirs) {
        if (-not (Test-Path -LiteralPath "$d\smplayer.exe")) { continue }
        $ini = if (Test-Path -LiteralPath "$d\smplayer.ini") { "$d\smplayer.ini" } else { "$($E.AppData)\SMPlayer\smplayer.ini" }
        $found += [pscustomobject]@{ Player = 'SMPlayer'; Exe = "$d\smplayer.exe"; Kind = 'smplayer'; Config = $ini; Managed = $true
            Note = $(if (Test-Path -LiteralPath "$d\mpv\mpv.exe") { 'bundled mpv; options in smplayer.ini (experimental)' } else { 'no bundled mpv found; options in smplayer.ini (experimental)' }) }
    }
    $qtSeen = @{}
    foreach ($d in @("$($E.ProgramFiles)\mpc-qt", "$($E.LocalAppData)\Programs\mpc-qt", "$($E.ProgramFilesX86)\mpc-qt", "$($E.UserProfile)\scoop\apps\mpc-qt\current")) {
        if (-not (Test-Path -LiteralPath "$d\mpc-qt.exe") -or $qtSeen[$d.ToLowerInvariant()]) { continue }
        $qtSeen[$d.ToLowerInvariant()] = $true
        $portable = Test-Path -LiteralPath "$d\portable.txt"
        $found += [pscustomobject]@{ Player = 'MPC-QT'; Exe = "$d\mpc-qt.exe"; Kind = 'mpcqt'
            Config = $(if ($portable) { "$d\config\settings.json" } else { "$($E.AppData)\mpc-qt\settings.json" }); Managed = $true
            Note = 'Options > Tweaks > mpv options file (libmpv; mpv.conf is not read otherwise)' }
    }
    foreach ($s in @(@{ Name = 'Jellyfin MPV Shim'; Dir = 'jellyfin-mpv-shim' }, @{ Name = 'Plex-MPV-Shim'; Dir = 'plex-mpv-shim' })) {
        $cd = "$($E.AppData)\$($s.Dir)"
        if (-not (Test-Path -LiteralPath "$cd\conf.json")) { continue }
        $found += [pscustomobject]@{ Player = $s.Name; Exe = $hostExe; Kind = 'shim'; Config = "$cd\conf.json"; Managed = [bool]$hostExe
            Note = $(if ($hostExe) { "external mpv ($hostExe) with the block in $cd\mpv.conf" } else { 'not changed: its bundled mpv has no VapourSynth; install mpv (shinchiro build), then run PushMPC Setup again' }) }
    }
    $kodiDirs = @("$($E.ProgramFiles)\Kodi", "$($E.ProgramFilesX86)\Kodi", "$($E.LocalAppData)\Programs\Kodi") | Where-Object { Test-Path -LiteralPath "$_\kodi.exe" }
    $kodiUser = @()
    foreach ($d in $kodiDirs) { if (Test-Path -LiteralPath "$d\portable_data" -PathType Container) { $kodiUser += "$d\portable_data\userdata" } }
    if ((Test-Path -LiteralPath "$($E.AppData)\Kodi" -PathType Container) -or $kodiDirs) { $kodiUser += "$($E.AppData)\Kodi\userdata" }
    foreach ($u in ($kodiUser | Select-Object -Unique)) {
        $found += [pscustomobject]@{ Player = 'Kodi'; Exe = $hostExe; Kind = 'kodi'; Config = "$u\playercorefactory.xml"; Managed = [bool]$hostExe
            Note = $(if ($hostExe) { "videos play in mpv ($hostExe) as Kodi's external player" } else { 'not changed: Kodi needs mpv (shinchiro build) as its external player; install mpv, then run PushMPC Setup again' }) }
    }
    foreach ($d in @("$($E.LocalAppData)\Programs\Stremio", "$($E.ProgramFiles)\Stremio")) {
        $exe = @('stremio-shell-ng.exe', 'stremio.exe') | ForEach-Object { "$d\$_" } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        if (-not $exe) { continue }
        $found += [pscustomobject]@{ Player = 'Stremio'; Exe = $exe; Kind = 'none'; Config = ''; Managed = $false
            Note = "manual: Stremio resets mpv's video filters itself; play through mpv as the external player (docs\players-mpv.md)" }
    }
    # one mpv.conf may serve several mpv installs: one target each
    $byConf = @{}
    $result = @()
    foreach ($f in $found) {
        if ($f.Kind -ne 'mpvconf') { $result += $f; continue }
        $k = $f.Config.ToLowerInvariant()
        if ($byConf[$k]) { $byConf[$k].Exe += "; $($f.Exe)"; continue }
        $byConf[$k] = $f; $result += $f
    }
    return , $result
}

# ------------------------------------------------------------------------------------------------ mpv.conf
function Install-PmMpvConf {
    [CmdletBinding(SupportsShouldProcess = $true)] param([string]$Conf, [string]$Block)
    $dir = Split-Path -Parent $Conf
    $backup = "$Conf.pushmpc-backup"; $absent = "$Conf.pushmpc-absent"
    $createdDir = -not (Test-Path -LiteralPath $dir)
    if (Test-Path -LiteralPath $Conf) {
        $t = Read-PmText $Conf
        $nl = Get-PmNewline $t.Text
        $Block = $Block.Replace("`r`n", "`n"); if ($nl -eq "`r`n") { $Block = $Block.Replace("`n", "`r`n") }
        $f = Find-PmBlock $t.Text
        if ($f) { $new = $t.Text.Remove($f.Start, $f.Length).Insert($f.Start, $Block) }
        else {
            if (-not (Test-Path -LiteralPath $backup) -and $PSCmdlet.ShouldProcess($backup, 'back up mpv.conf')) { [IO.File]::WriteAllBytes($backup, $t.Bytes) }
            $new = $Block + $t.Text
        }
        if ($new -ne $t.Text -and $PSCmdlet.ShouldProcess($Conf, 'write the PushMPC block')) { Write-PmText $Conf $new $t.Bom }
        return (Get-PmConflicts $t.Text)
    }
    if ($PSCmdlet.ShouldProcess($Conf, 'create mpv.conf with the PushMPC block')) {
        if ($createdDir) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        [IO.File]::WriteAllText($absent, $(if ($createdDir) { 'created-dir' } else { 'file' }))
        Write-PmText $Conf $Block $false
    }
    return @()
}
function Uninstall-PmMpvConf {
    [CmdletBinding(SupportsShouldProcess = $true)] param([string]$Conf)
    $backup = "$Conf.pushmpc-backup"; $absent = "$Conf.pushmpc-absent"
    if (-not (Test-Path -LiteralPath $Conf)) {
        if ((Test-Path -LiteralPath $absent) -and $PSCmdlet.ShouldProcess($absent, 'remove')) { Remove-Item -LiteralPath $absent }
        return 'no mpv.conf'
    }
    $t = Read-PmText $Conf
    $stripped = Remove-PmBlockText $t.Text
    if (Test-Path -LiteralPath $absent) {
        $how = [IO.File]::ReadAllText($absent)
        if (-not $stripped.Trim()) {
            if ($PSCmdlet.ShouldProcess($Conf, 'remove the mpv.conf PushMPC created')) {
                Remove-Item -LiteralPath $Conf; Remove-Item -LiteralPath $absent
                $dir = Split-Path -Parent $Conf
                if ($how -eq 'created-dir' -and -not (Get-ChildItem -LiteralPath $dir -Force)) { Remove-Item -LiteralPath $dir }
            }
            return 'removed (PushMPC had created it)'
        }
        if ($PSCmdlet.ShouldProcess($Conf, 'remove the PushMPC block')) { Write-PmText $Conf $stripped $t.Bom; Remove-Item -LiteralPath $absent }
        return 'block removed; your later additions kept'
    }
    if (Test-Path -LiteralPath $backup) {
        $orig = [IO.File]::ReadAllBytes($backup)
        $enc = [Text.UTF8Encoding]::new($false).GetBytes($stripped)
        if ($t.Bom) { $enc = [byte[]](0xEF, 0xBB, 0xBF) + $enc }
        if ([Convert]::ToBase64String($enc) -eq [Convert]::ToBase64String($orig)) {
            if ($PSCmdlet.ShouldProcess($Conf, 'restore the backup')) { [IO.File]::WriteAllBytes($Conf, $orig); Remove-Item -LiteralPath $backup }
            return 'restored exactly from the backup'
        }
        if ($PSCmdlet.ShouldProcess($Conf, 'remove the PushMPC block')) { Write-PmText $Conf $stripped $t.Bom }
        return "block removed; mpv.conf changed since the install, so the backup stays at $backup"
    }
    if ($stripped -ne $t.Text -and $PSCmdlet.ShouldProcess($Conf, 'remove the PushMPC block')) { Write-PmText $Conf $stripped $t.Bom }
    return 'block removed (no backup was found)'
}

# ------------------------------------------------------------------------------------------------ SMPlayer
function Get-PmSmplayerFlags([string]$ScriptPath, [int]$Buffered, [int]$Concurrent) {
    return "--vf-append=$(Get-PmVfOption $ScriptPath $Buffered $Concurrent) --hwdec=auto-copy --hr-seek-framedrop=no --interpolation=no"
}
# [advanced] mplayer_additional_options: line index and value (a missing section or key: -1)
function Find-PmIniKey([string[]]$Lines, [string]$Section, [string]$Key) {
    $in = $false; $secAt = -1
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $t = $Lines[$i].Trim()
        if ($t.StartsWith('[')) { $in = $t -ieq "[$Section]"; if ($in) { $secAt = $i }; continue }
        if ($in -and $t -match "^$([regex]::Escape($Key))\s*=(.*)$") { return [pscustomobject]@{ Section = $secAt; Line = $i; Value = $Matches[1] } }
    }
    return [pscustomobject]@{ Section = $secAt; Line = -1; Value = $null }
}
function Install-PmSmplayer {
    [CmdletBinding(SupportsShouldProcess = $true)] param([string]$Ini, [string]$Flags)
    if (-not (Test-Path -LiteralPath $Ini)) { return @('smplayer.ini does not exist yet: start SMPlayer once, then run the install again') }
    $t = Read-PmText $Ini
    $nl = Get-PmNewline $t.Text
    $lines = [Collections.Generic.List[string]]::new([string[]]($t.Text -split "`r?`n"))
    $side = "$Ini.pushmpc-backup.json"
    $k = Find-PmIniKey $lines 'advanced' 'mplayer_additional_options'
    $cur = if ($k.Value -ne $null) { $k.Value } else { '' }
    if (Test-Path -LiteralPath $side) {
        $s = Get-Content -LiteralPath $side -Raw | ConvertFrom-Json
        $orig = [string]$s.value; $hadKey = [bool]$s.hadKey
    } else { $orig = $cur; $hadKey = $k.Line -ge 0 }
    $value = ($orig.Trim() + ' ' + $Flags).Trim()
    if ($k.Line -ge 0) { $lines[$k.Line] = "mplayer_additional_options=$value" }
    elseif ($k.Section -ge 0) { $lines.Insert($k.Section + 1, "mplayer_additional_options=$value") }
    else { if ($lines.Count -and $lines[$lines.Count - 1] -ne '') { $lines.Add('') }; $lines.Add('[advanced]'); $lines.Add("mplayer_additional_options=$value"); $lines.Add('') }
    if (-not (Test-Path -LiteralPath $side) -and $PSCmdlet.ShouldProcess($side, 'save the old SMPlayer options')) {
        [IO.File]::WriteAllText($side, (@{ value = $orig; hadKey = $hadKey; flags = $Flags } | ConvertTo-Json -Compress))
    }
    if ($PSCmdlet.ShouldProcess($Ini, 'add the PushMPC options')) { Write-PmText $Ini (($lines -join $nl)) $t.Bom }
    return @()
}
function Uninstall-PmSmplayer {
    [CmdletBinding(SupportsShouldProcess = $true)] param([string]$Ini)
    $side = "$Ini.pushmpc-backup.json"
    if (-not (Test-Path -LiteralPath $side)) { return 'not installed' }
    $s = Get-Content -LiteralPath $side -Raw | ConvertFrom-Json
    if (-not (Test-Path -LiteralPath $Ini)) { if ($PSCmdlet.ShouldProcess($side, 'remove')) { Remove-Item -LiteralPath $side }; return 'smplayer.ini is gone' }
    $t = Read-PmText $Ini
    $nl = Get-PmNewline $t.Text
    $lines = [Collections.Generic.List[string]]::new([string[]]($t.Text -split "`r?`n"))
    $k = Find-PmIniKey $lines 'advanced' 'mplayer_additional_options'
    $note = 'options restored'
    if ($k.Line -ge 0) {
        $v = $k.Value
        $pos = $v.IndexOf([string]$s.flags)
        if ($pos -ge 0) { $rest = ($v.Remove($pos, ([string]$s.flags).Length)).Trim() } else { $rest = $v; $note = 'the PushMPC options were no longer there' }
        if ($rest -eq ([string]$s.value).Trim()) { $rest = [string]$s.value }
        if (-not $s.hadKey -and -not $rest) { $lines.RemoveAt($k.Line) } else { $lines[$k.Line] = "mplayer_additional_options=$rest" }
    }
    if ($PSCmdlet.ShouldProcess($Ini, 'remove the PushMPC options')) { Write-PmText $Ini ($lines -join $nl) $t.Bom; Remove-Item -LiteralPath $side }
    return $note
}

# ------------------------------------------------------------------------------------------------ JSON settings (MPC-QT, shims)
# A settings file another program owns: the file is copied to <file>.pushmpc-backup once (or <file>.pushmpc-absent), the
# values found before are kept in <file>.pushmpc-backup.json with the hash of what we wrote. Uninstall: when the file
# is still exactly what we wrote, the copy goes back byte for byte; else only our values are put back.
function Get-PmJson([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return [ordered]@{} }
    $t = (Read-PmText $Path).Text
    if (-not $t.Trim()) { return [ordered]@{} }
    $o = $t | ConvertFrom-Json
    $h = [ordered]@{}
    foreach ($p in $o.PSObject.Properties) { $h[$p.Name] = $p.Value }
    return $h
}
function Set-PmJsonValues {
    [CmdletBinding(SupportsShouldProcess = $true)] param([string]$Path, [hashtable]$Values)
    $side = "$Path.pushmpc-backup.json"; $backup = "$Path.pushmpc-backup"; $absent = "$Path.pushmpc-absent"
    $exists = Test-Path -LiteralPath $Path
    $cur = Get-PmJson $Path
    if (-not (Test-Path -LiteralPath $side)) {
        $old = [ordered]@{}
        foreach ($k in $Values.Keys) { $old[$k] = if ($cur.Contains($k)) { @{ had = $true; value = $cur[$k] } } else { @{ had = $false } } }
        if ($PSCmdlet.ShouldProcess($Path, 'back up')) {
            $dir = Split-Path -Parent $Path
            if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null; [IO.File]::WriteAllText($absent, 'created-dir') }
            elseif ($exists) { [IO.File]::Copy($Path, $backup, $true) } else { [IO.File]::WriteAllText($absent, 'file') }
            [IO.File]::WriteAllText($side, (@{ old = $old; written = '' } | ConvertTo-Json -Depth 8))
        }
    }
    foreach ($k in $Values.Keys) { $cur[$k] = $Values[$k] }
    if ($PSCmdlet.ShouldProcess($Path, "set $(@($Values.Keys) -join ', ')")) {
        $bom = $exists -and (Read-PmText $Path).Bom
        Write-PmText $Path (([pscustomobject]$cur | ConvertTo-Json -Depth 20) + "`n") $bom
        $s = Get-Content -LiteralPath $side -Raw | ConvertFrom-Json
        $s.written = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
        [IO.File]::WriteAllText($side, ($s | ConvertTo-Json -Depth 8))
    }
}
function Restore-PmJsonValues {
    [CmdletBinding(SupportsShouldProcess = $true)] param([string]$Path)
    $side = "$Path.pushmpc-backup.json"; $backup = "$Path.pushmpc-backup"; $absent = "$Path.pushmpc-absent"
    if (-not (Test-Path -LiteralPath $side)) { return 'not installed' }
    $s = Get-Content -LiteralPath $side -Raw | ConvertFrom-Json
    $note = ''; $rmDir = ''
    if (-not (Test-Path -LiteralPath $Path)) { $note = 'the file is gone' }
    elseif ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $s.written) {
        if (Test-Path -LiteralPath $backup) {
            if ($PSCmdlet.ShouldProcess($Path, 'restore the backup')) { [IO.File]::Copy($backup, $Path, $true) }
            $note = 'restored exactly from the backup'
        } elseif (Test-Path -LiteralPath $absent) {
            $how = [IO.File]::ReadAllText($absent)
            if ($PSCmdlet.ShouldProcess($Path, 'remove the file PushMPC created')) {
                Remove-Item -LiteralPath $Path
                $dir = Split-Path -Parent $Path
                if ($how -eq 'created-dir' -and -not (Get-ChildItem -LiteralPath $dir -Force | Where-Object { $_.FullName -ne $side -and $_.FullName -ne $absent })) { $rmDir = $dir }
            }
            $note = 'removed (PushMPC had created it)'
        }
    } else {
        $cur = Get-PmJson $Path
        foreach ($p in $s.old.PSObject.Properties) {
            if ($p.Value.had) { $cur[$p.Name] = $p.Value.value } else { $cur.Remove($p.Name) }
        }
        if ($PSCmdlet.ShouldProcess($Path, 'put the old values back')) { Write-PmText $Path (([pscustomobject]$cur | ConvertTo-Json -Depth 20) + "`n") ((Read-PmText $Path).Bom) }
        $note = 'old values put back (the file changed since the install, so the copy stays at ' + $backup + ')'
        if ($PSCmdlet.ShouldProcess($side, 'remove')) { Remove-Item -LiteralPath $side; Remove-Item -LiteralPath $absent -ErrorAction SilentlyContinue }
        return $note
    }
    if ($PSCmdlet.ShouldProcess($side, 'remove')) {
        foreach ($f in $side, $backup, $absent) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f } }
        if ($rmDir -and -not (Get-ChildItem -LiteralPath $rmDir -Force)) { Remove-Item -LiteralPath $rmDir }
    }
    return $note
}

# ------------------------------------------------------------------------------------------------ MPC-QT
function Get-PmMpcQtOwnFile($E) { "$($E.LocalAppData)\PushMPC\mpc-qt-mpv.conf" }
function Install-PmMpcQt {
    [CmdletBinding(SupportsShouldProcess = $true)] param($E, [string]$Settings, [string]$Block)
    $cur = Get-PmJson $Settings
    $side = "$Settings.pushmpc-backup.json"
    $useOwn = $true
    if (Test-Path -LiteralPath $side) {
        $s = Get-Content -LiteralPath $side -Raw | ConvertFrom-Json
        $useOwn = -not ($s.old.tweaksMpvOptionsFileChkBox.had -and $s.old.tweaksMpvOptionsFileChkBox.value -eq $true)
    } elseif ($cur.Contains('tweaksMpvOptionsFileChkBox') -and $cur['tweaksMpvOptionsFileChkBox'] -eq $true) { $useOwn = $false }
    $w = @()
    if ($useOwn) {
        $file = Get-PmMpcQtOwnFile $E
        $w += Install-PmMpvConf $file $Block
        Set-PmJsonValues $Settings @{ tweaksMpvOptionsFileChkBox = $true; tweaksMpvOptionsFilePath = $file.Replace('\', '/') }
    } else {
        # the user's own options file (an empty path means MPC-QT's placeholder: <ConfigLocation>/mpv/mpv.conf, which
        # Qt puts in %LOCALAPPDATA% on Windows)
        $file = [string]$cur['tweaksMpvOptionsFilePath']
        if (-not $file) { $file = "$($E.LocalAppData)\mpv\mpv.conf" }
        $file = $file.Replace('/', '\')
        $w += Install-PmMpvConf $file $Block
        if (-not (Test-Path -LiteralPath $side) -and $PSCmdlet.ShouldProcess($side, 'record the options file')) {
            [IO.File]::WriteAllText($side, (@{ old = @{}; written = ''; userFile = $file } | ConvertTo-Json -Depth 8))
        }
        $w += "your MPC-QT options file $file got the block"
    }
    if ($Block -match 'file=\[[^\]]* ') { $w += 'MPC-QT splits its options file at spaces: the plugin path has a space, so MPC-QT cannot load it' }
    return $w
}
function Uninstall-PmMpcQt {
    [CmdletBinding(SupportsShouldProcess = $true)] param($E, [string]$Settings)
    $side = "$Settings.pushmpc-backup.json"
    if (-not (Test-Path -LiteralPath $side)) { return 'not installed' }
    $s = Get-Content -LiteralPath $side -Raw | ConvertFrom-Json
    if ($s.PSObject.Properties['userFile'] -and $s.userFile) {
        $r = Uninstall-PmMpvConf $s.userFile
        if ($PSCmdlet.ShouldProcess($side, 'remove')) { Remove-Item -LiteralPath $side }
        return "your options file: $r"
    }
    $r1 = Uninstall-PmMpvConf (Get-PmMpcQtOwnFile $E)
    $r2 = Restore-PmJsonValues $Settings
    return "settings.json: $r2; options file: $r1"
}

# ------------------------------------------------------------------------------------------------ Jellyfin / Plex MPV shims
function Install-PmShim {
    [CmdletBinding(SupportsShouldProcess = $true)] param([string]$ConfJson, [string]$HostExe, [string]$Block)
    $w = @(Install-PmMpvConf (Join-Path (Split-Path -Parent $ConfJson) 'mpv.conf') $Block)
    Set-PmJsonValues $ConfJson @{ mpv_ext = $true; mpv_ext_path = $HostExe }
    return $w
}
function Uninstall-PmShim {
    [CmdletBinding(SupportsShouldProcess = $true)] param([string]$ConfJson)
    $r1 = Uninstall-PmMpvConf (Join-Path (Split-Path -Parent $ConfJson) 'mpv.conf')
    $r2 = Restore-PmJsonValues $ConfJson
    return "conf.json: $r2; mpv.conf: $r1"
}

# ------------------------------------------------------------------------------------------------ Kodi (playercorefactory.xml)
$script:PmXmlBegin = '<!-- >>> PushMPC managed: PushMPC uninstall removes this -->'
$script:PmXmlEnd = '<!-- <<< PushMPC managed -->'
function Get-PmXmlEscaped([string]$s) { [Security.SecurityElement]::Escape($s) }
function Get-PmKodiPlayer([string]$HostExe, [string]$Indent, [string]$Nl, [switch]$Unmarked) {
    $lines = @('<player name="PushMPC mpv" type="ExternalPlayer" audio="false" video="true">',
        "  <filename>$(Get-PmXmlEscaped $HostExe)</filename>",
        '  <args>"{1}" --fs --force-window=immediate</args>',
        '  <hidexbmc>false</hidexbmc>', '  <hideconsole>true</hideconsole>', '  <warpcursor>none</warpcursor>',
        '</player>')
    if (-not $Unmarked) { $lines = @($script:PmXmlBegin) + $lines + @($script:PmXmlEnd) }
    return (($lines | ForEach-Object { "$Indent$_" }) -join $Nl) + $Nl
}
function Get-PmKodiRules([string]$Indent, [string]$Nl) {
    $lines = @($script:PmXmlBegin, '<rules action="prepend">', '  <rule video="true" player="PushMPC mpv"/>', '</rules>', $script:PmXmlEnd)
    return (($lines | ForEach-Object { "$Indent$_" }) -join $Nl) + $Nl
}
# every marked span with its indentation and line end
function Remove-PmXmlSpans([string]$Text) {
    while ($true) {
        $b = $Text.IndexOf($script:PmXmlBegin)
        if ($b -lt 0) { return $Text }
        $e = $Text.IndexOf($script:PmXmlEnd, $b)
        if ($e -lt 0) { return $Text }
        $start = $b; while ($start -gt 0 -and ($Text[$start - 1] -eq ' ' -or $Text[$start - 1] -eq "`t")) { $start-- }
        $end = $e + $script:PmXmlEnd.Length
        if ($end -lt $Text.Length -and $Text[$end] -eq "`r") { $end++ }
        if ($end -lt $Text.Length -and $Text[$end] -eq "`n") { $end++ }
        $Text = $Text.Remove($start, $end - $start)
    }
}
function Install-PmKodi {
    [CmdletBinding(SupportsShouldProcess = $true)] param([string]$Xml, [string]$HostExe)
    $backup = "$Xml.pushmpc-backup"; $absent = "$Xml.pushmpc-absent"
    if (-not (Test-Path -LiteralPath $Xml)) {
        $nl = "`r`n"
        $text = '<?xml version="1.0" encoding="UTF-8"?>' + $nl + '<playercorefactory>' + $nl + '  <players>' + $nl + (Get-PmKodiPlayer $HostExe '    ' $nl) +
            '  </players>' + $nl + (Get-PmKodiRules '  ' $nl) + '</playercorefactory>' + $nl
        if ($PSCmdlet.ShouldProcess($Xml, 'create playercorefactory.xml')) {
            $dir = Split-Path -Parent $Xml; $how = 'file'
            if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null; $how = 'created-dir' }
            [IO.File]::WriteAllText($absent, $how)
            Write-PmText $Xml $text $false
        }
        return @()
    }
    $t = Read-PmText $Xml
    $nl = Get-PmNewline $t.Text
    $base = Remove-PmXmlSpans $t.Text
    $root = [regex]::Match($base, '<playercorefactory\b[^>]*>')
    if (-not $root.Success -or $base -match '<playercorefactory\b[^>]*/>') { return @("$Xml has no <playercorefactory> element: not changed (add the PushMPC mpv player by hand, see docs\players-mpv.md)") }
    $new = $base
    $pl = [regex]::Match($new, '<players\b[^>]*>')
    if ($pl.Success -and $new -notmatch '<players\b[^>]*/>') {
        $at = $pl.Index + $pl.Length; if ($new.Substring($at).StartsWith("`r`n")) { $at += 2 } elseif ($new.Substring($at).StartsWith("`n")) { $at += 1 } else { $new = $new.Insert($at, $nl); $at += $nl.Length }
        $new = $new.Insert($at, (Get-PmKodiPlayer $HostExe '    ' $nl))
    } else {
        $close = $new.LastIndexOf('</playercorefactory>')
        if ($close -lt 0) { return @("$Xml is not complete: not changed") }
        $ls = $close; while ($ls -gt 0 -and ($new[$ls - 1] -eq ' ' -or $new[$ls - 1] -eq "`t")) { $ls-- }
        $span = "  $($script:PmXmlBegin)$nl  <players>$nl" + (Get-PmKodiPlayer $HostExe '    ' $nl -Unmarked) + "  </players>$nl  $($script:PmXmlEnd)$nl"
        $new = $new.Insert($ls, $span)
    }
    $close = $new.LastIndexOf('</playercorefactory>')
    $ls = $close; while ($ls -gt 0 -and ($new[$ls - 1] -eq ' ' -or $new[$ls - 1] -eq "`t")) { $ls-- }
    $new = $new.Insert($ls, (Get-PmKodiRules '  ' $nl))
    if (-not (Test-Path -LiteralPath $backup) -and -not (Test-Path -LiteralPath $absent) -and $base -eq $t.Text -and $PSCmdlet.ShouldProcess($backup, 'back up playercorefactory.xml')) { [IO.File]::WriteAllBytes($backup, $t.Bytes) }
    if ($new -ne $t.Text -and $PSCmdlet.ShouldProcess($Xml, 'add the PushMPC mpv player')) { Write-PmText $Xml $new $t.Bom }
    $w = @()
    if ($base -match '<rule\b[^>]*video="true"' -and $base -match '<rules\b[^>]*action="prepend"') { $w += 'your own prepend rules come after the PushMPC rule in this file; Kodi uses the first rule that matches' }
    return $w
}
function Uninstall-PmKodi {
    [CmdletBinding(SupportsShouldProcess = $true)] param([string]$Xml)
    $backup = "$Xml.pushmpc-backup"; $absent = "$Xml.pushmpc-absent"
    if (-not (Test-Path -LiteralPath $Xml)) { if ((Test-Path -LiteralPath $absent) -and $PSCmdlet.ShouldProcess($absent, 'remove')) { Remove-Item -LiteralPath $absent }; return 'no playercorefactory.xml' }
    $t = Read-PmText $Xml
    $stripped = Remove-PmXmlSpans $t.Text
    if (Test-Path -LiteralPath $absent) {
        $how = [IO.File]::ReadAllText($absent)
        if ($stripped -notmatch '<(player|rule)\b') {
            if ($PSCmdlet.ShouldProcess($Xml, 'remove the file PushMPC created')) {
                Remove-Item -LiteralPath $Xml; Remove-Item -LiteralPath $absent
                $dir = Split-Path -Parent $Xml
                if ($how -eq 'created-dir' -and -not (Get-ChildItem -LiteralPath $dir -Force)) { Remove-Item -LiteralPath $dir }
            }
            return 'removed (PushMPC had created it)'
        }
        if ($PSCmdlet.ShouldProcess($Xml, 'remove the PushMPC player')) { Write-PmText $Xml $stripped $t.Bom; Remove-Item -LiteralPath $absent }
        return 'PushMPC player removed; your later additions kept'
    }
    if (Test-Path -LiteralPath $backup) {
        $orig = [IO.File]::ReadAllBytes($backup)
        $enc = [Text.UTF8Encoding]::new($false).GetBytes($stripped); if ($t.Bom) { $enc = [byte[]](0xEF, 0xBB, 0xBF) + $enc }
        if ([Convert]::ToBase64String($enc) -eq [Convert]::ToBase64String($orig)) {
            if ($PSCmdlet.ShouldProcess($Xml, 'restore the backup')) { [IO.File]::WriteAllBytes($Xml, $orig); Remove-Item -LiteralPath $backup }
            return 'restored exactly from the backup'
        }
    }
    if ($stripped -ne $t.Text -and $PSCmdlet.ShouldProcess($Xml, 'remove the PushMPC player')) { Write-PmText $Xml $stripped $t.Bom }
    return 'PushMPC player removed'
}

# ------------------------------------------------------------------------------------------------ user PATH (VSScript.dll)
function Get-PmUserPath($E) {
    if ($E.Fake) { if (Test-Path -LiteralPath $E.UserPathFile) { return [IO.File]::ReadAllText($E.UserPathFile) } else { return '' } }
    $p = [Environment]::GetEnvironmentVariable('Path', 'User'); if ($p) { return $p } else { return '' }
}
function Set-PmUserPath($E, [string]$Value) {
    if ($E.Fake) { [IO.File]::WriteAllText($E.UserPathFile, $Value) } else { [Environment]::SetEnvironmentVariable('Path', $Value, 'User') }
}
function Get-PmStateFile($E) { "$($E.LocalAppData)\PushMPC\mpv-integration.json" }

# ------------------------------------------------------------------------------------------------ actions
function Invoke-PmMpvIntegration {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Action, [string]$Root, [string]$ScriptPath, [string[]]$MpvExe, [int]$BufferedFrames = 4, [int]$ConcurrentFrames = 2, [switch]$NoSmplayer, [switch]$RuntimeOnPath)
    $E = Get-PmEnv $Root
    if (-not $ScriptPath) { $ScriptPath = "$($E.LocalAppData)\PushMPC\vsplugin\pushmpc.vpy" }
    $players = Find-PmPlayers $E $MpvExe
    $state = Get-PmStateFile $E
    $runtime = "$($E.LocalAppData)\PushMPC\vapoursynth"
    # VapourSynth R7x+ portable keeps VSScript.dll in its wheel's package folder (lib\site-packages\vapoursynth)
    if (Test-Path -LiteralPath "$runtime\lib\site-packages\vapoursynth\VSScript.dll") { $runtime = "$runtime\lib\site-packages\vapoursynth" }
    $rows = @()
    switch ($Action) {
        'Status' {
            foreach ($p in $players) {
                $installed = $false; $warn = @()
                if ($p.Kind -eq 'mpvconf' -and (Test-Path -LiteralPath $p.Config)) { $txt = (Read-PmText $p.Config).Text; $installed = [bool](Find-PmBlock $txt); $warn = Get-PmConflicts $txt }
                if ($p.Kind -in 'smplayer', 'mpcqt', 'shim') { $installed = Test-Path -LiteralPath "$($p.Config).pushmpc-backup.json" }
                if ($p.Kind -eq 'kodi' -and (Test-Path -LiteralPath $p.Config)) { $installed = (Read-PmText $p.Config).Text.Contains($script:PmXmlBegin) }
                $rows += [pscustomobject]@{ Player = $p.Player; Exe = $p.Exe; Config = $p.Config; Installed = $installed; Managed = $p.Managed
                    Backup = ($p.Config -and ((Test-Path -LiteralPath "$($p.Config).pushmpc-backup") -or (Test-Path -LiteralPath "$($p.Config).pushmpc-absent") -or (Test-Path -LiteralPath "$($p.Config).pushmpc-backup.json")))
                    Note = (@($p.Note) + $warn) -join '; ' }
            }
            $rows += [pscustomobject]@{ Player = 'VapourSynth runtime'; Exe = ''; Config = $runtime; Installed = (Test-Path -LiteralPath "$runtime\VSScript.dll"); Managed = $true; Backup = $false
                Note = $(if ((Get-PmUserPath $E).Split(';') -contains $runtime) { 'on the user PATH' } else { 'not on the user PATH' }) }
        }
        'Install' {
            $done = @()
            foreach ($p in $players) {
                if (-not $p.Managed -or ($p.Kind -eq 'smplayer' -and $NoSmplayer)) { $rows += [pscustomobject]@{ Player = $p.Player; Config = $p.Config; Result = 'skipped'; Note = $p.Note }; continue }
                $procs = @{ mpcqt = 'mpc-qt'; shim = $(if ($p.Player -like 'Jellyfin*') { 'jellyfin-mpv-shim' } else { 'plex-mpv-shim' }); kodi = 'kodi' }
                if (-not $E.Fake -and $procs.ContainsKey($p.Kind) -and @(Get-Process -Name $procs[$p.Kind] -ErrorAction SilentlyContinue).Count) {
                    $rows += [pscustomobject]@{ Player = $p.Player; Config = $p.Config; Result = 'skipped'; Note = "$($p.Player) is running (it rewrites its settings when it closes): close it and run the install again" }; continue
                }
                $block = Get-PmBlock $ScriptPath $BufferedFrames $ConcurrentFrames "`r`n"
                switch ($p.Kind) {
                    'mpvconf' { $w = Install-PmMpvConf $p.Config $block }
                    'smplayer' { $w = Install-PmSmplayer $p.Config (Get-PmSmplayerFlags $ScriptPath $BufferedFrames $ConcurrentFrames) }
                    'mpcqt' { $w = Install-PmMpcQt $E $p.Config $block }
                    'shim' { $w = Install-PmShim $p.Config $p.Exe $block }
                    'kodi' { $w = Install-PmKodi $p.Config $p.Exe }
                }
                $done += [pscustomobject]@{ Kind = $p.Kind; Config = $p.Config }
                $rows += [pscustomobject]@{ Player = $p.Player; Config = $p.Config; Result = 'installed'; Note = (@($p.Note) + @($w)) -join '; ' }
            }
            $addedPath = $false; $pathBefore = $null; $pathWritten = $null
            if ($RuntimeOnPath) {
                $cur = Get-PmUserPath $E
                # appended; but in front when another VapourSynth (SVP's mpv64 folder, RC2 F5) is on the user PATH, which mpv
                # would otherwise load first (VSScript.dll is found through the PATH). SVP's own mpv loads its copy from its folder.
                $other = @($cur.Split(';') | Where-Object { $_ -and $_ -ne $runtime -and (Test-Path -LiteralPath (Join-Path $_.Trim() 'VSScript.dll')) })
                $new = if ($other.Count) { $runtime + $(if ($cur) { ';' + $cur.TrimStart(';') } else { '' }) } else { $(if ($cur) { $cur.TrimEnd(';') + ';' } else { '' }) + $runtime }
                if (-not ($cur.Split(';') -contains $runtime) -and $PSCmdlet.ShouldProcess('user PATH', "add $runtime")) { Set-PmUserPath $E $new; $addedPath = $true; $pathBefore = $cur; $pathWritten = $new }
            }
            if ($PSCmdlet.ShouldProcess($state, 'record the installed targets')) {
                New-Item -ItemType Directory -Path (Split-Path -Parent $state) -Force | Out-Null
                $old = if (Test-Path -LiteralPath $state) { Get-Content -LiteralPath $state -Raw | ConvertFrom-Json } else { $null }
                $keepPath = $addedPath -or ($old -and $old.addedPath)
                # the user PATH as it was, and as written: an untouched PATH goes back exactly (separators included)
                if (-not $addedPath -and $old -and $old.PSObject.Properties['pathBefore']) { $pathBefore = $old.pathBefore; $pathWritten = $old.pathWritten }
                $all = @($done) + @(if ($old) { @($old.targets) | Where-Object { $c = $_.Config; -not ($done | Where-Object { $_.Config -ieq $c }) } })
                [IO.File]::WriteAllText($state, (@{ targets = $all; addedPath = [bool]$keepPath; runtime = $runtime; pathBefore = $pathBefore; pathWritten = $pathWritten } | ConvertTo-Json -Depth 4))
            }
        }
        'Uninstall' {
            $targets = @()
            if (Test-Path -LiteralPath $state) { $s = Get-Content -LiteralPath $state -Raw | ConvertFrom-Json; $targets = @($s.targets) } else { $s = $null }
            foreach ($p in $players) { if ($p.Managed -and -not ($targets | Where-Object { $_.Config -ieq $p.Config })) { $targets += [pscustomobject]@{ Kind = $p.Kind; Config = $p.Config } } }
            foreach ($t in $targets) {
                $r = switch ($t.Kind) {
                    'mpvconf' { Uninstall-PmMpvConf $t.Config }
                    'smplayer' { Uninstall-PmSmplayer $t.Config }
                    'mpcqt' { Uninstall-PmMpcQt $E $t.Config }
                    'shim' { Uninstall-PmShim $t.Config }
                    'kodi' { Uninstall-PmKodi $t.Config }
                    default { 'unknown target kind' }
                }
                $rows += [pscustomobject]@{ Player = $t.Kind; Config = $t.Config; Result = $r; Note = '' }
            }
            if ($s -and $s.addedPath) {
                $cur = Get-PmUserPath $E
                if ($s.PSObject.Properties['pathWritten'] -and $null -ne $s.pathWritten -and $cur -ceq $s.pathWritten) { $new = [string]$s.pathBefore }
                else { $new = (@($cur.Split(';') | Where-Object { $_ -and $_ -ne $runtime -and $_ -ne $s.runtime })) -join ';' }
                if ($PSCmdlet.ShouldProcess('user PATH', "remove $runtime")) { Set-PmUserPath $E $new }
            }
            if ((Test-Path -LiteralPath $state) -and $PSCmdlet.ShouldProcess($state, 'remove')) { Remove-Item -LiteralPath $state }
        }
    }
    return $rows
}

if ($Action -ne 'None' -and $MyInvocation.InvocationName -ne '.') {
    Invoke-PmMpvIntegration -Action $Action -Root $Root -ScriptPath $ScriptPath -MpvExe $MpvExe -BufferedFrames $BufferedFrames -ConcurrentFrames $ConcurrentFrames `
        -NoSmplayer:$NoSmplayer -RuntimeOnPath:$RuntimeOnPath -WhatIf:$WhatIfPreference | Format-Table -AutoSize -Wrap
}
