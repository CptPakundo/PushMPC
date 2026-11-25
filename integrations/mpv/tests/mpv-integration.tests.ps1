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
Check (($rows | Where-Object Player -eq 'MPC-QT').Managed -and ($rows | Where-Object Player -eq 'MPC-QT').Config -eq "$r\AppData\Roaming\mpc-qt\settings.json") 'MPC-QT managed through its settings.json'
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

# 8. mpv.net: installed (user config folder) and portable (portable_config next to mpvnet.exe)
$r = New-Root; $roots += $r
New-Item -ItemType Directory "$r\AppData\Local\Programs\mpv.net" -Force | Out-Null; [IO.File]::WriteAllText("$r\AppData\Local\Programs\mpv.net\mpvnet.exe", 'fake')
New-Item -ItemType Directory "$r\AppData\Roaming\mpv.net" -Force | Out-Null
$conf = "$r\AppData\Roaming\mpv.net\mpv.conf"; [IO.File]::WriteAllText($conf, "# mpv.net`r`nkeep-open=yes`r`n")
$portDir = "$r\Tools\mpvnet-portable"; New-Item -ItemType Directory "$portDir\portable_config" -Force | Out-Null; [IO.File]::WriteAllText("$portDir\mpvnet.exe", 'fake')
$before = Snapshot $r
$rows = Run 'Status' $r @{ MpvExe = @("$portDir\mpvnet.exe") }
Check (@($rows | Where-Object Player -eq 'mpv.net').Count -eq 2) 'both mpv.net installs found'
Check (($rows | Where-Object { $_.Player -eq 'mpv.net' -and $_.Exe -like '*Programs*' }).Config -eq $conf) 'installed mpv.net uses %APPDATA%\mpv.net\mpv.conf'
Check (($rows | Where-Object { $_.Player -eq 'mpv.net' -and $_.Exe -like '*portable*' }).Config -eq "$portDir\portable_config\mpv.conf") 'portable mpv.net uses its portable_config'
Run 'Install' $r @{ MpvExe = @("$portDir\mpvnet.exe") } | Out-Null
Check ((Read-PmText $conf).Text.StartsWith($script:PmBegin) -and (Read-PmText $conf).Text.EndsWith("keep-open=yes`r`n")) 'mpv.net mpv.conf gets the block on top'
Check (Test-Path "$portDir\portable_config\mpv.conf") 'portable mpv.net mpv.conf created'
Run 'Uninstall' $r | Out-Null
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'mpv.net restored byte for byte'

