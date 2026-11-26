<#
  Local playback checks for NVIDIA VFG above 8x, the renderer's share of the GPU budget and the HDR to SDR route, in
  MPC-HC with the installed PushMPC (or a development filter and core). Needs an NVIDIA RTX 40/50 card, MPC-HC with
  LAV Video and MPC Video Renderer, ffprobe and nvidia-smi on PATH. One run at a time; nothing else should use the GPU.

    test-vfg16-mpchc.ps1 -Hdr4k <4K HDR10 file> -Sdr1080 <1080p 23.976 fps file> -Low15 <15 fps file> -OutDir <dir>
                         [-Start4k <s>] [-Seconds 25] [-Only <tag,tag>] [-GpuLock <file>] [-FilterAx <dev .ax>] [-CoreFolder <dev core>]

  Hygiene (every run): the clip's duration from ffprobe, a start at or before duration - 20 s; one baseline per clip with
  PushMPC off that must play; only PushMPC between LAV Video and MPC Video Renderer (every other external filter in
  MPC-HC must be disabled, checked before each run); the GPU idle (under 10 %) before each run; a run that logs no
  "first sample", delivers no frames or ends early is INVALID and stops the batch. Every value the runs write (MPC-HC,
  PushMPC, MPC Video Renderer settings) is exported first and restored byte for byte at the end.

  The plan (tags; -Only picks some):
    hdr-*      a clean 4K HDR A/B at a fixed 10x Medium: the renderer's tone map (Player) against PushMPC's (PushMPC),
               clean-up Off and On separately, windowed (maximised) and fullscreen
    hdr-look-* the picture of both routes (Smoother Off, a screen grab 8 s in)
    auto-hdr-* Auto on the same file (expected: PushMPC tone-maps, 10x); the windowed one runs 80 s for the delivery cap
    m10-*      1080p 10x windowed (default size and maximised) and fullscreen; m10-auto-w: Auto windowed (10x or 5x)
    m12-*/m16-* the 15 fps file at 12x (180 fps) and 16x (240 fps), windowed and fullscreen
    high10     VFG High at 10x on the 15 fps file (150 fps)
#>
param(
    [Parameter(Mandatory)][string]$Hdr4k,
    [Parameter(Mandatory)][string]$Sdr1080,
    [Parameter(Mandatory)][string]$Low15,
    [Parameter(Mandatory)][string]$OutDir,
    [double]$Start4k = -1,
    [int]$Seconds = 25,
    [string]$Only = '',
    [string]$GpuLock = '',
    [string]$FilterAx = '',
    [string]$CoreFolder = '',
    [string]$MpcHc = "$env:ProgramFiles\MPC-HC\mpc-hc64.exe"
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$logs = "$env:LOCALAPPDATA\PushMPC\logs"
$ffprobe = (Get-Command ffprobe).Source
$filtersKey = 'HKCU:\Software\MPC-HC\MPC-HC\Filters\x64'
$pmKey = 'HKCU:\Software\PushMPC'; $dbgKey = 'HKCU:\Software\PushMPC\Filter'
$vrKey = 'HKCU:\Software\MPC-BE Filters\MPC Video Renderer'
$clsid = 'HKCU:\Software\Classes\CLSID\{798D4EC3-E59C-4A4E-A2EE-C804AEAE04CC}\InprocServer32'
Add-Type -Namespace V16 -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int c);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
'@
Add-Type -AssemblyName System.Drawing

