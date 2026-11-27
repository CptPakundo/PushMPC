# Live check of the late reload take-over (PushMPC test tool; the tester runs it, never an agent without the GPU lock).
# Plays a clip through the BUILD's PushMpcFilter.ax (late_reload_host.exe: loaded directly, no registration, the
# installed MPC Video Renderer read-only) with the debug value ReloadTimeoutMs = 200, so every Auto rebuild passes its
# bound and is taken over late. Nothing of the installed player or filter is replaced.
# Steps, in this order:
#   1. the GPU lock file (-Lock) is taken (refused when someone else holds it; -LockHeld when the caller already holds it)
#   2. reg export HKCU\Software\PushMPC -> <Out>\pushmpc-before.reg
#   3. HKCU\Software\PushMPC\Filter: CoreFolder = -CoreFolder, ReloadTimeoutMs = 200
#   4. late_reload_host.exe <ax> <clip> <seconds>
#   5. restore.ps1 (-RestoreScript), then reg export again; byte-identical to step 2? If not, the step-2 export is
#      imported (delete + import) and compared again
#   6. the lock is released (only if this script took it)
# Example:
#   powershell -ExecutionPolicy Bypass -File filter\tools\late_reload_live.ps1 -Clip <clip> -CoreFolder <staged core folder>
#       -Out <results folder> -RestoreScript <the test pass's restore script> -Lock <GPU lock file>
param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Ax = (Join-Path $PSScriptRoot '..\bin\PushMpcFilter.ax'),
    [string]$HostExe = (Join-Path $PSScriptRoot '..\build\late_reload_host.exe'),
    [Parameter(Mandatory = $true)][string]$CoreFolder,   # pushcore.dll, manifest and shaders, laid out as an installed core folder
    [int]$Seconds = 75,
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$RestoreScript,
    [Parameter(Mandatory = $true)][string]$Lock,
    [switch]$LockHeld
)
$ErrorActionPreference = 'Stop'
foreach ($p in @($Clip, $Ax, $HostExe, "$CoreFolder\pushcore.dll", $RestoreScript)) { if (-not (Test-Path -LiteralPath $p)) { throw "missing: $p" } }
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
        & powershell -ExecutionPolicy Bypass -File $RestoreScript 2>&1 | ForEach-Object { "restore: $_" }
        reg export 'HKCU\Software\PushMPC' $after /y | Out-Null
        $same = (Get-FileHash $before).Hash -eq (Get-FileHash $after).Hash
        "re-export identical to the export before the run: $same"
        if (-not $same) {
            reg delete 'HKCU\Software\PushMPC' /f 2>$null | Out-Null
            reg import $before 2>&1 | Out-Null
            reg export 'HKCU\Software\PushMPC' $after /y | Out-Null
            "after importing $before again: identical $((Get-FileHash $before).Hash -eq (Get-FileHash $after).Hash)"
        }
    }
} finally {
    if ($took) { Remove-Item $Lock -Force; "released $Lock" }
}
exit $code
