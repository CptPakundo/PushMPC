# SPDX-License-Identifier: GPL-3.0-or-later
# pmml tester script for Windows (see tester.sh): run on a real GPU by a tester, never by CI. Reads only; writes a report.
#   powershell -File tester.ps1 -Cli pmml_cli.exe -Runtime <dir> [-Clip clip.y4m] [-Out pmml-report]
param([Parameter(Mandatory)][string]$Cli, [Parameter(Mandatory)][string]$Runtime, [string]$Clip = '', [string]$Out = 'pmml-report')
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $Out | Out-Null
& { Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, AdapterRAM | Format-List | Out-String
    if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) { nvidia-smi --query-gpu=name,driver_version,utilization.gpu,memory.used,clocks.gr,power.draw --format=csv } } > "$Out\system.txt" 2>&1
foreach ($fl in 'core', 'directml') { if (Test-Path "$Runtime\ort\$fl") { & $Cli list --runtime $Runtime --flavor $fl > "$Out\devices-$fl.txt" 2>&1 } }
foreach ($size in '1920x1080', '3840x2160') { foreach ($m in 5, 2) { & $Cli auto --runtime $Runtime --size $size --fps 23.976 --mult $m --pairs 8 --cache $Out --refresh > "$Out\auto-$size-x$m.txt" 2>&1 } }
if ($Clip) {
    $backends = Get-Content "$Out\devices-*.txt" | Where-Object { $_ -match '^(ort|ncnn):\S+' } | ForEach-Object { ($_ -split '\s+')[0] } | Sort-Object -Unique
    foreach ($b in $backends) { foreach ($t in 'fast', 'balanced', 'quality') { foreach ($o in @(@(), @('--flow-once'))) {
        & $Cli run --runtime $Runtime --backend $b --tier $t --clip $Clip --frames 17 --mult 4 @o >> "$Out\quality.txt" 2>&1 } } }
}
"report in $Out (send the whole folder)"
