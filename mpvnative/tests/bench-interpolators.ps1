<#
  bench-interpolators.ps1: a tester script (run by a person on their own NVIDIA RTX PC, never by CI) that compares
  the interpolators the mpv Auto can pick, on the same clip and the same multiplier:
    fruc     FFmpeg's fruc_vulkan (NVIDIA optical flow), hwdec=vulkan, gpu-api=vulkan
    native   vf_pushmpc (pushcore on D3D11, zero copy), hwdec=d3d11va, gpu-api=d3d11
    vs       the VapourSynth route (vf=vapoursynth with pushmpc.vpy), when -VpyScript is given
  Each runs -Seconds of real playback (window, display-resample); the script reads mpv's own counters (dropped,
  mistimed, delayed frames, the filter's output rate) and samples GPU load with nvidia-smi. Nothing is installed or
  changed: mpv runs with --no-config and every option on the command line. Look at the picture too: the numbers say
  whether it keeps up, not how it looks.

  bench-interpolators.ps1 -Mpv <PushMPC mpv.exe> -Clip <video> [-Multiplier 2] [-Seconds 30] [-VpyScript <pushmpc.vpy>] [-VsMpv <mpv.exe>]
#>
param(
    [Parameter(Mandatory)][string]$Mpv,
    [Parameter(Mandatory)][string]$Clip,
    [int]$Multiplier = 2,
    [int]$Seconds = 30,
    [string]$VpyScript = '',
    [string]$VsMpv = ''
)
$ErrorActionPreference = 'Stop'
$work = Join-Path ([IO.Path]::GetTempPath()) "pushmpc-bench-$PID"
New-Item -ItemType Directory $work -Force | Out-Null
$lua = Join-Path $work 'report.lua'
@"
local t0 = nil
mp.register_event('playback-restart', function() if not t0 then t0 = mp.get_time() end end)
mp.add_periodic_timer(1, function()
  if t0 and mp.get_time() - t0 >= $Seconds then
    local p = function(n) return tostring(mp.get_property(n, '?')) end
    mp.msg.warn('BENCH estimated-vf-fps=' .. p('estimated-vf-fps') .. ' display-fps=' .. p('display-fps') ..
      ' frame-drop-count=' .. p('frame-drop-count') .. ' decoder-frame-drop-count=' .. p('decoder-frame-drop-count') ..
      ' mistimed-frame-count=' .. p('mistimed-frame-count') .. ' vo-delayed-frame-count=' .. p('vo-delayed-frame-count') ..
      ' vsync-jitter=' .. p('vsync-jitter') .. ' hwdec-current=' .. p('hwdec-current'))
    mp.commandv('quit')
  end
end)
"@ | Set-Content $lua -Encoding ascii

$common = @('--no-config', '--fs', '--video-sync=display-resample', '--interpolation=no', '--hr-seek-framedrop=no',
            '--loop-file=inf', '--msg-level=all=warn', "--script=$lua")
$runs = [ordered]@{
    fruc   = @{ exe = $Mpv; args = @('--hwdec=vulkan', '--vo=gpu-next', '--gpu-api=vulkan', "--vf=fruc_vulkan=fps=source_fps*$Multiplier") }
    native = @{ exe = $Mpv; args = @('--hwdec=d3d11va', '--vo=gpu-next', '--gpu-api=d3d11', "--vf=pushmpc=smoother=$Multiplier") }
}
if ($VpyScript) {
    $exe = if ($VsMpv) { $VsMpv } else { $Mpv }
    $runs['vs'] = @{ exe = $exe; args = @('--hwdec=auto-copy', '--vo=gpu-next', "--vf=vapoursynth=file=[$VpyScript]:buffered-frames=8:concurrent-frames=2:user-data=[smoother=$Multiplier]") }
}
$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
$results = @()
foreach ($name in $runs.Keys) {
    $r = $runs[$name]
    $log = Join-Path $work "$name.log"
    Write-Host "== $name ($Seconds s) ..."
    $p = Start-Process -FilePath $r.exe -ArgumentList ($common + $r.args + @("--log-file=$log", $Clip)) -PassThru
    $gpu = @()
    while (-not $p.HasExited) {
        if ($smi) { $gpu += [double](& nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits | Select-Object -First 1) }
        Start-Sleep -Seconds 1
    }
    $line = (Select-String -Path $log -Pattern 'BENCH (.*)' | Select-Object -Last 1)
    $row = [ordered]@{ route = $name }
    if ($line) { foreach ($kv in $line.Matches[0].Groups[1].Value -split ' ') { $k, $v = $kv -split '=', 2; $row[$k] = $v } }
    else { $row['error'] = (Get-Content $log -Tail 5) -join ' | ' }
    if ($gpu.Count) { $row['gpu-util-avg'] = [math]::Round(($gpu | Measure-Object -Average).Average, 1) }
    $results += [pscustomobject]$row
}
$results | Format-Table -AutoSize | Out-String -Width 400 | Write-Host
$out = Join-Path $work 'results.json'
$results | ConvertTo-Json | Set-Content $out
Write-Host "results: $out (logs in $work)"
