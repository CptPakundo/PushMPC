<#
  Headless test of pushmpc-install.ps1's mpv route (stage of the vsplugin component, mpv-apply / mpv-remove, status,
  and install / uninstall as dry runs). Everything happens in a temporary folder: a stage, a fake root for
  mpv-integration.ps1 (-MpvRoot: APPDATA, LOCALAPPDATA, Program Files and the user PATH are files under it) and an
  install folder inside that root. No mpv, VapourSynth or MPC-HC change on this machine; no registry write.
  Prints PASS/FAIL per check; exit code = number of failures.

    powershell -ExecutionPolicy Bypass -File scripts\test-mpv-install.ps1
#>
$ErrorActionPreference = 'Stop'
$script = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'pushmpc-install.ps1'
$tmp = Join-Path $env:TEMP ("PushMPC-MpvTest-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$stage = Join-Path $tmp 'stage'; $root = Join-Path $tmp 'root'
$inst = Join-Path $root 'AppData\Local\PushMPC'
$mpvDir = Join-Path $root 'Program Files\mpv'; $conf = Join-Path $mpvDir 'portable_config\mpv.conf'
$rtDir = Join-Path $inst 'vapoursynth'; $userPath = Join-Path $root 'user-path.txt'
$todoPin = Join-Path $root 'pins\vapoursynth-pin.todo.json'
$fails = 0
function Check([string]$what, [bool]$ok) { if ($ok) { Write-Host "  PASS  $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:fails++ } }
function Run([string]$action, [string[]]$more = @()) {
    # a TODO pin file of the test's own: the real pins are filled, and a test must never download the runtime
    if (-not (Test-Path $todoPin)) {
        New-Item -ItemType Directory -Force (Split-Path $todoPin) | Out-Null
        '{"python":{"version":"TODO","url":"TODO","sha256":"TODO"},"vapoursynth":{"tag":"TODO","url":"TODO","sha256":"TODO"}}' | Set-Content -LiteralPath $todoPin -Encoding ASCII
    }
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script, $action, '-StageDir', $stage, '-InstallDir', $inst, '-MpvRoot', $root, '-VsPinFile', $todoPin) + $more
    $out = & powershell.exe @a 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { Write-Host $out; throw "$action exited $LASTEXITCODE" }
    return $out
}
function Hash([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
function Snapshot { Get-ChildItem -LiteralPath $root -Recurse -File -Force | Sort-Object FullName | ForEach-Object { "$($_.FullName.Substring($root.Length))=$(Hash $_.FullName)" } }
function StateProp([string]$n) { $p = Join-Path $inst 'state.json'; if (-not (Test-Path $p)) { return $null }; $s = Get-Content $p -Raw | ConvertFrom-Json; if ($s.PSObject.Properties[$n]) { $s.$n } else { $null } }
try {
    New-Item -ItemType Directory -Force $root | Out-Null
    $fakeDll = Join-Path $tmp 'pushmpc_vs.dll'; [IO.File]::WriteAllText($fakeDll, 'fake plugin')

    Write-Host 'stage without pushmpc_vs.dll'
    $o = Run 'stage' @('-VsPluginDll', (Join-Path $tmp 'missing\pushmpc_vs.dll'))
    Check 'warns that the vsplugin component is skipped' ($o -match 'vsplugin \(the mpv route\) not staged: missing')
    Check 'filter and core staged, no vsplugin folder' ((Test-Path "$stage\filter\manifest.txt") -and (Test-Path "$stage\core\manifest.txt") -and -not (Test-Path "$stage\vsplugin"))

    Write-Host 'stage with a (fake) pushmpc_vs.dll'
    $o = Run 'stage' @('-VsPluginDll', $fakeDll)
    $m = Get-Content "$stage\vsplugin\manifest.txt"
    Check 'vsplugin manifest: component, version, 4 files' (($m -contains 'component=vsplugin') -and ($m | Where-Object { $_ -match '^version=\d' }) -and @($m | Where-Object { $_ -like 'file *' }).Count -eq 4)
    Check 'has pushmpc_vs.dll, pushmpc.vpy, mpv-integration.ps1, vapoursynth-pin.json' (@('pushmpc_vs.dll', 'pushmpc.vpy', 'mpv-integration.ps1', 'vapoursynth-pin.json' | Where-Object { Test-Path "$stage\vsplugin\$_" }).Count -eq 4)

    Write-Host 'no mpv in the fake root'
    $o = Run 'mpv-apply'
    Check 'reports no mpv / SMPlayer, installs nothing' ($o -match 'no mpv or SMPlayer found' -and -not (Test-Path "$inst\vsplugin"))

    Write-Host 'shinchiro portable mpv with its own mpv.conf; runtime pins still TODO'
    New-Item -ItemType Directory -Force (Split-Path $conf) | Out-Null
    [IO.File]::WriteAllText("$mpvDir\mpv.exe", 'fake'); [IO.File]::WriteAllText("$mpvDir\updater.bat", 'rem')
    [IO.File]::WriteAllText($conf, "vo=gpu-next`r`n[film]`r`nhwdec=d3d11va`r`n")
    $confHash = Hash $conf
    $o = Run 'mpv-apply'
    Check 'detects the shinchiro mpv' ($o -match 'mpv \(shinchiro build\)')
    Check 'vsplugin installed' (Test-Path "$inst\vsplugin\pushmpc_vs.dll")
    Check 'TODO pins: runtime skipped with a clear message' ($o -match 'not filled in yet' -and -not (Test-Path $rtDir))
    Check 'mpv.conf untouched without a runtime' ((Hash $conf) -eq $confHash -and -not (Test-Path "$conf.pushmpc-backup"))

    Write-Host 'runtime present (fake VSScript.dll): dry run'
    New-Item -ItemType Directory -Force $rtDir | Out-Null; [IO.File]::WriteAllText("$rtDir\VSScript.dll", 'fake')
    $before = Snapshot
    $o = Run 'mpv-apply' @('-DryRun')
    Check 'dry run changes nothing' ($null -eq (Compare-Object $before (Snapshot)))

    Write-Host 'apply'
    $o = Run 'mpv-apply'
    $t = [IO.File]::ReadAllText($conf)
    Check 'mpv.conf has the PushMPC block with the installed pushmpc.vpy' ($t.StartsWith('# >>> PushMPC managed') -and $t.Contains("$($inst.Replace('\', '/'))/vsplugin/pushmpc.vpy"))
    Check 'runtime folder on the (fake) user PATH' ((Test-Path $userPath) -and ((Get-Content $userPath -Raw) -split ';' | ForEach-Object Trim) -contains $rtDir)
    Check 'state.json records mpvRoute' ($null -ne (StateProp 'mpvRoute'))
    $o = Run 'status'
    Check 'status lists vsplugin and the installed mpv' ($o -match 'vsplugin\s+version' -and $o -match 'mpv \(shinchiro build\): installed True')
    $once = Hash $conf
    Run 'mpv-apply' | Out-Null
    Check 'a second apply changes nothing' ((Hash $conf) -eq $once)

    Write-Host 'remove'
    $o = Run 'mpv-remove'
    Check 'mpv.conf restored byte for byte, backup gone' ((Hash $conf) -eq $confHash -and -not (Test-Path "$conf.pushmpc-backup"))
    Check 'runtime folder off the user PATH' (-not (((Get-Content $userPath -Raw) -split ';' | ForEach-Object Trim) -contains $rtDir))
    Check 'vsplugin removed; a runtime without our manifest kept' (-not (Test-Path "$inst\vsplugin") -and (Test-Path "$rtDir\VSScript.dll"))
    Check 'state.json without mpvRoute, mpv-integration.json gone' ($null -eq (StateProp 'mpvRoute') -and -not (Test-Path "$inst\mpv-integration.json"))

    Write-Host '-NoMpv'
    $o = Run 'mpv-apply' @('-NoMpv')
    Check 'skipped' ($o -match 'skipped \(-NoMpv\)' -and -not (Test-Path "$inst\vsplugin") -and (Hash $conf) -eq $confHash)

    Write-Host 'install and uninstall, dry runs (MPC-HC, MPC-BE, regsvr32, logon task off)'
    $before = Snapshot
    $off = @('-DryRun', '-NoMpcHc', '-NoMpcBe', '-NoRegister', '-NoTask', '-NoExisting', '-KeepSettings')
    $o = Run 'install' $off
    Check 'install runs the mpv step' ($o -match 'mpv, SMPlayer, MPC-QT' -and $o -match 'would move .*vsplugin')
    $o = Run 'uninstall' $off
    Check 'uninstall dry run reaches the end' ($o -match 'Removed')
    Check 'dry runs change nothing' ($null -eq (Compare-Object $before (Snapshot)))

    Write-Host "SVP's VapourSynth on the (fake) user PATH (RC2 F5)"
    Remove-Item $rtDir -Recurse -Force
    $svp = Join-Path $root 'Program Files\SVP 4\mpv64'
    New-Item -ItemType Directory -Force $svp | Out-Null; [IO.File]::WriteAllText("$svp\VSScript.dll", 'fake SVP'); [IO.File]::WriteAllText("$svp\mpv.exe", 'fake SVP mpv')
    [IO.File]::WriteAllText($userPath, "C:\Windows;$svp")
    $o = Run 'mpv-apply'
    Check "SVP's VSScript.dll is not taken as the user's own" ($o -match 'belongs to SVP' -and $o -notmatch 'your own installation')
    Check 'PushMPC wants its own runtime (TODO pins: skipped), mpv.conf untouched' ($o -match 'not filled in yet' -and (Hash $conf) -eq $confHash)
    Check "SVP's mpv is detected and left alone" ($o -match "SVP's mpv" -and $o -match 'left alone')
    $own = Join-Path $root 'VapourSynth'
    New-Item -ItemType Directory -Force $own | Out-Null; [IO.File]::WriteAllText("$own\VSScript.dll", 'fake own')
    [IO.File]::WriteAllText($userPath, "C:\Windows;$svp;$own")
    $o = Run 'mpv-apply'
    Check 'a VapourSynth outside SVP still counts as the user''s own' ($o -match [regex]::Escape("your own installation ($own)"))
    Run 'mpv-remove' | Out-Null
    Check 'mpv.conf restored after the own-runtime apply' ((Hash $conf) -eq $confHash)
    New-Item -ItemType Directory -Force $rtDir | Out-Null; [IO.File]::WriteAllText("$rtDir\VSScript.dll", 'fake')
    [IO.File]::WriteAllText($userPath, "C:\Windows;$svp")
    Run 'mpv-apply' | Out-Null
    $parts = @((Get-Content $userPath -Raw).Split(';') | ForEach-Object Trim | Where-Object { $_ })
    Check 'our runtime goes in front of SVP''s folder on the user PATH' ($parts[0] -eq $rtDir -and $parts -contains $svp)
    Run 'mpv-remove' | Out-Null
    $parts = @((Get-Content $userPath -Raw).Split(';') | ForEach-Object Trim | Where-Object { $_ })
    Check 'remove takes only our entry off the user PATH' (($parts -join ';') -eq "C:\Windows;$svp")

    Write-Host 'repair with a same-version component whose files differ (RC2 F3), dry run'
    foreach ($n in 'filter', 'core') { & robocopy "$stage\$n" "$inst\$n" /E /NFL /NDL /NJH /NJS /NP | Out-Null }
    $o = Run 'install' $off
    Check 'identical components: already installed' ($o -match 'filter \S+ is already installed \(every file matches' -and $o -match 'core \S+ is already installed \(every file matches')
    $ax = "$inst\filter\PushMpcFilter.ax"; [IO.File]::AppendAllText($ax, 'x')
    $mf = "$inst\filter\manifest.txt"; $fi = Get-Item $ax
    $lines = @(Get-Content $mf | ForEach-Object { if ($_ -match '^file \S+ \d+ PushMpcFilter\.ax$') { "file $(Hash $ax) $($fi.Length) PushMpcFilter.ax" } else { $_ } })
    [IO.File]::WriteAllLines($mf, $lines)
    $o = Run 'install' $off
    Check 'same version, other files: the filter is replaced' ($o -match 'filter : same version \S+, but the files differ from this setup''s: PushMpcFilter\.ax' -and $o -match 'would move .*\\filter')
    Check 'the core still counts as installed' ($o -match 'core \S+ is already installed')
    [IO.File]::AppendAllText("$inst\core\pushcore.dll", 'x')
    $o = Run 'install' $off
    Check 'an installed file that does not match its own manifest is repaired too' ($o -match 'core : the installed files do not match their manifest: pushcore\.dll')
} finally {
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
}
Write-Host ''
Write-Host ("{0} failure(s)" -f $fails)
exit $fails
