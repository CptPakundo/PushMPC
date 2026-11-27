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
# reg.exe writes "The operation completed successfully." to stderr; under Windows PowerShell 5.1 with
# ErrorActionPreference Stop that is a NativeCommandError, so every native call runs through Invoke-Native (Continue,
# then $LASTEXITCODE checked).
# -SelfTestKey <HKCU\...scratch key>: tests only the export / delete / import / byte-compare path on that scratch key
# (no clip, no GPU, no lock; the key is created, changed, restored and deleted).
# Example:
#   powershell -ExecutionPolicy Bypass -File filter\tools\late_reload_live.ps1 -Clip <clip> -CoreFolder <core folder
#       built from the same tree as the filter> -Out <results folder> -Lock <GPU lock file>
param(
    [string]$Clip = '',
    [string]$Ax = '',        # default: ..\bin\PushMpcFilter.ax next to this script
    [string]$HostExe = '',   # default: ..\build\late_reload_host.exe
    [string]$CoreFolder = '',   # pushcore.dll, manifest.txt and shaders\, laid out as an installed core folder
    [int]$Seconds = 75,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Lock = '',
    [switch]$LockHeld,
    [string]$SelfTestKey = ''
)
$ErrorActionPreference = 'Stop'
# a native command with ErrorActionPreference Continue (its stderr is output, not an error); returns its exit code
function Invoke-Native([string]$exe, [string[]]$argv, [string]$what) {
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $o = & $exe @argv 2>&1 | ForEach-Object { "$_" }; $rc = $LASTEXITCODE } finally { $ErrorActionPreference = $old }
    "  $what -> exit $rc$(if ($rc -ne 0 -and $o) { ': ' + ($o -join ' ') })"
    $script:lastRc = $rc
}
# the key back to the export: deleted (whatever was written goes), imported, exported again and byte-compared
function Restore-Key([string]$key, [string]$before, [string]$after) {
    Invoke-Native reg @('delete', $key, '/f') "reg delete $key"   # exit 1 when the key is already gone: fine
    Invoke-Native reg @('import', $before) "reg import $before"
    $imp = $script:lastRc
    Invoke-Native reg @('export', $key, $after, '/y') "reg export $key -> $after"
    $script:restored = $imp -eq 0 -and $script:lastRc -eq 0 -and (Get-FileHash $before).Hash -eq (Get-FileHash $after).Hash
    "re-export byte-identical to the export before the run: $($script:restored)"
}
New-Item -ItemType Directory -Force $Out | Out-Null
if ($SelfTestKey) {
    if (-not $SelfTestKey.StartsWith('HKCU\Software\', [System.StringComparison]::OrdinalIgnoreCase) -or $SelfTestKey -match 'PushMPC') { throw "-SelfTestKey must be a scratch key under HKCU\Software, not PushMPC's" }
    $ps = 'HKCU:' + $SelfTestKey.Substring(4)
    New-Item -Path "$ps\Sub" -Force | Out-Null
    Set-ItemProperty -Path $ps -Name A -Value 'one' -Type String; Set-ItemProperty -Path "$ps\Sub" -Name B -Value 7 -Type DWord
    $b = Join-Path $Out 'selftest-before.reg'; $a = Join-Path $Out 'selftest-after.reg'
    Invoke-Native reg @('export', $SelfTestKey, $b, '/y') "reg export $SelfTestKey -> $b"
    Set-ItemProperty -Path $ps -Name A -Value 'changed' -Type String; Set-ItemProperty -Path $ps -Name Extra -Value 1 -Type DWord; Remove-Item "$ps\Sub" -Recurse
    Restore-Key $SelfTestKey $b $a
    $ok = $script:restored -and (Get-ItemProperty $ps).A -eq 'one' -and -not (Get-ItemProperty $ps).PSObject.Properties['Extra'] -and (Get-ItemProperty "$ps\Sub").B -eq 7
    Invoke-Native reg @('delete', $SelfTestKey, '/f') "reg delete $SelfTestKey (scratch key removed)"
    "self-test: $(if ($ok) { 'PASS' } else { 'FAIL' })"
    exit $(if ($ok) { 0 } else { 1 })
}
if (-not $Clip -or -not $CoreFolder -or -not $Lock) { throw "-Clip, -CoreFolder and -Lock are needed (or -SelfTestKey)" }
if (-not $Ax) { $Ax = Join-Path $PSScriptRoot '..\bin\PushMpcFilter.ax' }
if (-not $HostExe) { $HostExe = Join-Path $PSScriptRoot '..\build\late_reload_host.exe' }
$core =Join-Path $CoreFolder 'pushcore.dll'
foreach ($p in @($Clip, $Ax, $HostExe, $core, (Join-Path $CoreFolder 'manifest.txt'), (Join-Path $CoreFolder 'shaders'))) { if (-not (Test-Path -LiteralPath $p)) { throw "missing: $p" } }
$coreItem = Get-Item -LiteralPath $core; $axItem = Get-Item -LiteralPath $Ax
"core   $($coreItem.FullName)  $($coreItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))  $($coreItem.Length) bytes"
"filter $($axItem.FullName)  $($axItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))  $($axItem.Length) bytes"
if ($coreItem.LastWriteTime -lt $axItem.LastWriteTime) { throw "the core is older than the filter: build the core from the filter's tree and stage it again" }
$took = $false
if (-not $LockHeld) {
    try { $fs = [System.IO.File]::Open($Lock, 'CreateNew', 'Write'); $w = New-Object System.IO.StreamWriter($fs); $w.WriteLine("late_reload_live.ps1 $(Get-Date -Format s) pid $PID"); $w.Close(); $took = $true }
    catch { throw "$Lock is held by someone else: $(Get-Content $Lock -ErrorAction SilentlyContinue)" }
} elseif (-not (Test-Path $Lock)) { throw "-LockHeld given but $Lock does not exist" }
$before = Join-Path $Out 'pushmpc-before.reg'
$after = Join-Path $Out 'pushmpc-after.reg'
$code = -1
try {
    Invoke-Native reg @('export', 'HKCU\Software\PushMPC', $before, '/y') "reg export HKCU\Software\PushMPC -> $before"
    if ($script:lastRc -ne 0) { throw "reg export failed ($($script:lastRc)): nothing was changed" }
    try {
        New-Item -Path 'HKCU:\Software\PushMPC\Filter' -Force | Out-Null
        Set-ItemProperty -Path 'HKCU:\Software\PushMPC\Filter' -Name CoreFolder -Value $CoreFolder -Type String
        Set-ItemProperty -Path 'HKCU:\Software\PushMPC\Filter' -Name ReloadTimeoutMs -Value 200 -Type DWord
        "set CoreFolder=$CoreFolder ReloadTimeoutMs=200"
        $log = Join-Path $Out ("host-{0}.txt" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
        $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        try { & $HostExe $Ax $Clip $Seconds 2>&1 | ForEach-Object { $l = "$_"; $l; Add-Content -LiteralPath $log -Value $l -Encoding UTF8 }; $code = $LASTEXITCODE }
        finally { $ErrorActionPreference = $old }
        "late_reload_host exit $code (0 = taken over, 2 = played but no take-over, 1 = did not play); output $log"
    } finally {
        # back to the export: the key deleted (the test's values and whatever the filter wrote go), then imported
        Restore-Key 'HKCU\Software\PushMPC' $before $after
        if (-not $script:restored) { $code = 3; "REGISTRY NOT RESTORED: compare $before and $after (exit 3)" }
    }
} finally {
    if ($took) { Remove-Item $Lock -Force; "released $Lock" }
}
exit $code
