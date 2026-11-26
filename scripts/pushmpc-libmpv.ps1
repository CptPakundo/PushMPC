<#
  pushmpc-libmpv.ps1: PushMPC's own libmpv for players that bring their own: mpv.net, MPC-QT, Jellyfin Desktop
  (and Jellyfin Media Player) and Stremio (Windows).

  Their libmpv has FFmpeg built in, so PushMPC's filter cannot be added to it at run time; through VapourSynth it gets
  only CPU copies of each picture. This script puts a small forwarder in the player's folder under the name of its
  libmpv (pushmpc-libmpv-forward.dll -> libmpv-2.dll): it loads PushMPC's own libmpv (from PushMPC's mpv, FFmpeg with
  the "pushmpc" filter: decoder pictures stay on the graphics card) and falls back to the player's own library, kept
  next to it as libmpv-2.pushmpc-original.dll, whenever PushMPC's is missing. The forwarder also loads
  pushmpc-libmpv.conf (PushMPC's block: pushmpc.lua, which picks the native filter) into every player it starts.
  A player is only changed when a self-test passes: its libmpv's functions are all forwarded, and the forwarder in its
  folder plays a picture through the filter. Otherwise the player keeps its own library (the VapourSynth route).

    pushmpc-libmpv.ps1 status                 players found, swapped or not
    pushmpc-libmpv.ps1 apply   [-Players ...] swap where the self-test passes (asks for administrator permission once
                                              when a player lives in Program Files); registers the logon check
    pushmpc-libmpv.ps1 check                  at logon: a player update put its own library back: swap again
    pushmpc-libmpv.ps1 restore                every player's own library back, the logon check removed
  -InstallDir: PushMPC's folder (PushMPC's mpv in <InstallDir>\mpv-native). -Root: a fake root for tests (Program
  Files, AppData under it). -Players: names to limit to (mpv.net, MPC-QT, Jellyfin Desktop, Jellyfin Media Player,
  Stremio). -NoTask: no logon task. -DryRun, -Machine as in pushmpc-install.ps1. Exit 0 = done (a player not swapped is
  reported, not an error). Works with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('status', 'apply', 'check', 'restore', 'selftest')]
    [string]$Action = 'status',
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'PushMPC'),
    [string]$Root = '',
    [string[]]$Players = @(),
    [string]$Dir = '',          # selftest (internal): one player folder
    [string]$DllName = '',      # selftest (internal)
    [switch]$NoTask,
    [switch]$Elevated,
    [string]$ResultFile = '',
    [switch]$DryRun,
    [switch]$Machine
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Players = @($Players | ForEach-Object { "$_" -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })   # (-File passes one string)

$TaskPath = '\PushMPC\'; $TaskName = 'libmpv check'
$SwapDir = Join-Path $InstallDir 'libmpv-swap'
$StateFile = Join-Path $SwapDir 'libmpv.json'
$MpvDir = Join-Path $InstallDir 'mpv-native'
$OurLib = Join-Path $MpvDir 'libmpv-2.dll'
$Forwarder = Join-Path $MpvDir 'pushmpc-libmpv-forward.dll'

function M([string]$t) { [Console]::Out.WriteLine("@@$t"); [Console]::Out.Flush() }
function Info([string]$t) { if ($Machine) { M "info|$t" } else { Write-Host "    $t" }; if ($ResultFile) { Add-Content -LiteralPath $ResultFile -Value "info|$t" } }
function Warn([string]$t) { if ($Machine) { M "warn|$t" } else { Write-Host "    $t" -ForegroundColor Yellow }; if ($ResultFile) { Add-Content -LiteralPath $ResultFile -Value "warn|$t" } }
function Get-Sha256([string]$p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToUpperInvariant() } else { '' } }
function Test-Admin { ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }

