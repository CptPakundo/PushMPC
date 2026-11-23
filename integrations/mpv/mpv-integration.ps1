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
   - with -RuntimeOnPath: %LOCALAPPDATA%\PushMPC\vapoursynth on the user's PATH (mpv loads VSScript.dll from the DLL
     search path); removed again only if this script added it.
  Detected but not changed: SVP's own mpv (SVP manages it), MPC-QT (libmpv; set up by hand, see docs\players-mpv.md).
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
    if (-not $E.Fake) { foreach ($c in @(Get-Command mpv.exe -All -ErrorAction SilentlyContinue)) { if ($c.Source) { $mpvs += $c.Source } } }
    foreach ($d in @("$($E.LocalAppData)\Programs\mpv", "$($E.ProgramFiles)\mpv", "$($E.UserProfile)\scoop\apps\mpv\current", "$($E.UserProfile)\scoop\apps\mpv-git\current")) {
        if (Test-Path -LiteralPath "$d\mpv.exe") { $mpvs += "$d\mpv.exe" }
    }
    $svpDirs = @("$($E.ProgramFilesX86)\SVP 4\mpv64", "$($E.ProgramFiles)\SVP 4\mpv64")
    $smDirs = @("$($E.ProgramFiles)\SMPlayer", "$($E.ProgramFilesX86)\SMPlayer")
    $seen = @{}
    foreach ($m in $mpvs) {
        $key = $m.ToLowerInvariant(); if ($seen[$key]) { continue }; $seen[$key] = $true
        $dir = Split-Path -Parent $m
        if ($svpDirs | Where-Object { $_ -and $dir -ieq $_ }) { continue }
        if ($smDirs | Where-Object { $_ -and $dir -ieq "$_\mpv" }) { continue }
        $shin = (Test-Path -LiteralPath "$dir\updater.bat") -or (Test-Path -LiteralPath "$dir\installer\mpv-install.bat") -or (Test-Path -LiteralPath "$dir\mpv.com")
        $portable = Test-Path -LiteralPath "$dir\portable_config" -PathType Container
        $found += [pscustomobject]@{ Player = $(if ($shin) { 'mpv (shinchiro build)' } else { 'mpv' }); Exe = $m; Kind = 'mpvconf'
            Config = $(if ($portable) { "$dir\portable_config\mpv.conf" } else { "$($E.AppData)\mpv\mpv.conf" }); Managed = $true
            Note = $(if ($portable) { 'portable_config next to mpv.exe' } else { 'user config folder' }) }
    }
    foreach ($d in $svpDirs) {
        if (Test-Path -LiteralPath "$d\mpv.exe") { $found += [pscustomobject]@{ Player = "SVP's mpv"; Exe = "$d\mpv.exe"; Kind = 'none'; Config = ''; Managed = $false; Note = 'left alone: SVP manages its own mpv and filter' } }
    }
    foreach ($d in $smDirs) {
        if (-not (Test-Path -LiteralPath "$d\smplayer.exe")) { continue }
        $ini = if (Test-Path -LiteralPath "$d\smplayer.ini") { "$d\smplayer.ini" } else { "$($E.AppData)\SMPlayer\smplayer.ini" }
        $found += [pscustomobject]@{ Player = 'SMPlayer'; Exe = "$d\smplayer.exe"; Kind = 'smplayer'; Config = $ini; Managed = $true
            Note = $(if (Test-Path -LiteralPath "$d\mpv\mpv.exe") { 'bundled mpv; options in smplayer.ini (experimental)' } else { 'no bundled mpv found; options in smplayer.ini (experimental)' }) }
    }
    foreach ($d in @("$($E.ProgramFiles)\mpc-qt", "$($E.LocalAppData)\Programs\mpc-qt", "$($E.ProgramFilesX86)\mpc-qt")) {
        if (Test-Path -LiteralPath "$d\mpc-qt.exe") { $found += [pscustomobject]@{ Player = 'MPC-QT'; Exe = "$d\mpc-qt.exe"; Kind = 'none'; Config = ''; Managed = $false; Note = 'manual setup: MPC-QT (libmpv) does not read mpv.conf; see docs\players-mpv.md' } }
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
                if ($p.Kind -eq 'smplayer') { $installed = Test-Path -LiteralPath "$($p.Config).pushmpc-backup.json" }
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
                if ($p.Kind -eq 'mpvconf') { $w = Install-PmMpvConf $p.Config (Get-PmBlock $ScriptPath $BufferedFrames $ConcurrentFrames "`r`n") }
                else { $w = Install-PmSmplayer $p.Config (Get-PmSmplayerFlags $ScriptPath $BufferedFrames $ConcurrentFrames) }
                $done += [pscustomobject]@{ Kind = $p.Kind; Config = $p.Config }
                $rows += [pscustomobject]@{ Player = $p.Player; Config = $p.Config; Result = 'installed'; Note = (@($p.Note) + @($w)) -join '; ' }
            }
            $addedPath = $false
            if ($RuntimeOnPath) {
                $cur = Get-PmUserPath $E
                if (-not ($cur.Split(';') -contains $runtime) -and $PSCmdlet.ShouldProcess('user PATH', "add $runtime")) { Set-PmUserPath $E ($(if ($cur) { $cur.TrimEnd(';') + ';' } else { '' }) + $runtime); $addedPath = $true }
            }
            if ($PSCmdlet.ShouldProcess($state, 'record the installed targets')) {
                New-Item -ItemType Directory -Path (Split-Path -Parent $state) -Force | Out-Null
                $old = if (Test-Path -LiteralPath $state) { Get-Content -LiteralPath $state -Raw | ConvertFrom-Json } else { $null }
                $keepPath = $addedPath -or ($old -and $old.addedPath)
                $all = @($done) + @(if ($old) { @($old.targets) | Where-Object { $c = $_.Config; -not ($done | Where-Object { $_.Config -ieq $c }) } })
                [IO.File]::WriteAllText($state, (@{ targets = $all; addedPath = [bool]$keepPath; runtime = $runtime } | ConvertTo-Json -Depth 4))
            }
        }
        'Uninstall' {
            $targets = @()
            if (Test-Path -LiteralPath $state) { $s = Get-Content -LiteralPath $state -Raw | ConvertFrom-Json; $targets = @($s.targets) } else { $s = $null }
            foreach ($p in $players) { if ($p.Managed -and -not ($targets | Where-Object { $_.Config -ieq $p.Config })) { $targets += [pscustomobject]@{ Kind = $p.Kind; Config = $p.Config } } }
            foreach ($t in $targets) {
                $r = if ($t.Kind -eq 'mpvconf') { Uninstall-PmMpvConf $t.Config } else { Uninstall-PmSmplayer $t.Config }
                $rows += [pscustomobject]@{ Player = $t.Kind; Config = $t.Config; Result = $r; Note = '' }
            }
            if ($s -and $s.addedPath) {
                $cur = Get-PmUserPath $E
                $parts = @($cur.Split(';') | Where-Object { $_ -and $_ -ne $runtime -and $_ -ne $s.runtime })
                if ($PSCmdlet.ShouldProcess('user PATH', "remove $runtime")) { Set-PmUserPath $E ($parts -join ';') }
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
