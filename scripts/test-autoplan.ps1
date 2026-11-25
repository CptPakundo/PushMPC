<#
  Headless checks of Setup's Auto plan and capture mode (pushmpc-install.ps1 autoplan, pushmpc-capture.ps1), in a scratch
  folder: no GPU, no player, nothing of the user's touched (HKCU writes are skipped with -NoRegister).
    test-autoplan.ps1 [-Scratch <dir>]
  Checks: the GPU family per adapter name (pushcore's familyOf rules), the frame generation order and upscaler per
  family, the AMD FSR SDK 2.4 offer, pmml.txt from the TensorRT-RTX EP, from a pmml-autobench.json measurement, removed
  again when DirectML is the choice, and capture mode's Start menu entry made and removed.
#>
param([string]$Scratch = (Join-Path ([IO.Path]::GetTempPath()) "pushmpc-autoplan-$PID"))
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$inst = Join-Path $here 'pushmpc-install.ps1'
$cap = Join-Path $here 'pushmpc-capture.ps1'
$ps = (Get-Process -Id $PID).Path
$fail = 0; $n = 0
function Test-That([bool]$ok, [string]$what) { $script:n++; if ($ok) { Write-Host "ok   $what" } else { Write-Host "FAIL $what" -ForegroundColor Red; $script:fail++ } }
function Get-Plan([string]$gpu, [string]$dir, [string[]]$more = @()) {
    $env:PUSHMPC_TEST_GPU = $gpu
    $lines = & $ps -NoProfile -ExecutionPolicy Bypass -File $inst autoplan -Machine -NoRegister -InstallDir $dir @more
    $h = @{}; foreach ($l in $lines) { if ("$l" -match '^@@plan\|([^=]+)=(.*)$') { $h[$Matches[1]] = $Matches[2] } }
    return $h
}
if (Test-Path $Scratch) { Remove-Item $Scratch -Recurse -Force }
New-Item -ItemType Directory -Force $Scratch | Out-Null

# 1. family and route per adapter (vendor:device:name as Win32_VideoController reports them)
$cases = @(
    @('10DE:2B85:NVIDIA GeForce RTX 5090', 'RTX 50', 'NVIDIA VFG', 'RTX Video Super Resolution'),
    @('10DE:2684:NVIDIA GeForce RTX 4090', 'RTX 40', 'NVIDIA VFG', 'RTX Video Super Resolution'),
    @('10DE:2204:NVIDIA GeForce RTX 3090', 'RTX 30', 'RTX 30 port', 'RTX Video Super Resolution'),
    @('10DE:1E84:NVIDIA GeForce RTX 2070 SUPER', 'RTX 20', 'DLSS-G', 'RTX Video Super Resolution'),
    @('10DE:2184:NVIDIA GeForce GTX 1660', 'GTX', 'optical flow', 'GTX 16'),
    @('10DE:1F82:NVIDIA GeForce GTX 1650', 'GTX', 'PushFlow, then FSR 3.1 FG', 'GTX 16'),
    @('10DE:1B80:NVIDIA GeForce GTX 1080', 'GTX', 'PushFlow', 'NIS'),
    @('1002:7550:AMD Radeon RX 9070 XT', 'RX 9000', 'FSR 4', 'FSR 4.1'),
    @('1002:744C:AMD Radeon RX 7900 XTX', 'RX 7000', 'AMD FRC', 'ZLUDA'),
    @('1002:73BF:AMD Radeon RX 6800 XT', 'RX 6000', 'AMD FRC', 'FSR SDK 2.4'),
    @('1002:731F:AMD Radeon RX 5700 XT', 'RX 5000', 'AMD FRC', 'FSR'),
    @('1002:164E:AMD Radeon(TM) Graphics', 'Radeon', 'AMD FRC', 'FSR'),
    @('8086:56A0:Intel(R) Arc(TM) A770 Graphics', 'Intel', 'XeSS-FG', 'NIS'),
    @('5143:0C36:Qualcomm(R) Adreno(TM) X1-85 GPU', 'other', 'PushFlow', 'NIS'),
    @('1414:008C:Microsoft Basic Display Adapter', 'none', '', '')
)
foreach ($c in $cases) {
    $p = Get-Plan $c[0] (Join-Path $Scratch 'none')
    Test-That ($p['family'] -eq $c[1]) "$($c[0].Split(':')[2]): family $($p['family']) (want $($c[1]))"
    if ($c[2]) { Test-That ($p['directshow'] -like "*$($c[2])*" -and $p['directshow'] -like "*$($c[3])*") "  route: $($p['directshow'])" }
    else { Test-That ($p['directshow'] -like '*none found*' -and $p['mpv'] -like 'portable*') "  no hardware GPU: $($p['directshow']) / $($p['mpv'])" }
    $offer = $c[1] -in 'RX 6000', 'RX 7000', 'RX 9000'
    Test-That (($p['fsrSdk240'] -like 'on:*') -eq $offer) "  FSR SDK 2.4: $($p['fsrSdk240'])"
}
$p = Get-Plan '1002:73BF:AMD Radeon RX 6800 XT' (Join-Path $Scratch 'none') @('-FidelityFx', 'Package')
Test-That ($p['fsrSdk240'] -like 'offered, declined*') "RX 6000 with -FidelityFx Package: $($p['fsrSdk240'])"

# 2. pmml.txt: a fake install (PushFlow's folder) and a fake pmml runtime with the TensorRT-RTX EP
$id = Join-Path $Scratch 'install'
$pf = Join-Path $id 'runtime\runtime\experimental\pushflow'
$rt = Join-Path $Scratch 'pmml'
New-Item -ItemType Directory -Force $pf, (Join-Path $rt 'graphs'), (Join-Path $rt 'eps\trt-rtx') | Out-Null
Set-Content (Join-Path $rt 'graphs\synth-yuv.onnx') 'stub'
Set-Content (Join-Path $rt 'eps\trt-rtx\ep.json') '{"name": "trt-rtx"}'
$p = Get-Plan '10DE:2684:NVIDIA GeForce RTX 4090' $id @('-PmmlRuntime', $rt, '-Apply')
$txt = Join-Path $pf 'pmml.txt'
Test-That ($p['pmmlBackend'] -eq 'ort:core:trt-rtx:0') "RTX 4090 + TensorRT-RTX EP: backend $($p['pmmlBackend'])"
Test-That ((Test-Path $txt) -and (Get-Content $txt -Raw) -match "(?m)^backend=ort:core:trt-rtx:0" -and (Get-Content $txt -Raw) -match [regex]::Escape("runtime=$rt")) "pmml.txt written: $((Get-Content $txt -ErrorAction SilentlyContinue) -join ' | ')"
Test-That (Test-Path (Join-Path $id 'auto-plan.json')) 'auto-plan.json written'
# an AMD card keeps DirectML (pmml's order without a measurement): pmml.txt goes again
$p = Get-Plan '1002:744C:AMD Radeon RX 7900 XTX' $id @('-PmmlRuntime', $rt, '-Apply')
Test-That (-not $p['pmmlBackend'] -and -not (Test-Path $txt)) "RX 7900 XTX without a measurement: DirectML, pmml.txt removed ($($p['pmmlWhy']))"
# a measurement on this card picks the fastest network that ran
$cache = @'
{
  "schema": 1,
  "entries": {
    "AMD Radeon RX 7900 XTX|1002:744C|test|1920x1080": {"time": 1, "pmml": "1", "results": {
      "a": {"candidate": "ort:directml:dml:0|balanced|pert", "ok": 1, "error": "", "initMs": 1, "packMs": 1, "netMs": 9.5, "synthMs": 1, "netW": 960, "netH": 544, "describe": ""},
      "b": {"candidate": "ort:core:amdgpu:0|balanced|pert", "ok": 1, "error": "", "initMs": 1, "packMs": 1, "netMs": 6.25, "synthMs": 1, "netW": 960, "netH": 544, "describe": ""},
      "c": {"candidate": "ort:core:openvino:0|balanced|pert", "ok": 0, "error": "no device", "initMs": 0, "packMs": 0, "netMs": 0, "synthMs": 0, "netW": 0, "netH": 0, "describe": ""}
    }}
  }
}
'@
Set-Content (Join-Path $rt 'pmml-autobench.json') $cache
$p = Get-Plan '1002:744C:AMD Radeon RX 7900 XTX' $id @('-PmmlRuntime', $rt, '-Apply')
Test-That ($p['pmmlBackend'] -eq 'ort:core:amdgpu:0' -and (Get-Content $txt -Raw) -match '(?m)^backend=ort:core:amdgpu:0') "measured: $($p['pmmlWhy'])"

# 3. capture mode: the Start menu entry made and removed (scratch Start menu, no PushFPS download)
$sm = Join-Path $Scratch 'startmenu'
$ci = Join-Path $Scratch 'capinstall'
& $cap enable -InstallDir $ci -StartMenu $sm -Players @('KMPlayer 64X', 'Plex for Windows (native upscaling only)')
Test-That (Test-Path (Join-Path $sm 'PushMPC Capture.lnk')) 'capture enable: Start menu entry'
$st = Get-Content (Join-Path $ci 'capture\capture.json') -Raw | ConvertFrom-Json
Test-That ($st.enabled -and @($st.players).Count -eq 2) "capture state: $(@($st.players) -join ', ')"
$o = @(& $ps -NoProfile -ExecutionPolicy Bypass -File $cap launch -InstallDir $ci -DryRun -Machine -PushFpsDir (Join-Path $Scratch 'no-pushfps'))
Test-That (($o -join ' ') -match 'would ask, then install PushFPS') "capture launch without its PushFPS: asks first ($($o -join ' | '))"
# the player's window opened directly (a stand-in PushFPS 4.5.3 and a charmap window as the player)
$fake = Join-Path $Scratch 'fake-pushfps'; New-Item -ItemType Directory -Force $fake | Out-Null
Copy-Item "$env:SystemRoot\System32\notepad.exe" (Join-Path $fake 'PushFPS.exe'); Set-Content (Join-Path $fake 'PushFPS-VERSION.txt') 'PushFPS 4.5.3'
$np = $null; try { $np = Start-Process (Join-Path $env:SystemRoot 'System32\charmap.exe') -PassThru; Start-Sleep 3; $np.Refresh() } catch { }
if ($np -and -not $np.HasExited -and $np.MainWindowHandle -and $np.MainWindowHandle -ne [IntPtr]::Zero) {
    $o = @(& $ps -NoProfile -ExecutionPolicy Bypass -File $cap launch -InstallDir $ci -DryRun -Machine -PushFpsDir $fake -Player charmap)
    $uri = "screen:0:$([uint64]$np.MainWindowHandle.ToInt64()):0:0:2:0:0:0:0"
    Test-That (($o -join ' ') -match [regex]::Escape("--page min $uri")) "capture launch: the player's window as the last argument ($($o -join ' | '))"
    Set-Content (Join-Path $fake 'PushFPS-VERSION.txt') 'PushFPS 9.9.9'
    $o = @(& $ps -NoProfile -ExecutionPolicy Bypass -File $cap launch -InstallDir $ci -DryRun -Machine -PushFpsDir $fake -Player charmap)
    Test-That (($o -join ' ') -match '--dialog screen' -and ($o -join ' ') -notmatch 'screen:0:') "capture launch with an untested PushFPS: the window picker ($($o -join ' | '))"
} else { Write-Host 'skip window start checks: no desktop window on this machine' }
if ($np) { Stop-Process -Id $np.Id -Force -ErrorAction SilentlyContinue }
& $ps -NoProfile -ExecutionPolicy Bypass -File $cap disable -InstallDir $ci -StartMenu $sm
Test-That (-not (Test-Path $sm) -and -not (Test-Path (Join-Path $ci 'capture'))) 'capture disable: entry and folder removed'

# 4. the VLC installer's version check (file versions come as "3,0,24,0" or "3.0.24.0")
$vlcInst = Join-Path (Split-Path $here) 'vlcplugin\install\pushmpc-vlc.ps1'
if (Test-Path $vlcInst) {
    foreach ($c in @(@('3,0,24,0', 'VLC 3.0'), @('3.0.24.0', 'VLC 3.0'), @('3.0.21', 'VLC 3.0'), @('4.0.0', 'not VLC 3.0'), @('3,1,0,0', 'not VLC 3.0'), @('13.0.1', 'not VLC 3.0'))) {
        $r = "$(& $ps -NoProfile -ExecutionPolicy Bypass -File $vlcInst -TestVersion $c[0])".Trim()
        Test-That ($r -eq $c[1]) "pushmpc-vlc.ps1 version '$($c[0])': $r (want $($c[1]))"
    }
}

Remove-Item Env:PUSHMPC_TEST_GPU -ErrorAction SilentlyContinue
Remove-Item $Scratch -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "$($n - $fail)/$n passed"
if ($fail) { exit 1 }