function Get-Duration([string]$f) { [double](& $ffprobe -v error -show_entries format=duration -of csv=p=0 $f) }
function Get-Smi { [int]((nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits | Select-Object -First 1).Trim()) }
function Assert-Chain {
    $other = @(Get-ChildItem $filtersKey -ErrorAction SilentlyContinue | ForEach-Object { Get-ItemProperty $_.PSPath } |
               Where-Object { $_.Enabled -ne 0 -and $_.Name -ne 'PushMPC Filter' })
    if ($other.Count) { return "other external filters are enabled in MPC-HC: $(($other | ForEach-Object Name) -join ', ')" }
    return ''
}
function Wait-Idle { for ($i = 0; $i -lt 30; $i++) { $u = Get-Smi; if ($u -lt 10) { return $u }; Start-Sleep -Seconds 2 }; return Get-Smi }
function Pct($a, $q) { if (-not $a.Count) { return $null }; $s = $a | Sort-Object; $s[[math]::Min($s.Count - 1, [int][math]::Floor($q * ($s.Count - 1)))] }
function Test-Log([string]$log, [bool]$active) {
    if (-not $log -or -not (Test-Path $log)) { return 'no filter log' }
    $t = Get-Content $log
    $q = { param($l) [double](($l -split 'qpc ')[1] -split ' ')[0] }
    $seg = $t | Where-Object { $_ -match 'NewSegment start' } | Select-Object -First 1
    $eos = $t | Where-Object { $_ -match 'EndOfStream from upstream' } | Select-Object -First 1
    if ($seg -and $eos -and ((& $q $eos) - (& $q $seg)) -lt 5000) { return "the video ended $([int]((& $q $eos) - (& $q $seg))) ms after it started" }
    if ($active -and -not ($t | Where-Object { $_ -match 'first sample' })) { return 'no first sample' }
    $g = $t | Where-Object { $_ -match 'graph: PushMPC Filter' } | Select-Object -First 1
    if ($active -and $g -and ($g -notmatch 'in<-LAV Video Decoder' -or $g -notmatch 'out->MPC Video Renderer')) { return "not LAV -> PushMPC -> MPC-VR: $g" }
    return ''
}
function Invoke-Run([string]$tag, [string]$file, [double]$start, [bool]$fs, [hashtable]$set, [hashtable]$dbg, [bool]$max, [int]$grab, [int]$secs, [bool]$off) {
    foreach ($k in $set.Keys) { Set-ItemProperty $pmKey -Name $k -Value $set[$k] -Type String }
    if ($dbg.Count -and -not (Test-Path $dbgKey)) { New-Item $dbgKey | Out-Null }
    foreach ($k in $dbg.Keys) { Set-ItemProperty $dbgKey -Name $k -Value ([int]$dbg[$k]) -Type DWord }
    if ($off) { $env:PUSHMPC_FILTER_Enable = '0' }
    $smiFile = Join-Path $OutDir "$tag-smi.csv"
    $smi = Start-Process nvidia-smi -ArgumentList '--query-gpu=utilization.gpu,memory.used', '--format=csv,noheader,nounits', '-lms', '1000' -RedirectStandardOutput $smiFile -WindowStyle Hidden -PassThru
    $a = @("`"$file`"", '/play', '/start', [int]($start * 1000)); if ($fs) { $a += '/fullscreen' }
    $t0 = Get-Date
    $p = Start-Process $MpcHc -ArgumentList $a -PassThru
    try {
        if ($max) { Start-Sleep -Seconds 2; $p.Refresh(); [void][V16.U]::ShowWindow($p.MainWindowHandle, 3) }
        if ($grab -gt 0) {
            while (((Get-Date) - $t0).TotalSeconds -lt $grab) { Start-Sleep -Milliseconds 200 }
            $p.Refresh(); $h = $p.MainWindowHandle; [void][V16.U]::SetForegroundWindow($h); Start-Sleep -Milliseconds 300
            $r = New-Object V16.U+RECT; [void][V16.U]::GetWindowRect($h, [ref]$r)
            $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T); $g = [System.Drawing.Graphics]::FromImage($bmp)
            $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $bmp.Save((Join-Path $OutDir "$tag-grab.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        }
        while (((Get-Date) - $t0).TotalSeconds -lt $secs -and -not $p.HasExited) { Start-Sleep -Milliseconds 250 }
        $early = $p.HasExited
        if (-not $early) { $p.Refresh(); [void][V16.U]::PostMessage($p.MainWindowHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }
        $closed = $p.WaitForExit(30000)
    } finally {
        Stop-Process -Id $smi.Id -ErrorAction SilentlyContinue
        Remove-Item env:PUSHMPC_FILTER_Enable -ErrorAction SilentlyContinue
        foreach ($k in $dbg.Keys) { Remove-ItemProperty $dbgKey -Name $k -ErrorAction SilentlyContinue }
    }
    Start-Sleep -Milliseconds 300
    $r = [ordered]@{ tag = $tag; pid = $p.Id; endedEarly = $early; exit = $(if ($closed) { $p.ExitCode } else { 'HUNG' }) }
    $log = Get-ChildItem $logs -Filter "filter-*-$($p.Id).log" | Where-Object { $_.Name -notlike 'filter-exit*' } | Sort-Object LastWriteTime | Select-Object -Last 1
    $csv = Get-ChildItem $logs -Filter "filter-frames-*-$($p.Id).csv" | Sort-Object LastWriteTime | Select-Object -Last 1
    $gcsv = Get-ChildItem $logs -Filter "filter-gpu-*-$($p.Id).csv" | Sort-Object LastWriteTime | Select-Object -Last 1
    foreach ($f in @($log, $csv, $gcsv)) { if ($f) { Copy-Item $f.FullName $OutDir } }
    $r.log = if ($log) { Join-Path $OutDir $log.Name } else { '' }
    $u = @(Get-Content $smiFile | Where-Object { $_ -match '^\d+' }); if ($u.Count) {
        $r.gpuUtil = "$((@($u | ForEach-Object { [int](($_ -split ',')[0]) }) | Measure-Object -Minimum -Maximum | ForEach-Object { "$($_.Minimum)-$($_.Maximum)" }))"
        $r.vramMaxMB = (@($u | ForEach-Object { [int](($_ -split ',')[1]) }) | Measure-Object -Maximum).Maximum }
    if ($csv) {
        $rows = Import-Csv $csv.FullName
        $fg = $rows | Where-Object { $_.kind -eq '1' } | Select-Object -First 1
        $from = if ($fg) { [double]$fg.deliverQpcMs + 3000 } elseif ($rows.Count) { [double]$rows[0].deliverQpcMs + 3000 } else { 0 }
        $w = @($rows | Where-Object { [double]$_.deliverQpcMs -ge $from -and $_.kind -in '0', '1' })
        $del = @($w | Where-Object { $_.dropped -eq '0' })
        if ($del.Count -gt 2) {
            $span = ([double]$del[-1].deliverQpcMs - [double]$del[0].deliverQpcMs) / 1000
            $iv = for ($i = 1; $i -lt $del.Count; $i++) { [double]$del[$i].deliverQpcMs - [double]$del[$i - 1].deliverQpcMs }
            $m = ($iv | Measure-Object -Average).Average
            $r.fps = [math]::Round(($del.Count - 1) / $span, 2); $r.gen = @($del | Where-Object kind -eq '1').Count
            $r.dropGen = @($w | Where-Object { $_.dropped -eq '1' -and $_.kind -eq '1' }).Count; $r.dropReal = @($w | Where-Object { $_.dropped -eq '1' -and $_.kind -eq '0' }).Count
            $r.ivSd = [math]::Round([math]::Sqrt((($iv | ForEach-Object { ($_ - $m) * ($_ - $m) }) | Measure-Object -Average).Average), 3)
            $r.ivMax = [math]::Round(($iv | Measure-Object -Maximum).Maximum, 1)
        }
    }
    if ($gcsv) {
        $g = @(Import-Csv $gcsv.FullName | Where-Object { [double]$_.graphMs -gt 0 })
        if ($g.Count -gt 20) {
            $g = $g[[int]($g.Count * 0.15)..($g.Count - 1)]
            $r.graphP95 = Pct @($g | ForEach-Object { [double]$_.graphMs }) 0.95
            $r.fgBatchP95 = Pct @($g | ForEach-Object { [double]$_.fgBatchMs } | Where-Object { $_ -gt 0 }) 0.95
            $r.nrP95 = Pct @($g | ForEach-Object { [double]$_.nrMs } | Where-Object { $_ -gt 0 }) 0.95
        }
    }
    if ($log) {
        $r.auto = (Select-String -Path $log.FullName -Pattern 'Auto \((open|delivery|settings|display)' | Select-Object -First 4 | ForEach-Object { $_.Line }) -join ' || '
        $r.toneMap = (Select-String -Path $log.FullName -Pattern 'connect-time Auto: .*HDR to SDR|HDR to SDR in PushMPC' | Select-Object -First 2 | ForEach-Object { $_.Line }) -join ' || '
        $r.delivery = (Select-String -Path $log.FullName -Pattern 'delivery' | Select-Object -Last 3 | ForEach-Object { $_.Line }) -join ' || '
    }
    return [pscustomobject]$r
}

# --- the plan: tag, file, fullscreen, maximised, user settings, debug values, grab second, seconds
$sm = @{ Smoother = '10'; FgQuality = 'Medium' }
function Join-Settings([hashtable]$a, [hashtable]$b) { $h = @{}; foreach ($x in @($a, $b)) { foreach ($k in $x.Keys) { $h[$k] = $x[$k] } }; $h }
$plan = @(
    @('hdr-player-off-w', 'H', 0, 1, (Join-Settings $sm @{ HdrToSdr = 'Player'; CleanUp = 'Off' }), @{}, 0, $Seconds),
    @('hdr-pushmpc-off-w', 'H', 0, 1, (Join-Settings $sm @{ HdrToSdr = 'PushMPC'; CleanUp = 'Off' }), @{}, 0, $Seconds),
    @('hdr-player-off-f', 'H', 1, 0, (Join-Settings $sm @{ HdrToSdr = 'Player'; CleanUp = 'Off' }), @{}, 0, $Seconds),
    @('hdr-pushmpc-off-f', 'H', 1, 0, (Join-Settings $sm @{ HdrToSdr = 'PushMPC'; CleanUp = 'Off' }), @{}, 0, $Seconds),
    @('hdr-player-on-w', 'H', 0, 1, (Join-Settings $sm @{ HdrToSdr = 'Player'; CleanUp = 'On' }), @{}, 0, $Seconds),
    @('hdr-pushmpc-on-w', 'H', 0, 1, (Join-Settings $sm @{ HdrToSdr = 'PushMPC'; CleanUp = 'On' }), @{}, 0, $Seconds),
    @('hdr-player-on-f', 'H', 1, 0, (Join-Settings $sm @{ HdrToSdr = 'Player'; CleanUp = 'On' }), @{}, 0, $Seconds),
    @('hdr-pushmpc-on-f', 'H', 1, 0, (Join-Settings $sm @{ HdrToSdr = 'PushMPC'; CleanUp = 'On' }), @{}, 0, $Seconds),
    @('hdr-look-player', 'H', 1, 0, @{ Smoother = 'Off'; HdrToSdr = 'Player'; CleanUp = 'Off' }, @{}, 8, 12),
    @('hdr-look-pushmpc', 'H', 1, 0, @{ Smoother = 'Off'; HdrToSdr = 'PushMPC'; CleanUp = 'Off' }, @{}, 8, 12),
    @('auto-hdr-f', 'H', 1, 0, @{ Smoother = 'Auto'; HdrToSdr = 'Auto'; CleanUp = 'Auto'; FgQuality = 'Auto' }, @{}, 0, $Seconds),
    @('auto-hdr-w', 'H', 0, 1, @{ Smoother = 'Auto'; HdrToSdr = 'Auto'; CleanUp = 'Auto'; FgQuality = 'Auto' }, @{}, 0, 80),
    @('m10-w', 'S', 0, 0, (Join-Settings $sm @{ CleanUp = 'Off' }), @{}, 0, $Seconds),
    @('m10-wmax', 'S', 0, 1, (Join-Settings $sm @{ CleanUp = 'Off' }), @{}, 0, $Seconds),
    @('m10-f', 'S', 1, 0, (Join-Settings $sm @{ CleanUp = 'Off' }), @{}, 0, $Seconds),
    @('m10-auto-w', 'S', 0, 1, @{ Smoother = 'Auto'; CleanUp = 'Auto'; FgQuality = 'Auto' }, @{}, 0, $Seconds),
    @('m12-w', 'L', 0, 1, @{ Smoother = '12'; FgQuality = 'Medium'; CleanUp = 'Off' }, @{}, 0, $Seconds),
    @('m12-f', 'L', 1, 0, @{ Smoother = '12'; FgQuality = 'Medium'; CleanUp = 'Off' }, @{}, 0, $Seconds),
    @('m16-w', 'L', 0, 1, @{ Smoother = '16'; FgQuality = 'Medium'; CleanUp = 'Off' }, @{}, 0, $Seconds),
    @('m16-f', 'L', 1, 0, @{ Smoother = '16'; FgQuality = 'Medium'; CleanUp = 'Off' }, @{}, 0, $Seconds),
    @('high10', 'L', 1, 0, @{ Smoother = '10'; FgQuality = 'High'; CleanUp = 'Off' }, @{}, 0, $Seconds)
)
$want = @($Only -split ',' | Where-Object { $_ })
if ($want.Count) { $plan = @($plan | Where-Object { $want -contains $_[0] }) }
$files = @{ H = $Hdr4k; S = $Sdr1080; L = $Low15 }

# --- preflight: the lock, durations and starts
if ($GpuLock -and -not (Test-Path $GpuLock)) { throw "the GPU lock $GpuLock is not held: take it first" }
$starts = @{}
foreach ($k in $files.Keys) {
    if (-not (Test-Path $files[$k])) { throw "missing file: $($files[$k])" }
    $d = Get-Duration $files[$k]
    $need = ($plan | Where-Object { $_[1] -eq $k } | ForEach-Object { $_[7] } | Measure-Object -Maximum).Maximum
    $s = if ($k -eq 'H' -and $Start4k -ge 0) { $Start4k } else { 0 }
    if ($s -gt [math]::Max(0, $d - 20) -or ($need -and $s + $need -gt $d)) { throw "INVALID plan: $($files[$k]) is $([int]$d) s long; start $s s plus $need s does not fit (start <= duration - 20 s)" }
    $starts[$k] = $s
    "clip $k : $([math]::Round($d, 1)) s, start $s s"
}

# --- snapshot of everything the runs write, restored byte for byte at the end (the values of THIS moment)
$snap = Join-Path $OutDir ("snap-" + (Get-Date -Format 'yyyyMMdd-HHmmss')); New-Item -ItemType Directory -Force $snap | Out-Null
$keys = [ordered]@{ mpchc = 'HKCU\Software\MPC-HC'; pushmpc = 'HKCU\Software\PushMPC'; mpcvr = 'HKCU\Software\MPC-BE Filters'; clsid = 'HKCU\Software\Classes\CLSID\{798D4EC3-E59C-4A4E-A2EE-C804AEAE04CC}' }
foreach ($k in $keys.Keys) { & reg.exe export $keys[$k] (Join-Path $snap "$k.reg") /y 2>$null | Out-Null }
if ($FilterAx) { if (-not (Test-Path $clsid)) { New-Item $clsid -Force | Out-Null }; Set-ItemProperty $clsid -Name '(default)' -Value $FilterAx }
if ($CoreFolder) { $env:PUSHMPC_FILTER_CoreFolder = $CoreFolder; $env:PUSHMPC_FILTER_SkipManifestCheck = '1' }
$env:PUSHMPC_FILTER_FrameLog = '1'
$results = @(); $seen = @{}
try {
    foreach ($e in $plan) {
        $tag, $fk, $fs, $max, $set, $dbg, $grab, $secs = $e
        $file = $files[$fk]
        $why = Assert-Chain; if ($why) { "INVALID $tag : $why"; break }
        if (-not $seen[$fk]) {
            $idle = Wait-Idle
            $b = Invoke-Run "base-$fk" $file $starts[$fk] $false @{} @{} $false 0 $Seconds $true
            $why = Test-Log $b.log $false
            if (-not $why -and $b.endedEarly) { $why = 'the player closed during the baseline' }
            if ($why) { "INVALID base-$fk (PushMPC off): $why"; break }
            "baseline $fk ok (GPU $idle % before)"; $seen[$fk] = $true
        }
        $why = Assert-Chain; if ($why) { "INVALID $tag : $why"; break }
        $idle = Wait-Idle
        $res = Invoke-Run $tag $file $starts[$fk] ([bool]$fs) $set $dbg ([bool]$max) $grab $secs $false
        $res | Add-Member gpuBefore $idle
        $why = Test-Log $res.log $true
        if (-not $why -and -not $res.fps) { $why = 'no frames delivered' }
        if (-not $why -and $res.endedEarly) { $why = 'the player closed early' }
        $res | Add-Member valid ($why -eq '')
        $res | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $OutDir "$tag.json")
        $results += $res
        $res | Select-Object tag, gpuBefore, fps, gen, dropGen, dropReal, ivSd, ivMax, graphP95, fgBatchP95, nrP95, gpuUtil, vramMaxMB, exit | Format-List | Out-String -Width 300
        if ($why) { "INVALID $tag : $why"; break }
        "VALID $tag"
        Start-Sleep -Seconds 2
    }
} finally {
    Remove-Item env:PUSHMPC_FILTER_CoreFolder, env:PUSHMPC_FILTER_SkipManifestCheck, env:PUSHMPC_FILTER_FrameLog -ErrorAction SilentlyContinue
    foreach ($k in $keys.Keys) {
        $s = Join-Path $snap "$k.reg"; $n = Join-Path $snap "$k-end.reg"
        & reg.exe export $keys[$k] $n /y 2>$null | Out-Null
        if (-not (Test-Path $s)) { if (Test-Path $n) { & reg.exe delete $keys[$k] /f | Out-Null; "$k removed (absent before)" }; continue }
        if ((Get-FileHash $n).Hash -eq (Get-FileHash $s).Hash) { "$k unchanged"; continue }
        & reg.exe delete $keys[$k] /f | Out-Null; $null = cmd /c "reg import `"$s`" 2>nul"
        & reg.exe export $keys[$k] $n /y | Out-Null
        "$k restored: $(if ((Get-FileHash $n).Hash -eq (Get-FileHash $s).Hash) { 'byte-identical' } else { 'DIFFERS' })"
    }
    $results | Select-Object tag, valid, fps, dropGen, graphP95, fgBatchP95 | Format-Table -AutoSize | Out-String -Width 200
}
