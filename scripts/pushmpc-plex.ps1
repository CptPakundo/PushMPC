<#
  pushmpc-plex.ps1: frame generation in Plex HTPC and Plex for Windows (Windows).

  Plex plays video with its own libmpv, built without VapourSynth, so PushMPC's filter cannot run in it. This script
  puts a VapourSynth-enabled libmpv into Plex's folder (shinchiro's mpv build, the libmpv community Plex setups use;
  pinned by SHA-256 from its GitHub release), after a self-test of that library, with an exact backup of Plex's own.
  mpv-integration.ps1 then sees the new library and writes PushMPC's usual block into Plex's mpv.conf (no vf= line:
  pushmpc.lua adds the filter and fails softly) plus tls-ca-file=<Plex>\resources\cacert.pem for plex.direct streams.

    pushmpc-plex.ps1 status                  Plex installs, their versions, tested or not, swapped or not
    pushmpc-plex.ps1 apply   [-Yes]          swap where the Plex version is tested (asks for administrator permission:
                                             Plex lives in Program Files); registers the logon check
    pushmpc-plex.ps1 check                   at logon (scheduled task): after a Plex update, swap again when the new version
                                             is tested, else leave Plex's own (capture mode covers it)
    pushmpc-plex.ps1 restore                 put Plex's own library back everywhere, remove the logon check
  -InstallDir: PushMPC's folder. -PlexRoot: a folder that holds "Plex\Plex HTPC" and "Plex\Plex" (tests; default
  %ProgramFiles%). -Archive: the pinned mpv build archive already on disk. -NoTask: no logon task (tests).
  -DryRun, -Machine as in pushmpc-install.ps1. Exit 0 = done (a Plex that is not swapped is reported, not an error).
  Works with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('status', 'apply', 'check', 'restore')]
    [string]$Action = 'status',
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'PushMPC'),
    [string]$PlexRoot = "$env:ProgramFiles",
    [string]$Archive = '',
    [switch]$NoTask,
    [switch]$Elevated,
    [string]$ResultFile = '',
    [switch]$Yes,
    [switch]$DryRun,
    [switch]$Machine
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# shinchiro/mpv-winbuild-cmake: libmpv (mpv-dev, x86-64 baseline) with VapourSynth and Lua
$Libmpv = @{
    Version = '20261111-git-bcde6b93cc'
    Asset   = 'mpv-dev-x86_64-20261111-git-bcde6b93cc.7z'
    Url     = 'https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261111/mpv-dev-x86_64-20261111-git-bcde6b93cc.7z'
    Sha256  = '273C079B11EA5575822F972756F4C9A71149A5C10130353ABEB8586738938452'
    Size    = 31776374
}
# Plex versions this swap was tested with (CI: the libmpv loads and plays through PushMPC's block); other versions keep
# Plex's own library and get capture mode
$Tested = @{ 'Plex HTPC' = @('1.71.*'); 'Plex for Windows' = @('1.10*.*', '1.11*.*') }
$TaskPath = '\PushMPC\'; $TaskName = 'Plex libmpv check'
$PlexDir = Join-Path $InstallDir 'plex'
$StateFile = Join-Path $PlexDir 'plex.json'

