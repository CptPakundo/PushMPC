<#
  Tests of mpv-integration.ps1 against fake roots only (never the real profile): install / uninstall round trips that
  must restore every file byte for byte, idempotent installs, edits made after the install, conflict warnings, SMPlayer's
  ini, MPC-QT and SVP detection, the user PATH entry and -WhatIf.
    pwsh -File mpv-integration.tests.ps1      (exit code 0 = all passed)
#>
$ErrorActionPreference = 'Stop'
$module = Join-Path (Split-Path -Parent $PSScriptRoot) 'mpv-integration.ps1'
. $module
$script:pass = 0; $script:fail = 0
function Check([bool]$c, [string]$what) { if ($c) { $script:pass++ } else { $script:fail++; Write-Host "FAIL: $what" } }
function New-Root { $r = Join-Path ([IO.Path]::GetTempPath()) ('pushmpc-mpvtest-' + [guid]::NewGuid().ToString('N')); New-Item -ItemType Directory $r | Out-Null; $r }
function Hash([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
function Snapshot([string]$root) { Get-ChildItem -LiteralPath $root -Recurse -File -Force | Where-Object { $_.Name -ne 'mpv-integration.json' } | Sort-Object FullName | ForEach-Object { "$($_.FullName.Substring($root.Length))=$(Hash $_.FullName)" } }
function Run([string]$action, [string]$root, [hashtable]$extra = @{}) { Invoke-PmMpvIntegration -Action $action -Root $root -ScriptPath "$root\AppData\Local\PushMPC\vsplugin\pushmpc.vpy" @extra }
function Fake-Mpv([string]$dir, [switch]$Portable, [switch]$Shinchiro) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    [IO.File]::WriteAllText("$dir\mpv.exe", 'fake')
    if ($Shinchiro) { [IO.File]::WriteAllText("$dir\updater.bat", 'rem') }
    if ($Portable) { New-Item -ItemType Directory -Path "$dir\portable_config" -Force | Out-Null }
}
$roots = @()

# 1. shinchiro portable mpv with an existing CRLF mpv.conf that has profiles and a BOM
$r = New-Root; $roots += $r
Fake-Mpv "$r\Program Files\mpv" -Portable -Shinchiro
$conf = "$r\Program Files\mpv\portable_config\mpv.conf"
$orig = "# my settings`r`nvo=gpu-next`r`nprofile=high-quality`r`n`r`n[film]`r`nhwdec=d3d11va`r`n"
[IO.File]::WriteAllBytes($conf, [byte[]](0xEF, 0xBB, 0xBF) + [Text.Encoding]::UTF8.GetBytes($orig))
$before = Snapshot $r
$rows = Run 'Status' $r
Check (($rows | Where-Object Player -eq 'mpv (shinchiro build)').Config -eq $conf) 'shinchiro portable mpv detected with its portable_config'
Run 'Install' $r | Out-Null
$t = Read-PmText $conf
Check $t.Bom 'BOM kept'
Check ($t.Text.StartsWith($script:PmBegin)) 'block at the very top (global section)'
Check ($t.Text.Contains("vf-append=@pushmpc:vapoursynth=file=[$($r.Replace('\','/'))/AppData/Local/PushMPC/vsplugin/pushmpc.vpy]:buffered-frames=4:concurrent-frames=2`r`n")) 'vf-append line with the bracket-quoted path, CRLF'
foreach ($l in 'hwdec=auto-copy', 'hr-seek-framedrop=no', 'interpolation=no') { Check ($t.Text.Contains("$l`r`n")) "block has $l" }
Check ($t.Text.EndsWith($orig)) 'original content follows unchanged'
Check (Test-Path "$conf.pushmpc-backup") 'backup written'
$once = Hash $conf
Run 'Install' $r | Out-Null
Check ((Hash $conf) -eq $once) 'a second install changes nothing'
Check ((Run 'Status' $r | Where-Object Player -eq 'mpv (shinchiro build)').Installed) 'status: installed'
Run 'Uninstall' $r | Out-Null
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'uninstall restores every file byte for byte'
Check (-not (Test-Path "$conf.pushmpc-backup")) 'backup removed after the exact restore'

# 2. no mpv.conf at all (mpv in Program Files without portable_config -> %APPDATA%\mpv)
$r = New-Root; $roots += $r
Fake-Mpv "$r\Program Files\mpv"
$before = Snapshot $r
Run 'Install' $r | Out-Null
$conf = "$r\AppData\Roaming\mpv\mpv.conf"
Check (Test-Path $conf) 'mpv.conf created in %APPDATA%\mpv'
Check (Test-Path "$conf.pushmpc-absent") 'absent marker written'
Run 'Uninstall' $r | Out-Null
Check (-not (Test-Path "$r\AppData\Roaming\mpv")) 'the created folder is gone again'
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'nothing left behind'

# 3. the user edits mpv.conf after the install: the block goes, the edit stays, the backup is kept
$r = New-Root; $roots += $r
Fake-Mpv "$r\Program Files\mpv" -Portable
$conf = "$r\Program Files\mpv\portable_config\mpv.conf"
[IO.File]::WriteAllText($conf, "volume=50`n")
Run 'Install' $r | Out-Null
Check ((Read-PmText $conf).Text.Contains("hwdec=auto-copy`n") -and -not (Read-PmText $conf).Text.Contains("`r`n")) 'LF file gets an LF block'
Add-Content -LiteralPath $conf -Value 'fullscreen=yes' -NoNewline
$res = Run 'Uninstall' $r
$txt = [IO.File]::ReadAllText($conf)
Check ($txt -eq "volume=50`nfullscreen=yes") 'block removed, the later edit kept'
Check (Test-Path "$conf.pushmpc-backup") 'backup kept when the file changed since'
Check (($res | Where-Object Config -eq $conf).Result -match 'changed since') 'uninstall says why the backup stays'

# 4. conflicts: SVP's filter and a later hwdec
$r = New-Root; $roots += $r
Fake-Mpv "$r\Program Files\mpv" -Portable
$conf = "$r\Program Files\mpv\portable_config\mpv.conf"
[IO.File]::WriteAllText($conf, "hwdec=d3d11va`r`nvf=vapoursynth=~~/svp.vpy`r`n[p]`r`nhwdec=nvdec`r`n")
$rows = Run 'Install' $r
$note = ($rows | Where-Object Config -eq $conf).Note
Check ($note -match 'another VapourSynth filter') 'warns about a second VapourSynth filter'
Check ($note -match "hwdec=d3d11va") 'warns about a later global hwdec without copy-back'
Check ($note -notmatch 'nvdec') 'a hwdec inside a profile is not a global conflict'
Run 'Uninstall' $r | Out-Null
Check ([IO.File]::ReadAllText($conf) -eq "hwdec=d3d11va`r`nvf=vapoursynth=~~/svp.vpy`r`n[p]`r`nhwdec=nvdec`r`n") 'conflict case restored exactly'

# 5. SMPlayer (ini with and without the key), SVP's mpv and MPC-QT detection, the PATH entry
$r = New-Root; $roots += $r
New-Item -ItemType Directory "$r\Program Files\SMPlayer\mpv" -Force | Out-Null
[IO.File]::WriteAllText("$r\Program Files\SMPlayer\smplayer.exe", 'fake'); [IO.File]::WriteAllText("$r\Program Files\SMPlayer\mpv\mpv.exe", 'fake')
New-Item -ItemType Directory "$r\AppData\Roaming\SMPlayer" -Force | Out-Null
$ini = "$r\AppData\Roaming\SMPlayer\smplayer.ini"
[IO.File]::WriteAllText($ini, "[%General]`r`nversion=1`r`n`r`n[advanced]`r`nmplayer_additional_options=--volume-max=150`r`nother=1`r`n")
Fake-Mpv "$r\Program Files (x86)\SVP 4\mpv64"
New-Item -ItemType Directory "$r\Program Files\mpc-qt" -Force | Out-Null; [IO.File]::WriteAllText("$r\Program Files\mpc-qt\mpc-qt.exe", 'fake')
[IO.File]::WriteAllText("$r\user-path.txt", 'C:\Tools')
$before = Snapshot $r
$rows = Run 'Status' $r
Check (($rows | Where-Object Player -eq 'SMPlayer').Config -eq $ini) 'SMPlayer found with its ini'
Check (($rows | Where-Object Player -eq 'MPC-QT').Note -match 'manual') 'MPC-QT noted as manual'
Check (-not ($rows | Where-Object Player -eq "SVP's mpv").Managed) "SVP's mpv detected and left alone"
Check (-not ($rows | Where-Object { $_.Player -like 'mpv*' -and $_.Exe -like '*SMPlayer*' })) "SMPlayer's own mpv is not a separate mpv.conf target"
Run 'Install' $r @{ RuntimeOnPath = $true } | Out-Null
$line = (Get-Content $ini | Where-Object { $_ -like 'mplayer_additional_options=*' })
Check ($line -like 'mplayer_additional_options=--volume-max=150 --vf-append=@pushmpc:vapoursynth=file=*--hwdec=auto-copy --hr-seek-framedrop=no --interpolation=no') 'SMPlayer options appended after the user''s'
Check ([IO.File]::ReadAllText("$r\user-path.txt") -eq "C:\Tools;$r\AppData\Local\PushMPC\vapoursynth") 'runtime folder added to the user PATH'
Check (-not (Test-Path "$r\Program Files (x86)\SVP 4\mpv64\portable_config")) "SVP's mpv untouched"
Run 'Uninstall' $r | Out-Null
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'SMPlayer ini and PATH restored byte for byte'
# an ini without the key
[IO.File]::WriteAllText($ini, "[advanced]`r`nother=1`r`n")
$before = Snapshot $r
Run 'Install' $r | Out-Null
Check ((Get-Content $ini) -contains 'other=1' -and ((Get-Content $ini) | Where-Object { $_ -like 'mplayer_additional_options=--vf-append*' })) 'key added to an existing [advanced]'
Run 'Uninstall' $r | Out-Null
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'key removed again exactly'

# 6. -WhatIf writes nothing
$r = New-Root; $roots += $r
Fake-Mpv "$r\Program Files\mpv" -Portable
[IO.File]::WriteAllText("$r\Program Files\mpv\portable_config\mpv.conf", "volume=50`r`n")
$before = Snapshot $r
Invoke-PmMpvIntegration -Action Install -Root $r -WhatIf | Out-Null
Check ((Compare-Object $before (Snapshot $r)) -eq $null) '-WhatIf changes nothing'
Check (-not (Test-Path "$r\AppData\Local\PushMPC\mpv-integration.json")) '-WhatIf writes no state file'

# 7. a script path with ']' uses mpv's %n% quoting
Check ((Get-PmVfOption 'C:\a]b\pushmpc.vpy' 4 2) -eq '@pushmpc:vapoursynth=file=%18%C:/a]b/pushmpc.vpy:buffered-frames=4:concurrent-frames=2') "a ']' in the path is length-quoted"

foreach ($x in $roots) { Remove-Item -LiteralPath $x -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host "mpv-integration tests: $script:pass passed, $script:fail failed"
exit $(if ($script:fail) { 1 } else { 0 })
