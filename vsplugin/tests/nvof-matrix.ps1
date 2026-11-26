# The motion-source test matrix for a real NVIDIA GPU (VK_NV_optical_flow needs one: no hosted runner has it). Decides by
# evidence whether NVIDIA's optical flow (as the field, or as a block-matcher candidate: "hybrid") should be the default
# on RTX cards. It downloads public test clips (about 1.5 GB) into -Scratch, so run it only on a machine where that is
# fine; nothing is installed and nothing outside -Scratch is written.
#   nvof-matrix.ps1 -Bench <gpu_bench.exe> -Scratch <folder> [-Frames 40] [-NoDownload]
# Needs: gpu_bench.exe and gpu_test.exe from the vsplugin-gpu workflow's gpu-windows artifact; ffmpeg on PATH for Big
# Buck Bunny and VMAF (optional: without it those parts are skipped).
# Matrix (gpu_bench's methods): blocks q1 / q2 (with and without the motion-estimation fixes), NVOF medium (default:
# dense field, cost rejection, half-size input above 1440 lines) and its slow / fast / full-input / half-input /
# averaged / no-cost variants, hybrid; held-out distance 2 (t = 1/2) and 4 (t = 1/4, 1/2, 3/4); clips at 720p50 and
# 1080p50 (Xiph derf, the first 64 frames by HTTP range), Big Buck Bunny 60 fps 1080p, the synthetic zoom at 2160p;
# metrics PSNR-Y / SSIM-Y (and VMAF with ffmpeg); the pipelined throughput at x4 at 1080p and 2160p; nvidia-smi
# before and after (other programs loading the GPU skew timings).
param([Parameter(Mandatory)][string]$Bench, [Parameter(Mandatory)][string]$Scratch, [int]$Frames = 40, [switch]$NoDownload)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Scratch | Out-Null
$clips = Join-Path $Scratch 'clips'; $out = Join-Path $Scratch 'results'
New-Item -ItemType Directory -Force $clips, $out, (Join-Path $out 'y4m') | Out-Null
$csv = Join-Path $out 'bench.csv'; $md = Join-Path $out 'results.md'
$haveFfmpeg = [bool](Get-Command ffmpeg -ErrorAction SilentlyContinue)
# VMAF needs an ffmpeg built with libvmaf (many builds lack it): without it the VMAF column is skipped
$haveVmaf = $haveFfmpeg -and [bool]((ffmpeg -hide_banner -filters 2>&1 | Out-String) -match '\blibvmaf\b')
if ($haveFfmpeg -and -not $haveVmaf) { Write-Host 'ffmpeg has no libvmaf: no VMAF column (PSNR-Y / SSIM-Y only)' }

function Get-Range($url, $file, $bytes) {
    if (Test-Path $file) { return }
    Write-Host "fetch $url ($([math]::Round($bytes / 1MB)) MB)"
    # a clip the server does not have (crowd_run exists at 1080p50 only) is skipped, not the run
    try { Invoke-WebRequest -Uri $url -OutFile $file -Headers @{ Range = "bytes=0-$bytes" } }
    catch { Write-Warning "skipped $url : $($_.Exception.Message)"; Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue }
}
if (-not $NoDownload) {
    foreach ($n in 'park_joy', 'ducks_take_off', 'in_to_tree', 'old_town_cross', 'crowd_run') {
        Get-Range "https://media.xiph.org/video/derf/y4m/${n}_420_720p50.y4m" (Join-Path $clips "$n-720.y4m") (64 * 1382406 + 512)
        Get-Range "https://media.xiph.org/video/derf/y4m/${n}_1080p50.y4m" (Join-Path $clips "$n-1080.y4m") (64 * 3110406 + 512)
    }
    $bbb = Join-Path $clips 'bbb60-1080.y4m'
    if ($haveFfmpeg -and -not (Test-Path $bbb)) {
        ffmpeg -hide_banner -loglevel error -ss 100 -i 'http://distribution.bbb3d.renderfarming.net/video/mp4/bbb_sunflower_1080p_60fps_normal.mp4' -frames:v 72 -pix_fmt yuv420p -an -f yuv4mpegpipe $bbb
    }
}

"# Motion-source matrix ($(Get-Date -Format s))" | Set-Content $md
'```' | Add-Content $md
(nvidia-smi --query-gpu=name,driver_version,utilization.gpu,memory.used,clocks.gr --format=csv) | Add-Content $md
'```' | Add-Content $md
'| clip | test | method | PSNR-Y | SSIM-Y | ms | note |' | Add-Content $md
'|---|---|---|---|---|---|---|' | Add-Content $md

$y4ms = Get-ChildItem $clips -Filter *.y4m | Sort-Object Name
foreach ($c in $y4ms) {
    foreach ($stride in 2, 4) {
        & $Bench $c.FullName --clip $c.BaseName --test interp --stride $stride --frames $Frames --csv $csv --y4m-out (Join-Path $out 'y4m') | Where-Object { $_ -like '|*' } | Add-Content $md
    }
}
& $Bench synthetic2160 --clip synthetic-zoom-2160 --test interp --frames 24 --csv $csv | Where-Object { $_ -like '|*' } | Add-Content $md
foreach ($s in 'synthetic1080', 'synthetic2160') {
    & $Bench $s --clip "$s-throughput" --test throughput --frames 48 --csv $csv | Where-Object { $_ -like '|*' } | Add-Content $md
}
$gpuTest = Join-Path (Split-Path $Bench) 'gpu_test.exe'
if (Test-Path $gpuTest) { '```' | Add-Content $md; & $gpuTest --require | Where-Object { $_ -match 'timing|of which|optical|passed' } | Add-Content $md; '```' | Add-Content $md }

if ($haveVmaf) {
    '' | Add-Content $md; '| clip | method | VMAF |' | Add-Content $md; '|---|---|---|' | Add-Content $md
    foreach ($r in Get-ChildItem (Join-Path $out 'y4m') -Filter '*_ref.y4m') {
        $n = $r.Name -replace '_ref\.y4m$', ''
        foreach ($d in Get-ChildItem (Join-Path $out 'y4m') -Filter "${n}_*.y4m" | Where-Object { $_.Name -ne $r.Name }) {
            $m = $d.BaseName.Substring($n.Length + 1)
            $hit = ffmpeg -hide_banner -i $d.FullName -i $r.FullName -lavfi libvmaf -f null - 2>&1 | Select-String 'VMAF score: ([0-9.]+)' | Select-Object -First 1
            $v = if ($hit) { $hit.Matches[0].Groups[1].Value } else { 'n/a' }
            "| $n | $m | $v |" | Add-Content $md
        }
    }
}
'```' | Add-Content $md
(nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader) | Add-Content $md
'```' | Add-Content $md
Write-Host "results: $md"