function M([string]$t) { [Console]::Out.WriteLine("@@$t"); [Console]::Out.Flush() }
function Info([string]$t) { if ($Machine) { M "info|$t" } else { Write-Host "    $t" }; if ($ResultFile) { Add-Content -LiteralPath $ResultFile -Value "info|$t" } }
function Warn([string]$t) { if ($Machine) { M "warn|$t" } else { Write-Host "    $t" -ForegroundColor Yellow }; if ($ResultFile) { Add-Content -LiteralPath $ResultFile -Value "warn|$t" } }
function Get-Sha256([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToUpperInvariant() }

# Plex's version: its Apps entry (DisplayVersion, the version Plex shows), else the program's file version
function Get-PlexVersion([string]$dir, [string]$exe) {
    foreach ($root in 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall', 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall') {
        foreach ($k in @(Get-ChildItem $root -ErrorAction SilentlyContinue)) {
            $p = Get-ItemProperty $k.PSPath -ErrorAction SilentlyContinue
            if (-not $p -or -not $p.DisplayVersion -or "$($p.DisplayName)" -notlike 'Plex*' -or "$($p.DisplayName)" -like '*Media Server*') { continue }
            $loc = "$($p.InstallLocation)".TrimEnd('\'); $icon = "$($p.DisplayIcon)"
            if (($loc -and $loc -ieq $dir.TrimEnd('\')) -or ($icon -and $icon.Trim('"').StartsWith($dir.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase))) { return "$($p.DisplayVersion)" }
        }
    }
    if (Test-Path -LiteralPath $exe) { $vi = (Get-Item -LiteralPath $exe).VersionInfo; foreach ($v in @($vi.FileVersion, $vi.ProductVersion)) { if ("$v".Trim()) { return "$v".Trim() } } }
    return ''
}
function Get-Plex {
    $out = @()
    foreach ($a in @(@{ Name = 'Plex HTPC'; Dir = Join-Path $PlexRoot 'Plex\Plex HTPC'; Exe = 'Plex HTPC.exe' }, @{ Name = 'Plex for Windows'; Dir = Join-Path $PlexRoot 'Plex\Plex'; Exe = 'Plex.exe' })) {
        $dll = @('libmpv-2.dll', 'mpv-2.dll') | ForEach-Object { Join-Path $a.Dir $_ } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if (-not $dll) { continue }
        $exe = Join-Path $a.Dir $a.Exe
        $ver = Get-PlexVersion $a.Dir $exe
        $tested = [bool](@($Tested[$a.Name] | Where-Object { $ver -like $_ }).Count)
        $out += [pscustomobject]@{ Name = $a.Name; Dir = $a.Dir; Dll = $dll; Version = "$ver"; Tested = $tested; Sha = (Get-Sha256 $dll) }
    }
    return $out
}
function Read-State { if (Test-Path -LiteralPath $StateFile) { try { return @(Get-Content -LiteralPath $StateFile -Raw | ConvertFrom-Json) } catch { } }; return @() }
function Write-State($rows) { if ($DryRun) { return }; New-Item -ItemType Directory -Force $PlexDir | Out-Null; ConvertTo-Json @($rows) -Depth 4 | Set-Content -LiteralPath $StateFile -Encoding UTF8 }
function Test-Admin { ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }

# our libmpv-2.dll from the pinned archive (7z: Windows' tar reads it; 7-Zip when present)
function Get-OurLibmpv {
    $dir = Join-Path $PlexDir 'libmpv'
    $dll = Join-Path $dir 'libmpv-2.dll'
    $stamp = Join-Path $dir 'archive-sha256.txt'
    if ((Test-Path -LiteralPath $dll) -and (Test-Path -LiteralPath $stamp) -and (Get-Content -LiteralPath $stamp -Raw).Trim() -eq $Libmpv.Sha256) { return $dll }
    $arc = $Archive
    if (-not $arc) {
        $arc = Join-Path $PlexDir "download\$($Libmpv.Asset)"
        New-Item -ItemType Directory -Force (Split-Path $arc) | Out-Null
        if (-not ((Test-Path -LiteralPath $arc) -and (Get-Sha256 $arc) -eq $Libmpv.Sha256)) {
            Info "Downloading the VapourSynth-enabled libmpv ($([math]::Round($Libmpv.Size / 1MB)) MB, shinchiro's mpv build $($Libmpv.Version))"
            & (Join-Path $env:SystemRoot 'System32\curl.exe') -L --fail --retry 3 -C - -s -S -o $arc $Libmpv.Url
            if ($LASTEXITCODE -ne 0) { Warn "download failed (curl exit $LASTEXITCODE): $($Libmpv.Url)"; return '' }
        }
    }
    if ((Get-Sha256 $arc) -ne $Libmpv.Sha256) { Warn "$arc does not match its pinned SHA-256: not used"; return '' }
    $x = Join-Path $PlexDir 'extract'
    if (Test-Path -LiteralPath $x) { Remove-Item -LiteralPath $x -Recurse -Force }
    New-Item -ItemType Directory -Force $x | Out-Null
    $ErrorActionPreference = 'Continue'   # Windows PowerShell 5.1 turns a native tool's stderr into a terminating error
    try { & (Join-Path $env:SystemRoot 'System32\tar.exe') -xf $arc -C $x 2>&1 | Out-Null } catch { }
    $ErrorActionPreference = 'Stop'
    $found = Get-ChildItem -LiteralPath $x -Recurse -Filter 'libmpv-2.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) {
        $sz = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe") | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
        if ($sz) { & $sz x -y "-o$x" $arc | Out-Null; $found = Get-ChildItem -LiteralPath $x -Recurse -Filter 'libmpv-2.dll' -ErrorAction SilentlyContinue | Select-Object -First 1 }
    }
    if (-not $found) { Warn 'the mpv build archive could not be unpacked (no libmpv-2.dll): Plex is not changed'; return '' }
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item -LiteralPath $found.FullName $dll -Force
    Set-Content -LiteralPath $stamp -Value $Libmpv.Sha256
    Remove-Item -LiteralPath $x -Recurse -Force -ErrorAction SilentlyContinue
    return $dll
}
# the self-test, in a process of its own: the library loads, mpv_create / mpv_initialize work, and it has VapourSynth
function Test-OurLibmpv([string]$dll) {
    $t = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll))
    if (-not ($t.Contains('vapoursynth') -and $t.Contains('VSScript'))) { return 'no VapourSynth in this libmpv' }
    $code = @"
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class PmMpv {
  [DllImport("kernel32", SetLastError=true, CharSet=CharSet.Unicode)] public static extern IntPtr LoadLibraryW(string f);
  [DllImport("kernel32", CharSet=CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr m, string n);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Create();
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int Init(IntPtr h);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr GetStr(IntPtr h, [MarshalAs(UnmanagedType.LPStr)] string n);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Destroy(IntPtr h);
  public static string Run(string dll) {
    IntPtr m = LoadLibraryW(dll); if (m == IntPtr.Zero) return "LoadLibrary failed: " + Marshal.GetLastWin32Error();
    var c = (Create)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "mpv_create"), typeof(Create));
    var i = (Init)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "mpv_initialize"), typeof(Init));
    var g = (GetStr)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "mpv_get_property_string"), typeof(GetStr));
    var d = (Destroy)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "mpv_terminate_destroy"), typeof(Destroy));
    IntPtr h = c(); if (h == IntPtr.Zero) return "mpv_create failed";
    int r = i(h); if (r < 0) return "mpv_initialize failed: " + r;
    string v = Marshal.PtrToStringAnsi(g(h, "mpv-version")); d(h); return "OK " + v;
  }
}
'@
[PmMpv]::Run('$($dll.Replace("'", "''"))')
"@
    # from a file (a -Command argument loses its double quotes on the way to the child), in a child process
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "pushmpc-libmpv-test-$PID.ps1"
    $outFile = "$tmp.out"
    [IO.File]::WriteAllText($tmp, $code, (New-Object System.Text.UTF8Encoding($true)))
    try {
        $p = Start-Process -FilePath $ps -ArgumentList @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', "`"$tmp`"") -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput $outFile -RedirectStandardError "$outFile.err"
        $out = @(Get-Content -LiteralPath $outFile -ErrorAction SilentlyContinue) | Where-Object { "$_".Trim() } | Select-Object -Last 1
        if (-not $out) { $out = (@(Get-Content -LiteralPath "$outFile.err" -ErrorAction SilentlyContinue) | Select-Object -First 2) -join ' ' }
    } finally { Remove-Item -LiteralPath $tmp, $outFile, "$outFile.err" -Force -ErrorAction SilentlyContinue }
    if ("$out" -like 'OK *') { Info "libmpv self-test: $out"; return '' }
    return "self-test: $out (exit $($p.ExitCode))"
}
function Register-Check {
    if ($NoTask -or $DryRun) { return }
    $me = Join-Path $InstallDir 'filter\pushmpc-plex.ps1'; if (-not (Test-Path -LiteralPath $me)) { $me = $MyInvocation.ScriptName }
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $user = "$env:USERDOMAIN\$env:USERNAME"
    $act = New-ScheduledTaskAction -Execute $ps -Argument "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$me`" check -InstallDir `"$InstallDir`""
    $trg = New-ScheduledTaskTrigger -AtLogOn -User $user
    $pri = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
    Register-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Action $act -Trigger $trg -Principal $pri -Force | Out-Null
    Info "logon check registered ($TaskPath$TaskName): swaps again after a Plex update to a tested version"
}
function Unregister-Check { if (-not $DryRun) { Unregister-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue } }

