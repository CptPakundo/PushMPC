<#
  pushmpc-plex-native.ps1: vf_pushmpc inside Plex HTPC and Plex for Windows WITHOUT replacing their libmpv. Their
  libmpv-2.dll imports FFmpeg 5.x as separate DLLs; this installs a proxy avfilter-8.dll (all exports forwarded to the
  original, renamed avfilter-8-plex.dll) that adds the "pushmpc" filter, plus the per-release filter builds and the
  engine module, and puts `vf-append=@pushmpc:pushmpc` into Plex's mpv.conf. For Setup (scripts/pushmpc-install.ps1)
  or by hand; needs write access to the Plex folders (elevated).

    pushmpc-plex-native.ps1 install|uninstall|status|reapply|selftest [-App auto|htpc|desktop] [-From <folder>]
                            [-Opts 'smoother=4:...'] [-NoConf] [-AllowBoth] [-DryRun] [-Json]

    -From     folder with avfilter-8.dll (the proxy), pushmpc-avfilter-8.*.dll and pushmpc_engine.dll (default: here)
    -App      htpc = Plex HTPC, desktop = Plex for Windows, auto = every one installed (default)
    -Opts     vf_pushmpc options for the mpv.conf line (default none: the engine's Auto)
    -NoConf   leave Plex's mpv.conf alone (the filter is then available but not used)
    -AllowBoth install even when the VapourSynth libmpv swap (scripts/pushmpc-plex.ps1) or its mpv.conf block is there
    reapply   after a Plex update replaced avfilter-8.dll: install again (no-op when the proxy is still in place)
    selftest  loads that Plex's own libmpv-2.dll in a child process and plays a generated picture through vf=pushmpc
  Per app, install: backup of the original avfilter-8.dll (%ProgramData%\PushMPC\plex-native\<app>, with its SHA-256),
  rename to avfilter-8-plex.dll, proxy as avfilter-8.dll, the builds and the engine next to it, the mpv.conf block, a
  self-test. uninstall puts the original back (checked against the backup's SHA-256) and removes exactly what was added.
  -Json: one JSON object per app on stdout ({"target","action","state","detail",...}), the log on stderr.
  Exit: 0 done, 1 error, 2 usage, 3 not supported here (no Plex, no build for its FFmpeg, self-test failed).
#>
param(
    [Parameter(Position = 0)][string]$Action = '',
    [ValidateSet('auto', 'htpc', 'desktop')][string]$App = 'auto',
    [string]$From = '',
    [string]$Opts = '',
    [switch]$NoConf,
    [switch]$AllowBoth,
    [switch]$DryRun,
    [switch]$Json,
    [string]$PlexDir = ''   # selftest (internal) and tests: one Plex folder
)
$ErrorActionPreference = 'Stop'
if ($Action -notin 'install', 'uninstall', 'status', 'reapply', 'selftest') { Get-Help $MyInvocation.MyCommand.Path | Out-String | Write-Host; exit 2 }
if (-not $From) { $From = $PSScriptRoot }
$begin = '# >>> PushMPC native (Plex): do not edit between these lines (PushMPC uninstall removes them)'
$end = '# <<< PushMPC native (Plex)'
$vsMark = '# >>> PushMPC managed'
$apps = @(
    [pscustomobject]@{ Key = 'htpc'; Name = 'Plex HTPC'; Dir = "$env:ProgramFiles\Plex\Plex HTPC"; Conf = "$env:LOCALAPPDATA\Plex HTPC\mpv.conf" },
    [pscustomobject]@{ Key = 'desktop'; Name = 'Plex for Windows'; Dir = "$env:ProgramFiles\Plex\Plex"; Conf = "$env:LOCALAPPDATA\Plex\mpv.conf" }
)

function Emit($a, [string]$state, [string]$detail) {
    if ($Json) { [Console]::Out.WriteLine(([ordered]@{ target = $a.Key; action = $Action; state = $state; detail = $detail; dir = $a.Dir } | ConvertTo-Json -Compress)) }
    else { [Console]::Error.WriteLine("$($a.Name): $state" + $(if ($detail) { " ($detail)" } else { '' })) }
}
function Hash([string]$p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { '' } }
# mpv.conf is edited as bytes: the user's encoding (BOM or not), line endings and last newline are kept, and removing
# our block gives back exactly the bytes that were there (Windows PowerShell 5.1 and 7 alike).
$utf8 = New-Object System.Text.UTF8Encoding($false)
function ReadConf([string]$p) {
    $b = [IO.File]::ReadAllBytes($p)
    $bom = $b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF
    $t = if ($bom) { $utf8.GetString($b, 3, $b.Length - 3) } else { $utf8.GetString($b) }
    return @{ bom = $bom; text = $t; nl = $(if ($t -match "`r`n" -or -not ($t -match "`n")) { "`r`n" } else { "`n" }) }
}
function WriteConf([string]$p, [bool]$bom, [string]$text) {
    $body = $utf8.GetBytes($text)
    if ($bom) { $body = [byte[]](0xEF, 0xBB, 0xBF) + $body }
    [IO.File]::WriteAllBytes($p, [byte[]]$body)
}
$blockRx = '(?m)^' + [regex]::Escape($begin) + '\r?\n(?:[^\r\n]*\r?\n)*?' + [regex]::Escape($end) + '(?:\r?\n|$)'
function StripText([string]$t) { [regex]::Replace($t, $blockRx, '') }
function EnsureConf($a) {   # the block at the top of Plex's mpv.conf (replaced when present: -Opts may differ)
    if ($NoConf) { return }
    New-Item -ItemType Directory -Force (Split-Path $a.Conf) | Out-Null
    $vf = if ($Opts) { "vf-append=@pushmpc:pushmpc=$Opts" } else { 'vf-append=@pushmpc:pushmpc' }
    if (Test-Path -LiteralPath $a.Conf) {
        $c = ReadConf $a.Conf
        if (-not (Test-Path -LiteralPath "$($a.Conf).pushmpc-native-backup") -and -not (Test-Path -LiteralPath "$($a.Conf).pushmpc-native-absent")) {
            Copy-Item -LiteralPath $a.Conf "$($a.Conf).pushmpc-native-backup"
        }
    } else {
        New-Item -ItemType File "$($a.Conf).pushmpc-native-absent" -Force | Out-Null
        $c = @{ bom = $false; text = ''; nl = "`r`n" }
    }
    $nl = $c.nl
    WriteConf $a.Conf $c.bom ((($begin, $vf, 'interpolation=no', $end) -join $nl) + $nl + (StripText $c.text))
}
function RemoveConf($a) {
    $cb = "$($a.Conf).pushmpc-native-backup"; $ca = "$($a.Conf).pushmpc-native-absent"
    if (Test-Path -LiteralPath $a.Conf) {
        $c = ReadConf $a.Conf; $rest = StripText $c.text
        if ($rest -ne $c.text) { WriteConf $a.Conf $c.bom $rest }
        if ((Test-Path -LiteralPath $ca) -and -not $rest.Trim()) { Remove-Item -LiteralPath $a.Conf -Force }
    }
    Remove-Item -LiteralPath $cb, $ca -Force -ErrorAction SilentlyContinue
}
function BackupDir($a) { Join-Path $env:ProgramData "PushMPC\plex-native\$($a.Key)" }

# ---- selftest: Plex's libmpv in THIS process (called in a child process by the other actions)
if ($Action -eq 'selftest') {
    $dir = $PlexDir
    $log = Join-Path ([IO.Path]::GetTempPath()) ('pushmpc-plex-selftest-' + [guid]::NewGuid().ToString('N') + '.log')
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PlexHost {
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool SetDllDirectory(string p);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] public static extern IntPtr LoadLibrary(string p);
    [DllImport("kernel32", CharSet = CharSet.Unicode)] public static extern IntPtr GetModuleHandle(string n);
    [DllImport("kernel32", CharSet = CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr m, string n);
    [DllImport("libmpv-2.dll")] public static extern IntPtr mpv_create();
    [DllImport("libmpv-2.dll")] public static extern int mpv_initialize(IntPtr ctx);
    [DllImport("libmpv-2.dll")] public static extern int mpv_set_option_string(IntPtr ctx, byte[] name, byte[] value);
    [DllImport("libmpv-2.dll")] public static extern int mpv_command_string(IntPtr ctx, byte[] args);
    [DllImport("libmpv-2.dll")] public static extern IntPtr mpv_wait_event(IntPtr ctx, double timeout);
    [DllImport("libmpv-2.dll")] public static extern void mpv_terminate_destroy(IntPtr ctx);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr StatusFn();
    public static byte[] U(string s) { return System.Text.Encoding.UTF8.GetBytes(s + "\0"); }
}
'@
    [void][PlexHost]::SetDllDirectory($dir)
    if ([PlexHost]::LoadLibrary((Join-Path $dir 'libmpv-2.dll')) -eq [IntPtr]::Zero) { Write-Output "selftest: cannot load libmpv-2.dll (error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"; exit 3 }
    $ctx = [PlexHost]::mpv_create()
    foreach ($kv in @('vo=null', 'ao=null', "log-file=$log", 'msg-level=all=v')) {
        $i = $kv.IndexOf('='); [void][PlexHost]::mpv_set_option_string($ctx, [PlexHost]::U($kv.Substring(0, $i)), [PlexHost]::U($kv.Substring($i + 1)))
    }
    $vfrc = [PlexHost]::mpv_set_option_string($ctx, [PlexHost]::U('vf'), [PlexHost]::U('pushmpc=smoother=2'))
    if ([PlexHost]::mpv_initialize($ctx) -lt 0) { Write-Output 'selftest: mpv_initialize failed'; exit 3 }
    $status = ''
    $m = [PlexHost]::GetModuleHandle('avfilter-8.dll')
    if ($m -ne [IntPtr]::Zero) {
        $p = [PlexHost]::GetProcAddress($m, 'pushmpc_proxy_status')
        if ($p -ne [IntPtr]::Zero) { $status = [Runtime.InteropServices.Marshal]::PtrToStringAnsi(([Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($p, [PlexHost+StatusFn])).Invoke()) }
        else { $status = 'avfilter-8.dll is not the proxy' }
    }
    function ReadShared($p) { if (-not (Test-Path $p)) { return '' }; $s = [IO.File]::Open($p, 'Open', 'Read', 'ReadWrite, Delete'); try { (New-Object IO.StreamReader($s)).ReadToEnd() } finally { $s.Dispose() } }   # mpv keeps its log open
    # Plex's FFmpeg has no decoders of its own (--disable-decoders; Plex supplies codecs at run time) and no libavdevice:
    # a Y4M written here (rawvideo), then av://lavfi for other builds
    $y4m = Join-Path ([IO.Path]::GetTempPath()) ('pushmpc-plex-selftest-' + [guid]::NewGuid().ToString('N') + '.y4m')
    $fs = [IO.File]::Create($y4m); $hdr = [Text.Encoding]::ASCII.GetBytes("YUV4MPEG2 W64 H64 F24:1 Ip A1:1 C420jpeg`n"); $fs.Write($hdr, 0, $hdr.Length)
    $fr = [Text.Encoding]::ASCII.GetBytes("FRAME`n"); $px = New-Object byte[] 6144
    for ($n = 0; $n -lt 48; $n++) { for ($k = 0; $k -lt 4096; $k++) { $px[$k] = [byte](($k + $n * 5) % 256) }; for ($k = 4096; $k -lt 6144; $k++) { $px[$k] = 128 }; $fs.Write($fr, 0, $fr.Length); $fs.Write($px, 0, 6144) }
    $fs.Close()
    $cands = @($y4m, 'av://lavfi:testsrc=size=320x180:rate=24:duration=2')
    $reason = 'none'; $f = ''; $tried = @()
    foreach ($c in $cands) {
        [void][PlexHost]::mpv_command_string($ctx, [PlexHost]::U("loadfile `"$($c.Replace('\', '/'))`""))
        $deadline = (Get-Date).AddSeconds(40); $reason = 'timeout'
        while ((Get-Date) -lt $deadline) {
            $ev = [PlexHost]::mpv_wait_event($ctx, 1.0); $id = [Runtime.InteropServices.Marshal]::ReadInt32($ev)
            if ($id -eq 7) { $reason = @{ 0 = 'eof'; 2 = 'stop'; 3 = 'quit'; 4 = 'error' }[[Runtime.InteropServices.Marshal]::ReadInt32([Runtime.InteropServices.Marshal]::ReadIntPtr($ev, 16))]; break }
            if ($id -eq 1) { $reason = 'shutdown'; break }
        }
        Start-Sleep -Milliseconds 200
        $t = ReadShared $log
        $f = [regex]::Match($t, '(?:pushcore|portable-cpu|portable-vulkan) \([^\r\n]*?, x\d').Value
        $tried += "$(if ($c -like 'av://*') { 'lavfi' } else { [IO.Path]::GetFileName($c) -replace '^pushmpc-selftest-', '' -replace '^pushmpc-plex-selftest-[0-9a-f]+', 'generated' }): $reason"
        if ($reason -eq 'eof' -and $f) { break }
    }
    [PlexHost]::mpv_terminate_destroy($ctx)
    $t = ReadShared $log
    $why = if ($reason -eq 'eof' -and $f) { '' } else { '; log: ' + ((@([regex]::Matches($t, '[^\r\n]*(?:Failed|failed|rror|not found|No such)[^\r\n]*') | Select-Object -First 4 | ForEach-Object { $_.Value.Trim() })) -join ' | ') }
    Write-Output "selftest: vf option $(if ($vfrc -ge 0) { 'accepted' } else { "refused ($vfrc)" }); proxy: $status; playback: $($tried -join ', '); filter: $(if ($f) { $f } else { 'not seen' })$why"
    Remove-Item $log, $y4m -ErrorAction SilentlyContinue
    exit $(if ($vfrc -ge 0 -and $f -and $reason -eq 'eof') { 0 } else { 3 })
}

function SelfTest($a) {
    # a child process of the same host (powershell.exe 5.1 or pwsh 7): libmpv stays out of this process
    $ErrorActionPreference = 'Continue'
    $exe = (Get-Process -Id $PID).Path
    $r = & $exe -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath selftest -PlexDir $a.Dir 2>&1 | ForEach-Object { "$_" } | Out-String
    return @{ ok = ($LASTEXITCODE -eq 0); text = $r.Trim() }
}
function ProxyHash($a) { $p = Join-Path (BackupDir $a) 'proxy.sha256'; if (Test-Path $p) { (Get-Content $p).Trim() } else { Hash (Join-Path $From 'avfilter-8.dll') } }
function Installed($a) { (Test-Path (Join-Path $a.Dir 'avfilter-8-plex.dll')) -and ((Hash (Join-Path $a.Dir 'avfilter-8.dll')) -eq (ProxyHash $a)) }

function Install($a) {
    $d = $a.Dir; $bk = BackupDir $a
    $orig = Join-Path $d 'avfilter-8.dll'
    if (-not (Test-Path $orig)) { Emit $a 'unsupported' 'no avfilter-8.dll next to its libmpv (not a separate-FFmpeg build)'; return 3 }
    foreach ($f in 'avfilter-8.dll', 'pushmpc_engine.dll') { if (-not (Test-Path (Join-Path $From $f))) { Emit $a 'error' "$f is not in $From"; return 1 } }
    $builds = @(Get-ChildItem $From -Filter 'pushmpc-avfilter-8.*.dll')
    if (-not $builds) { Emit $a 'error' "no pushmpc-avfilter-8.*.dll in $From"; return 1 }
    if (-not $AllowBoth) {
        if ((Test-Path $a.Conf) -and ((Get-Content $a.Conf) -match [regex]::Escape($vsMark))) { Emit $a 'refused' "Plex's mpv.conf holds the VapourSynth route's block (pass -AllowBoth)"; return 1 }
    }
    if ($DryRun) { Emit $a $(if (Installed $a) { 'installed' } else { 'would-install' }) $d; return 0 }
    if (Installed $a) {   # idempotent: the proxy stays, the conf block is made sure of, the self-test runs again
        EnsureConf $a
        $st = SelfTest $a
        if (-not $st.ok) { Emit $a 'installed-inactive' "already in place; $($st.text)"; return 3 }
        Emit $a 'installed' "already in place; $($st.text)"; return 0
    }
    Get-Process -Name 'Plex', 'Plex HTPC' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    try {
        New-Item -ItemType Directory -Force $bk | Out-Null
        if (Test-Path (Join-Path $d 'avfilter-8-plex.dll')) { Remove-Item (Join-Path $d 'avfilter-8-plex.dll') -Force }   # stale, from before a Plex update
        Copy-Item $orig (Join-Path $bk 'avfilter-8.dll') -Force
        Set-Content (Join-Path $bk 'avfilter-8.dll.sha256') (Hash $orig)
        Move-Item $orig (Join-Path $d 'avfilter-8-plex.dll')
        $added = @('avfilter-8-plex.dll')
        Copy-Item (Join-Path $From 'avfilter-8.dll') $orig
        Set-Content (Join-Path $bk 'proxy.sha256') (Hash $orig)
        foreach ($f in @($builds.Name) + 'pushmpc_engine.dll') {
            if (Test-Path (Join-Path $d $f)) { continue }   # never overwrite Plex's own files
            Copy-Item (Join-Path $From $f) (Join-Path $d $f); $added += $f
        }
        $added | Set-Content (Join-Path $bk 'added.txt')
    } catch { Emit $a 'error' "cannot write $d ($($_.Exception.Message)); run elevated"; return 1 }
    EnsureConf $a
    $st = SelfTest $a
    if (-not $st.ok) { Emit $a 'installed-inactive' $st.text; return 3 }
    Emit $a 'installed' $st.text
    return 0
}

function Uninstall($a) {
    $d = $a.Dir; $bk = BackupDir $a
    if ($DryRun) { Emit $a 'would-uninstall' $d; return 0 }
    Get-Process -Name 'Plex', 'Plex HTPC' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    $detail = ''
    $cur = Join-Path $d 'avfilter-8.dll'; $plex = Join-Path $d 'avfilter-8-plex.dll'
    $want = if (Test-Path (Join-Path $bk 'avfilter-8.dll.sha256')) { (Get-Content (Join-Path $bk 'avfilter-8.dll.sha256')).Trim() } else { '' }
    if ((Test-Path $plex) -and ((Hash $cur) -eq (ProxyHash $a) -or -not (Test-Path $cur))) {
        if (Test-Path $cur) { Remove-Item $cur -Force }
        Move-Item $plex $cur
        $detail = if ($want -and (Hash $cur) -eq $want) { 'original avfilter-8.dll back (SHA-256 matches the backup)' } else { 'original avfilter-8.dll back (no backup hash to compare)' }
    } elseif (Test-Path $plex) {
        Remove-Item $plex -Force; $detail = 'Plex replaced avfilter-8.dll since (an update): left as it is, the stale original removed'
    }
    if (Test-Path (Join-Path $bk 'added.txt')) {
        foreach ($f in Get-Content (Join-Path $bk 'added.txt')) { if ($f -ne 'avfilter-8-plex.dll' -and (Test-Path (Join-Path $d $f))) { Remove-Item (Join-Path $d $f) -Force } }
    }
    RemoveConf $a
    if (Test-Path $bk) { Remove-Item $bk -Recurse -Force }
    Emit $a 'uninstalled' $detail
    return 0
}

$targets = @($apps | Where-Object { ($App -eq 'auto' -or $_.Key -eq $App) -and (Test-Path (Join-Path $_.Dir 'libmpv-2.dll')) })
if ($PlexDir) { $targets = @($apps | Where-Object { $_.Dir -eq $PlexDir }) }
if (-not $targets) { if ($Json) { [ordered]@{ target = $App; action = $Action; state = 'unsupported'; detail = 'no Plex HTPC / Plex for Windows found' } | ConvertTo-Json -Compress } else { [Console]::Error.WriteLine('no Plex found') }; exit 3 }
$rc = 0
foreach ($a in $targets) {
    $r = switch ($Action) {
        'install' { Install $a }
        'uninstall' { Uninstall $a }
        'reapply' { if (Installed $a) { Emit $a 'installed' 'proxy still in place'; 0 } else { Install $a } }
        'status' {
            $st = if (Installed $a) { 'installed' } elseif (Test-Path (Join-Path $a.Dir 'avfilter-8-plex.dll')) { 'needs-reapply' } else { 'not-installed' }
            $t = if ($st -eq 'installed') { (SelfTest $a).text } else { '' }
            Emit $a $st $t; 0
        }
    }
    $r = @($r)[-1]
    if ($r -is [int] -and $r -gt $rc) { $rc = $r }
}
exit $rc
