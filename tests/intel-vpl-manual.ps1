<#
.SYNOPSIS
  Intel Arc / Xe tester for PushMPC's VplFi (Intel VPL AI frame interpolation) and VplSr (Intel VPL AI super resolution).

.DESCRIPTION
  Runs vpl_ai_probe.exe (from the vpl-probe CI artifact) on a synthetic 60 fps test it generates with FFmpeg, then prints
  PASS/FAIL per feature with the numbers and the recommended default. Nothing is installed and no registry or PushMPC
  setting is touched; the only output is the folder given by -Out. Send back the printed report and
  <Out>\vpl_ai_probe.json.

  Needs: an FFmpeg build with libx264 on PATH (or -FFmpeg), about 2 GB free disk for the 4K test clip, 5-15 minutes.
  The 4K AI super resolution step comes last because one public report saw a GPU hang there; -SkipSr4k leaves it out.

  Gate for VplFi default ON (Arc / Xe only):
    1. quality: the generated frames' mean PSNR-Y against the dropped real frames is >= PushFlow Fast's on the same
       held-out test (reference numbers below, measured with the same metric code on an RTX 5090). If this machine's
       FFmpeg produced different test material (the 50/50 blend baseline differs by > 0.3 dB from the reference run),
       the comparison uses the gain over the blend baseline instead.
    2. cost: median ms per source frame (one real + one generated output) <= 20.8 ms, half of a 24p frame interval,
       leaving the other half for decode, upscaling and presentation. Checked at 1080p (where an integration would run
       FI on decoded video, before upscaling) and reported at 4K.
  VplSr is probe-only and never defaults on: the VPP is NV12 / 8-bit only (no HDR10 path), Intel ships it as a
  generic/conference-tuned model with experimental mode and algorithm switches, a public report shows a GPU hang at 4K
  output, and PushMPC already has upscalers measured on film. Its numbers are recorded for a later decision.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File intel-vpl-manual.ps1
  powershell -ExecutionPolicy Bypass -File intel-vpl-manual.ps1 -Probe .\vpl_ai_probe.exe -FFmpeg C:\tools\ffmpeg.exe
#>
param(
    [string]$Probe = '',
    [string]$FFmpeg = 'ffmpeg',
    [string]$Out = (Join-Path $env:TEMP 'pushmpc-vpl-probe'),
    [switch]$SkipSr4k,
    [int]$TimeoutSeconds = 1800
)
$ErrorActionPreference = 'Stop'

# Reference: PushFlow Fast (RIFE 4.25 lite, --backend pushflow --q 0) and VFG Medium through pushcore_gpu_harness,
# scored with vpl_ai_probe --score-harness; same synthetic material (FFmpeg 9.1), k = 4..89 (86 generated frames).
$Ref = @{
    '1080' = @{ PushFlowFast = 28.90; PushFlowFastSsim = 0.9331; VfgMedium = 30.56; Blend = 23.43; Repeat = 21.28 }
    '2160' = @{ PushFlowFast = 27.75; PushFlowFastSsim = 0.9188; VfgMedium = $null; Blend = 23.26; Repeat = 21.17 }
}
$BudgetMs = 20.8

if (-not $Probe) {
    foreach ($c in @((Join-Path $PSScriptRoot 'vpl_ai_probe.exe'), (Join-Path $PSScriptRoot '..\build-probe\Release\vpl_ai_probe.exe'), '.\vpl_ai_probe.exe')) {
        if (Test-Path $c) { $Probe = (Resolve-Path $c).Path; break }
    }
}
if (-not $Probe -or -not (Test-Path $Probe)) { Write-Host 'vpl_ai_probe.exe not found: pass -Probe <path> (it is in the vpl-ai-probe CI artifact)'; exit 2 }
if (-not (Get-Command $FFmpeg -ErrorAction SilentlyContinue)) { Write-Host "FFmpeg not found ($FFmpeg): pass -FFmpeg <path to ffmpeg.exe>"; exit 2 }
$ff = (Get-Command $FFmpeg).Source