# the swap itself (elevated): back up Plex's library once per Plex version, copy ours over it
function Invoke-Swap([object[]]$plex, [string]$ours) {
    $state = @(Read-State)
    $oursSha = Get-Sha256 $ours
    $rows = @()
    foreach ($p in $plex) {
        $old = $state | Where-Object { $_.Dir -eq $p.Dir } | Select-Object -First 1
        if ($p.Sha -eq $oursSha) { Info "$($p.Name) $($p.Version): PushMPC's libmpv is in place"; $rows += $old; continue }
        if (-not $p.Tested) { Warn "$($p.Name) $($p.Version): not a tested Plex version: its own libmpv stays (capture mode gives it frame generation)"; if ($old) { $rows += $old }; continue }
        $bak = Join-Path $PlexDir ("backup\" + ($p.Name -replace '[^A-Za-z0-9]', '') + '\' + (Split-Path $p.Dll -Leaf))
        if ($DryRun) { Info "[dry run] would back up $($p.Dll) to $bak and put PushMPC's libmpv in its place"; continue }
        New-Item -ItemType Directory -Force (Split-Path $bak) | Out-Null
        Copy-Item -LiteralPath $p.Dll $bak -Force
        try { Copy-Item -LiteralPath $ours $p.Dll -Force }
        catch { Copy-Item -LiteralPath $bak $p.Dll -Force -ErrorAction SilentlyContinue; Warn "$($p.Name): $($p.Dll) could not be replaced ($($_.Exception.Message)); is Plex running? Its own library stays"; continue }
        $rows += [pscustomobject]@{ Name = $p.Name; Dir = $p.Dir; Dll = $p.Dll; Version = $p.Version; Backup = $bak; PlexSha = $p.Sha; OursSha = $oursSha }
        Info "$($p.Name) $($p.Version): VapourSynth-enabled libmpv in place (Plex's own kept in $bak)"
    }
    Write-State $rows
}
function Invoke-Restore {
    foreach ($r in @(Read-State)) {
        if (-not $r -or -not $r.Dll) { continue }
        if (-not (Test-Path -LiteralPath $r.Dll)) { continue }
        $cur = Get-Sha256 $r.Dll
        if ($cur -ne $r.OursSha) { Info "$($r.Name): Plex replaced the library itself (an update): nothing to put back"; continue }
        if (-not (Test-Path -LiteralPath $r.Backup)) { Warn "$($r.Name): the backup $($r.Backup) is missing: reinstall Plex to get its own library back"; continue }
        if ($DryRun) { Info "[dry run] would put $($r.Backup) back to $($r.Dll)"; continue }
        Copy-Item -LiteralPath $r.Backup $r.Dll -Force
        Info "$($r.Name): Plex's own libmpv put back"
    }
    Unregister-Check
    if (-not $DryRun -and (Test-Path -LiteralPath $PlexDir)) { Remove-Item -LiteralPath $PlexDir -Recurse -Force }
}
# runs this script elevated (one UAC prompt) when this process cannot write to Program Files
function Invoke-Elevated([string]$act) {
    $rf = Join-Path ([IO.Path]::GetTempPath()) "pushmpc-plex-$PID.txt"
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $a = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", $act, '-Elevated', '-InstallDir', "`"$InstallDir`"", '-PlexRoot', "`"$PlexRoot`"", '-ResultFile', "`"$rf`"")
    if ($Archive) { $a += @('-Archive', "`"$Archive`"") }
    if ($NoTask) { $a += '-NoTask' }
    try { $p = Start-Process -FilePath $ps -ArgumentList $a -Verb RunAs -WindowStyle Hidden -Wait -PassThru }
    catch { Warn "administrator permission was not given: Plex is not changed (capture mode gives it frame generation)"; return 1 }
    if (Test-Path -LiteralPath $rf) { foreach ($l in Get-Content -LiteralPath $rf) { $k, $v = $l -split '\|', 2; if ($k -eq 'warn') { Warn $v } else { Info $v } }; Remove-Item -LiteralPath $rf -Force }
    return $p.ExitCode
}

switch ($Action) {
    'status' {
        $plex = @(Get-Plex)
        if (-not $plex.Count) { Info 'Plex HTPC / Plex for Windows: not found'; break }
        $state = @(Read-State)
        foreach ($p in $plex) {
            $s = $state | Where-Object { $_.Dir -eq $p.Dir } | Select-Object -First 1
            Info ("{0} {1} ({2}): {3}" -f $p.Name, $p.Version, $(if ($p.Tested) { 'tested' } else { 'untested version' }), $(if ($s -and $p.Sha -eq $s.OursSha) { 'VapourSynth-enabled libmpv (PushMPC)' } else { "Plex's own libmpv" }))
        }
    }
    'apply' {
        $plex = @(Get-Plex)
        if (-not $plex.Count) { Info 'Plex HTPC / Plex for Windows: not found'; break }
        if (-not @($plex | Where-Object Tested).Count) { foreach ($p in $plex) { Warn "$($p.Name) $($p.Version): not a tested Plex version: its own libmpv stays (capture mode gives it frame generation)" }; break }
        if (@(Get-Process -Name 'Plex', 'Plex HTPC' -ErrorAction SilentlyContinue).Count) { Warn 'Plex is running: close it and run Setup (Repair) again to give Plex frame generation'; break }
        $ours = Get-OurLibmpv
        if (-not $ours) { break }
        $why = Test-OurLibmpv $ours
        if ($why) { Warn "the VapourSynth-enabled libmpv failed its self-test ($why): Plex is not changed"; break }
        Info 'the VapourSynth-enabled libmpv passed its self-test (loads, initialises, has VapourSynth)'
        if (-not $Elevated -and -not (Test-Admin) -and -not $DryRun) { exit (Invoke-Elevated 'apply') }
        Invoke-Swap $plex $ours
        if (@(Read-State).Count) { Register-Check }
    }
    'check' {
        # at logon: a Plex update replaced our library; swap again when the new version is tested
        $state = @(Read-State)
        if (-not $state.Count) { break }
        $plex = @(Get-Plex)
        $ours = Join-Path $PlexDir 'libmpv\libmpv-2.dll'
        if (-not (Test-Path -LiteralPath $ours)) { break }
        $oursSha = Get-Sha256 $ours
        $todo = @($plex | Where-Object { $_.Sha -ne $oursSha -and $_.Tested })
        if (-not $todo.Count) { break }
        if (@(Get-Process -Name 'Plex', 'Plex HTPC' -ErrorAction SilentlyContinue).Count) { break }
        if (Test-OurLibmpv $ours) { break }
        Invoke-Swap $plex $ours
    }
    'restore' {
        if (-not @(Read-State).Count) { Unregister-Check; if (-not $DryRun -and (Test-Path -LiteralPath $PlexDir)) { Remove-Item -LiteralPath $PlexDir -Recurse -Force }; break }
        if (-not $Elevated -and -not (Test-Admin) -and -not $DryRun) { exit (Invoke-Elevated 'restore') }
        Invoke-Restore
    }
}
exit 0