# 9. MPC-QT with an existing settings.json (options file off), then with the user's own options file on
$r = New-Root; $roots += $r
New-Item -ItemType Directory "$r\Program Files\mpc-qt" -Force | Out-Null; [IO.File]::WriteAllText("$r\Program Files\mpc-qt\mpc-qt.exe", 'fake')
New-Item -ItemType Directory "$r\AppData\Roaming\mpc-qt" -Force | Out-Null
$sj = "$r\AppData\Roaming\mpc-qt\settings.json"
$origJson = "{`n    `"playerOpenMode`": 0,`n    `"tweaksMpvOptionsChkBox`": false,`n    `"tweaksMpvOptionsFileChkBox`": false,`n    `"tweaksMpvOptionsFilePath`": `"`"`n}`n"
[IO.File]::WriteAllText($sj, $origJson)
$before = Snapshot $r
Run 'Install' $r | Out-Null
$j = Get-Content $sj -Raw | ConvertFrom-Json
Check ($j.tweaksMpvOptionsFileChkBox -eq $true -and $j.tweaksMpvOptionsFilePath -eq "$r\AppData\Local\PushMPC\mpc-qt-mpv.conf".Replace('\', '/')) 'MPC-QT options file on and pointed at PushMPC''s own file'
Check ($j.playerOpenMode -eq 0) 'other MPC-QT settings kept'
Check ((Read-PmText "$r\AppData\Local\PushMPC\mpc-qt-mpv.conf").Text -match 'vf-append=@pushmpc:vapoursynth') 'own options file has the block'
Run 'Uninstall' $r | Out-Null
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'MPC-QT settings.json restored byte for byte'
# changed by MPC-QT after the install: only our two values go back
Run 'Install' $r | Out-Null
$j = Get-Content $sj -Raw | ConvertFrom-Json; $j.playerOpenMode = 2; [IO.File]::WriteAllText($sj, ($j | ConvertTo-Json))
Run 'Uninstall' $r | Out-Null
$j = Get-Content $sj -Raw | ConvertFrom-Json
Check ($j.playerOpenMode -eq 2 -and $j.tweaksMpvOptionsFileChkBox -eq $false -and $j.tweaksMpvOptionsFilePath -eq '') 'MPC-QT: later changes kept, our values put back'
Check (-not (Test-Path "$r\AppData\Local\PushMPC\mpc-qt-mpv.conf")) 'own options file removed'
# the user's own options file
$userOpts = "$r\User\my-mpv-opts.conf"; New-Item -ItemType Directory "$r\User" -Force | Out-Null; [IO.File]::WriteAllText($userOpts, "volume=80`n")
[IO.File]::WriteAllText($sj, "{`n    `"tweaksMpvOptionsFileChkBox`": true,`n    `"tweaksMpvOptionsFilePath`": `"$($userOpts.Replace('\', '/'))`"`n}`n")
$before = Snapshot $r
Run 'Install' $r | Out-Null
Check ((Read-PmText $userOpts).Text.StartsWith($script:PmBegin)) 'block goes into the user''s own MPC-QT options file'
Check ((Get-Content $sj -Raw).Contains('my-mpv-opts.conf')) 'settings.json left pointing at the user''s file'
Run 'Uninstall' $r | Out-Null
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'user''s MPC-QT options file restored byte for byte'

# 10. Jellyfin MPV Shim and Plex-MPV-Shim: external mpv + block; without an mpv: left alone
$r = New-Root; $roots += $r
New-Item -ItemType Directory "$r\AppData\Roaming\jellyfin-mpv-shim", "$r\AppData\Roaming\plex-mpv-shim" -Force | Out-Null
[IO.File]::WriteAllText("$r\AppData\Roaming\jellyfin-mpv-shim\conf.json", "{`n    `"mpv_ext`": false,`n    `"mpv_ext_path`": null,`n    `"player_name`": `"htpc`"`n}")
[IO.File]::WriteAllText("$r\AppData\Roaming\plex-mpv-shim\conf.json", "{`"mpv_ext`": false}")
$before = Snapshot $r
$rows = Run 'Install' $r
Check (($rows | Where-Object Player -eq 'Jellyfin MPV Shim').Result -eq 'skipped') 'shim left alone without a VapourSynth-capable mpv'
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'nothing written for the shims without mpv'
Run 'Uninstall' $r | Out-Null
Fake-Mpv "$r\Program Files\mpv" -Shinchiro
$before = Snapshot $r
Run 'Install' $r | Out-Null
$j = Get-Content "$r\AppData\Roaming\jellyfin-mpv-shim\conf.json" -Raw | ConvertFrom-Json
Check ($j.mpv_ext -eq $true -and $j.mpv_ext_path -eq "$r\Program Files\mpv\mpv.exe" -and $j.player_name -eq 'htpc') 'Jellyfin shim: external mpv set, other values kept'
Check ((Read-PmText "$r\AppData\Roaming\jellyfin-mpv-shim\mpv.conf").Text -match 'vf-append=@pushmpc') 'Jellyfin shim mpv.conf has the block'
Check ((Get-Content "$r\AppData\Roaming\plex-mpv-shim\conf.json" -Raw | ConvertFrom-Json).mpv_ext -eq $true) 'Plex shim: external mpv set'
Run 'Uninstall' $r | Out-Null
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'shims restored byte for byte'

# 11. Kodi playercorefactory.xml: absent, existing with <players>, existing without, edited after the install
$r = New-Root; $roots += $r
Fake-Mpv "$r\Program Files\mpv" -Shinchiro
New-Item -ItemType Directory "$r\Program Files\Kodi" -Force | Out-Null; [IO.File]::WriteAllText("$r\Program Files\Kodi\kodi.exe", 'fake')
New-Item -ItemType Directory "$r\AppData\Roaming\Kodi\userdata" -Force | Out-Null
$xml = "$r\AppData\Roaming\Kodi\userdata\playercorefactory.xml"
$before = Snapshot $r
$rows = Run 'Install' $r
Check (($rows | Where-Object Player -eq 'Kodi').Result -eq 'installed') 'Kodi found and installed'
$x = [xml](Get-Content $xml -Raw)
Check ($x.playercorefactory.players.player.name -eq 'PushMPC mpv' -and $x.playercorefactory.players.player.filename -eq "$r\Program Files\mpv\mpv.exe") 'Kodi: external player = the mpv found'
Check ($x.playercorefactory.rules.action -eq 'prepend' -and $x.playercorefactory.rules.rule.player -eq 'PushMPC mpv') 'Kodi: prepend rule for video'
Run 'Uninstall' $r | Out-Null
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'Kodi: created file removed again'
$userXml = "<playercorefactory>`r`n  <players>`r`n    <player name=`"VLC`" type=`"ExternalPlayer`" audio=`"false`" video=`"true`">`r`n      <filename>C:\vlc.exe</filename>`r`n    </player>`r`n  </players>`r`n  <rules action=`"append`">`r`n    <rule protocols=`"rtmp`" player=`"VLC`"/>`r`n  </rules>`r`n</playercorefactory>`r`n"
[IO.File]::WriteAllText($xml, $userXml)
$before = Snapshot $r
Run 'Install' $r | Out-Null
$x = [xml](Get-Content $xml -Raw)
Check (@($x.playercorefactory.players.player).Count -eq 2 -and @($x.playercorefactory.players.player)[0].name -eq 'PushMPC mpv') 'Kodi: our player first in the user''s <players>, theirs kept'
Check (@($x.playercorefactory.rules).Count -eq 2) 'Kodi: our prepend rules element added next to theirs'
Run 'Install' $r | Out-Null
Check (@(([xml](Get-Content $xml -Raw)).playercorefactory.players.player).Count -eq 2) 'Kodi: a second install adds nothing'
Run 'Uninstall' $r | Out-Null
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'Kodi: user file restored byte for byte'
[IO.File]::WriteAllText($xml, "<?xml version=`"1.0`"?>`n<playercorefactory>`n</playercorefactory>`n")
$before = Snapshot $r
Run 'Install' $r | Out-Null
Check (([xml](Get-Content $xml -Raw)).playercorefactory.players.player.name -eq 'PushMPC mpv') 'Kodi: <players> created when missing'
(Get-Content $xml -Raw).Replace('</playercorefactory>', "<!-- mine -->`n</playercorefactory>") | Set-Content $xml -NoNewline
Run 'Uninstall' $r | Out-Null
$t = Get-Content $xml -Raw
Check ($t -notmatch 'PushMPC' -and $t -match '<!-- mine -->') 'Kodi: later edits kept, our parts removed'

# 13. the user PATH comes back exactly (a trailing ';' included)
$r = New-Root; $roots += $r
Fake-Mpv "$r\Program Files\mpv"
[IO.File]::WriteAllText("$r\user-path.txt", 'C:\Tools;C:\Other;')
Run 'Install' $r @{ RuntimeOnPath = $true } | Out-Null
Check ([IO.File]::ReadAllText("$r\user-path.txt") -eq "C:\Tools;C:\Other;$r\AppData\Local\PushMPC\vapoursynth") 'runtime appended after a trailing separator'
Run 'Uninstall' $r | Out-Null
Check ([IO.File]::ReadAllText("$r\user-path.txt") -eq 'C:\Tools;C:\Other;') 'user PATH restored exactly, trailing separator kept'
# 14. Jellyfin Desktop: one mpv.conf per profile
$r = New-Root; $roots += $r
$jp = "$r\AppData\Local\Jellyfin Desktop\profiles"; New-Item -ItemType Directory "$jp\abc123", "$jp\def456" -Force | Out-Null
[IO.File]::WriteAllText("$jp\def456\mpv.conf", "sub-auto=fuzzy`r`n")
$before = Snapshot $r
$rows = Run 'Install' $r
Check (@($rows | Where-Object { $_.Player -like 'Jellyfin Desktop*' -and $_.Result -eq 'installed' }).Count -eq 2) 'Jellyfin Desktop: both profiles installed'
Check ((Read-PmText "$jp\abc123\mpv.conf").Text.StartsWith($script:PmBegin) -and (Read-PmText "$jp\def456\mpv.conf").Text.EndsWith("sub-auto=fuzzy`r`n")) 'Jellyfin Desktop: block on top of each profile mpv.conf'
Run 'Uninstall' $r | Out-Null
Check ((Compare-Object $before (Snapshot $r)) -eq $null) 'Jellyfin Desktop restored byte for byte'
# 12. Stremio detected, not changed
$r = New-Root; $roots += $r
New-Item -ItemType Directory "$r\AppData\Local\Programs\Stremio" -Force | Out-Null; [IO.File]::WriteAllText("$r\AppData\Local\Programs\Stremio\stremio-shell-ng.exe", 'fake')
$rows = Run 'Status' $r
Check ((($rows | Where-Object Player -eq 'Stremio').Managed -eq $false) -and (($rows | Where-Object Player -eq 'Stremio').Note -match 'external player')) 'Stremio detected and explained, not changed'
foreach ($x in $roots) { Remove-Item -LiteralPath $x -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host "mpv-integration tests: $script:pass passed, $script:fail failed"
exit $(if ($script:fail) { 1 } else { 0 })