New-Item -ItemType Directory -Force $Out | Out-Null
$probeArgs = @('--out', $Out, '--ffmpeg', $ff, '--sizes', '1080,2160', '--seconds', '3', '--timeout', "$TimeoutSeconds")
if ($SkipSr4k) { $probeArgs += @('--sr4k', '0') }
Write-Host "running $Probe $($probeArgs -join ' ')"
& $Probe @args
$code = $LASTEXITCODE
$jf = Join-Path $Out 'vpl_ai_probe.json'
if (-not (Test-Path $jf)) { Write-Host "the probe wrote no report (exit $code)"; exit 1 }
$j = Get-Content $jf -Raw | ConvertFrom-Json

$rep = New-Object System.Collections.Generic.List[string]
function Say([string]$s) { $rep.Add($s); Write-Host $s }
function F([object]$x) { if ($null -eq $x) { 'n/a' } else { '{0:N2}' -f [double]$x } }

Say ''
Say '==== PushMPC Intel VPL probe report ===='
Say "probe exit $code, status $($j.status)$(if ($j.timeoutStage) { ", TIMEOUT in '$($j.timeoutStage)' (a VPL call hung)" })"
Say "probe built with VPL API $($j.headerApiVersion); $($j.ffmpeg)"
foreach ($a in @($j.adapters)) { Say "adapter: $($a.name) $($a.vendorId)/$($a.deviceId) driver $($a.driver)" }
if (@($j.implementations).Count -eq 0) { Say 'VPL implementations: none' }
foreach ($i in @($j.implementations)) {
    Say "VPL impl #$($i.index): $($i.impl) $($i.implName) API $($i.apiVersion) $($i.mediaAdapterType) $($i.adapter) VAFI=$($i.advertisesVAFI) VASR=$($i.advertisesVASR)"
}

