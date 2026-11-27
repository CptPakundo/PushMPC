# Live check of the late reload take-over (PushMPC test tool; the tester runs it, never an agent without the GPU lock).
# Plays a clip through the BUILD's PushMpcFilter.ax (late_reload_host.exe: loaded directly, no registration, the
# installed MPC Video Renderer read-only) with the debug value ReloadTimeoutMs = 200, so every Auto rebuild passes its
# bound and is taken over late. No install file is touched: only HKCU\Software\PushMPC changes, and it is put back from
# this script's own export.
# Steps, in this order:
#   1. the core is checked: its pushcore.dll's path and time are printed next to the filter's; a core older than the
#      filter is refused (a stale core from an older build)
#   2. the GPU lock file (-Lock) is taken (refused when someone else holds it; -LockHeld when the caller already holds it)
#   3. reg export HKCU\Software\PushMPC -> <Out>\pushmpc-before.reg
#   4. HKCU\Software\PushMPC\Filter: CoreFolder = -CoreFolder, ReloadTimeoutMs = 200
#   5. late_reload_host.exe <ax> <clip> <seconds>
#   6. reg delete HKCU\Software\PushMPC, reg import <Out>\pushmpc-before.reg, reg export again and byte-compare
#   7. the lock is released (only if this script took it)
# Example:
#   powershell -ExecutionPolicy Bypass -File filter\tools\late_reload_live.ps1 -Clip <clip> -CoreFolder <core folder
#       built from the same tree as the filter> -Out <results folder> -Lock <GPU lock file>
param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Ax = '',        # default: ..\bin\PushMpcFilter.ax next to this script
    [string]$HostExe = '',   # default: ..\build\late_reload_host.exe
    [Parameter(Mandatory = $true)][string]$CoreFolder,   # pushcore.dll, manifest.txt and shaders\, laid out as an installed core folder
    [int]$Seconds = 75,
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Lock,
    [switch]$LockHeld
)
$ErrorActionPreference = 'Stop'
if (-not $Ax) { $Ax = Join-Path $PSScriptRoot '..\bin\PushMpcFilter.ax' }
if (-not $HostExe) { $HostExe = Join-Path $PSScriptRoot '..\build\late_reload_host.exe' }
$core =Join-Path $CoreFolder 'pushcore.dll'
foreach ($p in @($Clip, $Ax, $HostExe, $core, (Join-Path $CoreFolder 'manifest.txt'), (Join-Path $CoreFolder 'shaders'))) { if (-not (Test-Path -LiteralPath $p)) { throw "missing: $p" } }
$coreItem = Get-Item -LiteralPath $core; $axItem = Get-Item -LiteralPath $Ax
"core   $($coreItem.FullName)  $($coreItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))  $($coreItem.Length) bytes"
"filter $($axItem.FullName)  $($axItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))  $($axItem.Length) bytes"
if ($coreItem.LastWriteTime -lt $axItem.LastWriteTime) { throw "the core is older than the filter: build the core from the filter's tree and stage it again" }
New-Item -ItemType Directory -Force $Out | Out-Null
$took = $false
if (-not $LockHeld) {
    try { $fs = [System.IO.File]::Open($Lock, 'CreateNew', 'Write'); $w = New-Object System.IO.StreamWriter($fs); $w.WriteLine("late_reload_live.ps1 $(Get-Date -Format s) pid $PID"); $w.Close(); $took = $true }
    catch { throw "$Lock is held by someone else: $(Get-Content $Lock -ErrorAction SilentlyContinue)" }
} elseif (-not (Test-Path $Lock)) { throw "-LockHeld given but $Lock does not exist" }
$before = Join-Path $Out 'pushmpc-before.reg'
$after = Join-Path $Out 'pushmpc-after.reg'
$code = -1
try {
    reg export 'HKCU\Software\PushMPC' $before /y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "reg export failed ($LASTEXITCODE)" }
    "exported HKCU\Software\PushMPC -> $before"
    try {
        New-Item -Path 'HKCU:\Software\PushMPC\Filter' -Force | Out-Null
        Set-ItemProperty -Path 'HKCU:\Software\PushMPC\Filter' -Name CoreFolder -Value $CoreFolder -Type String
        Set-ItemProperty -Path 'HKCU:\Software\PushMPC\Filter' -Name ReloadTimeoutMs -Value 200 -Type DWord
        "set CoreFolder=$CoreFolder ReloadTimeoutMs=200"
        $log = Join-Path $Out ("host-{0}.txt" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
        & $HostExe $Ax $Clip $Seconds 2>&1 | Tee-Object -FilePath $log
        $code = $LASTEXITCODE
        "late_reload_host exit $code (0 = taken over, 2 = played but no take-over, 1 = did not play); output $log"
    } finally {
        # back to the export: the key deleted (the test's values and whatever the filter wrote go), then imported
        reg delete 'HKCU\Software\PushMPC' /f 2>$null | Out-Null
        reg import $before 2>&1 | Out-Null
        "reg import $before -> $LASTEXITCODE"
        reg export 'HKCU\Software\PushMPC' $after /y | Out-Null
        $same = (Get-FileHash $before).Hash -eq (Get-FileHash $after).Hash
        "re-export byte-identical to the export before the run: $same"
        if (-not $same) { $code = 3; "REGISTRY NOT RESTORED: compare $before and $after (exit 3)" }
    }
} finally {
    if ($took) { Remove-Item $Lock -Force; "released $Lock" }
}
exit $code