# ---------------------------------------------------------------------------------------------------- the self-test
# In a child process (selftest action): the player's folder as it is now (forwarder in place), its libmpv loaded the
# way the player loads it; a generated picture played through vf=pushmpc=smoother=2 on vo=null. Exit 0 = PushMPC's
# libmpv was used, the filter ran at x2, and playback reached the end.
if ($Action -eq 'selftest') {
    $log = Join-Path ([IO.Path]::GetTempPath()) ('pushmpc-libmpv-selftest-' + [guid]::NewGuid().ToString('N') + '.log')
    $dll = Join-Path $Dir $DllName
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class PmFwdHost {
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool SetDllDirectory(string p);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] public static extern IntPtr LoadLibrary(string p);
    [DllImport("kernel32", CharSet = CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr m, string n);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Create();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Status();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int Init(IntPtr h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int SetOpt(IntPtr h, byte[] n, byte[] v);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int Cmd(IntPtr h, byte[] a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Wait(IntPtr h, double t);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Destroy(IntPtr h);
    public static byte[] U(string s) { return System.Text.Encoding.UTF8.GetBytes(s + "\0"); }
    public static Delegate F(IntPtr m, string n, Type t) { IntPtr p = GetProcAddress(m, n); return p == IntPtr.Zero ? null : Marshal.GetDelegateForFunctionPointer(p, t); }
}
"@
    [void][PmFwdHost]::SetDllDirectory($Dir)
    $env:PUSHMPC_LIBMPV_CONF = 'off'     # the filter alone: the test does not depend on the player's block
    $m = [PmFwdHost]::LoadLibrary($dll)
    if ($m -eq [IntPtr]::Zero) { Write-Output "selftest: cannot load $DllName (error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"; exit 3 }
    $st = [PmFwdHost]::F($m, 'pushmpc_libmpv_status', [PmFwdHost+Status])
    $status = if ($st) { [Runtime.InteropServices.Marshal]::PtrToStringAnsi($st.Invoke()) } else { 'not the forwarder' }
    if ($status -notlike "PushMPC's libmpv*") { Write-Output "selftest: $status"; exit 3 }
    $create = [PmFwdHost]::F($m, 'mpv_create', [PmFwdHost+Create]); $init = [PmFwdHost]::F($m, 'mpv_initialize', [PmFwdHost+Init])
    $set = [PmFwdHost]::F($m, 'mpv_set_option_string', [PmFwdHost+SetOpt]); $cmd = [PmFwdHost]::F($m, 'mpv_command_string', [PmFwdHost+Cmd])
    $wait = [PmFwdHost]::F($m, 'mpv_wait_event', [PmFwdHost+Wait]); $destroy = [PmFwdHost]::F($m, 'mpv_terminate_destroy', [PmFwdHost+Destroy])
    $h = $create.Invoke()
    if ($h -eq [IntPtr]::Zero) { Write-Output 'selftest: mpv_create failed'; exit 3 }
    foreach ($kv in @('vo=null', 'ao=null', "log-file=$log", 'msg-level=all=v', 'hwdec=no')) { $i = $kv.IndexOf('='); [void]$set.Invoke($h, [PmFwdHost]::U($kv.Substring(0, $i)), [PmFwdHost]::U($kv.Substring($i + 1))) }
    $vfrc = $set.Invoke($h, [PmFwdHost]::U('vf'), [PmFwdHost]::U('pushmpc=smoother=2'))
    if ($init.Invoke($h) -lt 0) { Write-Output 'selftest: mpv_initialize failed'; exit 3 }
    [void]$cmd.Invoke($h, [PmFwdHost]::U('loadfile "av://lavfi:testsrc2=size=320x180:rate=24:duration=2"'))
    $deadline = (Get-Date).AddSeconds(40); $reason = 'timeout'
    while ((Get-Date) -lt $deadline) {
        $ev = $wait.Invoke($h, 1.0); $id = [Runtime.InteropServices.Marshal]::ReadInt32($ev)
        if ($id -eq 7) { $reason = @{ 0 = 'eof'; 2 = 'stop'; 3 = 'quit'; 4 = 'error' }[[Runtime.InteropServices.Marshal]::ReadInt32([Runtime.InteropServices.Marshal]::ReadIntPtr($ev, 16))]; break }
        if ($id -eq 1) { $reason = 'shutdown'; break }
    }
    $destroy.Invoke($h)
    $t = if (Test-Path -LiteralPath $log) { [IO.File]::ReadAllText($log) } else { '' }
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    $f = [regex]::Match($t, 'filter: [^\r\n]*?, x2[;)]').Value
    $ok = $vfrc -ge 0 -and $f -and $reason -eq 'eof'
    Write-Output ("selftest: {0}; vf {1}; playback {2}; {3}" -f $status, $(if ($vfrc -ge 0) { 'accepted' } else { "refused ($vfrc)" }), $reason, $(if ($f) { $f.Substring(0, [math]::Min(160, $f.Length)) } else { 'filter not seen' }))
    exit $(if ($ok) { 0 } else { 3 })
}
function Invoke-SelfTest([string]$dir, [string]$name) {
    $ps = (Get-Process -Id $PID).Path
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $r = & $ps -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath selftest -Dir $dir -DllName $name 2>&1 | ForEach-Object { "$_" } | Out-String
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $eap
    return @{ ok = ($rc -eq 0); text = $r.Trim() }
}

# ---------------------------------------------------------------------------------------------------- PE exports
# the export names of a DLL (no loading): the forwarder must cover every function the player's own libmpv offers
function Get-PeExports([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    $nsec = [BitConverter]::ToUInt16($b, $pe + 6); $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt = $pe + 24; $magic = [BitConverter]::ToUInt16($b, $opt)
    $dd = $opt + $(if ($magic -eq 0x20B) { 112 } else { 96 })
    $expRva = [BitConverter]::ToUInt32($b, $dd)
    if (-not $expRva) { return @() }
    $sec = $opt + $optSize
    $toOff = {
        param($rva)
        for ($i = 0; $i -lt $nsec; $i++) {
            $s = $sec + 40 * $i; $va = [BitConverter]::ToUInt32($b, $s + 12); $sz = [math]::Max([BitConverter]::ToUInt32($b, $s + 8), [BitConverter]::ToUInt32($b, $s + 16))
            if ($rva -ge $va -and $rva -lt $va + $sz) { return [int]($rva - $va + [BitConverter]::ToUInt32($b, $s + 20)) }
        }
        return -1
    }
    $e = & $toOff $expRva
    $n = [BitConverter]::ToUInt32($b, $e + 24); $names = & $toOff ([BitConverter]::ToUInt32($b, $e + 32))
    $out = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $n; $i++) {
        $o = & $toOff ([BitConverter]::ToUInt32($b, $names + 4 * $i)); $j = $o
        while ($b[$j] -ne 0) { $j++ }
        $out.Add([Text.Encoding]::ASCII.GetString($b, $o, $j - $o))
    }
    return $out.ToArray()
}

# ---------------------------------------------------------------------------------------------------- the players
function Get-Roots {
    if ($Root) { $r = $Root.TrimEnd('\'); return @{ PF = "$r\Program Files"; PF86 = "$r\Program Files (x86)"; LAD = "$r\AppData\Local"; UP = "$r\User" } }
    $pf86 = ${env:ProgramFiles(x86)}
    return @{ PF = $env:ProgramFiles; PF86 = $(if ($pf86) { $pf86 } else { $env:ProgramFiles }); LAD = $env:LOCALAPPDATA; UP = $env:USERPROFILE }
}
$Known = @(
    @{ Name = 'mpv.net'; Exe = 'mpvnet.exe'; Proc = 'mpvnet'; Dirs = { param($r) "$($r.LAD)\Programs\mpv.net", "$($r.PF)\mpv.net", "$($r.UP)\scoop\apps\mpv.net\current", "$($r.UP)\scoop\apps\mpvnet\current" } },
    @{ Name = 'MPC-QT'; Exe = 'mpc-qt.exe'; Proc = 'mpc-qt'; Dirs = { param($r) "$($r.PF)\mpc-qt", "$($r.LAD)\Programs\mpc-qt", "$($r.PF86)\mpc-qt", "$($r.UP)\scoop\apps\mpc-qt\current" } },
    @{ Name = 'Jellyfin Desktop'; Exe = 'Jellyfin Desktop.exe'; Proc = 'Jellyfin Desktop'; Dirs = { param($r) "$($r.PF)\Jellyfin Desktop", "$($r.LAD)\Programs\Jellyfin Desktop", "$($r.UP)\scoop\apps\jellyfin-desktop\current" } },
    @{ Name = 'Jellyfin Media Player'; Exe = 'JellyfinMediaPlayer.exe'; Proc = 'JellyfinMediaPlayer'; Dirs = { param($r) "$($r.PF)\Jellyfin Media Player", "$($r.LAD)\Programs\Jellyfin Media Player", "$($r.UP)\scoop\apps\jellyfin-media-player\current" } },
    @{ Name = 'Stremio'; Exe = 'stremio-shell-ng.exe'; Proc = 'stremio-shell-ng'; Dirs = { param($r) "$($r.LAD)\Programs\Stremio", "$($r.LAD)\Programs\LNV\Stremio-5", "$($r.PF)\Stremio" } }
)
function Get-Players {
    $r = Get-Roots
    $out = @(); $seen = @{}
    foreach ($k in $Known) {
        if ($Players.Count -and -not ($Players | Where-Object { $_ -ieq $k.Name })) { continue }
        foreach ($d in (& $k.Dirs $r)) {
            if (-not $d -or -not (Test-Path -LiteralPath (Join-Path $d $k.Exe))) { continue }
            $full = (Resolve-Path -LiteralPath $d).Path
            if ($seen[$full.ToLowerInvariant()]) { continue }; $seen[$full.ToLowerInvariant()] = $true
            $dll = @('libmpv-2.dll', 'mpv-2.dll') | Where-Object { Test-Path -LiteralPath (Join-Path $full $_) } | Select-Object -First 1
            if (-not $dll) { continue }
            $out += [pscustomobject]@{ Name = $k.Name; Proc = $k.Proc; Dir = $full; DllName = $dll; Dll = (Join-Path $full $dll); Sha = (Get-Sha256 (Join-Path $full $dll)) }
        }
    }
    return $out
}
function Get-OriginalPath($p) { Join-Path $p.Dir (([IO.Path]::GetFileNameWithoutExtension($p.DllName)) + '.pushmpc-original.dll') }
function Read-State { if (Test-Path -LiteralPath $StateFile) { try { return @((Get-Content -LiteralPath $StateFile -Raw | ConvertFrom-Json) | ForEach-Object { $_ } | Where-Object { $_ -and $_.Dir }) } catch { } }; return @() }
function Write-State($rows) { if ($DryRun) { return }; New-Item -ItemType Directory -Force $SwapDir | Out-Null; ConvertTo-Json @($rows) -Depth 4 | Set-Content -LiteralPath $StateFile -Encoding UTF8 }

# PushMPC's block for the forwarder to load: pushmpc.lua (from the vsplugin component, else PushMPC's mpv folder)
function Get-BlockText {
    $vsp = Join-Path $InstallDir 'vsplugin'
    $lua = @((Join-Path $vsp 'pushmpc.lua'), (Join-Path $MpvDir 'pushmpc.lua')) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $lua) { return '' }
    $q = { param($v) $v = $v.Replace('\', '/'); if ($v -match '^[A-Za-z0-9_./:\-]+$') { $v } else { '%' + [Text.Encoding]::UTF8.GetByteCount($v) + '%' + $v } }
    $l = @('# PushMPC: loaded into this player by PushMPC''s libmpv (pushmpc-libmpv.ps1 writes it; restore removes it)', "scripts-append=$(& $q $lua)")
    $vpy = Join-Path $vsp 'pushmpc.vpy'
    if (Test-Path -LiteralPath $vpy) { $l += "script-opts-append=pushmpc-vpy=$(& $q $vpy)" }
    $l += @('hr-seek-framedrop=no', 'interpolation=no')
    return ($l -join "`r`n") + "`r`n"
}

# ---------------------------------------------------------------------------------------------------- swap / restore
function Invoke-Swap([object[]]$todo) {
    $state = @(Read-State)
    $fwdSha = Get-Sha256 $Forwarder
    $fwdExports = @(Get-PeExports $Forwarder)
    $block = Get-BlockText
    $rows = @($state | Where-Object { $d = $_.Dir; -not ($todo | Where-Object { $_.Dir -eq $d }) })
    foreach ($p in $todo) {
        $old = $state | Where-Object { $_.Dir -eq $p.Dir } | Select-Object -First 1
        $orig = Get-OriginalPath $p
        $bakDir = Join-Path $SwapDir ("backup\" + ($p.Name -replace '[^A-Za-z0-9]', '') + '-' + ((Get-FileHash -InputStream ([IO.MemoryStream]::new([Text.Encoding]::UTF8.GetBytes($p.Dir.ToLowerInvariant()))) -Algorithm SHA256).Hash.Substring(0, 8)))
        if ($p.Sha -eq $fwdSha) {
            # in place already: refresh the path file and the block (PushMPC moved or updated)
            if (-not $DryRun) { Set-Content -LiteralPath (Join-Path $p.Dir 'pushmpc-libmpv.txt') -Value $OurLib -Encoding UTF8; if ($block) { [IO.File]::WriteAllText((Join-Path $p.Dir 'pushmpc-libmpv.conf'), $block) } }
            Info "$($p.Name): PushMPC's libmpv is in place"; if ($old) { $rows += $old }; continue
        }
        # the player's own (a first swap, or an update put it back)
        $theirs = @(Get-PeExports $p.Dll)
        $miss = @($theirs | Where-Object { $_ -like 'mpv_*' -and $fwdExports -notcontains $_ })
        if ($miss.Count) { Warn "$($p.Name): its libmpv has functions PushMPC's does not ($($miss -join ', ')): its own library stays (VapourSynth route)"; if ($old) { $rows += $old }; continue }
        if ($DryRun) { Info "[dry run] would keep $($p.Dll) as $orig and put PushMPC's libmpv forwarder in its place"; continue }
        try {
            New-Item -ItemType Directory -Force $bakDir | Out-Null
            Copy-Item -LiteralPath $p.Dll (Join-Path $bakDir $p.DllName) -Force
            Copy-Item -LiteralPath $p.Dll $orig -Force
            Copy-Item -LiteralPath $Forwarder $p.Dll -Force
            Set-Content -LiteralPath (Join-Path $p.Dir 'pushmpc-libmpv.txt') -Value $OurLib -Encoding UTF8
        } catch {
            Copy-Item -LiteralPath (Join-Path $bakDir $p.DllName) $p.Dll -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $orig, (Join-Path $p.Dir 'pushmpc-libmpv.txt') -Force -ErrorAction SilentlyContinue
            Warn "$($p.Name): $($p.Dir) could not be changed ($($_.Exception.Message)): its own library stays"; continue
        }
        $t = Invoke-SelfTest $p.Dir $p.DllName
        if (-not $t.ok) {
            Copy-Item -LiteralPath $orig $p.Dll -Force
            Remove-Item -LiteralPath $orig, (Join-Path $p.Dir 'pushmpc-libmpv.txt') -Force -ErrorAction SilentlyContinue
            Warn "$($p.Name): the self-test did not pass ($($t.text)): its own library is back (VapourSynth route)"; continue
        }
        if ($block) { [IO.File]::WriteAllText((Join-Path $p.Dir 'pushmpc-libmpv.conf'), $block) }
        $rows += [pscustomobject]@{ Name = $p.Name; Dir = $p.Dir; DllName = $p.DllName; OrigSha = $p.Sha; FwdSha = $fwdSha; Backup = (Join-Path $bakDir $p.DllName) }
        Info "$($p.Name): PushMPC's libmpv in place, frame generation with the picture kept on the graphics card ($($t.text -replace '^selftest: ', ''))"
    }
    Write-State $rows
}
function Invoke-Restore {
    $left = @()
    foreach ($r in @(Read-State)) {
        $p = [pscustomobject]@{ Dir = $r.Dir; DllName = $r.DllName }
        $dll = Join-Path $r.Dir $r.DllName; $orig = Get-OriginalPath $p
        $extra = @($orig, (Join-Path $r.Dir 'pushmpc-libmpv.txt'), (Join-Path $r.Dir 'pushmpc-libmpv.conf'))
        if ($DryRun) { Info "[dry run] would put $($r.Name)'s own libmpv back"; continue }
        try {
            if ((Test-Path -LiteralPath $dll) -and (Get-Sha256 $dll) -eq $r.FwdSha) {
                $src = if ((Get-Sha256 $orig) -eq $r.OrigSha) { $orig } elseif ((Get-Sha256 $r.Backup) -eq $r.OrigSha) { $r.Backup } else { '' }
                if (-not $src) { Warn "$($r.Name): its own libmpv is missing (no backup matches): reinstall $($r.Name)"; $left += $r; continue }
                Copy-Item -LiteralPath $src $dll -Force
                Info "$($r.Name): its own libmpv put back"
            } else { Info "$($r.Name): the player replaced its library itself (an update): nothing to put back" }
            Remove-Item -LiteralPath $extra -Force -ErrorAction SilentlyContinue
        } catch { Warn "$($r.Name): $($_.Exception.Message) (is it running?)"; $left += $r }
    }
    if ($left.Count) { Write-State $left; return }
    Unregister-Check
    if (-not $DryRun -and (Test-Path -LiteralPath $SwapDir)) { Remove-Item -LiteralPath $SwapDir -Recurse -Force }
}
function Register-Check {
    if ($NoTask -or $DryRun -or $Root) { return }
    $me = Join-Path $InstallDir 'filter\pushmpc-libmpv.ps1'; if (-not (Test-Path -LiteralPath $me)) { $me = $PSCommandPath }
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $user = "$env:USERDOMAIN\$env:USERNAME"
    $act = New-ScheduledTaskAction -Execute $ps -Argument "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$me`" check -InstallDir `"$InstallDir`""
    $trg = New-ScheduledTaskTrigger -AtLogOn -User $user
    $pri = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
    Register-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Action $act -Trigger $trg -Principal $pri -Force | Out-Null
    Info "logon check registered ($TaskPath$TaskName): swaps again after a player update"
}
function Unregister-Check { if (-not $DryRun -and -not $Root) { Unregister-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue } }
function Test-Writable([string]$dir) {
    $f = Join-Path $dir ('.pushmpc-write-' + [guid]::NewGuid().ToString('N'))
    try { [IO.File]::WriteAllText($f, ''); Remove-Item -LiteralPath $f -Force; return $true } catch { return $false }
}
function Invoke-Elevated([string]$act) {
    $rf = Join-Path ([IO.Path]::GetTempPath()) "pushmpc-libmpv-$PID.txt"
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $a = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", $act, '-Elevated', '-InstallDir', "`"$InstallDir`"", '-ResultFile', "`"$rf`"")
    if ($NoTask) { $a += '-NoTask' }
    if ($Players.Count) { $a += '-Players'; $a += "`"$($Players -join ',')`"" }
    try { $p = Start-Process -FilePath $ps -ArgumentList $a -Verb RunAs -WindowStyle Hidden -Wait -PassThru }
    catch { Warn "administrator permission was not given: the players in Program Files keep their own libmpv (VapourSynth route)"; return 1 }
    if (Test-Path -LiteralPath $rf) { foreach ($l in Get-Content -LiteralPath $rf) { $k, $v = $l -split '\|', 2; if ($k -eq 'warn') { Warn $v } else { Info $v } }; Remove-Item -LiteralPath $rf -Force }
    return $p.ExitCode
}
function Get-Todo([object[]]$found) {
    $fwdSha = Get-Sha256 $Forwarder
    return @($found | Where-Object { $_.Sha -ne $fwdSha -or -not (Test-Path -LiteralPath (Join-Path $_.Dir 'pushmpc-libmpv.txt')) })
}

switch ($Action) {
    'status' {
        $found = @(Get-Players)
        if (-not $found.Count) { Info 'mpv.net / MPC-QT / Jellyfin Desktop / Stremio: none found'; break }
        $fwdSha = Get-Sha256 $Forwarder
        foreach ($p in $found) { Info ("{0} ({1}): {2}" -f $p.Name, $p.Dir, $(if ($p.Sha -and $p.Sha -eq $fwdSha) { "PushMPC's libmpv" } else { 'its own libmpv' })) }
    }
    'apply' {
        if (-not (Test-Path -LiteralPath $OurLib) -or -not (Test-Path -LiteralPath $Forwarder)) { Info "PushMPC's mpv is not installed (no $OurLib or forwarder): the players keep their own libmpv"; break }
        $found = @(Get-Players)
        if (-not $found.Count) { Info 'mpv.net / MPC-QT / Jellyfin Desktop / Stremio: none found'; break }
        $running = @($found | Where-Object { Get-Process -Name $_.Proc -ErrorAction SilentlyContinue })
        foreach ($p in $running) { Warn "$($p.Name) is running: close it and run Setup (Repair) again to give it PushMPC's libmpv" }
        $todo = @(Get-Todo @($found | Where-Object { $running -notcontains $_ }))
        $needAdmin = @($todo | Where-Object { -not (Test-Writable $_.Dir) })
        if ($needAdmin.Count -and -not $Elevated -and -not (Test-Admin) -and -not $DryRun) {
            $mine = @($todo | Where-Object { $needAdmin -notcontains $_ })
            if ($mine.Count) { Invoke-Swap $mine }
            # the rest in one elevated run (it reads and writes the same state file)
            $script:Players = @($needAdmin | ForEach-Object Name)
            [void](Invoke-Elevated 'apply')
        } else { Invoke-Swap $todo }
        if (@(Read-State).Count) { Register-Check }
    }
    'check' {
        if (-not @(Read-State).Count) { break }
        if (-not (Test-Path -LiteralPath $OurLib) -or -not (Test-Path -LiteralPath $Forwarder)) { break }
        $found = @(Get-Players | Where-Object { -not (Get-Process -Name $_.Proc -ErrorAction SilentlyContinue) })
        $todo = @(Get-Todo $found)
        if ($todo.Count) { Invoke-Swap $todo }
    }
    'restore' {
        $state = @(Read-State)
        if (-not $state.Count) { Unregister-Check; if (-not $DryRun -and (Test-Path -LiteralPath $SwapDir)) { Remove-Item -LiteralPath $SwapDir -Recurse -Force }; break }
        $needAdmin = @($state | Where-Object { (Test-Path -LiteralPath $_.Dir) -and -not (Test-Writable $_.Dir) })
        if ($needAdmin.Count -and -not $Elevated -and -not (Test-Admin) -and -not $DryRun) { exit (Invoke-Elevated 'restore') }
        Invoke-Restore
    }
}
exit 0
