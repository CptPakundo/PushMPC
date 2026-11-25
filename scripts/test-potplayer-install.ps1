<#
  Headless test of pushmpc-install.ps1's PotPlayer steps (no PotPlayer needed, nothing outside the test keys is written).
  Uses a fake registry root HKCU:\Software\PushMPC-Test\PotPlayerMini64, a fake PotPlayerMini64.exe in a temporary
  folder and the actions potplayer-apply / potplayer-remove. Every round trip must give back the fake root exactly
  (reg export before and after). Prints PASS/FAIL per check; exit code = number of failures. CI: integrations.yml.

    powershell -ExecutionPolicy Bypass -File scripts\test-potplayer-install.ps1
#>
$ErrorActionPreference = 'Stop'
$script = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'pushmpc-install.ps1'
$testRoot = 'HKCU:\Software\PushMPC-Test'
$root = "$testRoot\PotPlayerMini64"; $ov = "$root\Override"
$tmp = Join-Path $env:TEMP ("PushMPC-Test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$exeDir = Join-Path $tmp 'PotPlayer'; $exe = Join-Path $exeDir 'PotPlayerMini64.exe'; $state = Join-Path $tmp 'install'
$clsid = '{798D4EC3-E59C-4A4E-A2EE-C804AEAE04CC}'; $vsClsid = '{3AB7506B-FC4A-4144-8EE3-A97FAB4F9CB3}'
$fails = 0
function Check([string]$what, [bool]$ok) { if ($ok) { Write-Host "  PASS  $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:fails++ } }
function Run([string]$action, [string[]]$more = @()) {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script, $action, '-InstallDir', $state, '-PotRegRoot', $root, '-PotPlayerExe', $exe) + $more
    $out = & powershell.exe @a 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { Write-Host $out; throw "$action exited $LASTEXITCODE" }
    return $out
}
function Val([string]$k, [string]$n) { $v = (Get-ItemProperty -Path $k -Name $n -ErrorAction SilentlyContinue).$n; if ($v -is [uint32] -or ($v -is [int64] -and $v -gt 2147483647)) { $v = [int64]$v; if ($v -gt 2147483647) { $v -= 4294967296 } }; $v }
function Export-Fake {
    $f = Join-Path $tmp ('export-' + [guid]::NewGuid().ToString('N') + '.reg')
    if (-not (Test-Path $root)) { return '<absent>' }
    reg export ($root -replace '^HKCU:', 'HKCU') $f /y | Out-Null
    $t = [IO.File]::ReadAllText($f); Remove-Item $f; return $t
}
function New-Entry([string]$name, [hashtable]$v) {
    $k = "$ov\$name"; New-Item -Path $k -Force | Out-Null
    foreach ($n in $v.Keys) { $t = if ($v[$n] -is [int]) { 'DWord' } else { 'String' }; New-ItemProperty $k -Name $n -Value $v[$n] -PropertyType $t | Out-Null }
}
function Reset-Fake([switch]$Empty, [switch]$WithSvp) {
    if (Test-Path $testRoot) { Remove-Item $testRoot -Recurse -Force }
    if (Test-Path $state) { Remove-Item $state -Recurse -Force }
    New-Item -Path "$root\Settings" -Force | Out-Null
    New-ItemProperty "$root\Settings" -Name VideoRen2 -Value 0 -PropertyType DWord | Out-Null
    if ($Empty) { return }
    New-Entry '0000' @{ Type = 1; Disabled = 0; Name = 'LAV Video Decoder'; CLSID = '{EE30215D-164F-4A92-A4EB-9D4C13390F9F}'; Path = 'C:\LAV\LAVVideo.ax'; FilterType = 0; Merit = 0x00800003; MeritHi = 0; mod0000 = '{73646976-0000-0010-8000-00AA00389B71}' }
    if ($WithSvp) { New-Entry '0001' @{ Type = 0; Disabled = 0; Name = 'VapourSynth Filter'; CLSID = $vsClsid; FilterType = 0; Merit = 0x00200000; MeritHi = 0 } }
    New-Entry $(if ($WithSvp) { '0002' } else { '0001' }) @{ Type = -2 }
}
try {
    New-Item -ItemType Directory -Force $exeDir | Out-Null
    Set-Content -Path $exe -Value 'fake' -Encoding ASCII

    Write-Host 'dry run against the fake root'
    Reset-Fake
    $before = Export-Fake
    $o = Run 'potplayer-apply' @('-DryRun')
    Check 'dry run lists the entry 0001' ($o -match 'would write .*Override\\0001')
    Check 'dry run changes nothing' ((Export-Fake) -eq $before)

    Write-Host 'apply in front of the end marker'
    $o = Run 'potplayer-apply'
    $e = Get-ItemProperty "$ov\0001" -ErrorAction SilentlyContinue
    Check 'entry 0001 added' ($null -ne $e)
    Check 'entry values (Type 1 file, Disabled 0, FilterType 0 Prefer, CLSID, Path, Merit)' ($e.Type -eq 1 -and $e.Disabled -eq 0 -and $e.FilterType -eq 0 -and $e.CLSID -eq $clsid -and $e.Path -eq (Join-Path $state 'filter\PushMpcFilter.ax') -and $e.Merit -eq 0x00200001 -and $e.MeritHi -eq 0)
    Check 'media types NV12 and P010 (major, sub in turn)' ($e.mod0000 -eq '{73646976-0000-0010-8000-00AA00389B71}' -and $e.mod0001 -eq '{3231564E-0000-0010-8000-00AA00389B71}' -and $e.mod0003 -eq '{30313050-0000-0010-8000-00AA00389B71}')
    Check 'end marker moved to 0002' ((Val "$ov\0002" Type) -eq -2)
    Check 'the user''s entry 0000 untouched' ((Val "$ov\0000" Name) -eq 'LAV Video Decoder')
    Check 'renderer setting untouched' ((Val "$root\Settings" VideoRen2) -eq 0)
    $o = Run 'potplayer-apply'
    Check 'a second apply updates the same entry' ((Test-Path "$ov\0001") -and -not (Test-Path "$ov\0003") -and (Val "$ov\0002" Type) -eq -2)
    $o = Run 'potplayer-remove'
    Check 'remove gives back the key exactly' ((Export-Fake) -eq $before)

    Write-Host 'SVP''s VapourSynth entry'
    Reset-Fake -WithSvp
    $before = Export-Fake
    Run 'potplayer-apply' | Out-Null
    Check 'SVP entry disabled' ((Val "$ov\0001" Disabled) -eq 1)
    Check 'our entry at 0002, end at 0003' ((Val "$ov\0002" CLSID) -eq $clsid -and (Val "$ov\0003" Type) -eq -2)
    Run 'potplayer-remove' | Out-Null
    Check 'SVP enabled again and the key exactly as before' ((Export-Fake) -eq $before)

    Write-Host 'no filter list yet (PotPlayer never had an external filter)'
    Reset-Fake -Empty
    $before = Export-Fake
    Run 'potplayer-apply' | Out-Null
    Check 'entry 0000 and end marker 0001 created' ((Val "$ov\0000" CLSID) -eq $clsid -and (Val "$ov\0001" Type) -eq -2)
    Run 'potplayer-remove' | Out-Null
    Check 'the created list is removed again, key exactly as before' ((Export-Fake) -eq $before)

    Write-Host 'the user added a filter after PushMPC'
    Reset-Fake
    Run 'potplayer-apply' | Out-Null
    Rename-Item "$ov\0002" '0003'; New-Entry '0002' @{ Type = 1; Disabled = 0; Name = 'Later filter'; CLSID = '{11111111-2222-3333-4444-555555555555}'; Path = 'C:\x.ax'; FilterType = 0; Merit = 0; MeritHi = 0 }
    Run 'potplayer-remove' | Out-Null
    Check 'later filter moved down to 0001, end marker at 0002' ((Val "$ov\0001" Name) -eq 'Later filter' -and (Val "$ov\0002" Type) -eq -2 -and -not (Test-Path "$ov\0003"))
    Check 'no PushMPC entry left' (-not (Get-ChildItem $ov | Where-Object { (Get-ItemProperty $_.PSPath).CLSID -eq $clsid }))

    Write-Host 'INI mode'
    Reset-Fake
    $before = Export-Fake
    Set-Content -Path (Join-Path $exeDir 'PotPlayerMini64.ini') -Value '[Settings]' -Encoding Unicode
    $o = Run 'potplayer-apply'
    Check 'INI mode: warns, registry untouched' ($o -match 'INI mode' -and (Export-Fake) -eq $before)
    Remove-Item (Join-Path $exeDir 'PotPlayerMini64.ini')

    Write-Host '-NoPotPlayer'
    $o = Run 'potplayer-apply' @('-NoPotPlayer')
    Check '-NoPotPlayer changes nothing' ($o -match 'skipped' -and (Export-Fake) -eq $before)
}
finally {
    if (Test-Path $testRoot) { Remove-Item $testRoot -Recurse -Force }
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
}
Write-Host "potplayer install tests: $fails failure(s)"
exit $fails
