<#
  libmpv-host.ps1: plays a generated test picture through another program's libmpv-2.dll (Stremio's, Plex's), with
  PushMPC's options, in this PowerShell process: their DLL is loaded, their program is not run. CI only.
    pwsh libmpv-host.ps1 -Dll <libmpv-2.dll> [-Option key=value ...] [-Seconds 3] [-Log <file>]
  Prints the end-of-file reason and the log lines about PushMPC, d3d11vpp, vapoursynth and Lua. Exit code: 0 = the
  file played to its end, 1 = playback ended with an error, 2 = the DLL could not be used.
#>
param([Parameter(Mandatory = $true)][string]$Dll, [string[]]$Option = @(), [int]$Seconds = 3, [string]$Log = '')
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent (Resolve-Path -LiteralPath $Dll).Path
if (-not $Log) { $Log = Join-Path ([IO.Path]::GetTempPath()) ('libmpv-host-' + [guid]::NewGuid().ToString('N') + '.log') }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LibMpvHost {
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool SetDllDirectory(string p);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] public static extern IntPtr LoadLibrary(string p);
    [DllImport("libmpv-2.dll")] public static extern IntPtr mpv_create();
    [DllImport("libmpv-2.dll")] public static extern int mpv_initialize(IntPtr ctx);
    [DllImport("libmpv-2.dll")] public static extern int mpv_set_option_string(IntPtr ctx, byte[] name, byte[] value);
    [DllImport("libmpv-2.dll")] public static extern int mpv_command_string(IntPtr ctx, byte[] args);
    [DllImport("libmpv-2.dll")] public static extern IntPtr mpv_wait_event(IntPtr ctx, double timeout);
    [DllImport("libmpv-2.dll")] public static extern void mpv_terminate_destroy(IntPtr ctx);
    [DllImport("libmpv-2.dll")] public static extern IntPtr mpv_error_string(int err);
    public static byte[] U(string s) { return System.Text.Encoding.UTF8.GetBytes(s + "\0"); }
}
'@
[void][LibMpvHost]::SetDllDirectory($dir)
if ([LibMpvHost]::LoadLibrary((Join-Path $dir 'libmpv-2.dll')) -eq [IntPtr]::Zero) { Write-Host "cannot load $Dll (error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"; exit 2 }
$ctx = [LibMpvHost]::mpv_create()
if ($ctx -eq [IntPtr]::Zero) { Write-Host 'mpv_create failed'; exit 2 }
$set = @('vo=null', 'ao=null', "log-file=$Log", 'msg-level=all=v') + $Option
foreach ($kv in $set) {
    $i = $kv.IndexOf('='); $k = $kv.Substring(0, $i); $v = $kv.Substring($i + 1)
    $rc = [LibMpvHost]::mpv_set_option_string($ctx, [LibMpvHost]::U($k), [LibMpvHost]::U($v))
    if ($rc -lt 0) { Write-Host "option $k : $([Runtime.InteropServices.Marshal]::PtrToStringAnsi([LibMpvHost]::mpv_error_string($rc)))" }
}
if ([LibMpvHost]::mpv_initialize($ctx) -lt 0) { Write-Host 'mpv_initialize failed'; exit 2 }
[void][LibMpvHost]::mpv_command_string($ctx, [LibMpvHost]::U("loadfile `"av://lavfi:testsrc=size=640x360:rate=24:duration=$Seconds`""))
$deadline = (Get-Date).AddSeconds($Seconds + 30); $reason = 'timeout'
while ((Get-Date) -lt $deadline) {
    $ev = [LibMpvHost]::mpv_wait_event($ctx, 1.0)
    $id = [Runtime.InteropServices.Marshal]::ReadInt32($ev)
    if ($id -eq 7) {   # MPV_EVENT_END_FILE: data -> mpv_event_end_file { int reason; int error; ... }
        $data = [Runtime.InteropServices.Marshal]::ReadIntPtr($ev, 16)
        $r = [Runtime.InteropServices.Marshal]::ReadInt32($data)
        $reason = @{ 0 = 'eof'; 2 = 'stop'; 3 = 'quit'; 4 = 'error'; 5 = 'redirect' }[$r]
        break
    }
    if ($id -eq 1) { $reason = 'shutdown'; break }
}
[LibMpvHost]::mpv_terminate_destroy($ctx)
Write-Host "end of file: $reason"
if (Test-Path -LiteralPath $Log) { Get-Content -LiteralPath $Log | Select-String -Pattern 'pushmpc|d3d11vpp|vapoursynth|lua|Can.t load|script' | Select-Object -First 25 | ForEach-Object { "  $($_.Line)" } }
exit $(if ($reason -eq 'eof') { 0 } else { 1 })
