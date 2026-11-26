<#
.SYNOPSIS
  Windows on Arm tester for PushMPC's own ARM64 mpv: Snapdragon X (Adreno, Vulkan) and NVIDIA N1X / RTX Spark.

.DESCRIPTION
  Hosted CI has no Adreno and no NVIDIA GPU, so this script checks on real hardware what CI cannot:
    1. the machine: OS architecture, GPUs and drivers, the Vulkan devices mpv sees, NVIDIA's runtime folder;
    2. each engine on its own, on a synthetic 1080p24 and 4K24 clip made by the build's own ffmpeg (no media needed):
       auto, nvidia (RTX Spark only), portable-vulkan (the GPU engine on this GPU's Vulkan driver), portable-cpu;
       software frames and D3D11 decoder frames (the copy fallback); which engine opened, the multiplier, the output
       frame count and the speed (output frames per second: must stay above 2x / 4x the clip rate for real-time);
    3. real playback in a window (vo=gpu-next, display-resample) for 15 s per clip: the rate mpv sees from the filter
       and the dropped frames.
  Nothing is installed, no registry value or PushMPC setting is changed; the only output is the folder -Out (logs and
  report.txt). Send back report.txt and the *.log files.

  Needs: PushMPC's ARM64 mpv, installed by Setup (default %LOCALAPPDATA%\PushMPC\mpv-native) or the unzipped release asset
  PushMPC-mpv-windows-arm64-<version>.zip (-MpvDir). About 15 minutes, 3 GB free disk (4K test clip, ffv1).
  On RTX Spark, Setup has put NVIDIA's runtime in %LOCALAPPDATA%\PushMPC\runtime\nvidia-vfx; -NvidiaRuntime points elsewhere.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File arm64-mpv-manual.ps1
  powershell -ExecutionPolicy Bypass -File arm64-mpv-manual.ps1 -MpvDir C:\tmp\PushMPC-mpv-windows-arm64 -Skip4k
#>
param(
    [string]$MpvDir = '',
    [string]$Out = (Join-Path $env:TEMP 'pushmpc-arm64-test'),
    [string]$NvidiaRuntime = '',
    [switch]$Skip4k,
    [switch]$SkipPlayback
)
$ErrorActionPreference = 'Stop'
if (-not $MpvDir) {
    foreach ($c in (Join-Path $env:LOCALAPPDATA 'PushMPC\mpv-native'), (Join-Path $env:LOCALAPPDATA 'Programs\PushMPC\mpv-native')) { if (Test-Path (Join-Path $c 'mpv.com')) { $MpvDir = $c; break } }
}
if (-not $MpvDir -or -not (Test-Path (Join-Path $MpvDir 'mpv.com'))) { throw "PushMPC's ARM64 mpv not found: pass -MpvDir <folder with mpv.com, ffmpeg.exe, pushmpc_engine.dll>" }
$ff = Join-Path $MpvDir 'ffmpeg.exe'; $fp = Join-Path $MpvDir 'ffprobe.exe'; $mpv = Join-Path $MpvDir 'mpv.com'; $eng = Join-Path $MpvDir 'pushmpc_engine.dll'
foreach ($f in $ff, $fp, $eng) { if (-not (Test-Path $f)) { throw "$f is missing (the release asset has ffmpeg.exe, ffprobe.exe and pushmpc_engine.dll next to mpv.exe)" } }
New-Item -ItemType Directory -Force $Out | Out-Null
$report = Join-Path $Out 'report.txt'; Set-Content $report "PushMPC ARM64 mpv tester, $(Get-Date -Format s)"
function R([string]$t) { Write-Host $t; Add-Content $report $t }
function Machine([string]$path) { $b = [IO.File]::ReadAllBytes($path); $pe = [BitConverter]::ToInt32($b, 0x3C); switch ([BitConverter]::ToUInt16($b, $pe + 4)) { 0xAA64 { 'ARM64' } 0x8664 { 'x64' } 0xA641 { 'ARM64EC' } default { 'other' } } }
function Frames([string]$f) { "$(& $fp -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 $f)".Trim() }

# ---- 1. the machine
R '== machine'
$os = Get-CimInstance Win32_OperatingSystem; $cpu = @(Get-CimInstance Win32_Processor)[0]
R "Windows $($os.Version) ($($os.Caption)); CPU $($cpu.Name) (WMI architecture $($cpu.Architecture): 12 = ARM64); PROCESSOR_ARCHITECTURE=$env:PROCESSOR_ARCHITECTURE"
foreach ($g in Get-CimInstance Win32_VideoController) { R "GPU: $($g.Name), driver $($g.DriverVersion) ($($g.DriverDate))" }
R "mpv: $MpvDir (mpv.exe $(Machine (Join-Path $MpvDir 'mpv.exe')), engine $(Machine $eng))"
R "$((& $mpv --version 2>&1 | Select-Object -First 1))"
R 'Vulkan devices mpv sees:'
(& $mpv --no-config --vo=gpu-next --gpu-api=vulkan --vulkan-device=help 2>&1 | Out-String).Trim() -split "`n" | ForEach-Object { R "  $_" }
$vfx = if ($NvidiaRuntime) { $NvidiaRuntime } else { Join-Path $env:LOCALAPPDATA 'PushMPC\runtime\nvidia-vfx' }
R "NVIDIA runtime folder: $vfx ($(if (Test-Path (Join-Path $vfx 'nvvfx\libs\NVVideoEffects.dll')) { 'present' } else { 'absent: the nvidia engine cannot run' }))"
if ($NvidiaRuntime) { $env:PUSHMPC_NVVFX_DIR = $NvidiaRuntime }

# ---- 2. each engine through ffmpeg
$clips = @(@{ n = '1080p'; s = '1920x1080' })
if (-not $Skip4k) { $clips += @{ n = '4k'; s = '3840x2160' } }
foreach ($c in $clips) {
    $c.f = Join-Path $Out "clip-$($c.n).mkv"
    if (-not (Test-Path $c.f)) { & $ff -v error -y -f lavfi -i "testsrc2=size=$($c.s):rate=24000/1001:duration=10" -pix_fmt yuv420p -c:v ffv1 $c.f; if ($LASTEXITCODE) { throw "could not make $($c.f)" } }
    $c.N = [int](Frames $c.f)
}
R ''; R '== engines (ffmpeg; output frames per second = speed; real time needs >= 47.95 at x2, 95.9 at x4)'
$runs = @(
    @{ id = 'auto'; vf = 'pushmpc' },
    @{ id = 'nvidia'; vf = 'pushmpc=backend=nvidia' },
    @{ id = 'portable-vulkan'; vf = 'pushmpc=backend=portable-vulkan' },
    @{ id = 'portable-cpu'; vf = 'pushmpc=backend=portable-cpu' },
    # G15: PushFlow through pmml on Snapdragon (Setup's pmml runtime: ONNX Runtime ARM64 + the QNN EP): the Hexagon NPU,
    # and the Adreno GPU through QNN; Auto takes ml only after Setup's measurement (pmml_cli auto) says it fits
    @{ id = 'ml-qnn-npu'; vf = 'pushmpc=backend=ml:extra=ml_backend\=qnn' },
    @{ id = 'ml-qnn-gpu'; vf = 'pushmpc=backend=ml:extra=ml_backend\=qnn-gpu' },
    @{ id = 'auto-x4'; vf = 'pushmpc=smoother=4' },
    @{ id = 'auto-d3d11'; vf = 'format=nv12,hwupload,pushmpc,hwdownload,format=nv12'; hw = $true }
)
foreach ($c in $clips) {
    foreach ($r in $runs) {
        $tag = "$($c.n)-$($r.id)"; $log = Join-Path $Out "ff-$tag.log"; $elog = Join-Path $Out "engine-$tag.log"; $o = Join-Path $Out "out-$tag.nut"
        Remove-Item $elog, $o -ErrorAction SilentlyContinue
        $env:PUSHMPC_ENGINE_LOG = $elog; $env:PUSHMPC_ENGINE = $eng
        $a = @('-v', 'info', '-y'); if ($r.hw) { $a += '-init_hw_device', 'd3d11va=dx', '-filter_hw_device', 'dx' }
        $a += '-i', $c.f, '-vf', $r.vf, '-c:v', 'rawvideo', '-f', 'nut', $o
        $sw = [Diagnostics.Stopwatch]::StartNew()
        & $ff @a > $log 2>&1; $rc = $LASTEXITCODE
        $sw.Stop()
        $n = if (Test-Path $o) { [int](Frames $o) } else { 0 }
        $line = (Select-String -Path $log -Pattern '\] (nvidia|portable-vulkan|portable-cpu|pushcore|ml) \(' | Select-Object -First 1).Line
        $pass = (Select-String -Path $log -Pattern 'passes through|did not open' | Select-Object -First 1).Line
        $k = if ($c.N) { [math]::Round($n / $c.N, 2) } else { 0 }
        R ("{0,-24} exit {1}, {2} of {3} frames (x{4}), {5:N1} output fps in {6:N1} s" -f $tag, $rc, $n, $c.N, $k, ($n / [math]::Max(0.001, $sw.Elapsed.TotalSeconds)), $sw.Elapsed.TotalSeconds)
        if ($line) { R "    $($line -replace '^.*\] ', '')" }
        if ($pass) { R "    $($pass -replace '^.*\] ', '')" }
        if (Test-Path $elog) { Get-Content $elog | Where-Object { $_ -match 'not used|open:|Auto:|close:' } | Select-Object -First 6 | ForEach-Object { R "    engine: $_" } }
        Remove-Item $o -ErrorAction SilentlyContinue
    }
}

# ---- 3. playback in a window
if (-not $SkipPlayback) {
    R ''; R '== playback (vo=gpu-next, display-resample, 15 s per clip, PushMPC Auto)'
    $lua = Join-Path $Out 'pm-stat.lua'
    Set-Content $lua @'
mp.add_timeout(15, function()
    local function p(n) local v = mp.get_property(n); return v or "?" end
    print(string.format("PMSTAT estimated-vf-fps=%s display-fps=%s frame-drop-count=%s vo-drop-frame-count=%s vsync-ratio=%s hwdec=%s", p("estimated-vf-fps"), p("display-fps"), p("frame-drop-count"), p("vo-drop-frame-count"), p("vsync-ratio"), p("hwdec-current")))
    mp.command("quit")
end)
'@
    foreach ($c in $clips) {
        foreach ($hw in 'd3d11va', 'no') {
            $tag = "$($c.n)-hwdec-$hw"; $elog = Join-Path $Out "engine-play-$tag.log"; $env:PUSHMPC_ENGINE_LOG = $elog
            $log = & $mpv --no-config --vo=gpu-next --hwdec=$hw --video-sync=display-resample --interpolation=no --loop-file=inf --vf=pushmpc "--script=$lua" --msg-level=all=info $c.f 2>&1 | Out-String
            Set-Content (Join-Path $Out "mpv-$tag.log") $log
            R "$tag`: $((($log -split "`n") | Where-Object { $_ -match 'PMSTAT' } | Select-Object -First 1) -replace '^.*PMSTAT ', '')"
            (($log -split "`n") | Where-Object { $_ -match 'pushmpc' } | Select-Object -First 2) | ForEach-Object { R "    $($_.Trim())" }
        }
    }
}
R ''; R "Done. Send back $report and the *.log files in $Out."