# ---- VplFi
$fiRows = @()
foreach ($t in @($j.tests)) {
    foreach ($c in @($t.clips)) {
        $size = if ($c.clip -match '2160') { '2160' } else { '1080' }
        $mat = @($j.material) | Where-Object { $_.label -eq $c.clip } | Select-Object -First 1
        if ($c.fiQuery) { Say "FI query $($t.implementation) $($c.clip): $(($c.fiQuery.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' ')" }
        if (-not $c.fiRuns) { continue }
        foreach ($p in $c.fiRuns.PSObject.Properties) {
            $r = $p.Value
            if ($r -is [string] -or -not $r.generated -or -not $r.generated.n) { Say "FI $($c.clip) $($p.Name): no frames ($(if ($r -is [string]) { $r } else { "$($r.init) $($r.error)" }))"; continue }
            $blend = $mat.fiBaselines.blend.psnrY
            $ref = $Ref[$size]
            $sameMaterial = [math]::Abs([double]$blend - $ref.Blend) -le 0.3
            $psnr = [double]$r.generated.psnrY
            $qualOk = if ($sameMaterial) { $psnr -ge $ref.PushFlowFast } else { ($psnr - [double]$blend) -ge ($ref.PushFlowFast - $ref.Blend) }
            $ms = [double]$r.msPerSourceFrame.median
            $costOk = $ms -le $BudgetMs
            $fiRows += [pscustomobject]@{ Impl = $t.implementation; Size = $size; Mode = $p.Name; Psnr = $psnr; Ssim = $r.generated.ssimY; Ms = $ms; QualOk = $qualOk; CostOk = $costOk; Same = $sameMaterial; Blend = $blend; Memory = $r.memory; Err = $r.error }
            Say ("FI {0} {1} [{2}, {3}]: PSNR-Y {4} dB (p10 {5}), SSIM {6}; blend {7} dB; PushFlow Fast ref {8} dB{9}; ms/source frame {10} (p95 {11}), ms/generated {12}; passthrough {13} dB{14}" -f `
                $c.clip, $p.Name, $r.memory, $t.implementation, (F $psnr), (F $r.generated.psnrYp10), ('{0:N4}' -f [double]$r.generated.ssimY), (F $blend), (F $ref.PushFlowFast),
                $(if ($sameMaterial) { '' } else { ' (material differs: compared by gain over blend)' }), (F $ms), (F $r.msPerSourceFrame.p95), (F $r.msGeneratedCall.median),
                (F $r.passthroughCalibration.psnrY), $(if ($r.error) { "; ERROR $($r.error)" }))
        }
    }
}
Say ''
$best1080 = $fiRows | Where-Object { $_.Size -eq '1080' -and $_.QualOk -and $_.CostOk } | Sort-Object Psnr -Descending | Select-Object -First 1
$best2160 = $fiRows | Where-Object { $_.Size -eq '2160' -and $_.QualOk -and $_.CostOk } | Sort-Object Psnr -Descending | Select-Object -First 1
if ($fiRows.Count -eq 0) {
    Say "VplFi: FAIL ($($j.VplFi))"
    Say 'VplFi recommended default: OFF (not available on this system)'
} elseif ($best1080) {
    Say ("VplFi: PASS at 1080p (mode {0}: {1} dB >= PushFlow Fast, {2} ms/source frame <= {3} ms){4}" -f $best1080.Mode, (F $best1080.Psnr), (F $best1080.Ms), $BudgetMs,
        $(if ($best2160) { "; 4K also PASS (mode $($best2160.Mode))" } else { '; 4K FAIL (quality or cost), so FI runs before upscaling' }))
    Say "VplFi recommended default: ON for this GPU (mode $($best1080.Mode), FI on the decoded video before upscaling)"
} else {
    $f = $fiRows | Where-Object { $_.Size -eq '1080' } | Sort-Object Psnr -Descending | Select-Object -First 1
    $why = if (-not $f) { 'no 1080p run' } elseif (-not $f.QualOk) { "quality below PushFlow Fast ($(F $f.Psnr) dB)" } else { "too slow ($(F $f.Ms) ms > $BudgetMs ms)" }
    Say "VplFi: FAIL at 1080p: $why"
    Say 'VplFi recommended default: OFF (keep PushFlow; VplFi stays available as an opt-in)'
}

# ---- VplSr
Say ''
$srAny = $false
foreach ($t in @($j.tests)) {
    foreach ($c in @($t.clips)) {
        $mat = @($j.material) | Where-Object { $_.label -eq $c.clip } | Select-Object -First 1
        if ($c.srQuery) { Say "SR query $($t.implementation) $($c.clip): video=$($c.srQuery.video_default) system=$($c.srQuery.system_default)" }
        if (-not $c.srRun) { continue }
        $r = $c.srRun
        if ($r.score.n) { $srAny = $true }
        Say ("SR 2x -> {0} [{1}]: PSNR-Y {2} dB SSIM {3}; bilinear {4}, Lanczos {5} dB; ms {6} (p95 {7}){8}" -f $c.clip, $r.memory, (F $r.score.psnrY), $(if ($r.score.ssimY) { '{0:N4}' -f [double]$r.score.ssimY } else { 'n/a' }),
            (F $mat.srBaselines.bilinear.psnrY), (F $mat.srBaselines.lanczos.psnrY), (F $r.ms.median), (F $r.ms.p95), $(if ($r.error) { "; ERROR $($r.error)" }))
    }
}
Say "VplSr: $(if ($srAny) { 'PASS (runs; numbers above)' } else { "FAIL ($($j.VplSr))" })"
Say 'VplSr recommended default: OFF (probe only: NV12/8-bit only, experimental controls, a public 4K-output GPU hang report)'
Say ''
Say "full report: $jf"
$rep | Out-File -Encoding utf8 (Join-Path $Out 'tester-report.txt')
Say "this summary: $(Join-Path $Out 'tester-report.txt')"
exit 0
