<#
  Headless test of pushmpc-install.ps1's MPC-BE steps (no MPC-BE needed, nothing outside the test keys is written).
  Uses a fake registry root HKCU:\Software\PushMPC-Test\MPC-BE, a fake mpc-be64.exe in a temporary folder and the
  actions mpcbe-apply / mpcbe-remove. Prints PASS/FAIL per check; exit code = number of failures.

    powershell -ExecutionPolicy Bypass -File scripts\test-mpcbe-install.ps1
#>
$ErrorActionPreference = 'Stop'
$script = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'pushmpc-install.ps1'
$testRoot = 'HKCU:\Software\PushMPC-Test'
$root = "$testRoot\MPC-BE"; $bk = "$testRoot\MpcBeBackup"
$filters = "$root\ExternalFilters"; $dec = "$root\Filters\MPC Video Decoder"
$tmp = Join-Path $env:TEMP ("PushMPC-Test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$exeDir = Join-Path $tmp 'MPC-BE x64'; $exe = Join-Path $exeDir 'mpc-be64.exe'; $state = Join-Path $tmp 'install'
$clsid = '{798D4EC3-E59C-4A4E-A2EE-C804AEAE04CC}'; $vsClsid = '{3AB7506B-FC4A-4144-8EE3-A97FAB4F9CB3}'
$fails = 0
function Check([string]$what, [bool]$ok) { if ($ok) { Write-Host "  PASS  $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:fails++ } }
function Run([string]$action, [string[]]$more = @()) {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script, $action, '-InstallDir', $state, '-MpcBeRegRoot', $root, '-MpcBeBackupKey', $bk) + $more
    $out = & powershell.exe @a 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { Write-Host $out; throw "$action exited $LASTEXITCODE" }
    return $out
}
function Val([string]$k, [string]$n) { (Get-ItemProperty -Path $k -Name $n -ErrorAction SilentlyContinue).$n }
function Reset-Fake {
    if (Test-Path $testRoot) { Remove-Item $testRoot -Recurse -Force }
    if (Test-Path $state) { Remove-Item $state -Recurse -Force }
    New-Item -Path "$filters\000" -Force | Out-Null
    New-ItemProperty "$filters\000" -Name Name -Value 'Some filter' -PropertyType String | Out-Null
    New-ItemProperty "$filters\000" -Name CLSID -Value '{00000000-1111-2222-3333-444444444444}' -PropertyType String | Out-Null
    New-ItemProperty "$filters\000" -Name Enabled -Value 1 -PropertyType DWord | Out-Null
    New-Item -Path "$filters\001" -Force | Out-Null
    New-ItemProperty "$filters\001" -Name Name -Value 'VapourSynth Filter' -PropertyType String | Out-Null
    New-ItemProperty "$filters\001" -Name CLSID -Value $vsClsid -PropertyType String | Out-Null
    New-ItemProperty "$filters\001" -Name Enabled -Value 1 -PropertyType DWord | Out-Null
    New-Item -Path $dec -Force | Out-Null
    New-ItemProperty $dec -Name HwDecoder -Value 1 -PropertyType DWord | Out-Null
}
try {
    New-Item -ItemType Directory -Force $exeDir | Out-Null
    Set-Content -Path $exe -Value 'fake' -Encoding ASCII

    Write-Host 'MPC-BE not installed (this machine''s real detection, real default keys)'
    $realBefore = Test-Path 'HKCU:\Software\MPC-BE'
    $o = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script mpcbe-apply -InstallDir $state 2>&1 | Out-String
    Check 'reports "not installed"' ($o -match 'MPC-BE is not installed')
    Check 'HKCU\Software\MPC-BE not created' ((Test-Path 'HKCU:\Software\MPC-BE') -eq $realBefore)
    Check 'no MpcBeBackup key' (-not (Test-Path 'HKCU:\Software\PushMPC\MpcBeBackup'))

    Write-Host 'dry run against the fake root'
    Reset-Fake
    $o = Run 'mpcbe-apply' @('-MpcBeExe', $exe, '-DryRun')
    Check 'dry run lists the entry 002' ($o -match 'would write .*ExternalFilters\\002')
    Check 'dry run changes nothing' (-not (Test-Path "$filters\002") -and (Val $dec HwDecoder) -eq 1 -and -not (Test-Path $bk) -and (Val "$filters\001" Enabled) -eq 1)

    Write-Host 'apply'
    $o = Run 'mpcbe-apply' @('-MpcBeExe', $exe)
    $e = Get-ItemProperty "$filters\002" -ErrorAction SilentlyContinue
    Check 'entry 002 added' ($null -ne $e)
    Check 'entry values (Name, CLSID, SourceType 0, Enabled 1, LoadType 0 Prefer, Merit)' ($e.Name -eq 'PushMPC Filter' -and $e.CLSID -eq $clsid -and $e.SourceType -eq 0 -and $e.Enabled -eq 1 -and $e.LoadType -eq 0 -and $e.Merit -eq 0x00200001)
    Check 'DisplayName is the registered-filter moniker' ($e.DisplayName -eq "@device:sw:{083863F1-70DE-11D0-BD40-00A0C911CE86}\$clsid")
    Check 'MT_000 NV12, MT_001 P010 (76 chars each)' ($e.MT_000 -eq '{73646976-0000-0010-8000-00AA00389B71}{3231564E-0000-0010-8000-00AA00389B71}' -and $e.MT_001 -eq '{73646976-0000-0010-8000-00AA00389B71}{30313050-0000-0010-8000-00AA00389B71}' -and $e.MT_000.Length -eq 76)
    Check 'VapourSynth 001 disabled' ((Val "$filters\001" Enabled) -eq 0)
    Check 'HwDecoder 2, backup 1' ((Val $dec HwDecoder) -eq 2 -and (Val $bk HwDecoder) -eq 1)
    $st = Get-Content (Join-Path $state 'state.json') -Raw | ConvertFrom-Json
    Check 'state.json mpcBeEntry 002, mpcBeDisabledVapourSynth 001' ($st.mpcBeEntry -eq '002' -and $st.mpcBeDisabledVapourSynth -eq '001')

    Write-Host 'apply again (repair)'
    $o = Run 'mpcbe-apply' @('-MpcBeExe', $exe)
    Check 'updates 002, no 003' ($o -match 'updating entry 002' -and -not (Test-Path "$filters\003"))
    Check 'backup still holds the first value (1)' ((Val $bk HwDecoder) -eq 1)

    Write-Host 'remove, with a later entry after ours (renumbered)'
    New-Item -Path "$filters\003" -Force | Out-Null
    New-ItemProperty "$filters\003" -Name Name -Value 'Later filter' -PropertyType String | Out-Null
    $o = Run 'mpcbe-remove'
    Check 'ours removed, 003 renumbered to 002' (-not (Test-Path "$filters\003") -and (Val "$filters\002" Name) -eq 'Later filter')
    Check '000 and 001 kept, VapourSynth enabled again' ((Val "$filters\000" Name) -eq 'Some filter' -and (Val "$filters\001" Enabled) -eq 1)
    Check 'HwDecoder restored to 1, backup removed' ((Val $dec HwDecoder) -eq 1 -and -not (Test-Path $bk))
    $st = Get-Content (Join-Path $state 'state.json') -Raw | ConvertFrom-Json
    Check 'state.json without the MPC-BE values' (-not $st.PSObject.Properties['mpcBeEntry'])

    Write-Host 'HwDecoder absent before: absent after'
    Reset-Fake; Remove-ItemProperty $dec -Name HwDecoder
    Run 'mpcbe-apply' @('-MpcBeExe', $exe) | Out-Null
    Check 'backup "absent"' ((Val $bk HwDecoder) -eq 'absent')
    Run 'mpcbe-remove' | Out-Null
    Check 'value removed again' ($null -eq (Val $dec HwDecoder))

    Write-Host 'user changed HwDecoder after install: kept'
    Reset-Fake
    Run 'mpcbe-apply' @('-MpcBeExe', $exe) | Out-Null
    Set-ItemProperty $dec -Name HwDecoder -Value 4
    $o = Run 'mpcbe-remove'
    Check 'HwDecoder 4 kept' ((Val $dec HwDecoder) -eq 4 -and $o -match 'left at 4')

    Write-Host 'INI mode'
    Reset-Fake
    Set-Content -Path (Join-Path $exeDir 'mpc-be64.ini') -Value '[Settings]' -Encoding ASCII
    $o = Run 'mpcbe-apply' @('-MpcBeExe', $exe)
    Check 'INI mode warned' ($o -match 'INI mode')
    Check 'nothing written in INI mode' (-not (Test-Path "$filters\002") -and (Val $dec HwDecoder) -eq 1 -and -not (Test-Path $bk))
    Remove-Item (Join-Path $exeDir 'mpc-be64.ini')

    Write-Host '-NoMpcBe'
    $o = Run 'mpcbe-apply' @('-MpcBeExe', $exe, '-NoMpcBe')
    Check 'skipped' ($o -match 'skipped \(-NoMpcBe\)' -and -not (Test-Path "$filters\002"))
} finally {
    if (Test-Path $testRoot) { Remove-Item $testRoot -Recurse -Force }
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
}
Write-Host ''
Write-Host ("{0} failure(s)" -f $fails)
exit $fails
