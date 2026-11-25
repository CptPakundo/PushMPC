<#
  PushMPC installer: per user, no administrator rights, no PushFPS installation needed.

  Usage:
    pushmpc-install.ps1 install   [-StageDir <dir>] [-InstallDir <dir>] [-PackageCache <dir> | -RuntimePackage <zip>]
                                  [-ExperimentalExtras Ask|Yes|No] [-DryRun] [-NoMpcHc]
    pushmpc-install.ps1 update    -ReleaseManifest <release.json> [-PackageCache <dir>] [-AllowUnsigned] [-DryRun]
    pushmpc-install.ps1 apply-pending          (finish an update that waited for MPC-HC to close)
    pushmpc-install.ps1 rollback  -Component filter|core|runtime
    pushmpc-install.ps1 uninstall [-KeepSettings]
    pushmpc-install.ps1 status    [-Verify]
  Used by PushMPC-Setup.exe and PushMpcUpdater.exe (-Machine: "@@kind|text" lines instead of coloured text):
    pushmpc-install.ps1 plan            [-ReleaseManifest <json>]   what install/update needs from the network (no change)
    pushmpc-install.ps1 prepare-runtime -ReleaseManifest <json> [-PackageCache <dir>]   stage the release's runtime
    pushmpc-install.ps1 autoplan [-Apply]  Auto's engine choice for this PC per player route (-Apply: write auto-plan.json
                                        and pmml.txt as install does, e.g. after a GPU change)
  What install does on its own, with no questions: every player it finds gets PushMPC's native route (MPC-HC, MPC-BE,
    PotPlayer; the mpv family through mpv-integration.ps1, Stremio and Plex included; VLC when the build has the VLC
    module), PushFlow's runtime for this GPU goes into <InstallDir>\pmml (pmml-fetch.ps1 by ml-runtime-pin.json:
    TensorRT-RTX on RTX, OpenVINO on Intel; -Pmml No declines it), Auto's engine choice is logged and saved as
    auto-plan.json, PushFlow's pmml.txt is written from it, AMD
    FSR SDK 2.4 is fetched on Radeon RX 6000 / 7000 / 9000 (-FidelityFx Package declines it), and capture mode
    (pushmpc-capture.ps1, PushFPS's window capture) is switched on only for players without a native route
    (-Capture Auto|On|Off). -NoVlc skips VLC. Tests: PUSHMPC_TEST_GPU="<vendor>:<device>:<name>" stands in for the GPU.
  Test switches: -NoMpcHc (no MPC-HC, MPC-VR or NVIDIA profile change), -NoRegister (no regsvr32), -NoTask (no logon task),
    -NoMpcBe (no MPC-BE change); MPC-BE steps alone against a fake root:
    pushmpc-install.ps1 mpcbe-apply|mpcbe-remove -InstallDir <scratch> -MpcBeRegRoot <HKCU:\...> -MpcBeExe <fake mpc-be64.exe>
                        -MpcBeBackupKey <HKCU:\...> [-DryRun]
    -NoPotPlayer (no PotPlayer change); PotPlayer steps alone against a fake root:
    pushmpc-install.ps1 potplayer-apply|potplayer-remove -InstallDir <scratch> -PotRegRoot <HKCU:\...> -PotPlayerExe <fake PotPlayerMini64.exe> [-DryRun]
    -NoMpv (no mpv / SMPlayer change); the mpv route alone against a fake root (mpv-integration.ps1 -Root):
    pushmpc-install.ps1 mpv-apply|mpv-remove -StageDir <stage> -InstallDir <root>\AppData\Local\PushMPC -MpvRoot <root> [-DryRun]
  mpv route (preview): the optional "vsplugin" component (pushmpc_vs.dll, pushmpc.vpy,
    mpv-integration.ps1, vapoursynth-pin.json) is staged only when pushmpc_vs.dll is built. Install adds it, fetches the
    VapourSynth runtime pinned in vapoursynth-pin.json (skipped while the pins say TODO) and runs
    mpv-integration.ps1 -Action Install -RuntimeOnPath, only when mpv or SMPlayer is found; uninstall reverses it.
  For building releases (from a source checkout):
    pushmpc-install.ps1 stage     [-StageDir <dir>] [-WithRuntime -PackageCache <dir> | -RuntimePackage <zip>] [-RequireNvof]
                                  (-RequireNvof: fail when pushcore.dll is built without NVIDIA optical flow; release.yml)
    pushmpc-install.ps1 package   [-StageDir <dir>] [-ReleaseDir <dir>]

  Release model: PushMPC never ships NVIDIA's files. A PushMPC release holds the filter, the core and this installer.
  The runtime (FFmpeg and the NVIDIA components pushcore loads) comes from Veyra's official runtime package, which
  this installer downloads from Veyra's own GitHub release at install time, checks against the SHA-256 pinned for
  this release, and from which it extracts only the files PushMPC uses. -PackageCache <dir> (a folder that already
  holds that zip, e.g. from an earlier download) or -RuntimePackage <zip> skips the download; the SHA-256 is checked
  either way. A release manifest refers to the runtime as that Veyra package (url + sha256), never as files of ours.

  Layout (%LOCALAPPDATA%\PushMPC), one versioned component per folder, each with manifest.txt:
    filter\    PushMpcFilter.ax + PushMpcExit.dll           (registered per user from this path)
    core\      pushcore.dll + shaders\*.dxil                (built from the PushFPS source named in its manifest)
    runtime\   the subset of Veyra's official runtime package that pushcore loads (FFmpeg, NVIDIA components)
    previous\<component>\   the version before the last swap (rollback)
    staging\<component>-<version>\   an update waiting for MPC-HC to close (pending.json)
    state.json  what this installer changed outside its folder (MPC-HC / MPC-BE entries, a VapourSynth entry it disabled)
  manifest.txt: key=value lines, then "file <sha256> <size> <relative path>" per file.

  The runtime comes from Veyra's official release package, as PushFPS's own installer gets it: pinned SHA-256,
  verified before use; NVIDIA files that are not NVIDIA-signed originals (community-modified, e.g. the RTX 30 AI
  enhancement slot) are "experimental extras", left out unless chosen (-ExperimentalExtras Yes, or yes when asked).

  Works with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('stage', 'package', 'install', 'update', 'apply-pending', 'rollback', 'uninstall', 'status', 'plan', 'prepare-runtime', 'mpcbe-apply', 'mpcbe-remove', 'potplayer-apply', 'potplayer-remove', 'mpv-apply', 'mpv-remove', 'autoplan')]
    [string]$Action,
    [string]$StageDir = '',
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'PushMPC'),
    [string]$RuntimePackage = '',
    # a folder that may already hold Veyra's package zip (checked by SHA-256 before use); no default
    [string]$PackageCache = '',
    # stage: also stage the runtime subset (developer builds; a release never contains it)
    [switch]$WithRuntime,
    [string]$ReleaseDir = '',
    [ValidateSet('Ask', 'Yes', 'No')]
    [string]$ExperimentalExtras = 'Yes',   # unlocks and community ports are on by default (No leaves them out)
    [ValidateSet('Auto', 'NVIDIA', 'AMD')]
    [string]$Gpu = 'Auto',
    # where the seven NVIDIA Video Effects files (runtime\nvidia-vfg) come from: Package = Veyra's runtime package (default;
    # byte-identical to NVIDIA's wheel, no extra download), Wheel = NVIDIA's PyPI wheel (scripts\nvidia-vfx-fetch.ps1,
    # pinned). The package's own copy missing: the wheel is used either way.
    [ValidateSet('Package', 'Wheel')]
    [string]$VfxSource = 'Package',
    # AMD's FidelityFX DLLs: Package = the runtime package's SDK 2.3.0 set, Sdk240 = AMD FSR SDK 2.4.0 (upscaler 4.1.2: FSR 4.1 on
    # RX 6000 too; scripts\fidelityfx-fetch.ps1, pinned, AMD-signed), Auto = Sdk240 on an AMD GPU unless the unlock Fsr4Int8 is
    # switched off (PUSHMPC_DISABLE_FSR4INT8=1 or HKCU\Software\PushMPC\Unlocks Fsr4Int8 = 0)
    [ValidateSet('Auto', 'Package', 'Sdk240')]
    [string]$FidelityFx = 'Auto',
    # reuse an existing PushFPS / Veyra install's files as hard links (same volume) instead of copies
    [switch]$LinkExisting,
    # never reuse an existing install's files (always the package)
    [switch]$NoExisting,
    [string]$ReleaseManifest = '',
    [ValidateSet('filter', 'core', 'runtime', 'vsplugin')]
    [string]$Component = 'filter',
    [switch]$AllowUnsigned,
    [switch]$DryRun,
    [switch]$NoMpcHc,
    # MPC-BE: skip it; tests: a fake registry root, a fake mpc-be64.exe (counts as installed), the backup key
    [switch]$NoMpcBe,
    [string]$MpcBeRegRoot = 'HKCU:\Software\MPC-BE',
    [string]$MpcBeExe = '',
    [string]$MpcBeBackupKey = 'HKCU:\Software\PushMPC\MpcBeBackup',
    [switch]$NoPotPlayer,
    [string]$PotRegRoot = 'HKCU:\Software\DAUM\PotPlayerMini64',
    [string]$PotPlayerExe = '',
    [string]$PotLavPath = '',
    # mpv route: skip it; tests: a fake root for mpv-integration.ps1 (-Root); stage: the plugin build; the runtime pins
    [switch]$NoMpv,
    [string]$MpvRoot = '',
    [string]$VsPluginDll = '',
    [string]$VsPinFile = '',
    # VLC: skip it; stage: the VLC module build (libpushmpc_plugin.dll; staged as vlc\ with pushmpc-vlc.ps1)
    [switch]$NoVlc,
    [string]$VlcPlugin = '',
    # capture mode, the fallback for players without a native route: Auto = on only when such a player is found
    [ValidateSet('Auto', 'On', 'Off')]
    [string]$Capture = 'Auto',
    [string]$CaptureStartMenu = '',
    # tests: the folder that holds Plex\Plex HTPC and Plex\Plex (default %ProgramFiles%)
    [string]$PlexRoot = '',
    # the pmml runtime folder (PushFlow on other ONNX Runtime EPs); default: PMML_RUNTIME, HKCU MlRuntime, <InstallDir>\pmml
    [string]$PmmlRuntime = '',
    [switch]$Apply,
    # PushFlow's runtime for this GPU (pmml: TensorRT-RTX on RTX, OpenVINO on Intel) into <InstallDir>\pmml: Auto | No
    [ValidateSet('Auto', 'No')]
    [string]$Pmml = 'Auto',
    # Plex HTPC / Plex for Windows: a VapourSynth-enabled libmpv in Plex's folder (pushmpc-plex.ps1; asks for
    # administrator permission): Auto (tested Plex versions) | No
    [ValidateSet('Auto', 'No')]
    [string]$PlexSwap = 'Auto',
    [switch]$KeepSettings,
    [switch]$Verify,
    # build outputs for "stage" (default: this repository's build folders; PUSHMPC_BUILD_DIR overrides the build tree)
    [string]$FilterBin = '',
    [string]$CoreDll = '',
    [string]$ShaderDir = '',
    [switch]$RequireNvof,
    # machine-readable output for PushMPC-Setup.exe / PushMpcUpdater.exe
    [switch]$Machine,
    [switch]$NoRegister,
    [switch]$NoTask,
    # stage: the updater build (default <repo>\updater\bin\PushMpcUpdater.exe)
    [string]$UpdaterExe = '',
    # package: release version (tag v<version>; default the filter version), pre-release, signing key, no Setup build
    [string]$ReleaseVersion = '',
    [switch]$Prerelease,
    [string]$SigningKey = '',
    [switch]$NoSetup
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ------------------------------------------------------------------------------------------------ constants
$PushMpcRepo = 'https://github.com/CptPakundo/PushMPC'
# Future update feed (not used yet: "update" takes a local -ReleaseManifest for now).
$UpdateFeed = "$PushMpcRepo/releases/latest/download/release.json"
# Veyra's runtime packages that PushFPS 4.5.3 is built on (tag v2.2.1; 4.1.0 to 4.4.0 used v2.1.9), pinned like PushFPS's
# installer pins them (the SHA-256s are those of PushFPS 4.5.3's released Install-PushFPS.ps1). 2.2.1 has the same vendor
# runtimes as 2.1.9 (every file PushMPC takes under runtime\ and runtime_local\ is byte-identical, checked on the NVIDIA
# package); FFmpeg 9.0.1 was rebuilt (avutil-61.dll and swscale-10.dll differ). Since 2.2.1 the package's program sits under
# Veyra-2.2.1-<vendor>-win64-portable\versions\2.2.1\ (a Veyra.exe launcher at the top): the root is still found by
# veyra_qml_ui.exe, and an installed PushFPS has the program flat in its folder as before. The runtime component's version IS this package reference; Subset counts
# the revisions of the file list below (an installed runtime from an older list is extracted again from the same package).
# -Gpu picks the package as PushFPS's installer does (Auto: NVIDIA when a video controller's name says NVIDIA, else AMD;
# the AMD package also serves Intel and other GPUs: FSR, PushFlow and AMD FRC need no vendor files).
$Packages = @{
    NVIDIA = @{
        Version = 'veyra-2.2.1-nvidia'; Vendor = 'NVIDIA'
        Repo    = 'Likely7/Veyra-NRVideo'
        Tag     = 'v2.2.1'
        Asset   = 'Veyra-2.2.1-NVIDIA-win64-portable.zip'
        Url     = 'https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.2.1/Veyra-2.2.1-NVIDIA-win64-portable.zip'
        Sha256  = 'E09150D1594AA9416FA27457A42B4D50B5D814E05471216BEF95B9303544386B'
        Subset  = '3'   # 1: first list; 2: + VSR styles; 3: + DLSS-G (and its RTX 30/40 unlock files), FidelityFX, PushFlow
    }
    AMD = @{
        Version = 'veyra-2.2.1-amd'; Vendor = 'AMD'
        Repo    = 'Likely7/Veyra-NRVideo'
        Tag     = 'v2.2.1'
        Asset   = 'Veyra-2.2.1-AMD-win64-portable.zip'
        Url     = 'https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.2.1/Veyra-2.2.1-AMD-win64-portable.zip'
        Sha256  = 'B6440744DEE2AE9422838F486873C84ED8D6B1BC36B6AD29222E8EDA40077BF8'
        Subset  = '3'
    }
}
function Resolve-GpuVendor {
    if ($Gpu -ne 'Auto') { return $Gpu }
    # an installed runtime keeps its vendor (update, status); a new install looks at the video controllers
    $installed = Join-Path $InstallDir 'runtime\manifest.txt'
    if (Test-Path $installed) { $v = (Select-String -Path $installed -Pattern '^vendor=(.+)$' | Select-Object -First 1); if ($v -and $v.Matches[0].Groups[1].Value -in @('NVIDIA', 'AMD')) { return $v.Matches[0].Groups[1].Value } }
    $names = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    if ($names -match 'NVIDIA') { return 'NVIDIA' }
    return 'AMD'
}
$GpuVendor = Resolve-GpuVendor
$Runtime = $Packages[$GpuVendor]
# What pushcore loads from the runtime root. Paths relative to the
# package's root folder (the one holding veyra_qml_ui.exe). Required files must be in the package; the folders in
# $RuntimeDirs are taken whole when the package has them.
$RuntimeFilesByVendor = @{
    NVIDIA = @(
        'avutil-61.dll', 'swscale-10.dll',                                   # FFmpeg, imported by pushcore.dll
        'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll',            # VC++ runtime for FFmpeg and the NVIDIA Video Effects DLLs (fallback when the system has none)
        'runtime\config\ngx-local.json',                                     # NGX project id (required by the NGX core)
        'runtime\experimental\nvngx_vsr.dll',                                # RTX Video Super Resolution, standard family (NGX)
        'runtime\experimental\nvngx_truehdr.dll',                            # RTX Video HDR (NGX)
        'runtime\experimental\nvngx_dlssg.dll',                              # DLSS frame generation 310.9.1 (the fallback / high-multiplier method)
        'runtime\experimental\nr-original\nvngx_dlssnr.dll',                 # AI enhancement (NVIDIA's original NR runtime, RTX 50)
        'runtime\nvidia-vfg\NVVideoEffects.dll', 'runtime\nvidia-vfg\NVCVImage.dll', 'runtime\nvidia-vfg\nvngxruntime.dll',
        'runtime\nvidia-vfg\cudart64_12.dll', 'runtime\nvidia-vfg\nvVFXVideoFrameGeneration.dll',  # NVIDIA VFG (Video Effects SDK; models inside)
        'runtime\nvidia-vfg\nvVFXVideoSuperRes.dll', 'runtime\nvidia-vfg\nvngx_vsr.dll',           # RTX VSR high bitrate / streaming styles
        'runtime_local\amd\fidelityfx\amd_fidelityfx_loader_dx12.dll',      # FidelityFX: the optical flow DLSS-G uses, FSR upscaling and FSR FG
        'runtime_local\amd\fidelityfx\amd_fidelityfx_upscaler_dx12.dll',
        'runtime_local\amd\fidelityfx\amd_fidelityfx_framegeneration_dx12.dll'
    )
    AMD = @(
        'avutil-61.dll', 'swscale-10.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll',
        'runtime_local\amd\fidelityfx\amd_fidelityfx_loader_dx12.dll',      # FSR 4.1 / 3.1 upscaling, FSR 3.1 / FSR 4 ML frame generation
        'runtime_local\amd\fidelityfx\amd_fidelityfx_upscaler_dx12.dll',
        'runtime_local\amd\fidelityfx\amd_fidelityfx_framegeneration_dx12.dll'
    )
}
$RuntimeDirsByVendor = @{
    NVIDIA = @('runtime\experimental\nr-ampere',              # NR: the community SF build (RTX 20-50), PushFPS's default slot
               'runtime\experimental\dlssg-lite',             # DLSS-G performance mode (RTX 50)
               'runtime\experimental\fallback-dlssg-310.7',   # DLSS-G 310.7 for AdaMfgUnlock / AmpereMfgUnlock (RTX 40 / 30)
               'runtime\experimental\dlssg-kernels')          # DlssgTransfusion's kernels (RTX 30 / 40 DLSS-G)
    AMD    = @('runtime\amd-nr',                              # NR on RX 9000 (lmxxf, HIP)
               'runtime\amd-nr-rdna3')                        # NR on RX 7000 (RedDuke ZLUDA worker process)
}
$RuntimeFiles = $RuntimeFilesByVendor[$GpuVendor]
$RuntimeDirs = $RuntimeDirsByVendor[$GpuVendor]
# Per-file SHA-256 and size of each package's subset (computed from the pinned packages; v2.1.9 AMD downloaded and verified
# 2026-11-23; v2.2.1 NVIDIA read from the official package 2026-11-25: only avutil-61.dll / swscale-10.dll changed. The v2.2.1
# AMD values were checked against the official AMD 2.2.1 package by .github/workflows/runtime-pins.yml (2026-11-25, 684 files,
# run 45592417559): besides the two FFmpeg files, runtime\amd-nr-rdna3\veyra_amd_nr_worker.exe and its manifest changed. Files reused from an existing PushFPS / Veyra install, and files extracted from a package, must match.
$RuntimeFileHashesAmd = @{
    'avutil-61.dll' = '4504BCCF90872875B21445D0AC121FF984D65F74CFCDAF61D1E333AABD9D4699 1224192'
    'msvcp140.dll' = '0F885B509A685D2BBFA652FED26B5FB31D88FBDAB0A978C641D1C7B8AA460AA9 557728'
    'runtime\amd-nr\assets\block0-attention.f32' = '2863026814BAD0F2475F6EC03D35E7F5CD5FF9EA30355F3DAD37145B509E9004 32900'
    'runtime\amd-nr\assets\block0-ffn.f16' = '4A7F02E41DA6AD46DDA1947B2DA7820F0766865D209BA60E013CE799CC8F6CED 17472'
    'runtime\amd-nr\assets\block1-attention.f32' = '3B5212FA9AC96296E02D1EFC1A005FDB6A6ABDF49D82CF7A6EB664F1B7D66BBD 32900'
    'runtime\amd-nr\assets\block1-ffn.f16' = '41611E8E09D09B842F5FD61FE6531F42B05A2B38F74E19353152F44DC3CBA1E7 17472'
    'runtime\amd-nr\assets\block10-attention.f32' = '00A1EEA55845263E1EE1216D0D3FE98278227094DADD1AEF3BC58E1BCFFD2089 328208'
    'runtime\amd-nr\assets\block10-ffn.f16' = '1E434A5A20A2BF0080FEDA71B04ADBB0FB01FB40F2CA7E58A7BCCCF53E501ACC 295168'
    'runtime\amd-nr\assets\block11-attention.f32' = '4B9109AE8CAAB45532F2D6DBB9DB2C995B7CB765863B36E5F6D95A7C1F2EA1A3 328208'
    'runtime\amd-nr\assets\block11-ffn.f16' = 'C4687B6F089FA59A411DE48E81FFDB650141EFDF61E6C9693C1B88BF09696086 295168'
    'runtime\amd-nr\assets\block12-attention.f32' = 'DFB2E254BB811A745CDC9E09F8C645C3212C51984C4CF9E8C558F185E2E45596 328208'
    'runtime\amd-nr\assets\block12-ffn.f16' = '08EECAEA7DA6052941A3E86A061300C3C8CCFADF6D6DC909A88999500135D0B7 295168'
    'runtime\amd-nr\assets\block13-attention.f32' = '26F53766DA41E623C2169DCC06D68113EE8876775A3C76E528C57CDD07A1B3B2 328208'
    'runtime\amd-nr\assets\block13-ffn.f16' = 'C6352C980FCEB21D06A4A0C1786B444B70684F6C815BED0B012625D763F79A9A 295168'
    'runtime\amd-nr\assets\block14-attention.f32' = 'F0D728B707C5B74E9A3A133D49A4C12C4D5F20389CD24C360B42E64AEE221255 328208'
    'runtime\amd-nr\assets\block14-ds.f16' = '5D41B5B5D72A32B675DAE50F09896BFF9F2F4EE95CFAE3A036AAEB7D1A8329FD 65536'
    'runtime\amd-nr\assets\block14-ffn.f16' = '13E60841A2D56EECB1FF937DF3B7C6615A6CB185B0A41E2E3B1DE5DE57312B05 295168'
    'runtime\amd-nr\assets\block15-attention.f32' = '4ACC6D6F124B7541F63DC99B556D2C08D8AB740849C4872B55AE997FCEB265B5 1180704'
    'runtime\amd-nr\assets\block15-ffn.f16' = '819A777450C5E8631820F7FDBC0219BBE7FA4A9F08EBE365CFFECB7D962F4ACC 1180160'
    'runtime\amd-nr\assets\block16-attention.f32' = '2420A5E3376E4174D7C3B06DE75F357BCDC3139C85A8F6765E79C94CE8CDCAF5 1180704'
    'runtime\amd-nr\assets\block16-ffn.f16' = 'D9FF25FA10F6F576912A8055AF4D31352AA11D20C49CBD0F7842C98302AA83F3 1180160'
    'runtime\amd-nr\assets\block17-attention.f32' = '654E49B5B647B541A8CB422CA6F75DAC0651CAB9D51B9B42684C6A761DC4144F 1180704'
    'runtime\amd-nr\assets\block17-ffn.f16' = '364B50893418E4664ADA9050793BFFDCC00CA1DC8E941FA82432355A80A68D8A 1180160'
    'runtime\amd-nr\assets\block18-attention.f32' = '1FAB3B45FEDE1F46D27B2424B5B3030A7BD2E299243F200119F08AFE1C1A3D83 1180704'
    'runtime\amd-nr\assets\block18-ffn.f16' = 'F9FB9CA6347BEA216A92EDFD323253D91248B9BD8D7D3432F73B6F2B6001EBE3 1180160'
    'runtime\amd-nr\assets\block19-attention.f32' = 'B198481983E00876232DBF7AED83CAB8F801B021DF196AA305A1BDC625F1607F 1180704'
    'runtime\amd-nr\assets\block19-ffn.f16' = '21CD0B47CAC78D4CBCBB8BB987FC9C2670EECF4EB754B190532A232CDBC40187 1180160'
    'runtime\amd-nr\assets\block2-attention.f32' = '65DCF4E6B0138C708523622460541D000B15A35FE900FA3BBAA6B730C9FB93DE 32900'
    'runtime\amd-nr\assets\block2-ffn.f16' = '037649FC3AD6C4B87EC17DCE002C7FCF677BB55FA30EBA495158914411A63D28 17472'
    'runtime\amd-nr\assets\block20-attention.f32' = 'E1B505775B972B9F314480FCD63A3D0A0AA9C73E1C730BCDCB54D8F534825E35 1180704'
    'runtime\amd-nr\assets\block20-ffn.f16' = '50247F9AD8FB92973F6181A6ECDBC55B8B2771DCA4809CAF48479F526A5BC849 1180160'
    'runtime\amd-nr\assets\block21-attention.f32' = '2DB95107AD4B4255FBB9EA046B7355DB5A739A16AAA37EB5D6DE601D2C90DE4E 1180704'
    'runtime\amd-nr\assets\block21-ffn.f16' = 'C159D4A3A96F5EAE92E4BEDF739F8022D4A4DD1DBC3A2782A8064D6D236D2006 1180160'
    'runtime\amd-nr\assets\block22-attention.f32' = '6D52F6D588348943C62E653FA7390BF8C616F9B02F124CACB56F82122D2AACB5 1180704'
    'runtime\amd-nr\assets\block22-ds.f16' = 'DEC9C2E15A83203242045BE50EB75BD5933DF445FAE71F5ABB92EC26BE4D8B08 262144'
    'runtime\amd-nr\assets\block22-ffn.f16' = 'AD4460E682E856D84B002D35462B4225FF3811AAB63219B0D518624A3CC3E0DA 1180160'
    'runtime\amd-nr\assets\block23-attention.f32' = '38ED0B4DD766C7E8180617623BAB1B8B41DE4BA9FC7E2F35A92B9FCEDE7492A9 4458560'
    'runtime\amd-nr\assets\block23-ffwd-projection.f16' = '3C106FC821F7200EEAB41F6BC7DB40C25C8BB45E9F16429756161A5540DE4FFD 525312'
    'runtime\amd-nr\assets\block23-ffwd.f16' = '8A2DDC670D85F5CE32F2C749610C22E855F3876B2A673B0F5FDF91D3662E09EE 1048576'
    'runtime\amd-nr\assets\block24-attention.f32' = '610D709842DD5C73C961345F2D78BAB7A2C0A79F5DC92AB3825EF7AEC74A41B0 4458560'
    'runtime\amd-nr\assets\block24-ffwd-projection.f16' = 'D328DD6384669C4BD6AF26D7316FCF044BD599D20E696F09616620167A198343 525312'
    'runtime\amd-nr\assets\block24-ffwd.f16' = 'BDD2A4ECE4142511A564FC6A6313501EC2E8A0F69AC23EE43390DB028749E777 1048576'
    'runtime\amd-nr\assets\block25-attention.f32' = '6578415952042C5D2E036A3CB4BC524CB89DA02CF2A9F3A8AD4C2AFFC50F573F 4458560'
    'runtime\amd-nr\assets\block25-ffwd-projection.f16' = '9633CB19D3057E3118A5B295AB49DDDDA46B7C07CD5A04464C9EE00D9FC4464C 525312'
    'runtime\amd-nr\assets\block25-ffwd.f16' = '7434D51D436D01816C477CE622A9384AB2DAC6F0D6007804326DCE358FEEFE0E 1048576'
    'runtime\amd-nr\assets\block26-attention.f32' = '14ECF9615CB8C5DEA60103934AD30906740374BD806BE20F974904F1F1FA29DC 4458560'
    'runtime\amd-nr\assets\block26-ffwd-projection.f16' = '0D6DE74C8735ADF48AAE2AE72098FCE283EF55CDCD698970A6F808E2C6271F21 525312'
    'runtime\amd-nr\assets\block26-ffwd.f16' = '8D465C6C37969D074068DBFCE89B1A8CB4DC8F3B481BBD984B69929C26521106 1048576'
    'runtime\amd-nr\assets\block27-attention.f32' = 'E509694D1E2107CB4206EE2F8DEC7434642C144D75FB53E5B7B1D023A2F99FBA 4458560'
    'runtime\amd-nr\assets\block27-ffwd-projection.f16' = 'A26044145D4F21200C56C8F113B679A8E137B8A7E095DA9E12A2586F0E5D393D 525312'
    'runtime\amd-nr\assets\block27-ffwd.f16' = 'BE2CC1C7C3DFD315A8717460E333EBCEF01EEC6387358B298AB5AC38D5720F64 1048576'
    'runtime\amd-nr\assets\block28-attention.f32' = 'E31D69D01237862DF094C29F63A43C48BECA7A471F8F26BB54F9A50845313A3A 4458560'
    'runtime\amd-nr\assets\block28-ffwd-projection.f16' = '62A976498380D69938D52B17E084B1233651379F240DD0F50483F3E6E4CDE65E 525312'
    'runtime\amd-nr\assets\block28-ffwd.f16' = '5C4997B12DACC100066514DCDE620832A31DD6CA4FB01543B763DAB366422A40 1048576'
    'runtime\amd-nr\assets\block29-attention.f32' = 'D1798913270D3766B4218A0471489203C90913A31087DF1C963B423643D53558 4458560'
    'runtime\amd-nr\assets\block29-ffwd-projection.f16' = 'DE10060CF3A3A04EC495F4D58C644E6E3C4CD55D11E676004A4F83FCA7BC5865 525312'
    'runtime\amd-nr\assets\block29-ffwd.f16' = 'EB0DF4E10E053C9CE336BC37D568CFD6C1A566D1DA3604F42D12D7AFBEAAE478 1048576'
    'runtime\amd-nr\assets\block3-attention.f32' = '39ABD426B69DF7B8B382E61C122C4C4C499CD006475D260E13D36992A6D66C92 32900'
    'runtime\amd-nr\assets\block3-ffn.f16' = 'E669A241D6597A29ADA896451BF7612D8434C57E87F3B5963536FFACE848668C 17472'
    'runtime\amd-nr\assets\block30-attention.f32' = '2064A9393A177C1F09577861CFC11725EE5ADB3D67313D9A7577AAABE05BFBBC 4458560'
    'runtime\amd-nr\assets\block30-ffwd-projection.f16' = '2863A96A245B85EBB8A20DD6D186A69FC89F26490923DEC8F67BFFEF98708FE0 525312'
    'runtime\amd-nr\assets\block30-ffwd.f16' = '673E9B1878B6DAEF1C6FC6F1026D12D9BC0025823070F2F95EE6C5C48FC422B1 1048576'
    'runtime\amd-nr\assets\block31-contract.f16' = 'E89F848AA5874C02FEEB313C4FF0D953D82BE3B0DDFD8FE7D8482EEAEA85515E 8390656'
    'runtime\amd-nr\assets\block31-expand.f16' = 'EE20133146AE0410282390F8883451D0975ADFCF95D02F0E2E671A92A97D396A 8388608'
    'runtime\amd-nr\assets\block31-projection.f16' = '7399E57373DD927B2D2D6AB7948CB4278FD7B73901486374DDA0516AD5D6C0E2 2099200'
    'runtime\amd-nr\assets\block31-qkv.f16' = '0A98B968119392138726EB355CB5A0554CD491528597B3B9AD7024EEDBBA8E2E 6291520'
    'runtime\amd-nr\assets\block32-contract.f16' = '6937677A359A20302570ECBADF08F65FFB6501CF4B05D97CC0D72F30B51FD374 8390656'
    'runtime\amd-nr\assets\block32-expand.f16' = 'BC938271655CFF7920AFB506E26F138DA948EE2BDEF6A911962A0DC7148B149B 8388608'
    'runtime\amd-nr\assets\block32-projection.f16' = '171C81730C032E62582923A45DDD399C1C2261DF95A6CE2D45D41E9AEE7ED801 2099200'
    'runtime\amd-nr\assets\block32-qkv.f16' = 'B25E700CD37E0B3539BFD4A63B8A29A683319614D98A93464A2D81703B6C1F09 6291520'
    'runtime\amd-nr\assets\block33-contract.f16' = '7BD2A61634EEEDC8ABA20A9F63BC6537B5891BBF0D73E7A39F509C7EFBA7832A 8390656'
    'runtime\amd-nr\assets\block33-expand.f16' = '41F9634C2E6D72435CD1836EC9BCAFD8837BEC213232777BDA0523D9E00ECCDF 8388608'
    'runtime\amd-nr\assets\block33-projection.f16' = '949F9FED4F5BBCC0B76E52BAF508F9DFA93BF6969AD75FC4EA2097EC78E4DEEE 2099200'
    'runtime\amd-nr\assets\block33-qkv.f16' = 'E09D8EFF5FFD8AACB393FA5697B07B35DAAFDC60AB4094443EB7214BE7182B89 6291520'
    'runtime\amd-nr\assets\block34-contract.f16' = '4822783EAB47947B75F80C3B76EE375B3048B6D2A3187C76F9D28EAC8E3740EC 8390656'
    'runtime\amd-nr\assets\block34-expand.f16' = '9E9B956F0CEE19344CB30389C0295B2C87B44E6386DF8664398CB8EC384CEB18 8388608'
    'runtime\amd-nr\assets\block34-projection.f16' = '8DC7C691D8A2726CAAFBEE4B693B25A2D60133BF7C5FC3039E1BD8DD0A4471D5 2099200'
    'runtime\amd-nr\assets\block34-qkv.f16' = '9B74615099C88B0042083C9E0556D275D995B8DD97982073D65F83074761D772 6291520'
    'runtime\amd-nr\assets\block35-contract.f16' = 'EF0BDA0A2DB0A057D2C54D1090BFF22EF4E1CA9CBF0183391057D0E02BE44A69 8390656'
    'runtime\amd-nr\assets\block35-expand.f16' = '2B76DCBF5AB8FB289C0F5EE10350462F3E09B0DE99C9F28546AD42F7A4850B82 8388608'
    'runtime\amd-nr\assets\block35-projection.f16' = '295FA777E4833BC56C2AFB5345DEF1EA042B156E00C2EAAA53FD27B28FD85625 2099200'
    'runtime\amd-nr\assets\block35-qkv.f16' = '95E755734D9A3FA8766FC8388446333BB923196DE0B3F95A32B3AFD41222C77E 6291520'
    'runtime\amd-nr\assets\block36-contract.f16' = '7E289D70D28D5A1518FC63311C43C4C1014A3B0E3EEC11E801022E75063E0F31 8390656'
    'runtime\amd-nr\assets\block36-expand.f16' = '920F08090225FBB84A37E948910B7D8358561A6226CAC1730833557DA2C8F709 8388608'
    'runtime\amd-nr\assets\block36-projection.f16' = '95FE0447D0D0B4888F1DE17319B766E5CD3C908D9475DD93DA071E146C882013 2099200'
    'runtime\amd-nr\assets\block36-qkv.f16' = '0CFC8FB01B9FA9DE617955A62E26B5D6D35DA3FCDE798FB1138E33A57EB2B830 6291520'
    'runtime\amd-nr\assets\block37-contract.f16' = 'C07A2386D50611C7BF814337A01C8AAE11D50338FC325D3406B3658488499F0A 8390656'
    'runtime\amd-nr\assets\block37-expand.f16' = '297B76E31D887712525DEE55F5B617CB642A0C46CE1B0917016388F0906EC09E 8388608'
    'runtime\amd-nr\assets\block37-projection.f16' = '426CAE678D0216DDE0DE7DA94224FFBBF9B2DA69075B41A099CB9000E68AFCA3 2099200'
    'runtime\amd-nr\assets\block37-qkv.f16' = '0DCCD8F226727B82F34B24CB4E1B6927D7A42AD79A616E2C0E8726063992E387 6291520'
    'runtime\amd-nr\assets\block38-contract.f16' = '9C51B8F1AAF0E5E9E6447EE63D35EDFFF750070F2F0B59C7CB20911FE45D75F7 8390656'
    'runtime\amd-nr\assets\block38-expand.f16' = 'E8C3E47B70DB459C00547BFF56B1929D611B9874CEF55855602C543DE3EE2F32 8388608'
    'runtime\amd-nr\assets\block38-projection.f16' = '87B6F72B0C1F5E274E2A90FFD90DB6D3DF2DD3D5C232A611ACBB29C5916BE00F 2099200'
    'runtime\amd-nr\assets\block38-qkv.f16' = '3D10A47A9270449DCC020465DDE7A11381C76D645B67D0FC74B041168124B67A 6291520'
    'runtime\amd-nr\assets\block4-attention.f32' = 'EFACDAFF9C9B36EF0E5EAEB368E1014C05A4621943A64D68AEB9BBA5D0945877 32900'
    'runtime\amd-nr\assets\block4-ds.f16' = 'C922D8046A1B5BC6CC5F3CA16791A905E84C675AAE9EFC09024EAD6A8E037AE6 4096'
    'runtime\amd-nr\assets\block4-ffn.f16' = 'DD1F42AD43241536BF6D49F033A9B23F31E12DF39DD74A921A8487378F84CE1C 17472'
    'runtime\amd-nr\assets\block40-attention.f32' = '932A7FD566D3B21567472D4453DC98CA697A00D148F40F94DB72A683C4DA6220 4458560'
    'runtime\amd-nr\assets\block40-ffwd-projection.f16' = '470470D65E3D2E71619DE016E5D73E6CC2AEF73375E64F612CD2F756E36A25EE 525312'
    'runtime\amd-nr\assets\block40-ffwd.f16' = '8407C0AD3BEBBA3F4CEECD03E8ABBC310956ED9F88A96EF6C865ECE197CEB2B5 1048576'
    'runtime\amd-nr\assets\block41-attention.f32' = '5C26E2087A4C0B7E67CE1BE665CDBFDF89AF60348CE6F88C35175707AB782E83 4458560'
    'runtime\amd-nr\assets\block41-ffwd-projection.f16' = '17B3517AD7C40F0D43D2AA799FC4106DC59EBD0884B254A5E8E3FB0EB906AB1D 525312'
    'runtime\amd-nr\assets\block41-ffwd.f16' = 'CB056AAD381DDAD412B7B062249291F984B6C18590ED0DAC212186F6E8793573 1048576'
    'runtime\amd-nr\assets\block42-attention.f32' = '20229EF899F0B492E149FC3DF825A7CC1ACA7494F0E6E7DB3F5A0C50B24D9BEA 4458560'
    'runtime\amd-nr\assets\block42-ffwd-projection.f16' = '8CCD0AEA3CDD15B307BD2ECBE3997F48CB64FE553FB60463C5E62B7ED2596341 525312'
    'runtime\amd-nr\assets\block42-ffwd.f16' = '816591F4861DE97D5646BE06A4984E9BE200E039E7394995AAF91D32439AF860 1048576'
    'runtime\amd-nr\assets\block43-attention.f32' = '513A0F6D16249755402F6C256F903523E25BF2C56A46E26B6F44AE79B52AC49B 4458560'
    'runtime\amd-nr\assets\block43-ffwd-projection.f16' = '2994519D8F81227F719FC0C1E67871CD7482B66EF2262C0EA4CB185B5015E04B 525312'
    'runtime\amd-nr\assets\block43-ffwd.f16' = 'D3E09D6BA3B28E619AFF0BD08FDD9CF75688993898D2E9CE87F131036EB66614 1048576'
    'runtime\amd-nr\assets\block44-attention.f32' = 'CD4E3BF6C24182BE7BE0EAE2FC4F7C2B50439DFC01F83EAC8706F6060AE7C5AA 4458560'
    'runtime\amd-nr\assets\block44-ffwd-projection.f16' = 'F1EFFC6F54651E3C7FE22C67E1394CDEF08262866042A79A1A3773F15B9167FD 525312'
    'runtime\amd-nr\assets\block44-ffwd.f16' = 'DD842E9C4FD672AD222AD112636406462CCF3A86B162D955A8C3F22F0232311E 1048576'
    'runtime\amd-nr\assets\block45-attention.f32' = 'D1553F9932A8AB0F14B3EEC2F6F1A997709E733F7B60DF83168798B52C9980B3 4458560'
    'runtime\amd-nr\assets\block45-ffwd-projection.f16' = 'EDA328FD0105093CACCBCEE26DEFC3AD38AFE76D32053DECDF042B48F843F783 525312'
    'runtime\amd-nr\assets\block45-ffwd.f16' = '8F693006B3CDD24F8D09043D5311A54F846DFBA2600D64EDD9CF1F75CFFECABF 1048576'
    'runtime\amd-nr\assets\block46-attention.f32' = '292102EE452DD70B563EB3C4B320910EAFB5DAE39BCB6D046947B25BBE2439F7 4458560'
    'runtime\amd-nr\assets\block46-ffwd-projection.f16' = '4B02E6A15D551B981336EA335F71BD5F3E3FA751E0697EEC63FF81B8AA94D529 525312'
    'runtime\amd-nr\assets\block46-ffwd.f16' = '47CA875FC6A4D0E933D4659588EEB9946D0C04C5472B0112EF4EDEECCFEA5CEC 1048576'
    'runtime\amd-nr\assets\block47-attention.f32' = '9AEF5EC1E84785271C702F49A280CC25A155783864ECBCD0E133C43CD148A4CB 4458560'
    'runtime\amd-nr\assets\block47-ffwd-projection.f16' = '468BCED86F3B2EFA224A88E23E94CD3E51ED11D5AEBF7DAF11BF55353479798C 525312'
    'runtime\amd-nr\assets\block47-ffwd.f16' = '86E03E9B45BF420F48677140CCA3F77DCBB468F178E12628D72F1FCB27FE9483 1048576'
    'runtime\amd-nr\assets\block48-attention.f32' = 'F41B5D423524C5732668C7D401D59F216F20497B40B5F2469CC1D31E3C0A5D33 1180704'
    'runtime\amd-nr\assets\block48-ffn.f16' = 'B5D58FF9C5CA47C462BC948365D4E12ABED3F391D61D68C0D5530C82F8189C40 1180160'
    'runtime\amd-nr\assets\block48-weights.f16' = '9748D7B545421DD73A0793179300574C5B31324F30170EC949B76294DFE28CD2 262656'
    'runtime\amd-nr\assets\block49-attention.f32' = '53ABC334B988378F6357E923F1D781924639C4B88ACA91ADC5BB7158154F5B3D 1180704'
    'runtime\amd-nr\assets\block49-ffn.f16' = 'CB3B104F4E4A406E870D5C3B762074101430E54A6587F37127F41B76473C0C25 1180160'
    'runtime\amd-nr\assets\block5-attention.f32' = 'D46975C02312D7518D447CC803E468D744F330640662554DF17946E155775A86 98568'
    'runtime\amd-nr\assets\block5-ffn.f16' = 'A995F976A33277221AA59E310DD822C2EDFADF88DDF4927892A19CAC39944431 73856'
    'runtime\amd-nr\assets\block50-attention.f32' = 'CEF06E3CD0828783EB4CBB7118D27EBFB2666094E4A632DEFBBC9A63DDF78604 1180704'
    'runtime\amd-nr\assets\block50-ffn.f16' = '59185D84CCDF6B050D90A66166D1665E0871C4D6F1EA9181C44C696AC2991DFA 1180160'
    'runtime\amd-nr\assets\block51-attention.f32' = '9649336E74F67304005CF1B92654358638E6CACFBBA27C22CE182AABC34E35E6 1180704'
    'runtime\amd-nr\assets\block51-ffn.f16' = 'F52DF0DD3FCF9C5C695F295FD39E89DC3259F67AA8CDFA3D13356AC95EAFFF48 1180160'
    'runtime\amd-nr\assets\block52-attention.f32' = 'ACA58CCC9104F08B515C8B35E8C5C6BF6B0C464AB0E62F85C6D9C01B200DDDA9 1180704'
    'runtime\amd-nr\assets\block52-ffn.f16' = '5E3C67D4FDF075F8B06B19403B5A129084337BD8965763566E639A7AAAA327EA 1180160'
    'runtime\amd-nr\assets\block53-attention.f32' = '0AEB23EC47942C39ECE88FB1EA5B8282552178FD9E553FC9B152707606336582 1180704'
    'runtime\amd-nr\assets\block53-ffn.f16' = '111793E9DE4D8E3C6FDC2B5167D2F45D261A198A472105F95065F38D9A944E40 1180160'
    'runtime\amd-nr\assets\block54-attention.f32' = 'A8CE8361DF50CCBBE29FD8C0C582410C26C778FB6F18CC8CC0EADFAC855666B7 1180704'
    'runtime\amd-nr\assets\block54-ffn.f16' = '7C70FFAA721A9030F9C2F7D6BE544EBCCCC0570AA4440C913205738F0553B5F6 1180160'
    'runtime\amd-nr\assets\block55-attention.f32' = 'BFAB933BC5BDB035ADBE6088E9CA5DC9FA26DA1C5762B7166E6C525D39BEB249 1180704'
    'runtime\amd-nr\assets\block55-ffn.f16' = '87618F36092741591906B06D04563B96A0D185C675F951389BBD784CA99B607F 1180160'
    'runtime\amd-nr\assets\block56-attention.f32' = '12D3FA30D4D561B3A35D4038D45C88063599D4AACC60B9878FEC87F2A5FA074E 328208'
    'runtime\amd-nr\assets\block56-ffn.f16' = '1677FE24418D9CF8576DE10C6BED47821CFFADC7942F5C629AF5F2EEE93E9AD5 295168'
    'runtime\amd-nr\assets\block56-weights.f16' = '34A9DA1354B91803F0DD7CDF72C7B2A7B78D27243EAE7F83D792EA5A98F550BE 65792'
    'runtime\amd-nr\assets\block57-attention.f32' = '29CBD04DBADDF83A43F10F3AFACBC6F20A561F0CD716D65AEF7A225D88663C5B 328208'
    'runtime\amd-nr\assets\block57-ffn.f16' = '96AB2BE0CE671A761F2C59663558BCA84176D452909DDEB561F86A6F6643A586 295168'
    'runtime\amd-nr\assets\block58-attention.f32' = '7CB0B87433A93A1F6A3A6F6D72C10E16EE97D734ADF4108F5BA628044CA42346 328208'
    'runtime\amd-nr\assets\block58-ffn.f16' = '5689B32EAF7AFA62E50C899E07BA549EA3D3DA92B78FBD2D92C56260F71191F5 295168'
    'runtime\amd-nr\assets\block59-attention.f32' = 'C26EE8DC1D664981156655097D82D0EBE0FF16DA3238041487A0C145A3F4FEAA 328208'
    'runtime\amd-nr\assets\block59-ffn.f16' = 'A4D0EC1758BDDE4D8AFBEA6C755C995B66FFEBB947B7AE01A26BDC4FF5EAA7A3 295168'
    'runtime\amd-nr\assets\block6-attention.f32' = 'C6E540BD61505C9830AB2B6EB6F04CDC2424E76491C66657D7B1573140B09E93 98568'
    'runtime\amd-nr\assets\block6-ffn.f16' = '49A8A0B8D5E51A96D95D6FDA4C8F38B7EE6EE7433F0133A00CED17B4231F756C 73856'
    'runtime\amd-nr\assets\block60-attention.f32' = '997CB5B3360CAD6872B29C8DB72C88F51994704DF620A0A9880ED2A11A2D18B4 328208'
    'runtime\amd-nr\assets\block60-ffn.f16' = '485A5BC5189E6D386D559E6E67B67EC7DD5DA5264E305665F228F08826196C98 295168'
    'runtime\amd-nr\assets\block61-attention.f32' = 'DA74BAC4308B8A1066F307BBB2A6C4081933243B04D8108256AC30C8033A8510 328208'
    'runtime\amd-nr\assets\block61-ffn.f16' = 'C294F75929B8CFF2F3917DC88AACAD8477A8A7F7FEC996C8FA8232EC45720F26 295168'
    'runtime\amd-nr\assets\block62-attention.f32' = '6D5D0C3B87B4C19417993C485866136B297CE4830350082F8E269032BE8A5352 98568'
    'runtime\amd-nr\assets\block62-ffn.f16' = 'FBB793FF422361BCA89FC3517E53242C0AFBB497EB2A965CCCCB7A5554EBE824 73856'
    'runtime\amd-nr\assets\block62-weights.f16' = 'AD78E3194FDCECBADF92FF6F69E65DD29CDDE354080D085C9405E82A471A8E79 16512'
    'runtime\amd-nr\assets\block63-attention.f32' = 'A99D8FB2CE215BC6F58D402E0D21063D772B931F25F7991F118D8318BCDD8BC8 98568'
    'runtime\amd-nr\assets\block63-ffn.f16' = '918C2E238622761489E69F7D6216D058337DB85D3C8C12D7BC78E549DE9400BA 73856'
    'runtime\amd-nr\assets\block64-attention.f32' = '58628AC0B690D0FA031A5B15677FF3C89C93BD9F017CEC68657EA0E79B8BE642 98568'
    'runtime\amd-nr\assets\block64-ffn.f16' = 'D0DD50E82FBCA66F16487006E7125C74178B9FF47C93C020AA3E34E25AD03DBD 73856'
    'runtime\amd-nr\assets\block65-attention.f32' = 'D7896D758A867602E06AC74E78008B9E203A6E97FA51A14805A250FCBFEA8DEF 98568'
    'runtime\amd-nr\assets\block65-ffn.f16' = '776C674B9E6AAA366325672E8798049E2AF4DC5AFDD4B3C24BE224655044E66A 73856'
    'runtime\amd-nr\assets\block66-attention.f32' = '2385F51193EDDF5AF20AE3E7FF6DEAE53E4F4721A94E09FCB5C3E95E8AA334F7 32900'
    'runtime\amd-nr\assets\block66-ffn.f16' = 'F71732656125E198CD87AE9BB4E7BD41A4F5E37D37E05DBF5FA9375A56091FC0 17472'
    'runtime\amd-nr\assets\block66-weights.f16' = 'A357D6E59907FABBD17816BD2AD53C247EF46B9DD7AE790E0AAC4F69AE69A01C 4160'
    'runtime\amd-nr\assets\block67-attention.f32' = '41E0C063A31C09969B9321CAAF824AFAF1C0E5DE8622369E1AB3CA375B8AFC8A 32900'
    'runtime\amd-nr\assets\block67-ffn.f16' = 'EC09F21103B05F6DD931F72A0F7D0FF554F6E470B3D7F9338AC35CFE1F2E2B44 17472'
    'runtime\amd-nr\assets\block68-attention.f32' = '8C6D66654B28ABD2428BD79151EC56FF1A206502F96680ED34355668ADD02AA5 32900'
    'runtime\amd-nr\assets\block68-ffn.f16' = '258262728C588EDCC905DE95FCA2427E35F5FFA8698B7EFC1F3777B5DC6AE677 17472'
    'runtime\amd-nr\assets\block69-attention.f32' = 'D4730FF3474E05A11139B67F400058B65A836BA74E0671AD596CAC029A335165 32900'
    'runtime\amd-nr\assets\block69-ffn.f16' = 'A4425DCA34CAB43C4AB18A954318C3157AEB57D763AE6EA3FC1EFF1EAF989A4F 17472'
    'runtime\amd-nr\assets\block7-attention.f32' = 'F33FA74DDCA5601562CA0485F75B3E589C3E0C44F340EEC269FF35ED11E0CFD9 98568'
    'runtime\amd-nr\assets\block7-ffn.f16' = '20CF68ACA082EFA72CC8EEADBF9AEB1E194E3634D882E8567994B5EA6DD93FA5 73856'
    'runtime\amd-nr\assets\block8-attention.f32' = 'AB772386063B7E8FF0628AA7EDF59E0A88347CA40E1BACD1A1467171C9A50145 98568'
    'runtime\amd-nr\assets\block8-ds.f16' = '9ACBC3BF906E72B7B054F0E1E121AF8EDDB2AC013C1863BF8DE9AA6B95FBFF9D 16384'
    'runtime\amd-nr\assets\block8-ffn.f16' = 'D4B1F5BA6AEDC2BC7D460176FBD46D604F293CBB181B823E1262A31DC941417D 73856'
    'runtime\amd-nr\assets\block9-attention.f32' = '365D1E8128353829A2EB70F93EAC658CCC6221A588055900CAFB455533ABC9B5 328208'
    'runtime\amd-nr\assets\block9-ffn.f16' = '0A6D8BF7B87F54AC9CDE9CACEAD3215677F5ECC29703F6145335888F9A4AA34F 295168'
    'runtime\amd-nr\assets\decoder39-weights.f16' = '028E3361DC4D37D148AF05F716C4FF89723F0C9D274CEE31AE7708142150084E 1049600'
    'runtime\amd-nr\assets\head-matrix.f16' = 'BED9C10E656B2BD06A5917877341B5C41BA7E14CE59E4CB90E2A2EE6E978498E 1048576'
    'runtime\amd-nr\assets\HIP\gfx1200\boundary-fast.hsaco' = '71AFC8157C410D849B57A9823FEB7A7107013306AE6F9ADB55C51EB2A5888C6F 50904'
    'runtime\amd-nr\assets\HIP\gfx1200\boundary_reference.hsaco' = '693547DEC3C6E47900ABA1E85F220304F2BFB19313F2B1D7403561F46CF3D6CF 17832'
    'runtime\amd-nr\assets\HIP\gfx1200\c32-wave1-fast-norm900.hsaco' = '8BAA6C390005B2124A8EAEE3F57865E302A20B667CF5EF5FFDD5F84429ED672F 821568'
    'runtime\amd-nr\assets\HIP\gfx1200\c32-wave1-fast.hsaco' = '5B6D5D80E09BEFDF2F4AE9E211FEA2DE6F89BC2E6A3AB8923A3D1B06840EC3BE 835904'
    'runtime\amd-nr\assets\HIP\gfx1200\c32-wave1-rtz.hsaco' = '0BE2193A3D9F4EC0A4C552FA76DF023579BFFF15E33D808F73F9CDFCF8AEA3C0 867648'
    'runtime\amd-nr\assets\HIP\gfx1200\c32-wave1.hsaco' = 'C082056F859F9A9129B328D886F8711036CBAE53A6BFC6F2F280A585C108CE74 862272'
    'runtime\amd-nr\assets\HIP\gfx1200\c32_fast.hsaco' = 'FFF987B9ADA5AF154B3862CC2D2B8B0474A2AA82C6271E7EA378FC473B2FF617 38056'
    'runtime\amd-nr\assets\HIP\gfx1200\c32_fast_attention.hsaco' = '169DE549AA946CBBA505260DE2D7A0B228ED93F2E2806D005F874C4009C18B6C 29568'
    'runtime\amd-nr\assets\HIP\gfx1200\c32_fused_attention.hsaco' = '2B66DEB6C8C17BDE5E27734F49D205DA2651222A860A9183D5F1B88B41F893A6 39472'
    'runtime\amd-nr\assets\HIP\gfx1200\c32_fused_ffn_attention-packed.hsaco' = '75229A63DDD075E2BA536B360BFB5C30AFF45EE1B12D579E4319ACE90AC1B66D 219328'
    'runtime\amd-nr\assets\HIP\gfx1200\c32_fused_ffn_attention.hsaco' = '911C7E6CCB0D9E879CD02C75AADDEA74AF01DE7B948FA7974F76C36ED6829BFE 286528'
    'runtime\amd-nr\assets\HIP\gfx1200\c32_prefix_reference.hsaco' = 'C3B01C905B7B20AC357FD97D1BAA9D6C4EA7616AF0431543B41617C7324CBFF8 35784'
    'runtime\amd-nr\assets\HIP\gfx1200\c32_tiled.hsaco' = '9A2B98313A62611FEE3AB230DCD44AC794C1CBB1A2CCCC3598939612CA8A995A 31536'
    'runtime\amd-nr\assets\HIP\gfx1200\c32_wmma.hsaco' = '576AFA15B5E76B2FF10EEC95FF585E60AB8B7055F0F3EE3DF41478A3FEEF46A7 66504'
    'runtime\amd-nr\assets\HIP\gfx1200\c512-m32-deep.hsaco' = 'DB0201DC5D4E81E17058E3D1A545FC400F06BBF08E89537A31E3BA761747EF33 636024'
    'runtime\amd-nr\assets\HIP\gfx1200\c512-m32-mh.hsaco' = 'F8881C1D9B38E230E1C95861DC05B388DF9B69E118CA065C88AAC5EDF36790C8 1120200'
    'runtime\amd-nr\assets\HIP\gfx1200\c64-wave2-fast.hsaco' = 'DC10C540748AD40725A1EE7241DA69857A0680046099CCB121A951C0616705B3 1898560'
    'runtime\amd-nr\assets\HIP\gfx1200\c64-wave2.hsaco' = 'CB98C2FAD00CBF3C98D5ADBB3DA4D8269DBF241ED00D363399D40797E4436638 1792320'
    'runtime\amd-nr\assets\HIP\gfx1200\deep_fast-packed-fast.hsaco' = '43058688E04C11E84A1EC69D1EC24E059B99E8913A9FEC20377644D5CAA57E0A 693928'
    'runtime\amd-nr\assets\HIP\gfx1200\deep_fast-packed.hsaco' = '2C26DD8CB251624F6AC11BBB2ECF7E0D46A3788B96D12B6A328539289CEED4E4 696480'
    'runtime\amd-nr\assets\HIP\gfx1200\deep_fast.hsaco' = '04164C98F481A03659778619556C5B9E921DAAC3F5604B5888D7901577D96097 609144'
    'runtime\amd-nr\assets\HIP\gfx1200\deep_reference.hsaco' = 'C2779CDFF408360721E5CD1312D945AE1951CFFC955893821EFD75724C67852E 70312'
    'runtime\amd-nr\assets\HIP\gfx1200\deep_wmma.hsaco' = '6B708483D0A9FCEE96967862E92AA5895BE969AB2316749EB5860CFE39BE0AF1 82616'
    'runtime\amd-nr\assets\HIP\gfx1200\multi-pass-predict.hsaco' = '68FA718EDD874CCFC14633FCABB57963F7EEA309896C60984B91C466A09BA2DC 14328'
    'runtime\amd-nr\assets\HIP\gfx1200\multi-pass-skin.hsaco' = 'C33DCD30746F7DE2E7BD442322CADF183C87EAA29E6C086ACF8757767CD26C53 12888'
    'runtime\amd-nr\assets\HIP\gfx1200\multihead-fast-packed.hsaco' = '8D602696B3D97D6DBAC759350951D744D4AA37578ABCDDE52BEE681EF68718C4 104904'
    'runtime\amd-nr\assets\HIP\gfx1200\multihead-fast-padded-wave-packed.hsaco' = 'C9C4B021BE277801FBC9AF7C06012F68570B0E34124ECB398CABD11ACA4A5123 1278832'
    'runtime\amd-nr\assets\HIP\gfx1200\multihead-fast-padded-wave.hsaco' = '266DB0F4F386C21590BCFA4C570AC3A1C5E99C7AEF25E1B0F3D4234560AE9328 1222008'
    'runtime\amd-nr\assets\HIP\gfx1200\multihead-fast.hsaco' = '04D80A8CF63D5B0D908ECFBC3D25D12A3E4087BE5FBC0ECB962D8E7FE8D6AEBA 106952'
    'runtime\amd-nr\assets\HIP\gfx1200\multihead-reference.hsaco' = '4B02890CAAD85A42C185180CFD65B5051CD01D8402F8EC21536D9779BA3144D6 66824'
    'runtime\amd-nr\assets\HIP\gfx1200\multihead-tiled.hsaco' = '10AA32A6735464D21F37E197423942E7D33333C667B035F8300B9819A8FCC88E 49072'
    'runtime\amd-nr\assets\HIP\gfx1200\multihead-wmma.hsaco' = '8C1A097BFA170EBA0C0CE51E44CC8F6F7521E2B64D6EFF3E07230DEB64D419AD 93264'
    'runtime\amd-nr\assets\HIP\gfx1200\multihead_fused_attention.hsaco' = '5B1CAD8F5B5061FBDF9D26C98C68785F4A57E359BFDAAB6B0A330E51F59DB10B 366880'
    'runtime\amd-nr\assets\HIP\gfx1200\prefix_fast.hsaco' = '724CF9A7329B8FE2FF8846D0CDA97E51D01A775B2CDBEF90F2F67286294FFDB7 77496'
    'runtime\amd-nr\assets\HIP\gfx1200\SHA256SUMS' = 'F18C336F8967AAB53AA1B2AC0AC167F13E73A7BD5EF1E23703F4B2B23DA74196 3598'
    'runtime\amd-nr\assets\HIP\gfx1200\swin-persistent-fast.hsaco' = 'F56EDFAFC74CCA3EA7A793CF5E1AB8F2B31967A20096443AF34250B39784E00B 1465736'
    'runtime\amd-nr\assets\HIP\gfx1200\swin-persistent.hsaco' = '3BEA7EAF652890B74E2688AD949E81320E3624319B4ADA06F90AD4F5BE44FA39 1296904'
    'runtime\amd-nr\assets\HIP\gfx1200\vit-stream-fast.hsaco' = '201B405C78ADE73E05A8EE827B84E9A3A04FF91F09F42389F91680C22E091189 723608'
    'runtime\amd-nr\assets\HIP\gfx1200\vit-stream.hsaco' = '1C12309DDD8D74201D432CA977EE0561BCDC78EDE3A473B82F58C229EA3BD8DE 712344'
    'runtime\amd-nr\assets\HIP\gfx1200\vit-wide-deep.hsaco' = 'CA48690DA20F6920EB050BA385BC8918E498DC40A1360770E7432AE74B92EE79 617008'
    'runtime\amd-nr\assets\HIP\gfx1200\wave-pointwise.hsaco' = '1CFF01355F32152834286C303D38EFD05628757C6D4E0FD7F1C0A0F3DB9E701F 46704'
    'runtime\amd-nr\assets\HIP\gfx1201\boundary-fast.hsaco' = '40FA66062ECA6BBE654E480944783C27B9BD93970696E1FFCA9BBC8773CA9700 50904'
    'runtime\amd-nr\assets\HIP\gfx1201\boundary_reference.hsaco' = '1B1A514743924D43542ED9AC30E4CA50EC6E84E62D2BAFAB1960AAF061E0ADC8 17832'
    'runtime\amd-nr\assets\HIP\gfx1201\c32-wave1-fast-norm900.hsaco' = '337A4E13C9A1242666E530CD43A49BBF278CD9927DF3FA0C792966EC5252DB85 821568'
    'runtime\amd-nr\assets\HIP\gfx1201\c32-wave1-fast.hsaco' = '8A5D9DDC139CFD44C242ACADE1B684810AF6A40633A0EBC378209CD510818E83 835904'
    'runtime\amd-nr\assets\HIP\gfx1201\c32-wave1-rtz.hsaco' = '5519BE636D94E97855E75665740AF3F2322494AE4797AD245903223310F08554 867648'
    'runtime\amd-nr\assets\HIP\gfx1201\c32-wave1.hsaco' = '4E6132A537447BFE7B29BA77597DA015D7866D7D9F3E9BDD91BA7919783D3D01 862272'
    'runtime\amd-nr\assets\HIP\gfx1201\c32_fast.hsaco' = 'A639601561123594F6D8B335F884F4E229338875F460213AF1396BC2F28C90EC 38056'
    'runtime\amd-nr\assets\HIP\gfx1201\c32_fast_attention.hsaco' = 'B3ABD5620764F5A5BA4D86FB439E0A57688E346AFDD5945F6F1D9067DA6D032C 29568'
    'runtime\amd-nr\assets\HIP\gfx1201\c32_fused_attention.hsaco' = '5091D35E5AC53E4A8D16DE2F5FB1C8C057159C72D6605FAEB5A5CA7DAA65AACD 39472'
    'runtime\amd-nr\assets\HIP\gfx1201\c32_fused_ffn_attention-packed.hsaco' = 'C0634176BF9E0A79FFB0E28FB120DF310A646D4DEDCAAD69C34F7185718D2155 219328'
    'runtime\amd-nr\assets\HIP\gfx1201\c32_fused_ffn_attention.hsaco' = 'AEDB066C95A76AA60FE67C91443C2CF8189C5E4F8E849356695CB5CD9FDC9870 286528'
    'runtime\amd-nr\assets\HIP\gfx1201\c32_prefix_reference.hsaco' = '65103D0C3930DD22D7EC40E282E38285D68169C9C8860DF7D561A7C04FEF7640 35784'
    'runtime\amd-nr\assets\HIP\gfx1201\c32_tiled.hsaco' = '6E7DB748FBD492CD566F208A1AA3B7E48A42A9C0D8B31022148469BB46D01E8F 31536'
    'runtime\amd-nr\assets\HIP\gfx1201\c32_wmma.hsaco' = '4D239CA134400EE2B42C60F99DC24202F9A4FD38C21A471C01A98217DF04B294 66504'
    'runtime\amd-nr\assets\HIP\gfx1201\c512-m32-deep.hsaco' = 'A6866439FA89E770D2DE82EF683B3E61E31819F4CB3618D50999B2FF14159828 647800'
    'runtime\amd-nr\assets\HIP\gfx1201\c512-m32-mh.hsaco' = '9AFA918BC6252AB1BEEE5BCD15ED6AB838C612CC8368D814B48E28B67A7079B4 1120200'
    'runtime\amd-nr\assets\HIP\gfx1201\c64-wave2-fast.hsaco' = '475F515FE8D5FA2D1DC679FC74017ADFD35B7BA38BE1FB6C56DC67F9CE7690E0 1898560'
    'runtime\amd-nr\assets\HIP\gfx1201\c64-wave2.hsaco' = '3B5273ABC6AFC577BC4FD4FBBF7FEE6667F2288F2B9CB21AD9E4A288821AF69C 1792320'
    'runtime\amd-nr\assets\HIP\gfx1201\deep_fast-packed-fast.hsaco' = '00E39349BC8A1A02E5A61B4606D1541D5ED1E4988DE8C17DB3CC5C8380C095CE 693928'
    'runtime\amd-nr\assets\HIP\gfx1201\deep_fast-packed.hsaco' = '30D4D5739C990F0EC7C9AE1050DC1201751C2138BD02079A6C3675EA9B5F5EBE 696480'
    'runtime\amd-nr\assets\HIP\gfx1201\deep_fast.hsaco' = 'A92CAB05B61A41985DB7294AE0A7B2043FB9B4C878F9B67BD5A3055177E3C257 609144'
    'runtime\amd-nr\assets\HIP\gfx1201\deep_reference.hsaco' = '0D2E3E9658A8ABC53BE68B6FC108CE57594784D3ED56F237BEDD9766D86DB895 70312'
    'runtime\amd-nr\assets\HIP\gfx1201\deep_wmma.hsaco' = 'B562C8986300B0517818AE54A46108E725E1ECC6C6B6C88A3B7FA14C749A5B2F 82616'
    'runtime\amd-nr\assets\HIP\gfx1201\multi-pass-predict.hsaco' = '133F0975E39E7E716E146DAB15F05573D6767C8C1C99E01528F3C11509293900 14328'
    'runtime\amd-nr\assets\HIP\gfx1201\multi-pass-skin.hsaco' = '5FF15F49135BEEF3BC68D744B9D33F615812CE293D13C0783C88B0A3D9AAA011 12888'
    'runtime\amd-nr\assets\HIP\gfx1201\multihead-fast-packed.hsaco' = '31E258756683B9A60E455C1C315C7B22B9605D90AE784891073FB6C74BA8EAD7 104904'
    'runtime\amd-nr\assets\HIP\gfx1201\multihead-fast-padded-wave-packed.hsaco' = '71D2375CC719F151CA01C36CFD93D30574159D677F6C460C70E11ADF476FF2DA 1278832'
    'runtime\amd-nr\assets\HIP\gfx1201\multihead-fast-padded-wave.hsaco' = '85149EE55B0BB52365FC3F15AC0D7E8B54917D1610319B5D87E4E31866D070A7 1222008'
    'runtime\amd-nr\assets\HIP\gfx1201\multihead-fast.hsaco' = '3493FB0EDE45F233A5D09472BD7A76D7532387F1FD6B0C835D46C20C0D196C9A 106952'
    'runtime\amd-nr\assets\HIP\gfx1201\multihead-reference.hsaco' = '1CFA639E9F37B7872C69D738C7332B58EAAF0539CAF720CC29A844E770FC8CBE 66824'
    'runtime\amd-nr\assets\HIP\gfx1201\multihead-tiled.hsaco' = '3799AE40ECEC89796132BF6464A76FAF1ADFFFE65DF2F5E6406F47A86F67AA81 49072'
    'runtime\amd-nr\assets\HIP\gfx1201\multihead-wmma.hsaco' = 'BFC99774435FCF89985A795EA6E1C20DE3335053097B40206839ADF687D0E226 93264'
    'runtime\amd-nr\assets\HIP\gfx1201\multihead_fused_attention.hsaco' = 'F7F23560D490868EACB161D7E49C958548FAAA1D61384014EA0859B5E8CEF1B9 366880'
    'runtime\amd-nr\assets\HIP\gfx1201\prefix_fast.hsaco' = '81B3FACDA5A1512AF3940D7A82C5A2F6F2E5CC7A0F2428194F4420FC8B68A57C 77496'
    'runtime\amd-nr\assets\HIP\gfx1201\SHA256SUMS' = 'EF2A75748FEEE3D2BE27722461E4554342D3A47A0A46C141A9DBCB95D62FD712 3598'
    'runtime\amd-nr\assets\HIP\gfx1201\swin-persistent-fast.hsaco' = '9B58CAB4E4859645AA79C61D67972906D8DD76EB4D94C9491894461AF6BBEC39 1465736'
    'runtime\amd-nr\assets\HIP\gfx1201\swin-persistent.hsaco' = 'AD4600C9F824762BAF6F898988225A2C1169CAF88EC57B1E9C25CA0B03039AB8 1296904'
    'runtime\amd-nr\assets\HIP\gfx1201\vit-stream-fast.hsaco' = '8401ED6157F8ED840A1C070237F2DAE08762A84E04B7377F3E6ABB705B3DD2EE 723608'
    'runtime\amd-nr\assets\HIP\gfx1201\vit-stream.hsaco' = '7D570E3C92CC0F9D61E95D46D4651F11F3A562E84453E0B62DBDC93021B49E96 712344'
    'runtime\amd-nr\assets\HIP\gfx1201\vit-wide-deep.hsaco' = 'DADADB24EC212A592F41F0F74E777335929235996D84614D1AD76E1C808E2493 617008'
    'runtime\amd-nr\assets\HIP\gfx1201\wave-pointwise.hsaco' = '5E65693DA46044B697218194BB17EC8453952FA4A4A684C6ECC59DF1CECF12AA 46704'
    'runtime\amd-nr\assets\HIP\SHA256SUMS' = 'DB3AB5C5280BEE6F1BA82F51FC62F9BFB70387FDF56544551CB5BA7D42A62EF1 7836'
    'runtime\amd-nr\assets\hwc-to-vit.i32' = 'C942210AFD8FFC8A2A1E4ED81DF546E5E08D4C70E20F74FDDDC0FE564F224AB8 2621440'
    'runtime\amd-nr\assets\native-game-flags.txt' = '830255FBA2EAD4D86351C669C0A2CF92E6C99B325BC8296D5946F42DC9A6A97B 738'
    'runtime\amd-nr\assets\native_black_probe.hlsl' = '629ABDD90D2E7E0ACFD3241F3CDF0DD00DB8B5D94EEBB3B8895B5EF3F5AD5869 1357'
    'runtime\amd-nr\assets\native_c32_ds.hlsl' = '566FBC274C9D4FE9D6E094E186870F7642BCE282686CB5F9039DF902CBA5756A 1012'
    'runtime\amd-nr\assets\native_c32_ffn_fused.hlsli' = '8398DD3CBFB4AB450E772DAC561E94E5F7084DAACAC818B721509D28620F46B0 10033'
    'runtime\amd-nr\assets\native_c32_reframe.hlsl' = '261EDC63A479C05F794E4B8E288E166407E46C1572B81FA831781604D4621080 1102'
    'runtime\amd-nr\assets\native_c64.hlsl' = 'EFB60CD42B882825FF6FD027D77D35CBA90E93EFE32C5E6EFE49BBCD15A94D96 18387'
    'runtime\amd-nr\assets\native_c64_ds.hlsl' = '1D7087115A893AEFAB6500C1B61A142F02AE3C42D13FC0EB6D9D13D1921705FD 1603'
    'runtime\amd-nr\assets\native_c64_shift.hlsl' = '75C9E3982AC6BCA504AC781BADC16BE7070BA62613E07ABADA668918E48C02E1 1850'
    'runtime\amd-nr\assets\native_codec_decode.hlsl' = 'BE75A67F508E55B1F3B779F891D4A3B7B87A60D7BECAFAA9C1767C830E5CE579 10229'
    'runtime\amd-nr\assets\native_codec_encode.hlsl' = '81F2754B8734792768F27E35206CA8454C8729A1FE3827E80E6358B01E3070C0 5321'
    'runtime\amd-nr\assets\native_format_convert.hlsl' = 'DE81723FEBB8462E362ACCEBADA740BCDB6180B02372199C4E200B2E48D6983B 685'
    'runtime\amd-nr\assets\native_fp8_fast.hlsli' = 'DC04B0FAC0CE3D80BE147C5FF5C6C97FABEE8691D25AC67EF033F66B91F7C5B7 484'
    'runtime\amd-nr\assets\native_game_rgb_input.hlsl' = '4CEE56A655033D85D836E2F343DE020833611B3580AC6410ED5649FF0490BBA6 1507'
    'runtime\amd-nr\assets\native_half_corrected.hlsli' = '46D539D9B461A30F925893AFC0E4A6DFAD4C1FF0A66E36E677F2391B5516E05B 598'
    'runtime\amd-nr\assets\native_half_square.hlsli' = 'E2B806FC8DC6934439DB12219FE819B3912E88FA13BA8BE2E72E0A615AD0E1D5 785'
    'runtime\amd-nr\assets\native_head_pool.cso' = '3EBA707644E4F6824F8AF4A4147F58D8B0FA8CD4E2B64FE31F6CA368D4E69056 5248'
    'runtime\amd-nr\assets\native_head_pool.hlsl' = '85583B5509F4E19DBF40BFCAC206517E7799A0F576FDE7A03685C7BD018194E9 1321'
    'runtime\amd-nr\assets\native_head_pool_c128.cso' = '13A1A2A5CE5CCD3DF4AAC37440DA736AFF2B427CA24751F9F39E75C1BEB7DB0F 5248'
    'runtime\amd-nr\assets\native_head_pool_c256.cso' = '40750C8A1966775B60411C96190D2C7CF340AD10B652A0A71C4BF137D8F65109 5248'
    'runtime\amd-nr\assets\native_head_pool_c64.cso' = 'AABF024AF6873D3D22E98523A8D7F3D0F78CC3E830A8C61E36CFD37A758F49D9 5244'
    'runtime\amd-nr\assets\native_history_guard.hlsl' = '4FEAE5DE6551BCD0502C80A3F92CB38DBE4DCAEC5F5353AAF537B6884F9DE038 1073'
    'runtime\amd-nr\assets\native_matrix_expand.hlsl' = '5F731484ECBDFE0AD775BDC5FEA7040325DF36896944D55D3FA05891B7F0E4BC 1740'
    'runtime\amd-nr\assets\native_matrix_expand_packed.cso' = '44E2601986EDC9756EFF6EC2BE4F24CB4958C4A343BD9B494268573AF397AD58 33896'
    'runtime\amd-nr\assets\native_matrix_expand_packed_c128.cso' = 'A6EE6739BC6C242BB3E11A9C7B71ACDEE890364B81A6F24C07539FE7621F2976 33896'
    'runtime\amd-nr\assets\native_matrix_expand_packed_c64.cso' = '1901497BE62C7064FFA0325B38FC85601C67F6BEF7819083EABAD20E85EE2147 33896'
    'runtime\amd-nr\assets\native_matrix_pack.cso' = '74949E7E03D0BA45EB4B7E5061C03EA71E280796E4D701B786B532CC7780C00F 4112'
    'runtime\amd-nr\assets\native_matrix_pack.hlsl' = '7C8029F75C8532D3D6289712A578004603BD183CEE6F8D0F8778DCA8AC3F926A 2656'
    'runtime\amd-nr\assets\native_matrix_pack_c128.cso' = '0409B6B7DD9C9AFF6F57F5C693016A8F0256C44CBD462D027FEA18271A059AB7 4112'
    'runtime\amd-nr\assets\native_matrix_pack_c512.cso' = '76F07BADACFCA167F691B68BDC46A8D6115F0ADE7E95D1B7E070C95059CEDE8A 4112'
    'runtime\amd-nr\assets\native_matrix_pack_c64.cso' = 'C198E59D80CAC91C0DDE2058FD6A6FAFCFE0DB18293B5191320F8F56962960F4 4112'
    'runtime\amd-nr\assets\native_matrix_pack_fp8.cso' = '31B9465EBE1F28C01CE36162E1FC18765B13F4EC0EB6E685E10D49A186BC19F0 4656'
    'runtime\amd-nr\assets\native_matrix_pack_fp8_c128.cso' = 'F4E2EF0800ABB60C8D382453CB69A42FC9D3B940224487181750942CB8D725FE 4660'
    'runtime\amd-nr\assets\native_matrix_pack_fp8_c512.cso' = '227A5E2BEEDE4BC2ACE240776FCBC24FC84D9D8D865F85BD92562F41C035D071 4656'
    'runtime\amd-nr\assets\native_matrix_pack_fp8_c64.cso' = '3D79EA6ABA148DFE466C7B9AEF0454A21A969055BD739CF588AE537786AF07D1 4656'
    'runtime\amd-nr\assets\native_matrix_pack_fp8_mapped.cso' = '1FDCE512D9D754356B10102567F99C0D757391FD6C43F51DCBC1EFAB3AA930A5 4880'
    'runtime\amd-nr\assets\native_matrix_pack_fp8_mapped_c128.cso' = '37BE8306A023783836B8B5086AD0FAE1D17FF5BAD459136340FAE257F574D9DC 4880'
    'runtime\amd-nr\assets\native_matrix_pack_fp8_mapped_c64.cso' = '086C80800A4C4DA708D733418D266C5F0042522C14EB3965C2E9C0592D6FB521 4876'
    'runtime\amd-nr\assets\native_matrix_pack_fp8_mapped_src16_c128.cso' = '92A52CAEEA1002420C9A965C178F1E1B5774530EFA1A944AFDCA3BD8E7EEDE2B 4860'
    'runtime\amd-nr\assets\native_matrix_pack_fp8_mapped_src16_c64.cso' = '196493257AD40D47E60EC52DD3C2C603FD3D51C534CA63378B0B42436E43B0AF 4856'
    'runtime\amd-nr\assets\native_matrix_pack_fp8_mapped_src8.cso' = '2A80058B5815B8A5CBD38B7EC28132CBF53D0715BD8042F35EFE21E2F744961D 4060'
    'runtime\amd-nr\assets\native_matrix_pack_fp8_mapped_src8_c128.cso' = '4B7FC2CAE3CD7ADB24B7087D75BFC09F19B59690B8AC2D01DE0C376650A8FA76 4064'
    'runtime\amd-nr\assets\native_matrix_pack_fp8_mapped_src8_c64.cso' = '08F8F9822C3555AC56D3815C46EE8948D84EA487E3E1AEB596D2B09A5AB87E9E 4060'
    'runtime\amd-nr\assets\native_matrix_pack_mapped.cso' = 'B5E31A89D995B971617C0964CD80B07091B0C587E8CB3CBE1C97B5A6D8ED892D 4320'
    'runtime\amd-nr\assets\native_matrix_pack_mapped_c128.cso' = '03363F35B46094EB8A25F43C6F53BEA8B23F0A9DC59C49786A52C4080BA9E03F 4320'
    'runtime\amd-nr\assets\native_matrix_pack_mapped_c64.cso' = 'F5BAC58DB08439D189E419231B6BD65B7369164D38F61A48E0CA646B6E35C0E2 4320'
    'runtime\amd-nr\assets\native_matrix_qkv.cso' = '2DA1AFBB869F54C4DAF281EAE539C8BBF957DC2F170FF17AF6D3C505F6663B01 11964'
    'runtime\amd-nr\assets\native_matrix_qkv.hlsl' = '1300B10FFED96248455EBB4404AA40B7E45AD5E940E6763FDF0FCA658E53085F 1461'
    'runtime\amd-nr\assets\native_matrix_qkv_c128.cso' = '006086D0F9CEA6CE3CE03069E67363D18CC4686837E1D90E3E9E5E633AD7C7D0 12012'
    'runtime\amd-nr\assets\native_matrix_qkv_c512.cso' = 'C4E40F667AB56ACA35B9F591E1EBCC114712292DAC9F2280D01A41529B2D8061 12012'
    'runtime\amd-nr\assets\native_matrix_qkv_c64.cso' = '572C841B8E44B205E847C50D7B173D60778BDDE0FF9E913DF5062518CEA3A70D 12012'
    'runtime\amd-nr\assets\native_output_smooth.hlsl' = 'AC701903003966067E855F1105EAC5F03FC627761E42BE187ECFADCF0976990D 1205'
    'runtime\amd-nr\assets\native_post70.hlsl' = 'FC2D18F313DE670D78750937060B4A57CC81EBB149A63DBE384C06B26CFFAD3C 5136'
    'runtime\amd-nr\assets\native_rgb_reflect.hlsl' = 'A716CB3A7DD7474DFCB1AD988EA7CD38483FB0AC6CB6605F41E22DB3F12A7FC4 815'
    'runtime\amd-nr\assets\native_rgb_texture.hlsl' = '529B18C99F48C5A09AA05BFCDF239C157660FB5ADCA87E834FE24D83C640BB7D 569'
    'runtime\amd-nr\assets\native_sat_cast.hlsli' = 'C6B4CD56AF79D3646082D06214AEB32BFBA1160CCEB17BE2547134CB28A21FB2 793'
    'runtime\amd-nr\assets\native_scaled_integer_half.hlsli' = '4EA8D0640B66CB506F60FAF68ECF87513656B884149CB38F836B152A593ECA41 731'
    'runtime\amd-nr\assets\native_split.hlsl' = '84A422A8D2B92750DE3241A6F859849C3840FBFF3A30D85FF294C390F31D5C08 3216'
    'runtime\amd-nr\assets\native_split_window.hlsl' = '1509A41B265367D824FDE264FEF5EF18A1409A73E9888EEC7D218684C1EBB3AC 909'
    'runtime\amd-nr\assets\native_temporal_coordinates.hlsl' = '2180062886B0D6DC9C7938E8F4AC09AE562543C142CBE8ACBB7745C469AC29CA 3795'
    'runtime\amd-nr\assets\native_temporal_feed.hlsl' = '54E3A779015AA7A78B4F7B407CBC754F1FDAAD2AC8857F6E3C4C871DA414EE48 1413'
    'runtime\amd-nr\assets\native_temporal_sample.hlsl' = 'A0E05CA09E7E6FAD757825EE93E0BEC48BCC87FA408AAB544133F37EE24F56BD 7633'
    'runtime\amd-nr\assets\native_text_overlay.hlsl' = '14E58C01D2A4CE7944E09ADDFF501E57B96EDE3A332F20627DCE383D7D987D91 4034'
    'runtime\amd-nr\assets\native_vit_attention.hlsl' = '1618C27F2A10D753941CBD1B8B0A5E3301FE6EC769AE9C130CE5A61FEEABCACA 2182'
    'runtime\amd-nr\assets\native_vit_gather.hlsl' = '9DB00368BE0E1120FD882199FBE32614DD82ACFF90839A16172A7B74878C9534 289'
    'runtime\amd-nr\assets\native_vit_linear.hlsl' = '13C1ECD8737144975A850B5378FFF97AD7CE2A97DB99C0942F2E7C312FC4F631 4122'
    'runtime\amd-nr\assets\native_vit_pack.hlsl' = 'B78A889AFA5A7A851AE9C1EC5FBCA141CC76EC9855856F82CF735ACD0C0349DC 1406'
    'runtime\amd-nr\assets\native_vit_pack16.cso' = '4F5C6D1363FC2F3F21A621459E5FAF1468566C4870491E9BE4B9039ACDBF36E1 3968'
    'runtime\amd-nr\assets\native_vit_pack8.cso' = '9AA30D3AA868C9273FC3A2FF9EC62EAE9E7030345896AEB787B3440F7B181BFC 4520'
    'runtime\amd-nr\assets\native_vit_qkv.hlsl' = '05F11F152DB441466F6FB26EEC4508022A97E683D559173C0BBCE422034508F8 3032'
    'runtime\amd-nr\assets\native_wave_attention_direct.cso' = 'EBE3FBD0897E8E2BB3C31C200275F9DA202E7F35DBA3BF3AFA670F4C3A974EC7 8732'
    'runtime\amd-nr\assets\native_wave_attention_direct.hlsl' = '30CDD2A9890B84D1989418BFE957FF6C239CC875F20E9F5F0128C512BAA23280 14467'
    'runtime\amd-nr\assets\native_wave_attention_direct_c128.cso' = 'FDC9FF2E7C9E89522354717A4F4B0AE177B3F7384EF67F493B86AF2E5F445A24 8728'
    'runtime\amd-nr\assets\native_wave_attention_direct_c64.cso' = 'F4737FC8E8C1D6EE0E5A7CD4F32D18381E306D8703F86D509CC7EA73F1085DDD 8732'
    'runtime\amd-nr\assets\native_wave_attention_direct_fp8act.cso' = '3E3C6C13012CAE4CC31F8C3E3CD46768357AD0616EA02E7EBF8FD41F7F9486E5 9032'
    'runtime\amd-nr\assets\native_wave_attention_direct_fp8act_c128.cso' = 'F06CB74D5232C9CDC50E89765F8593ECAB2915FDDEC98F0FC8AF015DB591C79D 9028'
    'runtime\amd-nr\assets\native_wave_attention_direct_fp8act_c64.cso' = '1215AAFC974F1914237E7F188F7AA74E3E0CDF11671977B4235B41584EAFF939 9028'
    'runtime\amd-nr\assets\native_wave_attention_direct_fp8qkv_fp8act.cso' = '527A43BAA7D99857498758F8F58242B7387F574AC4DED8F29809DDEC895F0516 10132'
    'runtime\amd-nr\assets\native_wave_attention_direct_fp8qkv_fp8act_c128.cso' = '4894AE31FFACC06692243FF54FC880562B8A12A71781431042896A0C8DFCAD09 10132'
    'runtime\amd-nr\assets\native_wave_attention_direct_fp8qkv_fp8act_c512.cso' = 'D729BD9E77C97649BBBEE94B0D1EAA9825CD121D6921CED630FB12A14D4FC670 10136'
    'runtime\amd-nr\assets\native_wave_attention_direct_fp8qkv_fp8act_c64.cso' = '8354C047D3A1867203780CE929594F24F8887465656623A8EA60D5F3F6EC93F0 10132'
    'runtime\amd-nr\assets\native_wave_attention_fused_qkv.cso' = '7D4704732FE4D7B453F75C8831A64981F6D6E59395A21F7B06789840B71A1D54 15564'
    'runtime\amd-nr\assets\native_wave_attention_fused_qkv.hlsl' = '501492BE980260678AD1D1D5111FFFB8E38693F220ABD05A5EBE3778EDD722E3 7258'
    'runtime\amd-nr\assets\native_wave_attention_fused_qkv_c128.cso' = 'C663E897702A65D0423410E981BDF0F916D2228173E2DA8FF59DA2C23D9EDA4A 15568'
    'runtime\amd-nr\assets\native_wave_attention_fused_qkv_c64.cso' = '24070B467499A98391E8102751D45E2065E027FDA5E330E3BA006E22D1E86E86 15560'
    'runtime\amd-nr\assets\native_wave_attention_normalize.cso' = '43F3D34AD0DD39B98AE1E77ECABD75AAFB64827E5213892D208E3AB7EBBA1757 5212'
    'runtime\amd-nr\assets\native_wave_attention_normalize_c128.cso' = '0A415109CF1CE76A93A8C2EED1CE0AEF52CA6B7DA18CF0F737986E1DFDA942A2 5212'
    'runtime\amd-nr\assets\native_wave_attention_normalize_c64.cso' = '147202F3C2E4E6E2465722284AD5D786B93C662825D7D03ACB97C7B00AAD8A0D 5212'
    'runtime\amd-nr\assets\native_wave_av.cso' = 'AC98B8255F45E33D72F68ACB10190083CC1CCCF8D77457DA85168615D6BCCE92 46080'
    'runtime\amd-nr\assets\native_wave_av_c128.cso' = '78FDDEF775BB8CE2565E76A656B3B7F02DCC273A445A581901D4DD62AB6AF6DF 46084'
    'runtime\amd-nr\assets\native_wave_av_c64.cso' = '999CE73E44416F758BE7C02EF5B6D77B38FE7226EDA6F265AD5CEC2D6521D4AF 46084'
    'runtime\amd-nr\assets\native_wave_c32_av.cso' = '4C40ED3F5A240F445C79E0D68B759A995C4B052236988F854A1A35A314C64E08 46300'
    'runtime\amd-nr\assets\native_wave_c32_ds.cso' = 'FC200ED5C8B8DB91EE94D90D91C13D90A1525A62501EE1C1EF201F820F86C1E8 8036'
    'runtime\amd-nr\assets\native_wave_c32_ds.hlsl' = '87B49A034B6EEAEA44603F98973E4A361EEDB9B530891294692EE0CCE0257D7D 2444'
    'runtime\amd-nr\assets\native_wave_c32_ffn_blocked.cso' = '4B185E50DFB909C0C5078ACFD9DC3F8208E5A0339B66F355B78A028EC86484B5 38452'
    'runtime\amd-nr\assets\native_wave_c32_ffn_blocked.hlsl' = '4AA4439A930A84008152A4D13DF94758AA3503B37FC8342EC0BC0CA14896FB29 20070'
    'runtime\amd-nr\assets\native_wave_c32_ffn_local.cso' = '94676009A171E68D67971C54CCAC88E4443A92C2C799A1AACEDAC72D2A926719 8768'
    'runtime\amd-nr\assets\native_wave_c32_ffn_local.hlsl' = 'A6BA3DF867D13FDCD0372194E4A70A2D047D2382505E464EBD8EB5A7454809EE 2864'
    'runtime\amd-nr\assets\native_wave_c32_full_attention.cso' = 'E55211C56378EBE2DDC6430417C955FF245756226D7808CEC9676792B94D523C 46852'
    'runtime\amd-nr\assets\native_wave_c32_fused_attention.cso' = 'CA1A013E2DEB6ED176A10D543FB7A64D4F6421D0A7042987CA0358848874E4A6 26456'
    'runtime\amd-nr\assets\native_wave_c32_fused_attention_ffn.cso' = '0C10F1D6A6B09F935BDD7E879BFD3AA9B1AEB1E40D4529236ABFD813AD5DE257 46728'
    'runtime\amd-nr\assets\native_wave_c32_fused_qkv.cso' = 'FB9AF7545E50E5A6EACB8B071B76B4D6FAF6193479B1858BD296EE19393C3503 12388'
    'runtime\amd-nr\assets\native_wave_c32_qkv.cso' = '396A2C7D7070C9C5B15A0A0E3B559648464D2ABE9AFA32C0E5632CB614757C52 44420'
    'runtime\amd-nr\assets\native_wave_c32_scores.cso' = '6C547731AF2D4F4083A6C680AD08BC2EBCC80AE00F707BC3BBE4F0E28A4EE76A 53228'
    'runtime\amd-nr\assets\native_wave_c32_split_attention.cso' = '484189DFF737BA4174986C109103DBA09045550AFEF32A2966E33D2BCA2A4D9C 9232'
    'runtime\amd-nr\assets\native_wave_c32_split_attention.hlsl' = '783D56FCCD51345234FF49E465CADFAE263B324EC71A097E6E76074601625EFC 49170'
    'runtime\amd-nr\assets\native_wave_c32_split_normalize.cso' = 'E6CB11702A15C7E0837AB6A18939F48E1F429E57AF5150BD3EB84DC7C4A753BD 5108'
    'runtime\amd-nr\assets\native_wave_c32_split_projection.cso' = '9014C94D7437F60387197941FF2E1563774049DB4355E967D96CBDE6802F0A5B 6856'
    'runtime\amd-nr\assets\native_wave_c32_split_qkv.cso' = '46ED1D5D953F7C0EF32EA5FFE540ACDD3B6D7E0DC1F8A86F85C92FB631F5D6DB 9808'
    'runtime\amd-nr\assets\native_wave_contract.cso' = '182DF73D394844AF887EBE47A8A81537CE59ECD8AFAA03E6666ED108B793E1A8 6812'
    'runtime\amd-nr\assets\native_wave_contract.hlsl' = 'E13F9F9B2564ABCA01068B1A95DC5973346765B4103D11C5316B029F229BD9F4 2458'
    'runtime\amd-nr\assets\native_wave_contract_c128.cso' = 'F0EB17E0E09A1975DF8995DC6D68F2F853A8793D8B6E3453CEE2E97AF5C02FC3 6808'
    'runtime\amd-nr\assets\native_wave_contract_c64.cso' = 'C46FB7CDA2C453108C96991520510D120C79298D310FBCE8BD07B235C5C551A3 6808'
    'runtime\amd-nr\assets\native_wave_decoder_1024_512.cso' = '94036BC76F1B5DB8A300BB955490C911EB3582F80DCE2EDDFCE1F5B967CDD54C 85424'
    'runtime\amd-nr\assets\native_wave_decoder_128_64.cso' = '237986908DFEF322328B41B620E4EA1500FB90FBB0E4EF2B8AD0F7B439F6DFA7 84736'
    'runtime\amd-nr\assets\native_wave_decoder_128_64_out16.cso' = '28B6A7544CA574CC53FEBD1689BD7D3122D3E57A9B935AE0D784BA84B2E1F9B5 86584'
    'runtime\amd-nr\assets\native_wave_decoder_256_128.cso' = '85C35FD8A4D80B8B28E2DBA02C03A72324F7F01432A3E79FFD93146F109A588D 84744'
    'runtime\amd-nr\assets\native_wave_decoder_256_128_out16.cso' = 'DE333914114FC8D10F207637489BB9AA2997A1E6D43B35D1E9A36E19230C8065 86592'
    'runtime\amd-nr\assets\native_wave_decoder_512_256.cso' = 'B478F1BCE6DDF5DDB9DD43AF17E71ED1D884E36D86F850C627C9EB74F3262D96 84740'
    'runtime\amd-nr\assets\native_wave_decoder_64_32.cso' = '4DD5A1FF93FCB9E1C6D041B83E08C3D385D8246FDA4F7FDBC458B1E597E0408E 34192'
    'runtime\amd-nr\assets\native_wave_decoder_64_32_out16.cso' = '397294FEE758647E053BC6E145FB108A56C58FD8995DA260418793283A69CF3F 35160'
    'runtime\amd-nr\assets\native_wave_decoder_64_32_skip8.cso' = '040F87E568EAA5D283AD444F7DCEA473CF1A90C2ED2C9282C31158FD43A22F56 30168'
    'runtime\amd-nr\assets\native_wave_decoder_64_32_skip8_out16.cso' = '5A65611498A7470D72812EDCFFE724031F7DF138F15686D87063CC9236227D5C 31148'
    'runtime\amd-nr\assets\native_wave_decoder_entry.hlsl' = 'F8C63E707F0F5546561D53AD84A11D9D6E2AFAD1801BC16D090C8F8B736B3715 9142'
    'runtime\amd-nr\assets\native_wave_expand.cso' = '11C1A82AA85D04591D8DD0D8446893864A14A0C159DD0BD243E278F2B8CA7BD2 6528'
    'runtime\amd-nr\assets\native_wave_expand.hlsl' = '19045B550720A05AB44275AB0B9C95EC6AEFC18E24243DBE27E2556B19C33A12 1963'
    'runtime\amd-nr\assets\native_wave_expand_c128.cso' = '45E1562E2A85A47B39E5C2DA9C5ACCE6AEF3AFC816631310D571098BB1B4E9FB 6524'
    'runtime\amd-nr\assets\native_wave_expand_c64.cso' = 'C06FC435A076D24AAA9010015A35858149BA5DBF6D46FCC3F05606C3402C1BC8 6524'
    'runtime\amd-nr\assets\native_wave_ffn_blocked.hlsl' = '53265B59EBFA8DFB0705D12C6F6C78CC9E533658F48455F27BA998866FD853B0 5607'
    'runtime\amd-nr\assets\native_wave_ffn_contract_blocked.cso' = 'D98D34918E658B8A592DE40879D62111C55E9C97502E650E3BC8B8F40AD68CCE 7464'
    'runtime\amd-nr\assets\native_wave_ffn_contract_blocked_c128.cso' = 'FD04A7FF19D430CD04D394298436CC5894A4178FFF097D1CEB22CDBF8D287525 7464'
    'runtime\amd-nr\assets\native_wave_ffn_contract_blocked_c64.cso' = '05D0549D1A77B2BB12B5B71C0D5A45FF9729665F7467FA3DC81E6C391389F2B7 7464'
    'runtime\amd-nr\assets\native_wave_ffn_expand_blocked.cso' = 'CFAC91C07CB36CB4C210299417887D27C49E38C59C8A1BF2592DC37305B6ECD1 7448'
    'runtime\amd-nr\assets\native_wave_ffn_expand_blocked_c128.cso' = 'F3472D389711B3B776D26F00716755235A4CCB4351CE080F2C041245DC3761C5 7444'
    'runtime\amd-nr\assets\native_wave_ffn_expand_blocked_c64.cso' = '1966149C019B9C91B64560BF453D18E3E807EE3757BD4A5471E3EDE6422DADD3 7444'
    'runtime\amd-nr\assets\native_wave_ffn_fused.cso' = 'ED68BDBFDA9DA7AD7066D00A7B178BB6077D8D55523E5393198884E704263263 8696'
    'runtime\amd-nr\assets\native_wave_ffn_fused.hlsl' = '329A63031874C6D039CC1E260B96799E14AB35D4804554EDC7723B13875F7669 5114'
    'runtime\amd-nr\assets\native_wave_ffn_fused_c128.cso' = 'F4384E89FE2D26FC211F3E2A0AE6C628FE92150133CB29B4E005541F98FDD033 8692'
    'runtime\amd-nr\assets\native_wave_ffn_fused_c64.cso' = 'B8192D1BA8DF69505C9744591D491712EEDCEEADC85624E50FA4EA39796E8B85 8692'
    'runtime\amd-nr\assets\native_wave_ffn_fused_fp8act.cso' = '49987FC139364FC47D0E3B234C4D0A6EC70F408B5E74EDAE301E8A8670844B07 8888'
    'runtime\amd-nr\assets\native_wave_ffn_fused_fp8act_c128.cso' = '95B7F51515A11F193CF770E72D56397963A3842C15E221398B6353BC77331880 8872'
    'runtime\amd-nr\assets\native_wave_ffn_fused_fp8act_c64.cso' = '03251FA35011721457C6449BD0DD6D028BF14BC7F62A1127F4CA6E18B0241A8B 8876'
    'runtime\amd-nr\assets\native_wave_ffn_proj0_fused.cso' = '556DE1E06A9D5A5E858D2AAF93DDA9467845AAD33E7B814652F411F0DDC8A72B 10308'
    'runtime\amd-nr\assets\native_wave_ffn_proj0_fused.hlsl' = '145FF958B6055301FD9370CB8DB5F870BC7790EEDCD1DAF6224EC426D3DD5168 6225'
    'runtime\amd-nr\assets\native_wave_ffn_proj0_fused_c128.cso' = '826CD1C4CAAE896C7B570EC6C9A89C378B900AD456FF98E34EA68C8AC55399FE 10296'
    'runtime\amd-nr\assets\native_wave_ffn_proj0_fused_c64.cso' = '4E8BA8183F876A0DABB3CA41AB05B9BC9387C462294C8630FF6962C287F5F1A7 10296'
    'runtime\amd-nr\assets\native_wave_ffn_proj0_fused_f8in.cso' = '3B9361C591DFD11C00FF55DBCBB7D5BFE43DD6BE2EE393070D3FDC97299F76FE 10100'
    'runtime\amd-nr\assets\native_wave_ffn_proj0_fused_f8in_c128.cso' = '51B4959246A2D6FC71EEEA4446A23A734CB37CDB8AED555567D55246846CD3EA 10084'
    'runtime\amd-nr\assets\native_wave_ffn_proj0_fused_f8in_c64.cso' = '398DBA7B0C18800232F0E9335D43D4DA03F43E22C85BC5DA7A9CC9940F125845 10084'
    'runtime\amd-nr\assets\native_wave_head_project.cso' = '950053A75661E002D4A3087E425F15271EEF3A89A1B433C2B961379B849CC5A9 5960'
    'runtime\amd-nr\assets\native_wave_head_project.hlsl' = '5C7B49867E55796A08536C7459C0777595629B278EBDC7A972021E2F3CB7299D 1834'
    'runtime\amd-nr\assets\native_wave_head_project_c128.cso' = '68B90DDF5AFB0648019926C4F54404D9BD0B8165D1A942667ECE3BA40E0F3783 5960'
    'runtime\amd-nr\assets\native_wave_head_project_c256.cso' = '50F9B9A6ED3FCE6DE3E3AEA906512FDA72D692C1075ED875CA6B38E59A91DD7F 5960'
    'runtime\amd-nr\assets\native_wave_head_project_c64.cso' = 'DFF3C3CB9C668E55CF938B95229CA01CDFF9E0685023789319BD16143539BE04 5956'
    'runtime\amd-nr\assets\native_wave_prefix.cso' = '21BDFCD87C152F5D36E2E17E11ED4FA63D0901BE913F19E697CAFFCF8A2DB6F7 12812'
    'runtime\amd-nr\assets\native_wave_prefix.hlsl' = '52BE40D309975AC6DE780758931A52F0FF1C38CDCFBA7E32508600F380A93A24 4373'
    'runtime\amd-nr\assets\native_wave_project.cso' = 'F2E9FBFC03743028483EE0FD99CD1053387EA3F4999A2BBED7C023183691D2E7 10724'
    'runtime\amd-nr\assets\native_wave_project.hlsl' = 'FB26D68791407EE91E7F49D72F60A8720A59E377D6A326DCD875D7DB4623D981 11755'
    'runtime\amd-nr\assets\native_wave_project_c128.cso' = 'E5B387F0B1979E309AA3D906E5E69E5318F5AA7F0A89D348A2A6F785A156162A 10720'
    'runtime\amd-nr\assets\native_wave_project_c512.cso' = '796EDEE56184192E0A337E7CAAABE69EFA91D0910C5F2ACCCF35B1AAD1660B3C 9520'
    'runtime\amd-nr\assets\native_wave_project_c64.cso' = '4F4D2E670C734FA593CADEF03D9C3C75940A12161A9DE5025623AD88078A47EF 10720'
    'runtime\amd-nr\assets\native_wave_project_fp8act.cso' = '7E5A9BE658EE87D9CB40C720077DDE4FF5D32CC6EDF36DD00E04303377024499 8716'
    'runtime\amd-nr\assets\native_wave_project_fp8act_c128.cso' = '51441623582085AAFBB6C66AF1244B8460C1657B7C8249CA28202B6071359DF8 8716'
    'runtime\amd-nr\assets\native_wave_project_fp8act_c512.cso' = '761021FB6DD9F45708368A03528FD1E8F4F41FB9DBC33EA64D8C9A4318603893 8700'
    'runtime\amd-nr\assets\native_wave_project_fp8act_c64.cso' = '294B96E6CF7CC118F3EA31437F6021AB0FF350B8DC6235B87A843A7AD44F5C14 8712'
    'runtime\amd-nr\assets\native_wave_project_mapfeature.cso' = 'E4118BFFA2CAB86A7A992309AE9858F07F048A179975737B47A0508865AEBF66 11224'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_c128.cso' = '891557FBB4F780DD26EE6A7170A2C2CA02A210B859D59C46714ABDBEA63786D2 11220'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_c64.cso' = 'FAE8924A0B4DF84F651338503051D36356B0C1EBB8AF7D7E1B775DBF6CB3BF50 11220'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_fp8act.cso' = '749A39DD3EF4244B7ABBF9475F371A04003AFFEA99617DD8EE959B193FEC1AFA 9228'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_fp8act_c128.cso' = 'ABC5D57D26B7BCD723389F3DC493A117A83CE94A1E30B03692D80F1AF0B76781 9232'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_fp8act_c64.cso' = 'AA069EAA2FB6C2592C9CD356C6EFDFEF3EDBBE304DE3F6CCD81BBAE1891F126B 9228'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_fp8act_f16in_f8out_c128.cso' = '4123A338930A98F34434FBF46D9DCD543F87C6A1E356702A338EC29B744ACA2E 8632'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_fp8act_f16in_f8out_c64.cso' = 'E32C904DA3EC36C423048D36E9CD5158E52159DB12DD823406DE5731D89C6661 8628'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_fp8act_f8in_f8out.cso' = '0DCB6FC42AAE62D53516EDDA3CA26D63F40B4619C7DC432450B2FED38E6CDEAD 8836'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_fp8act_f8in_f8out_c128.cso' = '11476E29F9A91031655A343039EA00BC57DC1C7EE3B9924E3819C001B547C0CC 8832'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_fp8act_f8in_f8out_c64.cso' = '502DED04C73523C22D558D7B86B2D06C4CC349C735B1F1AC0133E49BA57906A0 8832'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_fp8act_f8out.cso' = '687BE15E03FFDF5102D43276E9565EAFE88E0D3E014DF16D19D6EC8CD0299506 8784'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_fp8act_f8out_c128.cso' = 'BA76D20F035926DB64F16D9AE7651542B6292D6746FC05C62BD58C45978124B8 8788'
    'runtime\amd-nr\assets\native_wave_project_mapfeature_fp8act_f8out_c64.cso' = '9E943D1FA9279978911E700FB58738360CFEB39F1E9EBCF8E8461AC12F009F77 8784'
    'runtime\amd-nr\assets\native_wave_project_mapoutput.cso' = '7F32A83AEAE05F6387916D334D671360C5ECECECFDC687D086DE8B43CDE15DEA 11596'
    'runtime\amd-nr\assets\native_wave_project_mapoutput_c128.cso' = 'D8DE0CD1AD5A1CD0A3DF8BC48E86A081756D6DEA9AFE60F184650A0529AE50E5 11592'
    'runtime\amd-nr\assets\native_wave_project_mapoutput_c64.cso' = '91731A1C664FA8F49748ED9CD787562E47205747A42F30373231C1471699BEEA 11592'
    'runtime\amd-nr\assets\native_wave_project_mapoutput_fp8act.cso' = '06CFF9A1DEC9976C9E204F10C5DC71EF389A0E5FC65B88068F732DD83851E69D 9588'
    'runtime\amd-nr\assets\native_wave_project_mapoutput_fp8act_c128.cso' = '46632D0844CB5055B158B3EC538989C00CDED2C58BD07A164DE26F1A5977F1BE 9592'
    'runtime\amd-nr\assets\native_wave_project_mapoutput_fp8act_c64.cso' = '562DA7F9EF9291F059EA868EFF94AE38006568802EBADCF31CB613924925188E 9588'
    'runtime\amd-nr\assets\native_wave_project_mapoutput_fp8act_f8in.cso' = 'D16404B83CF06B9A4F27C4FF6B74F38467B9EDE3566DB5A23A1D88C760AAE04D 9632'
    'runtime\amd-nr\assets\native_wave_project_mapoutput_fp8act_f8in_c128.cso' = 'FA76A028FAECA3AD2991F93C03ACE1A779087E32D14B2917EEC67DDAA41323A3 9636'
    'runtime\amd-nr\assets\native_wave_project_mapoutput_fp8act_f8in_c64.cso' = 'A6FE3D0168C5650559E83CA6D47A30814F6054085F52082FD64B1C410AD68A2A 9628'
    'runtime\amd-nr\assets\native_wave_project_mapoutput_fp8act_f8in_f8out.cso' = 'A3F36AA8AB0BC8049E82CEE838473B186EE292B509BA65AEC7B08187003C797D 10212'
    'runtime\amd-nr\assets\native_wave_project_mapoutput_fp8act_f8in_f8out_c128.cso' = 'D693917123F19414A039911B8613611359224CBEDBAF43F6E2CDF98EB8C7B587 10212'
    'runtime\amd-nr\assets\native_wave_project_mapoutput_fp8act_f8in_f8out_c64.cso' = '2CAA75C34870136DB7B72E77EA59A72EECC057320F4652128A6087ADFBB2B5A0 10204'
    'runtime\amd-nr\assets\native_wave_project_raw.cso' = '14ED64A85043F29B09DD9F00B87BF22C2ADCF88E3B84DCF28F4268F2A44B998A 9856'
    'runtime\amd-nr\assets\native_wave_project_raw_c128.cso' = 'A79CEA186D94D50FFE165F22B8B8151B23C5A5432B12DB10664D027709ED3BFD 9852'
    'runtime\amd-nr\assets\native_wave_project_raw_c512.cso' = '340B88AC789CDBDB935F608DD806A1ED4E55E1E1B55C0F785AA106D507061E8F 8644'
    'runtime\amd-nr\assets\native_wave_project_raw_c64.cso' = '7037F354C3B8C3D2449D2710E0FBE69AE130E9FB4C55BC0648DD385047F1718B 9852'
    'runtime\amd-nr\assets\native_wave_project_raw_fp8act.cso' = 'BE29071FADB4360C0627318D2D9C38E171F96E4DE1B989C5936419AB7B386D06 7848'
    'runtime\amd-nr\assets\native_wave_project_raw_fp8act_c128.cso' = '3DF0BD84329910EA836EFC1143BD788F141836CB33AF624E9E610BEF893B5D6D 7848'
    'runtime\amd-nr\assets\native_wave_project_raw_fp8act_c512.cso' = 'CD4EEB9C58BF59B3DDA35996FE3D940053E34DCF8C03C8C92EA9C2A770A98C62 7828'
    'runtime\amd-nr\assets\native_wave_project_raw_fp8act_c64.cso' = '54B5486708E13FEF68B19318E5A7B5412848FB96A01CF28C79B6D8F010E5E1A5 7848'
    'runtime\amd-nr\assets\native_wave_project_raw_mapoutput.cso' = '96365001AE30E38F37A0E96DBEDE54B3C631F94AE67E88B63425F4EBE323CABC 10732'
    'runtime\amd-nr\assets\native_wave_project_raw_mapoutput_c128.cso' = '631F728DA49893101019506F34ADA6484EFAE69AFCB2E6D62015AC4977DB22F4 10728'
    'runtime\amd-nr\assets\native_wave_project_raw_mapoutput_c64.cso' = 'FD6A640D20A3E81C48EFFD4664FE84D48D895C8CAB9F8F7CF28E5EEDEB8C0E49 10728'
    'runtime\amd-nr\assets\native_wave_project_raw_mapoutput_fp8act.cso' = '59AF2BB05662A4790026A97E80C5F102A4FFED88607DA228096E2C48E292DFD1 8716'
    'runtime\amd-nr\assets\native_wave_project_raw_mapoutput_fp8act_c128.cso' = '5595E7EEF970E5702C493F88FAA8C115E1195F236822736CAB2E207E56E8A61D 8716'
    'runtime\amd-nr\assets\native_wave_project_raw_mapoutput_fp8act_c64.cso' = '414966901DB133FDDBA05F6661D08C2FB2A034F6E08BFBAA7ADDF7A273A59CD5 8716'
    'runtime\amd-nr\assets\native_wave_project_raw_mapoutput_fp8act_f8in.cso' = '03417B49C546FD06E0D0C57D3F871EF8796C09EC39A3CA3C44EC2CAC06696A92 8804'
    'runtime\amd-nr\assets\native_wave_project_raw_mapoutput_fp8act_f8in_c128.cso' = '68AACD07D93609199058A3C0CB01F5A053A7F5AAB91D39C6D36872F994506F34 8804'
    'runtime\amd-nr\assets\native_wave_project_raw_mapoutput_fp8act_f8in_c64.cso' = 'D82B25370ABF638AA00C68555D216F3354BE0C94FED32A5269BE6B5EBF6C777B 8800'
    'runtime\amd-nr\assets\native_wave_project_stream08_c512.cso' = 'A22FA9C830FBFFD576E324A39E99B8AC791EDB86DB7ADF4B08AE1FA2AF973114 10904'
    'runtime\amd-nr\assets\native_wave_project_stream0f_c512.cso' = 'D7E202281EE3965BDEB22239C21826DCBC4F5F0B2A21D5AD937A6B29522E3302 10872'
    'runtime\amd-nr\assets\native_wave_project_stream18_c512.cso' = 'EB51B0D6A303283F63F37BA4F0D190FFFE7BB46B26E642339095C49722C8A2AA 11896'
    'runtime\amd-nr\assets\native_wave_project_stream1f_c512.cso' = '1FEB7767BD39825D89897F65DF484D88E51C035C5C7D28DD56D92D0ED9B85A5D 9740'
    'runtime\amd-nr\assets\native_wave_project_stream1raw_c512.cso' = 'D056F92713DC52DE5E6EE1432C7F774F410D03CF64DB515C0655675705B0560A 8940'
    'runtime\amd-nr\assets\native_wave_qkv.cso' = '44B620FA1C8A081B765EB43CF991F6C4839B097F140A2A981AA8136BF9E043E8 6824'
    'runtime\amd-nr\assets\native_wave_qkv.hlsl' = '10700E73A63EA49CAAA996292D29C13C05E05B72E88200D6F091222DDA062358 1950'
    'runtime\amd-nr\assets\native_wave_qkv_c128.cso' = 'B5D73500E9B7E0974DA40741B2FBBCB99F5B6C79FDC575839ADAA5A38699256B 6824'
    'runtime\amd-nr\assets\native_wave_qkv_c64.cso' = '5FC361A55DD894FDED479484ED73BA7D82F70BDBE6E2358CFEF357CB23806CEE 6796'
    'runtime\amd-nr\assets\native_wave_qkv_normalize.cso' = '079A32E974B0D320FA9B908549588CDEA0D7152E43D4004325E05907E5C54AE3 9648'
    'runtime\amd-nr\assets\native_wave_qkv_normalize.hlsl' = '0E55894C268C4CADC45FF914767D6222B359E6CA829AFAFB00081FD465CC95BA 6525'
    'runtime\amd-nr\assets\native_wave_qkv_normalize_c128.cso' = 'B76B903CDBD181B24190EC6AE65B3B260F56FDFFA0808E7285FD399F42D20A80 9648'
    'runtime\amd-nr\assets\native_wave_qkv_normalize_c64.cso' = '3FF2473EAB3BCE5769F68B54ECEFDC5446CB0037F3B25576318B5067A83B17D2 9612'
    'runtime\amd-nr\assets\native_wave_qkv_normalize_fp8qkv.cso' = 'A784821EF12EA9EC82F528144168D9E233EF9F7CA4376EE61F2C22B37A3E20D2 10220'
    'runtime\amd-nr\assets\native_wave_qkv_normalize_fp8qkv_c128.cso' = 'C6974708B1535DE24346D3E6A59945C6D4C41EA8CE5E9365A098B1C0D3A1EAD4 10220'
    'runtime\amd-nr\assets\native_wave_qkv_normalize_fp8qkv_c512.cso' = '381D16121EF5DEF4B0E78F5AB220777535C4DB94E2A0B119251775FE99E6FA8C 10216'
    'runtime\amd-nr\assets\native_wave_qkv_normalize_fp8qkv_c64.cso' = '9BD4F99DCB9E5520EAC2BFD996277BD4D2922FDC040CFEB3D9DEF6C2E27BE42A 10192'
    'runtime\amd-nr\assets\native_wave_qkv_normalize_fp8qkv_stream_c512.cso' = '071BA9F5800A4C8C7BA4508A22ADE0179A07D6BC662ED090FF27D75842DC9B75 10212'
    'runtime\amd-nr\assets\native_wave_scores.cso' = '3349DA08B020979DD66CACD7EA715B1AB315D55DE4620667F01D55248485635B 41352'
    'runtime\amd-nr\assets\native_wave_scores_c128.cso' = '30177B4CD601CE9E9E7473080B8B361046D0CDD8423665A4BCAEEFD0D9435A21 41380'
    'runtime\amd-nr\assets\native_wave_scores_c64.cso' = '62076A9BCEF51FE2088502BB4E07A308623A993F990062E4D076BFCD4DDC6303 41372'
    'runtime\amd-nr\assets\native_wave_split_attention.cso' = 'C74DE630DBF5E2C052EF0ADD9FC522ED21463924DCD716AFA585411007A48651 26044'
    'runtime\amd-nr\assets\native_wave_split_ffwd_parallel.cso' = 'FF9BA5BA7ECDB4B21E9D7E75232146B93C5117BA6D63DF1D3A401AD3860CAA89 10904'
    'runtime\amd-nr\assets\native_wave_split_ffwd_parallel.hlsl' = 'EE52271B114BB09672EECF97088E940F8B89745D50058737E741360BE45AA1B9 16272'
    'runtime\amd-nr\assets\native_wave_split_ffwd_parallel_stream8.cso' = '7D1682CB54BDF795832A73545D7609751100DA881C2AB96C73CDC79DE2FA86E9 11740'
    'runtime\amd-nr\assets\native_wave_split_ffwd_parallel_streamf.cso' = '40BFEC49355D6C31DE39D35067C0E31A7381417865A7DA482A40040978853B12 11540'
    'runtime\amd-nr\assets\native_wave_vit_attention.cso' = 'C6195C6220937625B1AD18B5A86A78BA1F15FBAC10567EE4B2D2F88267FC0795 12304'
    'runtime\amd-nr\assets\native_wave_vit_attention.hlsl' = '144C0260D6F2970DC493507D35899CF462F737F83A39ECB98611242FCAEE7EF2 4064'
    'runtime\amd-nr\assets\native_wave_vit_attention_fp8.cso' = '4A28227A27CB994D1CEC5BBF59DBAC59491740FD4E4C300B8B3B955B387B95BF 10696'
    'runtime\amd-nr\assets\native_wave_vit_attention_fp8.hlsl' = '0E1C6F7272B973F4E64ECBE6BCB2503DF80ECC69918AE6E4A3515413C9E328C0 4917'
    'runtime\amd-nr\assets\native_wave_vit_attention_half.cso' = '64E1AD2D70E253D769B7493C5889A5019D815AEFAC94E1671E5BEC9C2E22FB6A 8032'
    'runtime\amd-nr\assets\native_wave_vit_attention_half.hlsl' = '11F7C7A9085EA166D7DB8C65C465AD2FBE7A23666F0F8CF82743BEDDCF1DB42E 5416'
    'runtime\amd-nr\assets\native_wave_vit_attention_pack.cso' = '84114AFF84E20F5AC7AD2DE82AE48BED37B67420E4C35A2AFBF5370AAF42AE3C 3932'
    'runtime\amd-nr\assets\native_wave_vit_attention_pack8.cso' = 'ED277DA1774017AF70056E1CCC5EAF5E1F7686DD94B427DF371BFDFD2A0CA5F4 4500'
    'runtime\amd-nr\assets\native_wave_vit_blocked.hlsl' = '3A97910E02B4A0C2EABC0F9075D4F8169F356B6B01BEF5426C9998962209FED0 13912'
    'runtime\amd-nr\assets\native_wave_vit_combine.cso' = '5CCE0B58F5AF00FD9477A173BCC0AF1A4D59AEFA4BA001EB7B899F565EC637E4 7196'
    'runtime\amd-nr\assets\native_wave_vit_combine_1024.cso' = '7F728CD9FF2CC85394CFFEA7465A8A158F1CF5C6C5E38B5251CD33BE7433434D 7144'
    'runtime\amd-nr\assets\native_wave_vit_combine_m4.cso' = '5CCE0B58F5AF00FD9477A173BCC0AF1A4D59AEFA4BA001EB7B899F565EC637E4 7196'
    'runtime\amd-nr\assets\native_wave_vit_combine_packed_1024.cso' = '9C88C34DD83DB895C9A7E3635BF6A6E79A168EB57269ABFCCF6315B9C3B5721B 7192'
    'runtime\amd-nr\assets\native_wave_vit_combine_packed_1024_m4.cso' = '9C88C34DD83DB895C9A7E3635BF6A6E79A168EB57269ABFCCF6315B9C3B5721B 7192'
    'runtime\amd-nr\assets\native_wave_vit_expand.cso' = '6848E1191A5F808546981C63D642843516312F184AB2E3441666A55F51ABFB9B 7272'
    'runtime\amd-nr\assets\native_wave_vit_expand.hlsl' = '636A1E750B840D87BC6FF386B50D3D905FE5B8EAF7142ACFF712C4D9AFDC64C6 1977'
    'runtime\amd-nr\assets\native_wave_vit_expand_blocked.cso' = 'A5E58DE503BBA4C9B33CCC68A96CD5272AC3BE31D1F67160A4945910B250C02C 8808'
    'runtime\amd-nr\assets\native_wave_vit_expand_packed.cso' = 'AF8578F018E61544DE34B177417EF3863D430F7865C18BADE78890B2ABA3BA82 7296'
    'runtime\amd-nr\assets\native_wave_vit_expand_packed_m4.cso' = '04D2F1408CFF9CC13CB7D888285135818DE905DCA5CEC5BD672D2CBB41A2DD69 12164'
    'runtime\amd-nr\assets\native_wave_vit_ffn_fused.hlsl' = 'CCD244B842FB37BB1DEEF8A3EED53AC54CDF51153FEF72135C2F0A2BE4D37254 8165'
    'runtime\amd-nr\assets\native_wave_vit_qkv.cso' = 'C0FAA9D3D8FBBDAF8DD88FA6F928883CFD7596EA7C2D6E2D49B63896C3E6F362 8144'
    'runtime\amd-nr\assets\native_wave_vit_qkv.hlsl' = '73A2387A4F4962E0FCBF6BD13DB568CA97348FFEF3D8A73E2944D154710CB9B1 7399'
    'runtime\amd-nr\assets\native_wave_vit_qkv_fused_m1.cso' = '5AE1096CF002EDD1F23041210695EFD50EB830F8916C4922CBD52D11B7E8997E 10276'
    'runtime\amd-nr\assets\native_wave_vit_qkv_fused_m4.cso' = '0B17FA81817758392AC454A084CB213C8F4070EFED69C9D64CDB83F11B970FEF 18656'
    'runtime\amd-nr\assets\native_wave_vit_qkv_packed.cso' = 'C51B3E18871BD6883167136F98DAEDE63D43CEA7F4BBE0C655921FE498931AA5 7280'
    'runtime\amd-nr\assets\native_wave_vit_qkv_packed_m4.cso' = 'AFD3E88D4B7773EFB94D11535DB165BBF522864E941F53EFBF08661F7C15BB51 10836'
    'runtime\amd-nr\assets\native_wave_vit_reduce.hlsl' = '3C4E7F53209C6C8391917D54CB8E6AF5592536C6D27906F1BBA5F9CDB9544BBE 2519'
    'runtime\amd-nr\assets\native_wave_vit_reduce_1024.cso' = '522FCFE286CF5D2B00D6A2DF2AFF7C3906CCDAD31D35A46F65BAAAC9C5ACFB85 7952'
    'runtime\amd-nr\assets\native_wave_vit_reduce_4096.cso' = 'A72D3964340EE085187C2958F9B9CD38ADB273AC8406E85992C221E605CE61D6 7952'
    'runtime\amd-nr\assets\native_wave_vit_reduce_blocked.cso' = 'F95CA088EE0E5BD1B33E8343B603643078BB71F6856C2DBD75E36717FA9C0C69 10120'
    'runtime\amd-nr\assets\native_wave_vit_reduce_blocked_1024.cso' = '897EEDDBDCE518F9B36AD70D17E8EB098F5D0F871968760964E5B6F1F1440961 11288'
    'runtime\amd-nr\assets\native_wave_vit_reduce_packed_1024.cso' = '40FC28ABAEECDDE7F3BD5AD1A0973E955136B0F354824662D61E9FC65EEA6E83 10116'
    'runtime\amd-nr\assets\native_wave_vit_reduce_splitk.cso' = '824FD03DD0DB212303157E2406FA2CFE4AF15CD015534354F8AC51D36EACA396 5348'
    'runtime\amd-nr\assets\native_wave_vit_reduce_splitk_1024.cso' = '54C9ACF44BDA01F10CF1EC70EA5AA03329D9E0D08993DB092A2C50A262761377 6880'
    'runtime\amd-nr\assets\native_wave_vit_reduce_splitk_m4.cso' = '1C8D84F993373932CD5480CD0D82989273268DDEBEF0E3528B5796CBA81E3982 6760'
    'runtime\amd-nr\assets\native_wave_vit_reduce_splitk_packed_1024.cso' = '7BB902B64068E6E7E9AC8AAE242EDBFE427D39F5E64B1FA98AA892D8365297C3 5344'
    'runtime\amd-nr\assets\native_wave_vit_reduce_splitk_packed_1024_m4.cso' = 'F97B30F1A4B105B4274A8517DECEC4976BF118AC560A994CA34F63A0C1640F4A 6752'
    'runtime\amd-nr\assets\noise.f32' = 'AA38F7E6C5F20227F90EDCB223FB28C258006A9931941DF1442494CF5D64AC7F 201326592'
    'runtime\amd-nr\assets\normalized-output.f32' = 'C408381446132E07FA5581E930DC33FB790BEA7EC4276EF3621E322DF2492A55 33554432'
    'runtime\amd-nr\assets\post70-attention.f32' = 'C0DB6B0CB7BF76A3BC2593885EA758697C2D76169533D9D92FB651C84F418FEC 32900'
    'runtime\amd-nr\assets\post70-ffn.f16' = 'B0D323E29F4C36D8B3C0E040CE6DB6ADC173C1996A2ECF62E36B3B7BA3CBA25B 17472'
    'runtime\amd-nr\assets\post70-head.f16' = 'BA31B59D96B05A8E6E27E7AC950106CF7E52379ABD5B40CF46FE91E3241827A9 192'
    'runtime\amd-nr\assets\post70-history-head.f16' = '77C745C4E1D84A6227A2224291D83C37D1E9BD18D45E95ADD166B1174225CE8F 64'
    'runtime\amd-nr\assets\post70-scales.f16' = 'BCAE0CD6659162D3D1B3128EC155CB147AF1C79FD7A473A310629C59AD3C685C 128'
    'runtime\amd-nr\assets\preblock_attention_core.hlsl' = 'E67915791DF4A2011012D695B7BADE70879095A69B38765A4B62E345F04386A0 9810'
    'runtime\amd-nr\assets\preblock_attention_four_wave.hlsl' = 'AE97737AB877700F913F590F625BC95D3EB5950080D495EC7736C62347F68F44 12963'
    'runtime\amd-nr\assets\preblock_finish.hlsl' = '55D689A19EC22A07688D307FFCD5D9E05E5578D844BCD205E7DC6B959CA1F6E6 3474'
    'runtime\amd-nr\assets\preblock_input_mix.hlsl' = '64EF6BA687B1C61CD88328C6AE589AA2BA63C9E5B9E376CCCE9E91CC915261A9 8304'
    'runtime\amd-nr\assets\small\block0-attention.f32' = '60546870E40215DD952616AFBF1958695A989B125FEF0948003208D525BF0A45 32900'
    'runtime\amd-nr\assets\small\block0-ffn.f16' = 'D02292164637E7CDD03F7E3B9E332AEF248467E22E155C2F568609EC8399F80C 17472'
    'runtime\amd-nr\assets\small\block1-attention.f32' = '0A04E3E078A96E7874FF5B9E42BD1247F3A40701DC68E51879EAF26B4717E7AC 32900'
    'runtime\amd-nr\assets\small\block1-ffn.f16' = '2E447E8BDE04378C118A6E499DFE3377C5FB4D2FA7E9BA82DA9EB3CE9D122670 17472'
    'runtime\amd-nr\assets\small\block14-attention.f32' = '36CFC46DA6EEFE6D0AC0E1325576C543F72F6D88378F25CE42290837A088E002 328208'
    'runtime\amd-nr\assets\small\block14-ds.f16' = '7F2D403B8D25B2BB9EA1175656994347BA002F713CF42F957474A47AD69F40B4 65536'
    'runtime\amd-nr\assets\small\block14-ffn.f16' = 'EFE2EB34F8A4E8339C3EBB4402AF480C0A56FD2460D454A799E4DED02A74B25D 295168'
    'runtime\amd-nr\assets\small\block15-attention.f32' = '8750F0E441799A16D35F08C852DD59211D44DEEC0E80897462801D5D4BDFB1F4 1180704'
    'runtime\amd-nr\assets\small\block15-ffn.f16' = 'B6F867D2E6EF20082175608C1DFDAAF5965E5BF1B66EF952B51005E10DB37260 1180160'
    'runtime\amd-nr\assets\small\block22-attention.f32' = 'EEEA3B61BD48946CDDA6B23AA0EB56DEC1F518C4A26F62E07D90F69FA22659B5 1180704'
    'runtime\amd-nr\assets\small\block22-ds.f16' = '7D7B94402364125471CA8A6071E84F588B861E5F9097B1E030D39E29E16D75C2 262144'
    'runtime\amd-nr\assets\small\block22-ffn.f16' = 'B22444E32365043120C43859E3FE6D61E055D1135CF0FBBF1BED2DDDD3B383BC 1180160'
    'runtime\amd-nr\assets\small\block23-attention.f32' = '796F5E23FC9189DBA764EE596C008D6DB44750AA7667C8932A389D1B3F797C5E 4458560'
    'runtime\amd-nr\assets\small\block23-ffwd-projection.f16' = '3A28CC6DA2A5C0F128DDD9D4DA728D7FC885251C03285ECEB55BD5CAAE78C180 525312'
    'runtime\amd-nr\assets\small\block23-ffwd.f16' = 'FBDC379B902E187F5149E38D026AA867BDA12937BF3C5F921003E1FD42BFE4EB 1048576'
    'runtime\amd-nr\assets\small\block30-attention.f32' = '4F2E6BA5831CAE5F2BF63771C6A59288552FA6A10FF95C111C973B1B48527487 4458560'
    'runtime\amd-nr\assets\small\block30-ffwd-projection.f16' = '1B5AF5B15BC9FB50C97C432FED46707B45C8BD5406B992E610AE7C6D2F866DF3 525312'
    'runtime\amd-nr\assets\small\block30-ffwd.f16' = 'EAB2704BCD08D1FD97946E25A5A73A2D0169B5BE304E1753136453EE2838ED5B 1048576'
    'runtime\amd-nr\assets\small\block31-contract.f16' = '35BAF79D67C764790F9CFD5CCEC813566BF2A05B07816189852542704F1EB345 8390656'
    'runtime\amd-nr\assets\small\block31-expand.f16' = 'E1A560142595F75F90845087ED24CC2CF67F784C7B0086CED554260FFD812865 8388608'
    'runtime\amd-nr\assets\small\block31-projection.f16' = 'B632B3844ED713F1EDCA038CB75FEF441EB5C09C9740D23F2EC5A800B28980B0 2099200'
    'runtime\amd-nr\assets\small\block31-qkv.f16' = '7CC6569BD3CC223E9889272E2464B4E98BBE7808482C7DD82F7EDFDE60D295E2 6291520'
    'runtime\amd-nr\assets\small\block32-contract.f16' = '5802E388E83496169554EC490783C698F068605C2662CACB6434EB1B21F27579 8390656'
    'runtime\amd-nr\assets\small\block32-expand.f16' = '4A4559CEBD0F319BC57B243DF01CAA3A3444236E54CDA70C69FBAEDD6A17BEED 8388608'
    'runtime\amd-nr\assets\small\block32-projection.f16' = '8BA2B3DD40DC5486A5111D925A8CDEAB9FE85BBA49912DC0D02CF3D4AEF2667A 2099200'
    'runtime\amd-nr\assets\small\block32-qkv.f16' = '2341EF69DF44363D96445F2E4AD26306252D307DE1C6F30E564EC7809607F1E1 6291520'
    'runtime\amd-nr\assets\small\block33-contract.f16' = '514A9F7105E40473F2679A593D1C64860D72A9F2407E67C50F29F18799049134 8390656'
    'runtime\amd-nr\assets\small\block33-expand.f16' = '5E3768C0332EEF185A506BD511E910476CCE3BAB42F915B752BBE84E0047F625 8388608'
    'runtime\amd-nr\assets\small\block33-projection.f16' = '109C33CE4C45122A4E630638F4DF3A196B0C3C3B2A06DD636830F48A7B3474D5 2099200'
    'runtime\amd-nr\assets\small\block33-qkv.f16' = 'D3788C92104D424B951EE79F92EB50E538391F4F13CC5602F83C0DC56FAB6E81 6291520'
    'runtime\amd-nr\assets\small\block34-contract.f16' = 'F79A57E42E2F38CB106BE1EB236B7EBE1E2F2DFD5B6B23DEC6C92698A36A3848 8390656'
    'runtime\amd-nr\assets\small\block34-expand.f16' = '947E1DF8AC026FF9C7D0218229522D4FE1596380769603532CEAD1B716DFE14A 8388608'
    'runtime\amd-nr\assets\small\block34-projection.f16' = '4A451D328358F5EB9272835AAD1ED5465C96E2B33564B4DA2FB93747228D1C4C 2099200'
    'runtime\amd-nr\assets\small\block34-qkv.f16' = '9302E64A9AA2A2CEDEA6E6DAB448C8EFBB91FFD3CBEFCC238101916CE6F561C3 6291520'
    'runtime\amd-nr\assets\small\block35-contract.f16' = '64633B99A52FE1E4CE41621D56EE83DE6D76FCD1F40F23255027D6B4AE359B8C 8390656'
    'runtime\amd-nr\assets\small\block35-expand.f16' = '823C712FD668612427E2A16DA6CC915F0B808810715C003B4619DC268E69C692 8388608'
    'runtime\amd-nr\assets\small\block35-projection.f16' = 'BDD535EDDB0C6E8C03F29A0FFC1B8DFFE9FF7B3B91FB2C8AE62A7EAEA80891B3 2099200'
    'runtime\amd-nr\assets\small\block35-qkv.f16' = '42E04D9C04B2406E9C45624AB4B94FF843A4A378D98DF2A9CED6C1BA7B67604F 6291520'
    'runtime\amd-nr\assets\small\block36-contract.f16' = '2279E2FDB8B362CF285C5C9D0504DEA2229D289C665811A2F5E3ED39FAB40B86 8390656'
    'runtime\amd-nr\assets\small\block36-expand.f16' = '551FC51C14878E058ED09E5617109BF1D2C7542F2EAAE72B42184914AF032DF2 8388608'
    'runtime\amd-nr\assets\small\block36-projection.f16' = 'DB0E5CC9BF3297DD974DDB2E5AD668C39CD1E2F88D820CB96D232A22518FA406 2099200'
    'runtime\amd-nr\assets\small\block36-qkv.f16' = 'DA9CA3C5A3B087AC256A45844760979BF2619402029E8DEC040742799E42E343 6291520'
    'runtime\amd-nr\assets\small\block37-contract.f16' = 'B9CB460EAFAF17EEB2B586DF038F299EF5D815B080597062BDB7B15740AF9778 8390656'
    'runtime\amd-nr\assets\small\block37-expand.f16' = 'DC17F730527690B7072DB897644269EBB284845995BA1316D5C7EF1820C99FE8 8388608'
    'runtime\amd-nr\assets\small\block37-projection.f16' = 'E1860C156B687662F19C0226A4BD3A795846B1DD585D45DA8280FF39D062FAE3 2099200'
    'runtime\amd-nr\assets\small\block37-qkv.f16' = '526ECEF02A3BC11973C2E9C2C39580FADA8CA1540B849AD5A99CB003C6AE52D0 6291520'
    'runtime\amd-nr\assets\small\block38-contract.f16' = '2AD89E3B53089E06A480E6952C2447BB304E636CDCFDD5CB1A6FAADE0514FAA1 8390656'
    'runtime\amd-nr\assets\small\block38-expand.f16' = '77D5175292F2AA8EF33D7D8C4E5969EB1E5BD91FFEAABD061C0C9EC9499C741C 8388608'
    'runtime\amd-nr\assets\small\block38-projection.f16' = 'E1A71C8DF26E4C4E59C8166902145980F63F0C58400A73CD2CA770A7C6FBC7C4 2099200'
    'runtime\amd-nr\assets\small\block38-qkv.f16' = '3ECB6EC6FF7197A4342A9BA777582645DBECB8D0E3FC699C2B8873E0FC90AA6A 6291520'
    'runtime\amd-nr\assets\small\block4-attention.f32' = '2BED75139FB628893A9D69DAA6A75D6DD2004ADA9A87AA42E972A41E11A8FF4B 32900'
    'runtime\amd-nr\assets\small\block4-ds.f16' = 'CBE3DC49637EED2543308FB5755CFE51BAA2165FD13EA213BD223163271F02EA 4096'
    'runtime\amd-nr\assets\small\block4-ffn.f16' = '4388F542E5576FD36EF73269275B696054F34AF93F15FFD60A2E654B61E1B4D0 17472'
    'runtime\amd-nr\assets\small\block40-attention.f32' = 'CEEDE871D916EB58B37D04E8DE593EFFFBFB426FC57BED60989F4CF3756CA4B5 4458560'
    'runtime\amd-nr\assets\small\block40-ffwd-projection.f16' = 'E1C770D4E9E20ECD31F6C3EF66EAD7672F1AA165E5BB37CFC6CC11B7A4A206DC 525312'
    'runtime\amd-nr\assets\small\block40-ffwd.f16' = '18D1772B252D7187E374B61E90C74F2BE8A1647CF5F17B2736662CE5AD243C3B 1048576'
    'runtime\amd-nr\assets\small\block47-attention.f32' = '1187525E6C9684BE3AB1C3B8B4F4204A1BEBF2E6AF0DB02BDA98E67257E47DEA 4458560'
    'runtime\amd-nr\assets\small\block47-ffwd-projection.f16' = 'D01A47D4CACCBB17587DC9FD7CC284AAF8B67D7C244A9600DBAC8744C115E0EF 525312'
    'runtime\amd-nr\assets\small\block47-ffwd.f16' = '8DC6A2FC4514227C92541416595640113A32ED79F14DB261023FFC3D8C41ECED 1048576'
    'runtime\amd-nr\assets\small\block48-attention.f32' = '17CFC1868A5C94EAA8DD80CB2A05D6AFD9F0C026BE8AADFC52E0B6578693E551 1180704'
    'runtime\amd-nr\assets\small\block48-ffn.f16' = 'CB07647F8434DFCDF96D15D8BDD831B429988FFA4B2B8CC77056095C372478FD 1180160'
    'runtime\amd-nr\assets\small\block48-weights.f16' = '1A5CF9BD383EEBAF4C0540425C3052935A21AFABC7DD2F8D2322C911BEC33012 262656'
    'runtime\amd-nr\assets\small\block5-attention.f32' = '0AD947F3A340AA612A7809249BDF8FF35CF551A336B57341E217A8B82261250E 98568'
    'runtime\amd-nr\assets\small\block5-ffn.f16' = '2A7CEEA7A0C4A58ECD559E2512C5AE38FD919351027198951049BB37C057D969 73856'
    'runtime\amd-nr\assets\small\block55-attention.f32' = 'A24E4D0C3AF4B089DDA3F5682AC7C7BFC627ED4D039969FD5F0845ECF500D9F8 1180704'
    'runtime\amd-nr\assets\small\block55-ffn.f16' = 'A1FC85B7A64096393C4D23B10E82F0D1B375A3D8A73AB72131D8A5157464D5C7 1180160'
    'runtime\amd-nr\assets\small\block56-attention.f32' = '9F011796C18EA4DA7398E381D3B33BDD0678EBFDE7570970E553B86BE97CD588 328208'
    'runtime\amd-nr\assets\small\block56-ffn.f16' = '14F51E2332FF4D89CD4B06164ADF9372EB6E1FD3687412FDC3DAAF129E3D2A56 295168'
    'runtime\amd-nr\assets\small\block56-weights.f16' = '68FC8EB7C289908833FC1FB9BD986BC109FB2478D3CBA3096FDAA20E23FD05D3 65792'
    'runtime\amd-nr\assets\small\block61-attention.f32' = '6454E3E640E73103E2FC2AD67785E25115CF7C7F0E146915D6B35E50141C393E 328208'
    'runtime\amd-nr\assets\small\block61-ffn.f16' = '9F8A890138D1D184E43BD38321C58C4FDECFA9C5D634338B893DFE23F31AE0AA 295168'
    'runtime\amd-nr\assets\small\block62-attention.f32' = 'B546BC04D73BE9997D80600CAF04D9C68BB30219F30BAAD17E69594460FE051E 98568'
    'runtime\amd-nr\assets\small\block62-ffn.f16' = '5DEAB6883DCFEC11A45FABFE73F7CB85888E6C3CC4AA3B2418C744E281ABAFE0 73856'
    'runtime\amd-nr\assets\small\block62-weights.f16' = '5B807D34C5430DAE3E81FE398E98D4748B60FF6C8273270C2412919E1B5FDBDC 16512'
    'runtime\amd-nr\assets\small\block65-attention.f32' = '9311579130E3DD84AEAD88FE4B6113EEF1BF73E8FE3F8D407BC9047C33408CE5 98568'
    'runtime\amd-nr\assets\small\block65-ffn.f16' = '2FF22FB052558E60730CBBD18D31F8B486CC505FFF478773190C5C2AE397EF2D 73856'
    'runtime\amd-nr\assets\small\block66-attention.f32' = '701B30CDEFF64905CB552EBC810A9C44402B2DBE7AA5F61BE953DD045D93E571 32900'
    'runtime\amd-nr\assets\small\block66-ffn.f16' = 'A03D5F9591B08E6F1A1D304BAAC0CC7EE86EA1C3265C83FD88E26799EEB955A6 17472'
    'runtime\amd-nr\assets\small\block66-weights.f16' = '1A57A80D20F975DB3F894B4C47DDCFD9045096F6607BF1897515DEA916AA71E7 4160'
    'runtime\amd-nr\assets\small\block69-attention.f32' = '92368B5771F98D3AF9D6D68B0D12464B707C7ECBAACD394A153E2858B46F31F1 32900'
    'runtime\amd-nr\assets\small\block69-ffn.f16' = '752D0F663ED36C0FA9E8313C9220199413F178C498A642D6C215B86444C282D6 17472'
    'runtime\amd-nr\assets\small\block8-attention.f32' = '80BEDF731B6F5D351634A7CE5B5A7821019843A8950B90211A2CD673B60C7797 98568'
    'runtime\amd-nr\assets\small\block8-ds.f16' = '063EC9A0F418F1866413A2D1EA5557B2666EAA4E3B35234D653BAC31C96D7B80 16384'
    'runtime\amd-nr\assets\small\block8-ffn.f16' = '072B79F56F5E96DDFFC71B826E297F7833ACD21E0DF1A299EE79F42393E67AED 73856'
    'runtime\amd-nr\assets\small\block9-attention.f32' = '2B3606CD70DA4C2358D78CBCDF14CC0ABF41ABFA84545D94F4C3F937EB5CF2FB 328208'
    'runtime\amd-nr\assets\small\block9-ffn.f16' = '5D5116425A1EF14C752C0B3110770CD492089A0D30063F2EBB3AA766C7CCA5C8 295168'
    'runtime\amd-nr\assets\small\decoder39-weights.f16' = '24797422A70EBEED4C6E6866609D66FABDEC67B2C3B29BF8C5C1306F99437EDD 1049600'
    'runtime\amd-nr\assets\small\head-matrix.f16' = 'E522BB96BC79E3FC7A0BC654A697EB0527516074F7800F0F2F640A9A61EB74A7 1048576'
    'runtime\amd-nr\assets\small\model-manifest.json' = 'FDBF7E72E9C1062D168EAB4B3EBAFD23C7F12D2B4C54B7C5430DBF9DD3AB6C9A 25225'
    'runtime\amd-nr\assets\small\post70-attention.f32' = 'E683331AB1EC11AC1F450CCF7CE94550CDCD43FF52DE6B640D97797D526C607B 32900'
    'runtime\amd-nr\assets\small\post70-ffn.f16' = '1B205E8248EF7A9378DE06A28C58558430638B133C08E3C891287F45CA7F47C1 17472'
    'runtime\amd-nr\assets\small\post70-head.f16' = 'D754AED1A715511CC8244B70AF34F6C8DC5D62A70DD8A01EA4F6E4699EAE0434 192'
    'runtime\amd-nr\assets\small\post70-history-head.f16' = 'C4C5F5189360756001E96ABB43A482396AC8BDE56F6AC4C7C19AE63DB01D0980 64'
    'runtime\amd-nr\assets\small\post70-scales.f16' = 'A6EC06A1C6F8AAE7266C2785209B9DE5B49BDEBDEC89E2C9331706CE97354E5F 128'
    'runtime\amd-nr\assets\vit-to-hwc.i32' = 'CB950400C76A6A1602EAD35A817C851E552B5A301BC8E94561ED26785D1E2754 2621440'
    'runtime\amd-nr\HIP-API-LICENSE.txt' = 'C62D4AD85194ABDC5A27BD205003C47B4C8F1467BC26483ABB8AFBB6A3672645 1101'
    'runtime\amd-nr\LICENSE-lmxxf-MIT.txt' = 'AB181E59A43E92AE66881F1E85135F38BF467CF1CA0957878C649B4BE3D8EA1A 1082'
    'runtime\amd-nr\LmxxfNrRuntime-Original.dll' = 'DBEB6DEA7AFCAC922D5C2CCE83D96E5C42B27D165967B171BE21482C40D6AF68 1367040'
    'runtime\amd-nr\LmxxfNrRuntime.dll' = 'BE83772A75F317A5243D4C67A0E09D0E6C6BB9B687A07F2AF866BD43911867C2 1368064'
    'runtime\amd-nr\LOCAL-EXPERIMENT.txt' = 'D399E49F8C73E72602B8D0E622306F51431B9EEDDA276004FD412EFC41C228CD 717'
    'runtime\amd-nr\release.json' = '662BF89012C1C963DE32542540DCA05A85E9C165D8DD5FB7F40C19D8261A52AB 1500'
    'runtime\amd-nr\runtime-manifest.json' = '1071BA44AD6F2A30FF726E88B3169A8F42BA6EC1BE15497BE09E4486BD17D847 287156'
    'runtime\amd-nr-rdna3\cache\ComputeCache\coverage.txt' = '533770C8C676C16203E0C3F9527E1F1A26F8E68233F5E3442B7C29D68AEDD7CB 94'
    'runtime\amd-nr-rdna3\cache\ComputeCache\zluda2.db' = 'D8ECFF00995CE5CD17913AD71F6F11470AB9F26F2A1B49B461DC41F9D19C7EA1 105058304'
    'runtime\amd-nr-rdna3\kernels\conv1x1_512.hsaco' = '9A53C0C4B0C22A1BE8D794705FDF15242747742DBF612A930CC532E9BDE834E6 1165296'
    'runtime\amd-nr-rdna3\kernels\ffwd_512.hsaco' = '5F997B482B4F789F6100DE6239DB71A97535154B4D81E0B66B3A59BF6439CE2C 691720'
    'runtime\amd-nr-rdna3\kernels\kernels.txt' = 'EDE182E50614C1FBA513DD94E7E7F4AD3698E4885123401A429073813C504D58 17142'
    'runtime\amd-nr-rdna3\kernels\qkv_512.hsaco' = '808F4D2B43C7F7BAFAEA341BC1E1C80B07909953EBADD54EC9BA73A6D8E7A5EE 495992'
    'runtime\amd-nr-rdna3\kernels\swin_1h.hsaco' = 'AB40B1C0EBCBF7E43B069BB69159BF56E2F09BB0C7D08C1A50AA75117A501714 5996336'
    'runtime\amd-nr-rdna3\kernels\swin_2h.hsaco' = '96C09006BB896A6AE93F1DD4695CF23CF167261F4E7028364EF804511252705A 5318432'
    'runtime\amd-nr-rdna3\kernels\swin_4h.hsaco' = '1CA5AF7D20AF9AA5B5172F9EF50389364E925C60BE301CFB0DAC5DEB8AF002FB 4222288'
    'runtime\amd-nr-rdna3\kernels\swin_8h.hsaco' = 'CD22DD31AE6A4223C094D73BDCE61B1A4214D1707115E184C3B2AC8A11649120 7302736'
    'runtime\amd-nr-rdna3\kernels\vit_attention.hsaco' = '78844E14F6869DE6FE2A3C2E5A3306574241AAB62BF5FC3E6DA2A2892F9CFE62 246072'
    'runtime\amd-nr-rdna3\kernels\vit_ffn_expand.hsaco' = 'CFE6C174C03D29E438A954CB9B52874BECF78935D0F84FB900A7E36545B1F67F 320440'
    'runtime\amd-nr-rdna3\kernels\vit_gemm.hsaco' = 'BE4E1D846BDF40FCB936E22ACB2603FF64B45D6842154CB9C58344A5AE7B65FF 477728'
    'runtime\amd-nr-rdna3\kernels\vit_qkv.hsaco' = '836A7800C76E2E7910EC14B3C3D36B93C71A66305EAEDBAA7CD622090630D1DF 238720'
    'runtime\amd-nr-rdna3\LICENSE' = '3F4415C19770CFE988E9A272DEA54557762A3EA68AAD2C419B2AB066DE0BB17B 1294'
    'runtime\amd-nr-rdna3\licenses\ZLUDA-LICENSE-APACHE' = 'FD26644B0118C8AE84A8770BDBBE18F7DB14387B5CC19D1DE670AD2D98A7AD42 9724'
    'runtime\amd-nr-rdna3\licenses\ZLUDA-LICENSE-MIT' = '6AE314ACF8E015716CDFBED46FDC6DED0A599411B8B98CD61C87F9940F85FFC7 1024'
    'runtime\amd-nr-rdna3\nvngx_dlssnr.dll' = '2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F 266424944'
    'runtime\amd-nr-rdna3\README.md' = '87D14D5313A1F2E99BA4BE0FA505712175CB39E6FF6B6752A1CFBDA48CE215EE 26009'
    'runtime\amd-nr-rdna3\source-manifest.json' = '10F56A8E622EB3F9DA98114C18F91B4D7E9225002D3D9D4F1FD893943E1DC75C 1541'
    'runtime\amd-nr-rdna3\THIRD_PARTY.md' = '0E8C0E5E3B3E4FCDE90B7CC5D98DD83C0D914145D9F30612A5164D65F93B4088 1534'
    'runtime\amd-nr-rdna3\veyra-rdna3-runtime-manifest.json' = '16D62111663BAF4353496D30BF71041942C663210FFC2F4D9885319EB2DFE00F 6781'
    'runtime\amd-nr-rdna3\veyra_amd_nr_worker.exe' = '976331D7D9898287E75D903558E8CCD50A154314EBF5C504499054FED469B76E 318464'
    'runtime\amd-nr-rdna3\zluda\cubin_fatbin_map.txt' = 'DFC3B6F2C6B4162B1799407A724C54DD22C2E52CCE0A42DEEFDA3D0FC48A8AF1 1514'
    'runtime\amd-nr-rdna3\zluda\nvapi64.dll' = '2678A451024FD42C1E7BF355B0B371A700924F3D52C8E3752B49BDBCE868E8C7 349696'
    'runtime\amd-nr-rdna3\zluda\nvcuda.dll' = 'B5F118A8F08B3789319CC61E151E1C84FF0F85DC2620A09BD51E7D877FC1EBBE 450048'
    'runtime\amd-nr-rdna3\zluda\nvngx.dll' = '5E7FA7A752AE4FF36EBEB155312F0EC3BB11F7018C2DF1947262D0A390DC75CE 264704'
    'runtime\amd-nr-rdna3\zluda\zluda_nvapi.dll' = 'A7ADB0431ED69F7245312634F08990704D839AAE46F075D80F28FE85B409CAEB 215040'
    'runtime\amd-nr-rdna3\zluda\zluda_real.dll' = 'EE1D9215B3F2BAEDC8BC1AD29412B2FD3B27D15693FD3594D620C87378D14C43 70823424'
    'runtime_local\amd\fidelityfx\amd_fidelityfx_framegeneration_dx12.dll' = '02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18 40085776'
    'runtime_local\amd\fidelityfx\amd_fidelityfx_loader_dx12.dll' = 'E2D85AA05A9BD9ED8B38935FDF5199372CCA6F74C12015143BB6F945EE1608AA 26376'
    'runtime_local\amd\fidelityfx\amd_fidelityfx_upscaler_dx12.dll' = 'D0DCCCC74A43C44BA435B7A369B456E0970D8A4464E4BD683119B374F2C9FB46 28761864'
    'swscale-10.dll' = '9682D04936DBFBF5BC6DC6DE0076E4006A7C291DB9A160ED5AEA01B66989154B 1603072'
    'vcruntime140.dll' = 'D5E4D9A3E835FA679450145D6A7D94E36573A509317111904D9B3712C30D9066 124544'
    'vcruntime140_1.dll' = '1F2D41C4AA5DB0BC33EBF7B66D72943A817D7CE6CBE880502A9403823633093F 49792'
}
$RuntimeFileHashesNvidia = @{
    'avutil-61.dll' = '4504BCCF90872875B21445D0AC121FF984D65F74CFCDAF61D1E333AABD9D4699 1224192'
    'msvcp140.dll' = '0F885B509A685D2BBFA652FED26B5FB31D88FBDAB0A978C641D1C7B8AA460AA9 557728'
    'runtime\config\ngx-local.json' = 'A768D74B16AEDF98DA1C85BA2696C5085AEFA1499566AD6641028CC91091D54D 151'
    'runtime\experimental\dlssg-kernels\k4000.ptx' = 'A58AD1C5F8AB5E82EFD2E15AAEB178113C1720C3EBD8164CA10474B50E1D342A 71483'
    'runtime\experimental\dlssg-kernels\k4002.ptx' = '78CDADD921AFB74B78B1A2B2798FAE069577AA94931DBCC15C34F1ADBB6AF1FB 89932'
    'runtime\experimental\dlssg-kernels\k4004.ptx' = 'E23F640D68ECAF1FB417BAEFB18D11A90185B4E6E692467177ED4420B6BD3B35 48446'
    'runtime\experimental\dlssg-kernels\k4006.ptx' = '599EC85597282CFA04F3EA88F727D11D9A1ECA0B508E6CEC2A90750B6798FA29 98810'
    'runtime\experimental\dlssg-kernels\k4008.ptx' = '8B9F317546591EF6EF13909EDB8C551304ADE391129E9FA8D7ABD73ED63F0D51 17301'
    'runtime\experimental\dlssg-kernels\k4010.ptx' = 'F721C9D6B1970E3FEDA8663364F7E17BB4DE4E6BF280A3742E43420D8764463B 25248'
    'runtime\experimental\dlssg-kernels\k4012.ptx' = '70666762066C77B0458A4A551DB4F590664B51F164F5ED372A69FA0EDDB72C52 51047'
    'runtime\experimental\dlssg-kernels\k4014.ptx' = '673FD239C9F6D5614CC1E4A3E4FCCC2A816AD691812741B91E4259E64C052406 25546'
    'runtime\experimental\dlssg-kernels\k4016.ptx' = '03B91710C6B11DD9DEF1E988474A1EE2FCAD11C6AF73E90B220EB62A098B8563 23534'
    'runtime\experimental\dlssg-kernels\k4018.ptx' = 'DB1BE36D8786F255920754EA484B48F5FA1D7C1DCC3B69CFD5467850674768E2 23550'
    'runtime\experimental\dlssg-kernels\k4020.ptx' = '7710B2556F76CD501F578D0E1C4FB5039E7E18D94EFBAEB568011C74E93E2243 19689'
    'runtime\experimental\dlssg-kernels\k4022.ptx' = '3216F263AEB8C4AC2A1565FDC95D99A371DE6718015EDCB42E753CDF1307268E 14510'
    'runtime\experimental\dlssg-kernels\k4024.ptx' = 'C3973E1A88E48471ABEA699433E6097287E800B65467258EA4FECBD9D4CC0293 54172'
    'runtime\experimental\dlssg-kernels\k4026.ptx' = 'B02762FB2452BFDB979AE7987B60176B67249CC762CDDC4F498FAAFBF78FF676 29428'
    'runtime\experimental\dlssg-kernels\k4028.ptx' = '26C0CF15C83CC36A651AB68FE2C85E03F7C0092594ED03A765C8C66347622C31 29428'
    'runtime\experimental\dlssg-kernels\k4030.ptx' = 'BB110524EDE676DD0093B5E59AB0F2F8A15E27D19DEF02660B03E2A4F23E1572 25258'
    'runtime\experimental\dlssg-kernels\k4032.ptx' = '047B040D832ABE919AA268DF27E38A149D6B91016266FB240B9E54C53355D57A 25161'
    'runtime\experimental\dlssg-kernels\k4039.ptx' = '9F14F5C67B9ABF2CA9B17B415072D540CAFCFBC682CBA8BA5C2F4D2A308A8F75 9753'
    'runtime\experimental\dlssg-kernels\k4042.ptx' = '5E2D0C7E1A9F208624FDDA3B8A529979142FD1C3CD746964F7F755F13E587286 43249'
    'runtime\experimental\dlssg-kernels\k4044.ptx' = '13B50F00F512B91BDF90AD9F273F5CB8DFE4366ADEB9EA7FDA073F528C694FD2 76235'
    'runtime\experimental\dlssg-kernels\k4046.ptx' = '81045733D05A039EB34930C5EF96182125A2BB9CAB1DDAA4ADB874C364D8CAB4 76639'
    'runtime\experimental\dlssg-kernels\k4048.ptx' = 'AB908E60BF574E301129A9689E78AD458EF9E90F4B430AF5E50FC400FABF3D74 44469'
    'runtime\experimental\dlssg-kernels\k4050.ptx' = '988373CF94A46BC2E04B0C161FF336C4D59CCC016CEE4270B27626A611B49F53 79988'
    'runtime\experimental\dlssg-kernels\k4058.ptx' = '6845C21149ABE8D8E05755765C224DB2970D5F444A113FD8F390775EB1CF2900 49154'
    'runtime\experimental\dlssg-kernels\k4059.ptx' = '338A1DEB8D7FAA21553214C9F071906CE6BB85946E27A78C7D75C5CFF1E1A989 16879'
    'runtime\experimental\dlssg-kernels\k4060.ptx' = '8A7B13C21931EE40647D8D177FFCCE9629588569C40DF5345E424C32E42AFB61 55507'
    'runtime\experimental\dlssg-kernels\k4062.ptx' = '564DDD342268739E0447AE711A2B21EAA5135F4069D063C25EA516145DE1EE4D 53773'
    'runtime\experimental\dlssg-kernels\OutputPull.ptx' = 'C7C1951069FECC2A5D37D899FAD1F487FEEB616922E30B411A737DD17227249E 22605'
    'runtime\experimental\dlssg-kernels\OutputPushFine.ptx' = 'F8D9777F2ADA7D3959DCE515510CA7CBD2194A13AC721EBEDF52ED0F019523BF 20733'
    'runtime\experimental\dlssg-lite\nvngx_dlssg.dll' = 'CADEE1AA6DB3907BABDC3285DA2AEBC4553D92B89C38BA3EAE8ABD8BBA1BEA72 7506544'
    'runtime\experimental\fallback-dlssg-310.7\nvngx_dlssg.dll' = '135EAF0733C1E37381A8C28ABCF7A862404A54132B81787C04E35D09EFC5E36F 7519856'
    'runtime\experimental\nr-ampere\nvngx_dlssnr.dll' = 'CDFA6C1EC784867182164C7836F324F087B5C9F374D0C9FE46F54999FEABF3ED 276162560'
    'runtime\experimental\nr-original\nvngx_dlssnr.dll' = 'E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E 165840496'
    'runtime\experimental\nvngx_dlssg.dll' = 'FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82 7460976'
    'runtime\experimental\nvngx_truehdr.dll' = '9A80575F247190C05FE80EAC0C4BAA1D0D4D932348F26808310B5EC4BF9EEB4B 3955752'
    'runtime\experimental\nvngx_vsr.dll' = 'C3D88EEA5FF7A548EDEFA66414CF6E77464D0947277C904F324DD23ABF58A1ED 19140144'
    'runtime\nvidia-vfg\cudart64_12.dll' = '663B5FE65AA6C654B8B085A879F49DEB19F2CB033B8F2FF8DFFD06CDEFE0D1DB 584304'
    'runtime\nvidia-vfg\NVCVImage.dll' = '689A64586C804468127647B7ED24ACC36A9A9780502FFE95AD11AA4FE3286A39 2694256'
    'runtime\nvidia-vfg\nvngx_vsr.dll' = '0CC98643A76C05CA6D73EC81A898D545B18E5C080667D72854A1BFAEBD56E1ED 59120752'
    'runtime\nvidia-vfg\nvngxruntime.dll' = 'D2DBD1993000A2EED2C7AD8CD417FFC738EA180003DAACF190E737121869D637 83568'
    'runtime\nvidia-vfg\nvVFXVideoFrameGeneration.dll' = '270CF4FFF9329908F9770D306CB51B910EE04323AF5520CD02A98A2E22ABA795 207321712'
    'runtime\nvidia-vfg\nvVFXVideoSuperRes.dll' = '93601175949D0F5B5C033D6C9E58CA465A45EF1DD961EAEA34D4FFB979F72850 42608'
    'runtime\nvidia-vfg\NVVideoEffects.dll' = 'C6B042FC57BDB6BEA88C9C52CB951582377D3F1643ACF52A52D197195A1EA8F4 104560'
    'runtime_local\amd\fidelityfx\amd_fidelityfx_framegeneration_dx12.dll' = '02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18 40085776'
    'runtime_local\amd\fidelityfx\amd_fidelityfx_loader_dx12.dll' = 'E2D85AA05A9BD9ED8B38935FDF5199372CCA6F74C12015143BB6F945EE1608AA 26376'
    'runtime_local\amd\fidelityfx\amd_fidelityfx_upscaler_dx12.dll' = 'D0DCCCC74A43C44BA435B7A369B456E0970D8A4464E4BD683119B374F2C9FB46 28761864'
    'swscale-10.dll' = '9682D04936DBFBF5BC6DC6DE0076E4006A7C291DB9A160ED5AEA01B66989154B 1603072'
    'vcruntime140.dll' = 'D5E4D9A3E835FA679450145D6A7D94E36573A509317111904D9B3712C30D9066 124544'
    'vcruntime140_1.dll' = '1F2D41C4AA5DB0BC33EBF7B66D72943A817D7CE6CBE880502A9403823633093F 49792'
}
$RuntimeFileHashes = if ($GpuVendor -eq 'AMD') { $RuntimeFileHashesAmd } else { $RuntimeFileHashesNvidia }
# PushFlow (PushFPS's own frame generation: ONNX Runtime + DirectML and the RIFE models). Not in Veyra's packages:
# PushFPS's release ships it in PushFPS-files\runtime\experimental\pushflow. Identical in PushFPS 4.2.0, 4.3.0, 4.3.1,
# 4.4.0 and 4.5.3 (per-file SHA-256 below; 4.5.3 checked on an official 4.5.3 install and in its release zip, all 20 files). Sources, in order: an
# installed PushFPS, a PushFPS-<v>-win64.zip in -PackageCache or the own download folder, else PushFPS's GitHub release.
$PushFlow = @{
    Asset = 'PushFPS-4.5.3-win64.zip'
    Url   = 'https://github.com/DosukaSOL/PushFPS/releases/download/v4.5.3/PushFPS-4.5.3-win64.zip'
    Rel   = 'runtime\experimental\pushflow'
    Files = @{
        'DirectML.dll' = '9C9E6D822561C6C41B90E6994B3E8857CF1D66DBFB1E0C4C799C7C89B4E92DA1 18527776'
        'onnxruntime.dll' = '903C92C54ACC57CAA77D44D4C856C829EB82F5CF6755B47367A8153BD3FABEB6 14682160'
        'models.json' = 'EC63803B2526FD908907E861589C39D4B9FB9C8F59AC8E413318FF75ACD53946 3584'
        'licenses\DirectML_LICENSE.txt' = 'A05138E3A085FF60A44881EEDFA58DCCB03ECC1D7B1F6AE888418E8C2FEC4B8D 10439'
        'licenses\DirectML_ThirdPartyNotices.txt' = '2C95795C13FF48A58B6ED916F37901C23D964B5D9D601AF422F17AD2172E7950 4577'
        'licenses\ONNX_Runtime_LICENSE.txt' = 'C250D6278F0B47A6439FB7592B08B58A55EB9F535AA49A1DB63211C3F982B674 1094'
        'licenses\ONNX_Runtime_ThirdPartyNotices.txt' = 'FAC5BB85F568B38FD8F3A6AEA32737985841F23A1A43E9CE952F79474F883AA9 345046'
        'licenses\RIFE_LICENSE.txt' = '73A95F13134E15261B32792384CD65AB8248BAFC1E5090407D0C64112F240C38 1278'
        'models\pushflow-fast-body.onnx' = '6AEE08A359D92B2F6774E3B39278DF5492576CF24FF6226E5D2C2B77F7C00E7B 22513579'
        'models\pushflow-fast-enc0.onnx' = '88D5B82D269E8985A1D027642ED2E9B193C1714D295B1812F6E93F244F9D8287 31062'
        'models\pushflow-fast-enc1.onnx' = 'E60530BE03F0341F4D09FE39A990F064414F9E2ACFE0B1A916AB4B5093245A36 31062'
        'models\pushflow-fast-fp16.onnx' = 'A488F7A537D5F0EF498B61B1F770C3402E40275E8C686D552A0DB8F978CCC380 11336987'
        'models\pushflow-fast.onnx' = 'D352AA847C9A88CDC006616D5727C5F57D470CAF2352673EDA813BC4BEBEF111 22539306'
        'models\pushflow-fast.onnx.txt' = 'BF9E4DAD5B207C83AFAF57B1FE322641775CF84FC9E344A5E8C49CF02433F88B 62'
        'models\pushflow-quality-body.onnx' = '2C7DF58F97B42CC73519215039910EBC81CBC9ED7C0B6C775260D1F205DD38C0 22722797'
        'models\pushflow-quality-enc0.onnx' = '1923C604F7224E14B11D6397E6D8973649D6C827BD6EAF6E529ABE4F1F8F1E96 32039'
        'models\pushflow-quality-enc1.onnx' = '63937CA293C75F98033972CDAFD38A8EA2737E7E27C5AF08EB0F8034CEAA9832 32039'
        'models\pushflow-quality-fp16.onnx' = '23810DB5AB43514443E82A2FA322FE440BAD316389C4AEB8CFF06F16426F1CCD 11466364'
        'models\pushflow-quality.onnx' = 'C0B5803BA07DA68643760094ED0A84988A39237B506B82C851F73E681A4D0D5F 22748524'
        'models\pushflow-quality.onnx.txt' = '6775FC2A67772AD9FD964C192919C4764C21E3692E7EE8F424D4260FCF16543C 61'
    }
}
# Candidates that are experimental extras when not NVIDIA-signed (checked, not assumed)
$ExtraCandidates = @()    # (the community SF NR slot is in $RuntimeDirs now, kept by default)
# Licence texts that go with the package's files
$RuntimeNotices = @('LICENSE', 'NOTICE.md', 'THIRD_PARTY_NOTICES.md')
$RuntimeNoticeDirs = @('licenses')

$FilterClsid = '{798D4EC3-E59C-4A4E-A2EE-C804AEAE04CC}'
$VapourSynthClsid = '{3AB7506B-FC4A-4144-8EE3-A97FAB4F9CB3}'
$MpcFiltersKey = 'HKCU:\Software\MPC-HC\MPC-HC\Filters\x64'
$Components = @('filter', 'core', 'runtime')

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $here
# Run from an unpacked release (filter\ and core\ beside this script): that is the stage. From a source checkout:
# <repo>\stage\PushMPC.
if (-not $StageDir) {
    if ((Test-Path (Join-Path $here 'filter\manifest.txt')) -and (Test-Path (Join-Path $here 'core\manifest.txt'))) { $StageDir = $here }
    else { $StageDir = Join-Path $repo 'stage\PushMPC' }
}
if (-not $ReleaseDir) { $ReleaseDir = Join-Path $repo 'release' }

if ($Machine) { try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { } }
function M([string]$t) { [Console]::Out.WriteLine("@@$t"); [Console]::Out.Flush() }
function Step([string]$t) { if ($Machine) { M "step|$t"; return }; Write-Host ''; Write-Host "  $t" -ForegroundColor Green }
function Info([string]$t) { if ($Machine) { M "info|$t"; return }; Write-Host "    $t" }
function Warn([string]$t) { if ($Machine) { M "warn|$t"; return }; Write-Host "    $t" -ForegroundColor Yellow }
function Fail([string]$t) { if ($Machine) { M "fail|$t"; exit 1 }; Write-Host ''; Write-Host "  $t" -ForegroundColor Red; exit 1 }
function Dry([string]$t) { if ($DryRun) { if ($Machine) { M "dry|$t"; return }; Write-Host "    [dry run] $t" -ForegroundColor Cyan } }
# progress of this script's own work, 0..100 (PushMPC-Setup.exe maps it into its bar)
function Progress([int]$pct, [string]$t) { if ($Machine) { M "progress|$pct|$t" } }

function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $s = [System.IO.File]::OpenRead($path)
    try { return ([System.BitConverter]::ToString($sha.ComputeHash($s))).Replace('-', '') } finally { $s.Dispose(); $sha.Dispose() }
}

# ------------------------------------------------------------------------------------------------ manifests
function Read-Manifest([string]$dir) {
    $p = Join-Path $dir 'manifest.txt'
    if (-not (Test-Path $p)) { return $null }
    $m = @{ kv = @{}; files = @() }
    foreach ($line in [IO.File]::ReadAllLines($p)) {
        if (-not $line -or $line.StartsWith('#')) { continue }
        if ($line.StartsWith('file ')) {
            $parts = $line.Substring(5).Split(' ', 3)
            $m.files += [pscustomobject]@{ Sha = $parts[0]; Size = [int64]$parts[1]; Rel = $parts[2] }
        } elseif ($line.Contains('=')) {
            $i = $line.IndexOf('='); $m.kv[$line.Substring(0, $i)] = $line.Substring($i + 1)
        }
    }
    return $m
}
function Write-Manifest([string]$dir, [hashtable]$kv) {
    $lines = @('# PushMPC component manifest: key=value, then "file <sha256> <size> <relative path>"')
    foreach ($k in ($kv.Keys | Sort-Object)) { $lines += "$k=$($kv[$k])" }
    $root = (Resolve-Path $dir).Path.TrimEnd('\')
    foreach ($f in (Get-ChildItem $dir -Recurse -File | Where-Object { $_.Name -ne 'manifest.txt' } | Sort-Object FullName)) {
        $rel = $f.FullName.Substring($root.Length + 1)
        $lines += "file $(Get-Sha256 $f.FullName) $($f.Length) $rel"
    }
    [IO.File]::WriteAllLines((Join-Path $dir 'manifest.txt'), $lines, (New-Object System.Text.UTF8Encoding($false)))
}
function Test-ComponentFiles([string]$dir, [switch]$Hash) {
    $m = Read-Manifest $dir
    if (-not $m) { return 'no manifest.txt' }
    $bad = @()
    foreach ($f in $m.files) {
        $p = Join-Path $dir $f.Rel
        if (-not (Test-Path $p)) { $bad += "$($f.Rel) missing"; continue }
        if ((Get-Item $p).Length -ne $f.Size) { $bad += "$($f.Rel) size"; continue }
        if ($Hash -and (Get-Sha256 $p) -ne $f.Sha) { $bad += "$($f.Rel) hash" }
    }
    if ($bad.Count) { return ($bad -join ', ') }
    return ''
}
# RC2 F3: a repair / update compares the FILES, not only the version: an installed component counts as current only when
# its manifest lists the same files (SHA-256, size, path) as the payload's and every installed file matches its hash.
# A rebuilt same-version release (another filter build, another key, another core) is then replaced.
function Get-ManifestFileSet($m) { if (-not $m) { return '' }; return (@($m.files | ForEach-Object { "$($_.Sha.ToUpperInvariant()) $($_.Size) $($_.Rel.ToLowerInvariant())" } | Sort-Object) -join "`n") }
function Get-ComponentDifference([string]$installedDir, [string]$payloadDir) {
    $cur = Read-Manifest $installedDir; $new = Read-Manifest $payloadDir
    if (-not $cur) { return 'not installed' }
    if (-not $new) { return '' }
    if ($cur.kv['version'] -ne $new.kv['version']) { return "version $($cur.kv['version']) -> $($new.kv['version'])" }
    if ((Get-ManifestFileSet $cur) -ne (Get-ManifestFileSet $new)) {
        $a = @{}; foreach ($f in $cur.files) { $a[$f.Rel.ToLowerInvariant()] = $f.Sha.ToUpperInvariant() }
        $newRels = @($new.files | ForEach-Object { $_.Rel.ToLowerInvariant() })
        $list = @(foreach ($f in $new.files) { if ($a[$f.Rel.ToLowerInvariant()] -ne $f.Sha.ToUpperInvariant()) { $f.Rel } })
        $list += @($cur.files | Where-Object { $newRels -notcontains $_.Rel.ToLowerInvariant() } | ForEach-Object { "$($_.Rel) (not in this setup)" })
        $more = if ($list.Count -gt 4) { " and $($list.Count - 4) more" } else { '' }
        return "same version $($new.kv['version']), but the files differ from this setup's: $((@($list | Select-Object -First 4)) -join ', ')$more"
    }
    $bad = Test-ComponentFiles $installedDir -Hash
    if ($bad) { return "the installed files do not match their manifest: $bad" }
    return ''
}
function Component-Summary([string]$dir) {
    $m = Read-Manifest $dir
    if (-not $m) { return 'not installed' }
    $s = "version $($m.kv['version'])"
    if ($m.kv['pushfps']) { $s += ", PushFPS $($m.kv['pushfps'])" }
    if ($m.kv['requires-runtime']) { $s += ", needs runtime $($m.kv['requires-runtime'])" }
    if ($m.kv['package']) { $s += ", from $($m.kv['package']) (SHA-256 $($m.kv['package-sha256']))" }
    if ($m.kv['subset']) { $s += ", file list $($m.kv['subset'])" }
    if ($m.kv['extras']) { $s += ", extras: $($m.kv['extras'])" }
    return "$s, $($m.files.Count) files, $([math]::Round((($m.files | Measure-Object Size -Sum).Sum) / 1MB, 1)) MB"
}

# ------------------------------------------------------------------------------------------------ existing installs
# PushFPS or Veyra already on this PC: folders that may hold the same files (never written to; files are copied, or
# hard-linked with -LinkExisting when on the same volume).
function Find-ExistingInstalls {
    $dirs = @()
    foreach ($root in @('HKCU:', 'HKLM:')) {
        foreach ($view in @('Software\Microsoft\Windows\CurrentVersion\Uninstall', 'Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall')) {
            $k = "$root\$view"
            if (-not (Test-Path $k)) { continue }
            foreach ($e in Get-ChildItem $k -ErrorAction SilentlyContinue) {
                $p = Get-ItemProperty $e.PSPath -ErrorAction SilentlyContinue
                if (-not $p -or $p.DisplayName -notmatch '^(PushFPS|Veyra)') { continue }
                foreach ($v in @($p.InstallLocation, $p.DisplayIcon)) {
                    if (-not $v) { continue }
                    $d = ($v -replace '"', '') -replace ',\d+$', ''
                    if ($d -like '*.exe') { $d = Split-Path $d }
                    if (Test-Path $d) { $dirs += $d }
                }
            }
        }
    }
    foreach ($d in @((Join-Path $env:LOCALAPPDATA 'Programs\PushFPS'), (Join-Path $env:LOCALAPPDATA 'Programs\Veyra'))) { if (Test-Path $d) { $dirs += $d } }
    return @($dirs | ForEach-Object { (Resolve-Path $_).Path.TrimEnd('\') } | Select-Object -Unique)
}
function Test-PinnedFile([string]$path, [string]$pin) {
    if (-not (Test-Path $path -PathType Leaf)) { return $false }
    $sha, $size = $pin.Split(' ')
    if ((Get-Item $path).Length -ne [int64]$size) { return $false }
    return (Get-Sha256 $path) -ieq $sha
}
function Copy-Existing([string]$from, [string]$to) {
    New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
    if ($LinkExisting -and ([IO.Path]::GetPathRoot($from) -ieq [IO.Path]::GetPathRoot($to))) {
        if (Test-Path $to) { Remove-Item $to -Force }
        New-Item -ItemType HardLink -Path $to -Target $from | Out-Null
    } else { Copy-Item $from $to -Force }
}
# The runtime subset from an existing install: every required file must match its pinned SHA-256; the folders in
# $RuntimeDirs (files with a pin must match it) and the licence texts come along when present. Returns $true when $dest was filled.
function Get-ExistingRuntime([string]$dest, $pkg = $Runtime) {
    if ($NoExisting) { return $false }
    foreach ($dir in (Find-ExistingInstalls)) {
        $ok = $true
        foreach ($r in $RuntimeFiles) {
            $p = Join-Path $dir $r
            if (-not $RuntimeFileHashes.ContainsKey($r) -or -not (Test-PinnedFile $p $RuntimeFileHashes[$r])) { $ok = $false; break }
        }
        if (-not $ok) { Info "Existing install $dir : not the pinned runtime files (not used)"; continue }
        Step "Runtime files from the existing install $dir ($(if ($LinkExisting) { 'hard links where possible' } else { 'copied' }); every file checked)"
        if ($DryRun) { Dry "would take $($RuntimeFiles.Count) files and $($RuntimeDirs -join ', ') from $dir"; return $true }
        $i = 0
        foreach ($r in $RuntimeFiles) { $i++; Progress ([int](5 + 60 * $i / $RuntimeFiles.Count)) "Copying $r"; Copy-Existing (Join-Path $dir $r) (Join-Path $dest $r) }
        Progress 70 'Copying the runtime folders'
        foreach ($d in $RuntimeDirs) {
            $src = Join-Path $dir $d
            if (-not (Test-Path $src)) { continue }
            foreach ($f in Get-ChildItem $src -Recurse -File) {
                $rel = $f.FullName.Substring($dir.Length + 1)
                if ($RuntimeFileHashes.ContainsKey($rel) -and -not (Test-PinnedFile $f.FullName $RuntimeFileHashes[$rel])) { Warn "$rel differs from the package: not taken"; continue }
                Copy-Existing $f.FullName (Join-Path $dest $rel)
            }
        }
        foreach ($n in $RuntimeNotices) { $p = Join-Path $dir $n; if (Test-Path $p) { Copy-Item $p (Join-Path $dest $n) -Force } }
        foreach ($d in $RuntimeNoticeDirs) { $p = Join-Path $dir $d; if (Test-Path $p) { Copy-Item $p $dest -Recurse -Force } }
        $script:RuntimeSource = "existing install $dir"
        return $true
    }
    return $false
}
# PushFlow's runtime into <dest>\runtime\experimental\pushflow (every file checked against its pinned SHA-256)
function Add-PushFlow([string]$dest) {
    $target = Join-Path $dest $PushFlow.Rel
    $check = { param($root) foreach ($k in $PushFlow.Files.Keys) { if (-not (Test-PinnedFile (Join-Path $root $k) $PushFlow.Files[$k])) { return $false } }; return $true }
    # 1. an installed PushFPS
    if (-not $NoExisting) {
        foreach ($dir in (Find-ExistingInstalls)) {
            $src = Join-Path $dir $PushFlow.Rel
            if (-not (Test-Path $src) -or -not (& $check $src)) { continue }
            Info "PushFlow: from the existing install $dir"
            if ($DryRun) { Dry "would copy $($PushFlow.Files.Count) PushFlow files from $src"; return "existing install $dir" }
            foreach ($k in $PushFlow.Files.Keys) { Copy-Existing (Join-Path $src $k) (Join-Path $target $k) }
            return "existing install $dir"
        }
    }
    # 2. a PushFPS release zip on disk (any version with the same PushFlow files), else 3. PushFPS's GitHub release
    $zips = @()
    if ($PackageCache) { $zips += @(Get-ChildItem $PackageCache -Filter 'PushFPS-*-win64.zip' -ErrorAction SilentlyContinue | ForEach-Object FullName) }
    $own = Join-Path $InstallDir "download\$($PushFlow.Asset)"
    if (Test-Path $own) { $zips += $own }
    if (-not $zips.Count) {
        if ($DryRun) { Dry "would download $($PushFlow.Url) (PushFPS's release, for PushFlow only) and check every PushFlow file's SHA-256"; return "download $($PushFlow.Url)" }
        Step "Downloading PushFPS's release for PushFlow's runtime"
        Info $PushFlow.Url
        New-Item -ItemType Directory -Force (Split-Path $own) | Out-Null
        $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
        if (Test-Path $curl) { & $curl -L --fail --retry 3 --retry-delay 5 -C - -o $own $PushFlow.Url; if ($LASTEXITCODE -ne 0) { Warn "PushFlow download failed (curl exit $LASTEXITCODE): PushFlow stays unavailable"; return '' } }
        else { Invoke-WebRequest -Uri $PushFlow.Url -OutFile $own -UseBasicParsing }
        $zips += $own
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($zip in $zips) {
        if ($DryRun) { Dry "would take PushFlow from $zip (every file checked)"; return $zip }
        $tmp = Join-Path $InstallDir ("staging\pushflow-" + [guid]::NewGuid().ToString('N'))
        $z = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            foreach ($e in $z.Entries) {
                $m = [regex]::Match($e.FullName, '(?i)(^|/)runtime/experimental/pushflow/(.+)$')
                if (-not $m.Success -or -not $e.Name) { continue }
                $out = Join-Path $tmp ($m.Groups[2].Value.Replace('/', '\'))
                New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
            }
        } finally { $z.Dispose() }
        if (& $check $tmp) {
            foreach ($k in $PushFlow.Files.Keys) { $o = Join-Path $target $k; New-Item -ItemType Directory -Force (Split-Path $o) | Out-Null; Move-Item (Join-Path $tmp $k) $o -Force }
            Remove-Item $tmp -Recurse -Force
            Info "PushFlow: from $zip (every file matches)"
            return (Split-Path $zip -Leaf)
        }
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
        Warn "$zip : its PushFlow files do not match the pinned ones (not used)"
    }
    Warn 'PushFlow: no matching runtime found; PushFlow stays unavailable (the other methods are not affected)'
    return ''
}

# ------------------------------------------------------------------------------------------------ the runtime package
# Veyra's package: -RuntimePackage <zip>, else <PackageCache>\<asset>, else our own download folder
# (<InstallDir>\download), else downloaded from Veyra's GitHub release into that folder. Whatever is found must match
# the pinned SHA-256 ($pkg: Asset, Url, Sha256; the update flow passes the release manifest's values).
function Get-RuntimePackage($pkg = $Runtime) {
    $candidates = @()
    if ($RuntimePackage) { $candidates += $RuntimePackage }
    if ($PackageCache) { $candidates += (Join-Path $PackageCache $pkg.Asset) }
    $own = Join-Path $InstallDir "download\$($pkg.Asset)"
    $candidates += $own
    foreach ($zip in $candidates) {
        if (-not (Test-Path $zip)) { continue }
        if ((Get-Sha256 $zip) -ieq $pkg.Sha256) { Info "Runtime package: $zip (SHA-256 verified: $($pkg.Sha256))"; return $zip }
        Warn "$zip does not match the pinned SHA-256: not used"
    }
    if ($DryRun) {
        Dry "would download $($pkg.Url)"
        Dry "  to $own (resumable; about 750 MB), then check SHA-256 $($pkg.Sha256) and delete it on a mismatch"
        return $null
    }
    Step "Downloading Veyra's runtime package (about 750 MB) from Veyra's GitHub release"
    Info $pkg.Url
    New-Item -ItemType Directory -Force (Split-Path $own) | Out-Null
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    if (Test-Path $curl) {
        & $curl -L --fail --retry 3 --retry-delay 5 -C - -o $own $pkg.Url
        if ($LASTEXITCODE -ne 0) { Fail "Download failed (curl exit $LASTEXITCODE). Check the internet connection and run the installer again; it continues where it stopped." }
    } else { Invoke-WebRequest -Uri $pkg.Url -OutFile $own -UseBasicParsing }
    if ((Get-Sha256 $own) -ine $pkg.Sha256) { Remove-Item $own -Force; Fail 'The downloaded package does not match the pinned SHA-256, so it was deleted. Run the installer again.' }
    Info "SHA-256 verified: $($pkg.Sha256)"
    return $own
}
function Test-NvidiaOriginal([string]$path) {
    $vi = (Get-Item $path).VersionInfo
    if ($vi.CompanyName -notmatch 'NVIDIA') { return $true }   # not an NVIDIA file: not an NVIDIA licence question
    $sig = Get-AuthenticodeSignature -FilePath $path
    $signer = ''
    if ($sig.SignerCertificate) { $signer = $sig.SignerCertificate.Subject }
    return ($sig.Status -eq 'Valid' -and $signer -match 'NVIDIA')
}
# Extracts the runtime subset into $dest (only the selected entries are read from the zip).
function Expand-RuntimeSubset([string]$zip, [string]$dest) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $exe = $z.Entries | Where-Object { $_.FullName -match '(^|/)veyra_qml_ui\.exe$' } | Select-Object -First 1
        if (-not $exe) { Fail 'The runtime package has an unexpected layout (veyra_qml_ui.exe not found).' }
        $prefix = $exe.FullName.Substring(0, $exe.FullName.Length - 'veyra_qml_ui.exe'.Length)
        $want = @{}
        foreach ($r in ($RuntimeFiles + $ExtraCandidates + $RuntimeNotices)) { $want[($prefix + $r.Replace('\', '/')).ToLowerInvariant()] = $r }
        $got = @{}
        $todo = @()
        foreach ($e in $z.Entries) {
            $key = $e.FullName.ToLowerInvariant()
            $rel = $null
            if ($want.ContainsKey($key)) { $rel = $want[$key] }
            else { foreach ($d in ($RuntimeNoticeDirs + $RuntimeDirs)) { if ($key.StartsWith(($prefix + $d.Replace('\', '/') + '/').ToLowerInvariant()) -and $e.Name) { $rel = $e.FullName.Substring($prefix.Length).Replace('/', '\') } } }
            if ($rel) { $todo += [pscustomobject]@{ E = $e; Rel = $rel } }
        }
        $total = [double](($todo | ForEach-Object { $_.E.Length } | Measure-Object -Sum).Sum); if ($total -le 0) { $total = 1 }
        $doneBytes = 0.0
        foreach ($t in $todo) {
            $e = $t.E; $rel = $t.Rel
            $doneBytes += $e.Length
            Progress ([int](5 + 75 * $doneBytes / $total)) "Extracting $rel"
            $out = Join-Path $dest $rel
            New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
            $got[$rel] = $true
        }
        # NVIDIA's Video Effects files from NVIDIA's own wheel (pinned, the same SHA-256 as $RuntimeFileHashes): when asked
        # for (-VfxSource Wheel) or when the package does not carry them
        $vfx = @($RuntimeFiles | Where-Object { $_ -like 'runtime\nvidia-vfg\*' })
        $vfxMissing = @($vfx | Where-Object { -not $got.ContainsKey($_) })
        if ($vfx.Count -and ($VfxSource -eq 'Wheel' -or $vfxMissing.Count)) {
            $fetch = Join-Path $PSScriptRoot 'nvidia-vfx-fetch.ps1'
            if (-not (Test-Path $fetch)) { Fail "The runtime package lacks $($vfxMissing -join ', ') and nvidia-vfx-fetch.ps1 is not next to the installer." }
            Step "NVIDIA Video Effects files from NVIDIA's PyPI wheel (pinned SHA-256)"
            $names = @($vfx | ForEach-Object { Split-Path $_ -Leaf })
            $cache = if ($PackageCache) { $PackageCache } else { Join-Path $InstallDir 'download' }
            & $fetch -Dest (Join-Path $dest 'runtime\nvidia-vfg') -Cache $cache -Only $names -AcceptNvidiaLicense
            if ($LASTEXITCODE -ne 0) { Fail "nvidia-vfx-fetch.ps1 failed (exit $LASTEXITCODE)." }
            foreach ($r in $vfx) { $got[$r] = $true }
            $script:RuntimeSource = "$($Runtime.Asset) + nvidia-vfx $VfxSource"
        }
        foreach ($r in $RuntimeFiles) { if (-not $got.ContainsKey($r)) { Fail "The runtime package lacks $r." } }
        foreach ($r in $got.Keys) { if ($RuntimeFileHashes.ContainsKey($r) -and -not (Test-PinnedFile (Join-Path $dest $r) $RuntimeFileHashes[$r])) { Fail "$r does not match its pinned SHA-256." } }
    } finally { $z.Dispose() }
}
# The runtime component in $dest from the package: the subset, then the licence rule of PushFPS's installer:
#  - NVIDIA's files only as NVIDIA ships them: every NVIDIA file PushMPC needs must be an NVIDIA-signed original
#    (else the package is not the expected one: stop);
#  - an NVIDIA file that is not a signed original (a community-modified build) is an "experimental extra": left out
#    unless chosen (-ExperimentalExtras Yes, or yes when asked in a console);
#  - the package's licence texts (LICENSE, NOTICE, THIRD_PARTY_NOTICES, licenses\*) go with the files.
function Build-RuntimeComponent([string]$zip, [string]$dest, $pkg = $Runtime) {
    Step "Extracting PushMPC's subset of $($pkg.Asset)"
    Expand-RuntimeSubset $zip $dest
    foreach ($x in ($RuntimeFiles | Where-Object { $_ -like '*.dll' })) {
        if (-not (Test-NvidiaOriginal (Join-Path $dest $x))) { Fail "$x claims to be NVIDIA's but is not an NVIDIA-signed original: the package is not the expected one." }
    }
    $extras = @()
    foreach ($f in (Get-ChildItem $dest -Recurse -File -Include *.dll, *.exe)) {
        $rel = $f.FullName.Substring((Resolve-Path $dest).Path.TrimEnd('\').Length + 1)
        if ($RuntimeFiles -contains $rel) { continue }
        if (-not (Test-NvidiaOriginal $f.FullName)) { $extras += $rel }
    }
    $keep = $false
    if ($extras.Count) {
        if ($ExperimentalExtras -eq 'Yes') { $keep = $true }
        elseif ($ExperimentalExtras -eq 'Ask' -and -not [Console]::IsInputRedirected -and -not $DryRun) {
            Step "Optional: Veyra's experimental extras"
            foreach ($x in $extras) { Info "- $x (community-modified NVIDIA file: PushFPS's default AI enhancement slot, for RTX 20-40)" }
            Info 'They use NVIDIA files outside the terms NVIDIA licenses them under, so PushMPC leaves them out unless you choose them.'
            Info "Without them, AI enhancement uses NVIDIA's original runtime (RTX 50)."
            $keep = (Read-Host '    Install the experimental extras too? [y/N]') -match '^(y|yes|j|ja)$'
        }
    }
    foreach ($x in $extras) {
        if ($keep) { Info "Experimental extra kept by your choice: $x" } else { Remove-Item (Join-Path $dest $x) -Force; Info "Not installed: $x (install with -ExperimentalExtras Yes)" }
    }
    Get-ChildItem $dest -Recurse -Directory | Sort-Object FullName -Descending | Where-Object { -not (Get-ChildItem $_.FullName -Force) } | Remove-Item -Force
    $extraNote = 'none'
    if ($keep -and $extras.Count) { $extraNote = ($extras -join ';') }
    Add-FidelityFxSdk $dest
    $pf = Add-PushFlow $dest
    Write-RuntimeManifest $dest $pkg $extraNote $pf
    Info 'Licences: the package''s licence texts are in the runtime folder (LICENSE, THIRD_PARTY_NOTICES.md, licenses\).'
}
# AMD FSR SDK 2.4.0's signed DLLs over the package's 2.3.0 set (unlock Fsr4Int8; the self-test is the fetch script's pin,
# version and AMD-signature check, recorded in HKCU\Software\PushMPC\SelfTest\Fsr4Int8). A failure keeps the package's set.
function Add-FidelityFxSdk([string]$dest) {
    $want = $FidelityFx
    if ($want -eq 'Auto') {
        $off = $env:PUSHMPC_DISABLE_FSR4INT8 -match '^(1|true|yes|on)$'
        try { if ((Get-ItemProperty 'HKCU:\Software\PushMPC\Unlocks' -Name Fsr4Int8 -ErrorAction Stop).Fsr4Int8 -eq 0) { $off = $true } } catch {}
        $want = if ($GpuVendor -eq 'AMD' -and -not $off) { 'Sdk240' } else { 'Package' }
        if ($GpuVendor -eq 'AMD' -and $off) { Info 'FSR SDK 2.4.0: switched off (unlock Fsr4Int8): the package''s FidelityFX 2.3.0 stays' }
    }
    if ($want -ne 'Sdk240') { return }
    $fetch = Join-Path $PSScriptRoot 'fidelityfx-fetch.ps1'
    if (-not (Test-Path $fetch)) { Warn 'fidelityfx-fetch.ps1 is not next to the installer: the package''s FidelityFX 2.3.0 stays'; return }
    if ($DryRun) { Dry "would replace runtime_local\amd\fidelityfx with AMD FSR SDK 2.4.0's signed DLLs (fidelityfx-fetch.ps1)"; return }
    Step 'AMD FSR SDK 2.4.0 (FSR 4.1 upscaling on RX 6000 / 7000 / 9000), from AMD''s repository, pinned'
    $cache = if ($PackageCache) { Join-Path $PackageCache 'fidelityfx-v2.4.0' } else { Join-Path $InstallDir 'download\fidelityfx-v2.4.0' }
    & $fetch -Dest (Join-Path $dest 'runtime_local\amd\fidelityfx') -Cache $cache -Record
    if ($LASTEXITCODE -ne 0) { Warn "FSR SDK 2.4.0 not installed (fidelityfx-fetch.ps1 exit $LASTEXITCODE): the package's FidelityFX 2.3.0 stays" }
    else { $script:RuntimeSource = "$(if ($script:RuntimeSource) { $script:RuntimeSource } else { $Runtime.Asset }) + FidelityFX SDK 2.4.0" }
}
function Write-RuntimeManifest([string]$dest, $pkg, [string]$extraNote, [string]$pushflow) {
    $src = if ($script:RuntimeSource) { $script:RuntimeSource } else { $pkg.Asset }
    $pfNote = if ($pushflow) { $pushflow } else { 'not installed' }
    Write-Manifest $dest @{ component = 'runtime'; version = $pkg.Version; vendor = $pkg.Vendor; subset = $Runtime.Subset; package = $pkg.Asset; 'package-url' = $pkg.Url
                            'package-sha256' = $pkg.Sha256; extras = $extraNote; source = $src; pushflow = $pfNote
                            licence = 'LICENSE, THIRD_PARTY_NOTICES.md and licenses\ in this folder (NVIDIA components: licenses\nvidia-*; PushFlow: runtime\experimental\pushflow\licenses)' }
}
# Is the installed runtime this release's package with this installer's file list?
function Test-RuntimeCurrent([string]$dir, $pkg = $Runtime) {
    $m = Read-Manifest $dir
    if (-not $m) { return $false }
    if ($m.kv['version'] -ne $pkg.Version -or $m.kv['package-sha256'] -ne $pkg.Sha256 -or $m.kv['subset'] -ne $Runtime.Subset) { return $false }
    return -not (Test-ComponentFiles $dir)
}
# Builds the runtime component for install/update into staging\runtime-<version>: from a staged runtime of the same
# package and subset (developer builds), else from the package (cache, own download folder, or Veyra's release).
function Prepare-Runtime($pkg = $Runtime) {
    $staged = Join-Path $StageDir 'runtime'
    if (Test-RuntimeCurrent $staged $pkg) { Info "Runtime: the staged copy ($staged)"; return $staged }
    $dest = Join-Path $InstallDir "staging\runtime-$($pkg.Version)"
    if (-not $DryRun) { if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }; New-Item -ItemType Directory -Force $dest | Out-Null }
    # an existing PushFPS / Veyra install with the pinned files first (no download), else the package
    if (Get-ExistingRuntime $dest $pkg) {
        if ($DryRun) { Add-PushFlow $dest | Out-Null; return $null }
        Add-FidelityFxSdk $dest
        Write-RuntimeManifest $dest $pkg 'none (from an existing install)' (Add-PushFlow $dest)
        return $dest
    }
    $zip = Get-RuntimePackage $pkg
    if (-not $zip) { Add-PushFlow $dest | Out-Null; return $null }   # dry run without the package
    if ($DryRun) { Dry "would extract the runtime subset ($($RuntimeFiles.Count) files + licence texts) into $dest"; return $null }
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    New-Item -ItemType Directory -Force $dest | Out-Null
    Build-RuntimeComponent $zip $dest $pkg
    return $dest
}

# ------------------------------------------------------------------------------------------------ plan (for Setup and the updater)
# The runtime package a release manifest names for this GPU vendor (schema 3: one runtime entry per vendor; schema 2:
# one entry). Returns the package hashtable Prepare-Runtime takes, or $null when the manifest has no runtime.
function Get-ReleaseRuntime($rel) {
    $rts = @($rel.components | Where-Object { $_.name -eq 'runtime' })
    $c = $rts | Where-Object { $_.vendor -and $_.vendor -eq $GpuVendor } | Select-Object -First 1
    if (-not $c) { $c = $rts | Where-Object { -not $_.vendor } | Select-Object -First 1 }
    if (-not $c) { return $null }
    if ($c.format -ne 'veyra-package' -or $c.url -notmatch '^https://github\.com/[^/]+/[^/]+/releases/download/') {
        Fail 'The release manifest''s runtime is not a Veyra package reference (format veyra-package, a GitHub release URL): refused.'
    }
    if ($c.subset -and $c.subset -ne $Runtime.Subset) { Warn "The release's runtime file list (subset $($c.subset)) differs from this installer's ($($Runtime.Subset)): this installer's list is used." }
    return @{ Version = $c.version; Vendor = $GpuVendor; Asset = $(if ($c.asset) { $c.asset } else { Split-Path $c.url -Leaf }); Url = $c.url; Sha256 = $c.sha256 }
}
# An existing PushFPS / Veyra install with every pinned runtime file (checked, nothing copied), or ''
function Find-ExistingRuntimeDir {
    if ($NoExisting) { return '' }
    foreach ($dir in (Find-ExistingInstalls)) {
        $ok = $true
        foreach ($r in $RuntimeFiles) { if (-not $RuntimeFileHashes.ContainsKey($r) -or -not (Test-PinnedFile (Join-Path $dir $r) $RuntimeFileHashes[$r])) { $ok = $false; break } }
        if ($ok) { return $dir }
    }
    return ''
}
# Where PushFlow would come from: existing | zip | download
function Find-PushFlowSource {
    if (-not $NoExisting) {
        foreach ($dir in (Find-ExistingInstalls)) {
            $src = Join-Path $dir $PushFlow.Rel
            if (-not (Test-Path $src)) { continue }
            $all = $true
            foreach ($k in $PushFlow.Files.Keys) { if (-not (Test-PinnedFile (Join-Path $src $k) $PushFlow.Files[$k])) { $all = $false; break } }
            if ($all) { return 'existing' }
        }
    }
    if ($PackageCache -and @(Get-ChildItem $PackageCache -Filter 'PushFPS-*-win64.zip' -ErrorAction SilentlyContinue).Count) { return 'zip' }
    if (Test-Path (Join-Path $InstallDir "download\$($PushFlow.Asset)")) { return 'zip' }
    return 'download'
}

# ------------------------------------------------------------------------------------------------ code signing hook
# PUSHMPC_CODESIGN = a command line with {file}, e.g.
#   "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /a "{file}"
# Unset: nothing is signed (PushMPC's binaries are unsigned today; SmartScreen warns on the first run of Setup).
# Binaries are signed before their component manifest is written (the manifest holds their SHA-256).
function Invoke-CodeSign([string[]]$files) {
    if (-not $env:PUSHMPC_CODESIGN) { Info "Code signing: not configured (PUSHMPC_CODESIGN unset), $($files.Count) file(s) stay unsigned"; return }
    foreach ($f in $files) {
        $cmd = $env:PUSHMPC_CODESIGN.Replace('{file}', $f)
        Info "Signing $(Split-Path $f -Leaf)"
        & cmd.exe /d /c $cmd
        if ($LASTEXITCODE -ne 0) { Fail "Code signing failed for $f (exit $LASTEXITCODE)" }
    }
}
# release.json.sig: base64 RSA-SHA256 (PKCS#1 v1.5) over the file bytes. Key: -SigningKey <xml file>, else the
# PUSHMPC_SIGNING_KEY environment variable (the XML text; GitHub secret), else <repo>\deps\signing\release-private.xml.
function Get-SigningKeyXml {
    if ($SigningKey) { return [IO.File]::ReadAllText($SigningKey) }
    if ($env:PUSHMPC_SIGNING_KEY) { return $env:PUSHMPC_SIGNING_KEY }
    $p = Join-Path $repo 'deps\signing\release-private.xml'
    if (Test-Path $p) { return [IO.File]::ReadAllText($p) }
    return ''
}

# ------------------------------------------------------------------------------------------------ stage (build the components)
function Test-CoreNvof([string]$dll) {
    $t = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($dll))
    return $t.IndexOf('NvOFAPICreateInstanceD3D12', [StringComparison]::Ordinal) -ge 0
}
# the "PushCoreFeatures" value of pushcore.dll's version resource (cores built since 0.6.0 RC2: "nvof" or "no-nvof"), or ''
function Get-CoreFeatures([string]$dll) {
    $b = [IO.File]::ReadAllBytes($dll)
    $u = [Text.Encoding]::Unicode.GetString($b)
    $m = [regex]::Match($u, 'PushCoreFeatures\x00+([a-z0-9,\-]+)')
    if ($m.Success) { return $m.Groups[1].Value }
    return ''
}
function Invoke-Stage {
    $build = $env:PUSHMPC_BUILD_DIR
    if (-not $build) { $build = Join-Path $repo 'build' }
    if (-not $FilterBin) { $script:FilterBin = Join-Path $repo 'filter\bin' }
    if (-not $CoreDll) { $script:CoreDll = Join-Path $build 'pushmpc\pushcore.dll' }
    if (-not $ShaderDir) { $script:ShaderDir = Join-Path $build 'shaders' }
    foreach ($p in @((Join-Path $FilterBin 'PushMpcFilter.ax'), (Join-Path $FilterBin 'PushMpcExit.dll'), $CoreDll, $ShaderDir)) { if (-not (Test-Path $p)) { Fail "Build output missing: $p" } }
    $verHeader = Join-Path $repo 'filter\src\version.h'
    $filterVersion = ([regex]::Match([IO.File]::ReadAllText($verHeader), 'PUSHMPC_VERSION_STR "([^"]+)"')).Groups[1].Value
    $pushfps = (Get-Item $CoreDll).VersionInfo.ProductVersion
    Step "Staging PushMPC $filterVersion into $StageDir"
    if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
    $f = Join-Path $StageDir 'filter'; $c = Join-Path $StageDir 'core'; $r = Join-Path $StageDir 'runtime'
    New-Item -ItemType Directory -Force $f, $c, (Join-Path $c 'shaders'), $r | Out-Null
    Copy-Item (Join-Path $FilterBin 'PushMpcFilter.ax'), (Join-Path $FilterBin 'PushMpcExit.dll') $f
    Copy-Item (Join-Path $here 'pushmpc-install.ps1') $f   # the installer travels with the filter (uninstall, update)
    # its helpers for NVIDIA's wheel and AMD's FSR SDK 2.4.0 (fetched at install, pinned) travel with it
    foreach ($x in 'nvidia-vfx-fetch.ps1', 'nvidia-vfx-pin.json', 'fidelityfx-fetch.ps1', 'pushmpc-capture.ps1', 'pmml-fetch.ps1', 'ml-runtime-pin.json', 'pushmpc-plex.ps1') { if (Test-Path (Join-Path $here $x)) { Copy-Item (Join-Path $here $x) $f } }
    # the updater travels with the filter too (install copies it to <InstallDir>\PushMpcUpdater.exe; it updates itself from here)
    if (-not $UpdaterExe) { $script:UpdaterExe = Join-Path $repo 'updater\bin\PushMpcUpdater.exe' }
    if (-not (Test-Path $UpdaterExe)) { Fail "Build output missing: $UpdaterExe (run updater\build.cmd)" }
    Copy-Item $UpdaterExe $f
    $key = Join-Path $repo 'updater\release-key.xml'   # public key: the script's update action checks release.json.sig with it
    if (Test-Path $key) { Copy-Item $key $f }
    Invoke-CodeSign @((Join-Path $f 'PushMpcFilter.ax'), (Join-Path $f 'PushMpcExit.dll'), (Join-Path $f 'PushMpcUpdater.exe'))
    Write-Manifest $f @{ component = 'filter'; version = $filterVersion; repo = $PushMpcRepo; clsid = $FilterClsid }
    Copy-Item $CoreDll $c
    $preflight = Join-Path (Split-Path $CoreDll) 'pushcore_hip_preflight.exe'   # the AMD HIP self-test child (RX 9000 NR)
    if (Test-Path $preflight) { Copy-Item $preflight $c }
    Copy-Item (Join-Path $ShaderDir '*') (Join-Path $c 'shaders') -Recurse -Include *.dxil
    if (Test-Path (Join-Path $ShaderDir 'dis')) { Copy-Item (Join-Path $ShaderDir 'dis') (Join-Path $c 'shaders') -Recurse -Force }
    Get-ChildItem (Join-Path $c 'shaders') -Recurse -File | Where-Object { $_.Extension -ne '.dxil' } | Remove-Item -Force
    Invoke-CodeSign @(Get-ChildItem $c -File -Include *.dll, *.exe -Recurse | ForEach-Object FullName)
    # RC2 F1: NVIDIA optical flow (VFG's flow source) is built in only with the NVOF SDK headers; without them pushcore links
    # PushFPS's "unavailable" session, VFG fails with "failed backend 2" and Auto falls back to DLSS-G. The real session
    # resolves NvOFAPICreateInstanceD3D12 by name, so the name is in the DLL exactly when NVOF is built in. Recorded as
    # nvof=1|0 in the core manifest (and release.json); -RequireNvof (release.yml) refuses a core without it.
    $nvof = Test-CoreNvof $CoreDll
    $feat = Get-CoreFeatures $CoreDll
    if ($nvof) { Info "pushcore.dll: NVIDIA optical flow built in$(if ($feat) { " (features: $feat)" })" }
    elseif ($RequireNvof) { Fail "pushcore.dll was built WITHOUT NVIDIA optical flow (no NVOF SDK headers): NVIDIA VFG would not start. Not staged (-RequireNvof)." }
    else { Warn "pushcore.dll was built WITHOUT NVIDIA optical flow (no NVOF SDK headers): NVIDIA VFG will not start, Auto falls back to DLSS-G." }
    # RC2 F6: the core is the same for every GPU: both runtime packages are named (the install picks one by vendor)
    Write-Manifest $c @{ component = 'core'; version = "$filterVersion+pushfps$pushfps"; pushfps = $pushfps; 'requires-runtime' = ($Packages.NVIDIA.Version + '|' + $Packages.AMD.Version)
                         'runtime-package-nvidia' = $Packages.NVIDIA.Url; 'runtime-sha256-nvidia' = $Packages.NVIDIA.Sha256
                         'runtime-package-amd' = $Packages.AMD.Url; 'runtime-sha256-amd' = $Packages.AMD.Sha256
                         'runtime-subset' = $Runtime.Subset; abi = '3'; nvof = $(if ($nvof) { '1' } else { '0' }) }
    # The runtime is never part of a release. -WithRuntime stages it too, for a developer's local install from a
    # package already on disk (-PackageCache / -RuntimePackage); "package" leaves it out either way.
    if ($WithRuntime) {
        $zip = Get-RuntimePackage
        if ($zip) { New-Item -ItemType Directory -Force $r | Out-Null; Build-RuntimeComponent $zip $r }
        else { Info 'Runtime not staged (dry run without a package).' }
    } elseif (Test-Path $r) { Remove-Item $r -Recurse -Force }
    Stage-VsPlugin $filterVersion
    Stage-Vlc $filterVersion
    Step 'Staged'
    foreach ($n in $Components) {
        if ($n -eq 'runtime' -and -not $WithRuntime) { Info ("{0,-8} not staged: a release never contains it (Veyra's package at install time)" -f $n); continue }
        Info ("{0,-8} {1}" -f $n, (Component-Summary (Join-Path $StageDir $n)))
    }
    $vsm = Join-Path $StageDir 'vsplugin'
    if (Read-Manifest $vsm) { Info ("{0,-8} {1}" -f 'vsplugin', (Component-Summary $vsm)) } else { Info ("{0,-8} not staged (optional: the mpv route)" -f 'vsplugin') }
}
# The optional VLC component: the VLC 3 module (-VlcPlugin <libpushmpc_plugin.dll>, built by vlc-plugin.yml) and its
# installer pushmpc-vlc.ps1. Skipped when no plugin build is given.
function Stage-Vlc([string]$version) {
    $v = Join-Path $StageDir 'vlc'
    if (Test-Path $v) { Remove-Item $v -Recurse -Force }
    $inst = Join-Path $repo 'vlcplugin\install\pushmpc-vlc.ps1'
    if (-not $VlcPlugin -or -not (Test-Path -LiteralPath $VlcPlugin) -or -not (Test-Path -LiteralPath $inst)) { Info 'vlc not staged (no -VlcPlugin build): VLC is covered by capture mode'; return }
    New-Item -ItemType Directory -Force $v | Out-Null
    Copy-Item -LiteralPath $VlcPlugin (Join-Path $v 'libpushmpc_plugin.dll')
    Copy-Item -LiteralPath $inst $v
    Invoke-CodeSign @((Join-Path $v 'libpushmpc_plugin.dll'))
    Write-Manifest $v @{ component = 'vlc'; version = $version; 'vlc-api' = '3.0' }
}# The optional vsplugin component (the mpv route): pushmpc_vs.dll, pushmpc.vpy, mpv-integration.ps1 and the
# VapourSynth runtime pins. Skipped with a warning while pushmpc_vs.dll is not built (it needs the VapourSynth SDK headers).
function Stage-VsPlugin([string]$version) {
    $v = Join-Path $StageDir 'vsplugin'
    if (Test-Path $v) { Remove-Item $v -Recurse -Force }
    if ($NoMpv) { Info 'vsplugin not staged (-NoMpv)'; return }
    $dll = if ($VsPluginDll) { $VsPluginDll } else { Join-Path $repo 'vsplugin\bin\pushmpc_vs.dll' }
    $files = @($dll, (Join-Path $repo 'vsplugin\scripts\pushmpc.vpy'), (Join-Path $repo 'integrations\mpv\mpv-integration.ps1'), (Join-Path $repo 'integrations\mpv\pushmpc.lua'), (Join-Path $here 'vapoursynth-pin.json'))
    $missing = @($files | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
    if ($missing.Count) {
        Warn "vsplugin (the mpv route) not staged: missing $($missing -join ', ') (pushmpc_vs.dll is built by vsplugin\build.cmd once the VapourSynth SDK headers are in vsplugin\third_party\vapoursynth)"
        return
    }
    New-Item -ItemType Directory -Force $v | Out-Null
    Copy-Item -LiteralPath $files -Destination $v
    if ((Split-Path $dll -Leaf) -ne 'pushmpc_vs.dll') { Rename-Item (Join-Path $v (Split-Path $dll -Leaf)) 'pushmpc_vs.dll' }
    # the ArtCNN upscaling shaders pushmpc.lua falls back to (generated from the pinned ONNX weights; Python 3, stdlib only)
    $py = @('py', 'python3', 'python') | ForEach-Object { Get-Command $_ -ErrorAction SilentlyContinue } | Where-Object { $_.Source -notmatch 'WindowsApps' } | Select-Object -First 1
    $gen = Join-Path $repo 'integrations\mpv\make-shaders.py'
    if ($py -and (Test-Path -LiteralPath $gen)) {
        $pyArgs = @(); if ($py.Name -like 'py*.exe' -and $py.Name -notlike 'python*') { $pyArgs = @('-3') }
        & $py.Source @pyArgs $gen (Join-Path $v 'shaders') | ForEach-Object { Info $_ }
        if ($LASTEXITCODE -ne 0) { Warn 'ArtCNN shaders not generated (make-shaders.py failed): the vsplugin component goes without them'; Remove-Item (Join-Path $v 'shaders') -Recurse -Force -ErrorAction SilentlyContinue }
    } else { Warn 'ArtCNN shaders not generated (no Python 3): the vsplugin component goes without them' }
    Invoke-CodeSign @((Join-Path $v 'pushmpc_vs.dll'))
    Write-Manifest $v @{ component = 'vsplugin'; version = $version; 'vapoursynth-api' = '4'; preview = 'yes' }
}

# ------------------------------------------------------------------------------------------------ package (a release)
# release\PushMPC-<v>\ = this installer + filter\ + core\ (and Install-PushMPC.cmd), zipped as PushMPC-<v>.zip; the
# per-component zips and release.json for "update". No NVIDIA file may be in any of them (checked).
function Invoke-Package {
    foreach ($n in @('filter', 'core')) { $bad = Test-ComponentFiles (Join-Path $StageDir $n); if ($bad) { Fail "Stage $StageDir\$n is not complete ($bad). Run: pushmpc-install.ps1 stage" } }
    $fv = (Read-Manifest (Join-Path $StageDir 'filter')).kv['version']
    $cm = Read-Manifest (Join-Path $StageDir 'core')
    $out = Join-Path $ReleaseDir "PushMPC-$fv"
    # the optional vsplugin component (mpv route) goes in when it is staged and complete
    $relNames = @('filter', 'core')
    $vsStage = Join-Path $StageDir 'vsplugin'
    if (Read-Manifest $vsStage) {
        $bad = Test-ComponentFiles $vsStage
        if ($bad) { Warn "vsplugin left out of the release: the stage is not complete ($bad)" } else { $relNames += 'vsplugin' }
    } else { Info 'vsplugin (the mpv route) is not staged: the release has the MPC-HC / MPC-BE filter only' }
    if ((Read-Manifest (Join-Path $StageDir 'vlc')) -and -not (Test-ComponentFiles (Join-Path $StageDir 'vlc'))) { $relNames += 'vlc' }
    Step "Packaging PushMPC $fv into $ReleaseDir"
    if ($DryRun) { Dry "would write $out (installer, filter\, core\), PushMPC-$fv.zip, the component zips and release.json"; return }
    if (Test-Path $out) { Remove-Item $out -Recurse -Force }
    New-Item -ItemType Directory -Force $out | Out-Null
    Copy-Item (Join-Path $here 'pushmpc-install.ps1') $out
    foreach ($x in 'nvidia-vfx-fetch.ps1', 'nvidia-vfx-pin.json', 'fidelityfx-fetch.ps1') { if (Test-Path (Join-Path $here $x)) { Copy-Item (Join-Path $here $x) $out } }
    $cmd = @('@echo off', 'rem PushMPC: installs for this Windows user (no administrator rights). Veyra''s runtime package is downloaded from Veyra''s GitHub release.',
             'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pushmpc-install.ps1" install %*', 'pause')
    [IO.File]::WriteAllText((Join-Path $out 'Install-PushMPC.cmd'), ($cmd -join "`r`n") + "`r`n")
    foreach ($n in $relNames) { & robocopy (Join-Path $StageDir $n) (Join-Path $out $n) /E /NFL /NDL /NJH /NJS /NP | Out-Null; if ($LASTEXITCODE -ge 8) { Fail "copy $n failed" } }
    # guard: nothing of NVIDIA's (by company name) in a release
    $nv = Get-ChildItem $out -Recurse -File -Include *.dll, *.exe, *.ax | Where-Object { $_.VersionInfo.CompanyName -match 'NVIDIA' }
    if ($nv) { Fail "NVIDIA files in the release folder: $(($nv | ForEach-Object Name) -join ', ')" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zips = @{}
    foreach ($n in $relNames) {
        $v = (Read-Manifest (Join-Path $out $n)).kv['version']
        $zipPath = Join-Path $ReleaseDir "PushMPC-$n-$v.zip"
        if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
        [IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $out $n), $zipPath)
        $zips[$n] = @{ version = $v; file = (Split-Path $zipPath -Leaf); sha = (Get-Sha256 $zipPath); msha = (Get-Sha256 (Join-Path (Join-Path $out $n) 'manifest.txt')) }
    }
    $all = Join-Path $ReleaseDir "PushMPC-$fv.zip"
    if (Test-Path $all) { Remove-Item $all -Force }
    [IO.Compression.ZipFile]::CreateFromDirectory($out, $all, [IO.Compression.CompressionLevel]::Optimal, $true)
    $relVersion = if ($ReleaseVersion) { $ReleaseVersion.TrimStart('v') } else { $fv }
    $runtimes = @(foreach ($vendor in @('NVIDIA', 'AMD')) {
        $p = $Packages[$vendor]
        # the runtime is Veyra's package, referenced, never hosted by PushMPC (one entry per GPU vendor)
        [ordered]@{ name = 'runtime'; vendor = $vendor; format = 'veyra-package'; version = $p.Version; subset = $p.Subset; asset = $p.Asset
                    repository = "https://github.com/$($p.Repo)"; tag = $p.Tag; url = $p.Url; sha256 = $p.Sha256 }
    })
    $rel = [ordered]@{
        schema = 3
        release = $relVersion
        tag = "v$relVersion"
        repository = $PushMpcRepo
        channel = $(if ($Prerelease) { 'beta' } else { 'stable' })
        components = @(
            [ordered]@{ name = 'filter'; version = $zips.filter.version; url = $zips.filter.file; sha256 = $zips.filter.sha; manifestSha256 = $zips.filter.msha },
            [ordered]@{ name = 'core'; version = $zips.core.version; pushfps = $cm.kv['pushfps']; nvof = $cm.kv['nvof']; requiresRuntime = ($Packages.NVIDIA.Version + '|' + $Packages.AMD.Version); url = $zips.core.file; sha256 = $zips.core.sha; manifestSha256 = $zips.core.msha }
        ) + @(if ($zips.vsplugin) { [ordered]@{ name = 'vsplugin'; version = $zips.vsplugin.version; optional = $true; url = $zips.vsplugin.file; sha256 = $zips.vsplugin.sha; manifestSha256 = $zips.vsplugin.msha } }) + $runtimes
        note = 'Component URLs are relative to this release (https://github.com/CptPakundo/PushMPC/releases/download/<tag>/). The runtime is a reference to Veyra''s package on Veyra''s GitHub release; PushMPC hosts no NVIDIA file.'
        signature = 'release.json.sig next to this file: base64 RSA-SHA256 over the file bytes, checked with release-key.xml'
    }
    $json = Join-Path $ReleaseDir 'release.json'
    [IO.File]::WriteAllText($json, ($rel | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
    # sign the manifest, then check the signature against the public key the updater embeds
    $sigPath = "$json.sig"
    if (Test-Path $sigPath) { Remove-Item $sigPath -Force }
    $keyXml = Get-SigningKeyXml
    $pubPath = Join-Path $repo 'updater\release-key.xml'
    if ($keyXml) {
        $rsa = New-Object System.Security.Cryptography.RSACryptoServiceProvider
        try {
            $rsa.FromXmlString($keyXml)
            $sig = [Convert]::ToBase64String($rsa.SignData([IO.File]::ReadAllBytes($json), 'SHA256'))
        } finally { $rsa.Dispose() }
        [IO.File]::WriteAllText($sigPath, $sig, (New-Object System.Text.UTF8Encoding($false)))
        if (Test-Path $pubPath) {
            $pub = New-Object System.Security.Cryptography.RSACryptoServiceProvider
            try { $pub.FromXmlString([IO.File]::ReadAllText($pubPath)); $ok = $pub.VerifyData([IO.File]::ReadAllBytes($json), 'SHA256', [Convert]::FromBase64String($sig)) } finally { $pub.Dispose() }
            if (-not $ok) { Fail "release.json.sig does not verify with ${pubPath}: the signing key is not the one whose public half the updater embeds." }
            Info 'release.json signed (verified with updater\release-key.xml)'
        } else { Warn 'release.json signed, but updater\release-key.xml is missing: the updater cannot check it' }
    } else { Warn 'release.json is NOT signed (no -SigningKey, PUSHMPC_SIGNING_KEY or deps\signing\release-private.xml): updaters in the field refuse it' }
    # PushMPC-Setup.exe: the filter and core components embedded (installer\build.cmd links the payload in)
    if (-not $NoSetup) {
        $payload = Join-Path $ReleaseDir 'setup-payload.bin'
        Write-SetupPayload $out $payload $relVersion
        $buildCmd = Join-Path $repo 'installer\build.cmd'
        & cmd.exe /d /c "`"$buildCmd`" `"$payload`""
        if ($LASTEXITCODE -ne 0) { Fail "installer\build.cmd failed (exit $LASTEXITCODE)" }
        $setup = Join-Path $ReleaseDir 'PushMPC-Setup.exe'
        Copy-Item (Join-Path $repo 'installer\bin\PushMPC-Setup.exe') $setup -Force
        Remove-Item $payload -Force
        Invoke-CodeSign @($setup)
    }
    Step 'Packaged'
    Get-ChildItem $ReleaseDir -File | ForEach-Object { Info ("{0,-50} {1,12:N0} bytes" -f $_.Name, $_.Length) }
    Info 'Not in the release: the runtime (the installer gets Veyra''s package from Veyra''s GitHub release).'
}
# The payload PushMPC-Setup.exe embeds: "PMPAYLD1", uint32 count, then per file uint32 path length, the UTF-8 path
# (filter\..., core\..., payload.txt), uint64 size, the bytes. Uncompressed (about 5 MB).
function Write-SetupPayload([string]$releaseFolder, [string]$dest, [string]$version) {
    $root = (Resolve-Path $releaseFolder).Path.TrimEnd('\')
    $dirs = @('filter', 'core', 'vsplugin', 'vlc') | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ }
    $files = @(Get-ChildItem $dirs -Recurse -File | Sort-Object FullName)
    $info = [Text.Encoding]::UTF8.GetBytes("release=$version`r`nfilter=$((Read-Manifest (Join-Path $root 'filter')).kv['version'])`r`ncore=$((Read-Manifest (Join-Path $root 'core')).kv['version'])`r`n")
    $fs = [IO.File]::Create($dest)
    try {
        $bw = New-Object IO.BinaryWriter($fs)
        $bw.Write([Text.Encoding]::ASCII.GetBytes('PMPAYLD1'))
        $bw.Write([uint32]($files.Count + 1))
        $entry = {
            param([string]$rel, [byte[]]$bytes)
            $p = [Text.Encoding]::UTF8.GetBytes($rel)
            $bw.Write([uint32]$p.Length); $bw.Write($p); $bw.Write([uint64]$bytes.Length); $bw.Write($bytes)
        }
        & $entry 'payload.txt' $info
        foreach ($f in $files) { & $entry $f.FullName.Substring($root.Length + 1) ([IO.File]::ReadAllBytes($f.FullName)) }
        $bw.Flush()
    } finally { $fs.Dispose() }
    Info ("Setup payload: {0} files, {1:N1} MB" -f $files.Count, ((Get-Item $dest).Length / 1MB))
}

# ------------------------------------------------------------------------------------------------ MPC-HC
function Get-MpcHcProcesses { @(Get-Process -Name 'mpc-hc64', 'mpc-hc' -ErrorAction SilentlyContinue) }
# processes that have a file of this component loaded
function Get-Holders([string]$dir) {
    $full = [IO.Path]::GetFullPath($dir).TrimEnd('\') + '\'
    $h = @()
    foreach ($p in @(Get-Process -Name 'mpc-hc64', 'mpc-hc', 'mpc-be64', 'mpc-be', 'PotPlayerMini64', 'PotPlayer64', 'mpv', 'smplayer', 'mpc-qt' -ErrorAction SilentlyContinue)) {
        try { if ($p.Modules | Where-Object { $_.FileName -and $_.FileName.StartsWith($full, [StringComparison]::OrdinalIgnoreCase) }) { $h += $p } } catch { $h += $p }
    }
    return $h
}
function Get-MpcEntries {
    if (-not (Test-Path $MpcFiltersKey)) { return @() }
    @(Get-ChildItem $MpcFiltersKey | Where-Object { $_.PSChildName -match '^\d{4}$' } | ForEach-Object { [pscustomobject]@{ Key = $_.PSPath; Name = $_.PSChildName; Props = (Get-ItemProperty $_.PSPath) } })
}
function Read-State {
    $p = Join-Path $InstallDir 'state.json'
    if (Test-Path $p) { return (Get-Content $p -Raw | ConvertFrom-Json) }
    return [pscustomobject]@{ mpcEntry = ''; disabledVapourSynth = ''; installed = '' }
}
function Write-State($s) { if (-not $DryRun) { $s | ConvertTo-Json | Set-Content -Path (Join-Path $InstallDir 'state.json') -Encoding UTF8 } }
function Set-MpcHcEntry($state) {
    if ($NoMpcHc) { Info 'MPC-HC entry skipped (-NoMpcHc)'; return }
    foreach ($ini in @((Join-Path $env:APPDATA 'MPC-HC\mpc-hc64.ini'), (Join-Path ${env:ProgramFiles} 'MPC-HC\mpc-hc64.ini'))) {
        if (Test-Path $ini) { Warn "MPC-HC keeps its settings in $ini (INI mode): add PushMPC Filter in Options > External Filters by hand (Prefer)." ; return }
    }
    $entries = Get-MpcEntries
    $ours = $entries | Where-Object { $_.Props.CLSID -eq $FilterClsid } | Select-Object -First 1
    if ($ours) { $key = $ours.Key; $name = $ours.Name; Info "MPC-HC External Filters: updating entry $name" }
    else {
        $n = 0; while ($entries | Where-Object { $_.Name -eq ('{0:D4}' -f $n) }) { $n++ }
        $name = '{0:D4}' -f $n; $key = Join-Path $MpcFiltersKey $name
        Info "MPC-HC External Filters: adding entry $name"
    }
    $types = @('{73646976-0000-0010-8000-00AA00389B71}', '{3231564E-0000-0010-8000-00AA00389B71}', '{73646976-0000-0010-8000-00AA00389B71}', '{30313050-0000-0010-8000-00AA00389B71}')
    if ($DryRun) { Dry "would write $key (PushMPC Filter, Prefer, NV12/P010)" }
    else {
        # MPC-HC's Filters keys this entry creates: uninstall removes them again while they are empty
        $made = @(@((Split-Path $MpcFiltersKey), $MpcFiltersKey) | Where-Object { -not (Test-Path $_) })
        if ($made.Count -and -not (Get-StateValue $state 'mpcCreatedKeys')) { Set-StateValue $state 'mpcCreatedKeys' ($made -join '|') }
        New-Item -Path $key -Force | Out-Null
        New-ItemProperty $key -Name SourceType -PropertyType DWord -Value 0 -Force | Out-Null
        New-ItemProperty $key -Name Enabled -PropertyType DWord -Value 1 -Force | Out-Null
        New-ItemProperty $key -Name Name -PropertyType String -Value 'PushMPC Filter' -Force | Out-Null
        New-ItemProperty $key -Name CLSID -PropertyType String -Value $FilterClsid -Force | Out-Null
        New-ItemProperty $key -Name DisplayName -PropertyType String -Value "@device:sw:{083863F1-70DE-11D0-BD40-00A0C911CE86}\$FilterClsid" -Force | Out-Null
        for ($i = 0; $i -lt $types.Count; $i++) {
            New-ItemProperty $key -Name ('org{0:D4}' -f $i) -PropertyType String -Value $types[$i] -Force | Out-Null
            New-ItemProperty $key -Name ('mod{0:D4}' -f $i) -PropertyType String -Value $types[$i] -Force | Out-Null
        }
        New-ItemProperty $key -Name LoadType -PropertyType DWord -Value 0 -Force | Out-Null      # Prefer
        New-ItemProperty $key -Name Merit -PropertyType DWord -Value 0x00200001 -Force | Out-Null
    }
    $state.mpcEntry = $name
    # SVP's VapourSynth filter interpolates too: frame generation twice would stack. Disabled here (and noted), and
    # enabled again by uninstall only if this installer disabled it.
    $vs = $entries | Where-Object { $_.Props.CLSID -eq $VapourSynthClsid -and $_.Props.Enabled -eq 1 }
    foreach ($e in $vs) {
        Warn "SVP's VapourSynth Filter (entry $($e.Name)) is disabled: SVP's interpolation and PushMPC's frame generation would stack. Tick it again in MPC-HC Options > External Filters to use SVP."
        if ($DryRun) { Dry "would set $($e.Key) Enabled=0" } else { Set-ItemProperty $e.Key -Name Enabled -Value 0 }
        $state.disabledVapourSynth = $e.Name
    }
}
function Remove-MpcHcEntry($state) {
    $entries = Get-MpcEntries
    foreach ($e in ($entries | Where-Object { $_.Props.CLSID -eq $FilterClsid })) {
        if ($DryRun) { Dry "would remove $($e.Key)" } else { Remove-Item $e.Key -Recurse -Force }
        Info "MPC-HC External Filters: entry $($e.Name) removed"
    }
    # the Filters keys the install created, when nothing else is in them now (deepest first)
    $made = "$(Get-StateValue $state 'mpcCreatedKeys')"
    if ($made) {
        foreach ($k in @($made.Split('|') | Sort-Object Length -Descending)) {
            if (-not (Test-Path $k)) { continue }
            if (@(Get-ChildItem $k).Count -or (Get-Item $k).ValueCount) { continue }
            if ($DryRun) { Dry "would remove the empty key $k" } else { Remove-Item $k -Force; Info "MPC-HC: the empty key $k (made by the install) removed" }
        }
    }
    if ($state.disabledVapourSynth) {
        $e = $entries | Where-Object { $_.Name -eq $state.disabledVapourSynth -and $_.Props.CLSID -eq $VapourSynthClsid }
        if ($e -and $e.Props.Enabled -eq 0) {
            if ($DryRun) { Dry "would set $($e.Key) Enabled=1" } else { Set-ItemProperty $e.Key -Name Enabled -Value 1 }
            Info "SVP's VapourSynth Filter enabled again (this installer had disabled it)"
        }
    }
}
# MPC Video Renderer's HDR settings (user-approved): local tone mapping on, type Spline (7). The filter sets HdrDisplayNits
# to the display's peak when it first plays on an HDR display (it knows the display). The values found first are kept in
# HKCU\Software\PushMPC\MpcVrBackup ("absent" = the value did not exist) and restored by uninstall.
$MpcVrKey = 'HKCU:\Software\MPC-BE Filters\MPC Video Renderer'
$MpcVrBackupKey = 'HKCU:\Software\PushMPC\MpcVrBackup'
$MpcVrValues = @('HdrLocalToneMapping', 'HdrLocalToneMappingType', 'HdrDisplayNits')
function Set-MpcVrHdr {
    if ($DryRun) { Dry "would set $MpcVrKey HdrLocalToneMapping=1, HdrLocalToneMappingType=7 (Spline), keeping the old values in $MpcVrBackupKey"; return }
    if (-not (Test-Path $MpcVrKey)) { New-Item -Path $MpcVrKey -Force | Out-Null }
    if (-not (Test-Path $MpcVrBackupKey)) {
        New-Item -Path $MpcVrBackupKey -Force | Out-Null
        foreach ($n in $MpcVrValues) {
            $v = (Get-ItemProperty -Path $MpcVrKey -Name $n -ErrorAction SilentlyContinue).$n
            if ($null -ne $v) { New-ItemProperty -Path $MpcVrBackupKey -Name $n -Value ([int]$v) -PropertyType DWord -Force | Out-Null }
            else { New-ItemProperty -Path $MpcVrBackupKey -Name $n -Value 'absent' -PropertyType String -Force | Out-Null }
        }
    }
    New-ItemProperty -Path $MpcVrKey -Name 'HdrLocalToneMapping' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $MpcVrKey -Name 'HdrLocalToneMappingType' -Value 7 -PropertyType DWord -Force | Out-Null
    Info 'MPC Video Renderer: HDR local tone mapping on, type Spline (previous values kept for uninstall)'
}
function Restore-MpcVrHdr {
    if (-not (Test-Path $MpcVrBackupKey)) { return }
    if ($DryRun) { Dry "would restore $MpcVrKey from $MpcVrBackupKey"; return }
    foreach ($n in $MpcVrValues) {
        $v = (Get-ItemProperty -Path $MpcVrBackupKey -Name $n -ErrorAction SilentlyContinue).$n
        if ($null -eq $v) { continue }
        if ($v -is [string] -and $v -eq 'absent') { Remove-ItemProperty -Path $MpcVrKey -Name $n -ErrorAction SilentlyContinue }
        elseif (Test-Path $MpcVrKey) { New-ItemProperty -Path $MpcVrKey -Name $n -Value ([int]$v) -PropertyType DWord -Force | Out-Null }
    }
    Remove-Item -Path $MpcVrBackupKey -Recurse -Force
    Info 'MPC Video Renderer: HDR settings restored to what they were before PushMPC'
}

# ------------------------------------------------------------------------------------------------ MPC-BE
# MPC-BE (x64) keeps its settings in HKCU\Software\MPC-BE unless mpc-be64.ini exists next to mpc-be64.exe or in
# %APPDATA%\MPC-BE (INI mode: the user adds the filter by hand, as for MPC-HC). External filters are
# ExternalFilters\NNN (3 digits, read in order until the first missing number) with Name, SourceType (0 registered),
# Enabled, CLSID, DisplayName, LoadType (0 Prefer), Merit and MT_000... (major type GUID followed by the subtype GUID).
# Its own MPC Video Decoder defaults to D3D11 native decoding, which cannot feed a filter between it and the renderer
# (it then decodes in software): HwDecoder is set to 2 (D3D11 copy-back), the value found first kept in
# HKCU\Software\PushMPC\MpcBeBackup ("absent" = none) and restored on uninstall unless the user changed it since.
# Nothing is written when MPC-BE is not installed. Tests: -MpcBeRegRoot (a fake root), -MpcBeExe (a fake mpc-be64.exe
# that counts as installed), -MpcBeBackupKey; actions mpcbe-apply / mpcbe-remove run only these steps.
$MpcBeFilters = Join-Path $MpcBeRegRoot 'ExternalFilters'
$MpcBeDecoderKey = Join-Path $MpcBeRegRoot 'Filters\MPC Video Decoder'
function Get-MpcBeProcesses { @(Get-Process -Name 'mpc-be64', 'mpc-be' -ErrorAction SilentlyContinue) }
function Set-StateValue($state, [string]$name, $value) { $state | Add-Member -NotePropertyName $name -NotePropertyValue $value -Force }
function Get-StateValue($state, [string]$name) { if ($state.PSObject.Properties[$name]) { return $state.$name }; return '' }
# the installed mpc-be64.exe (same rules as the filter's MpcBeInstalled()), or '' when MPC-BE is not here
function Find-MpcBeExe {
    if ($MpcBeExe) { if (Test-Path -LiteralPath $MpcBeExe -PathType Leaf) { return $MpcBeExe }; return '' }
    $inFolder = { param($d) if ($d) { $p = Join-Path $d 'mpc-be64.exe'; if (Test-Path -LiteralPath $p -PathType Leaf) { return $p } }; return '' }
    foreach ($root in @('HKCU:', 'HKLM:')) {
        $ep = (Get-ItemProperty -Path "$root\Software\MPC-BE" -Name ExePath -ErrorAction SilentlyContinue).ExePath
        if ($ep) { if (Test-Path -LiteralPath $ep -PathType Leaf) { return $ep }; $p = & $inFolder $ep; if ($p) { return $p } }
        foreach ($u in @("$root\Software\Microsoft\Windows\CurrentVersion\Uninstall", "$root\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall")) {
            if (-not (Test-Path $u)) { continue }
            foreach ($k in (Get-ChildItem $u -ErrorAction SilentlyContinue)) {
                $pr = Get-ItemProperty $k.PSPath -ErrorAction SilentlyContinue
                if ($pr.DisplayName -like 'MPC-BE*') { $p = & $inFolder $pr.InstallLocation; if ($p) { return $p } }
            }
        }
    }
    foreach ($pf in @($env:ProgramW6432, $env:ProgramFiles) | Where-Object { $_ }) {
        foreach ($d in @('MPC-BE x64', 'MPC-BE')) { $p = & $inFolder (Join-Path $pf $d); if ($p) { return $p } }
    }
    return ''
}
function Get-MpcBeIni([string]$exe) {
    foreach ($ini in @((Join-Path (Split-Path -Parent $exe) 'mpc-be64.ini'), $(if (-not $MpcBeExe) { Join-Path $env:APPDATA 'MPC-BE\mpc-be64.ini' }))) {
        if ($ini -and (Test-Path -LiteralPath $ini)) { return $ini }
    }
    return ''
}
function Get-MpcBeEntries {
    if (-not (Test-Path $MpcBeFilters)) { return @() }
    @(Get-ChildItem $MpcBeFilters | Where-Object { $_.PSChildName -match '^\d{3}$' } | Sort-Object PSChildName | ForEach-Object { [pscustomobject]@{ Key = $_.PSPath; Name = $_.PSChildName; Props = (Get-ItemProperty $_.PSPath) } })
}
function Set-MpcBeEntry($state) {
    if ($NoMpcBe) { Info 'MPC-BE skipped (-NoMpcBe)'; return }
    $exe = Find-MpcBeExe
    if (-not $exe) { Info 'MPC-BE is not installed: nothing changed for it'; return }
    $ini = Get-MpcBeIni $exe
    if ($ini) {
        Warn "MPC-BE keeps its settings in $ini (INI mode): add PushMPC Filter in Options > External Filters by hand (Prefer), and set MPC Video Decoder's hardware decoding to D3D11 copy-back."
        return
    }
    $entries = Get-MpcBeEntries
    $ours = $entries | Where-Object { $_.Props.CLSID -eq $FilterClsid } | Select-Object -First 1
    if ($ours) { $key = $ours.Key; $name = $ours.Name; Info "MPC-BE External Filters: updating entry $name" }
    else {
        $n = 0; while ($entries | Where-Object { $_.Name -eq ('{0:D3}' -f $n) }) { $n++ }
        $name = '{0:D3}' -f $n; $key = Join-Path $MpcBeFilters $name
        Info "MPC-BE External Filters: adding entry $name"
    }
    $vids = '{73646976-0000-0010-8000-00AA00389B71}'
    $types = @("$vids{3231564E-0000-0010-8000-00AA00389B71}", "$vids{30313050-0000-0010-8000-00AA00389B71}")   # NV12, P010
    if ($DryRun) { Dry "would write $key (PushMPC Filter, Prefer, NV12/P010)" }
    else {
        New-Item -Path $key -Force | Out-Null
        New-ItemProperty $key -Name Name -PropertyType String -Value 'PushMPC Filter' -Force | Out-Null
        New-ItemProperty $key -Name SourceType -PropertyType DWord -Value 0 -Force | Out-Null
        New-ItemProperty $key -Name Enabled -PropertyType DWord -Value 1 -Force | Out-Null
        New-ItemProperty $key -Name CLSID -PropertyType String -Value $FilterClsid -Force | Out-Null
        New-ItemProperty $key -Name DisplayName -PropertyType String -Value "@device:sw:{083863F1-70DE-11D0-BD40-00A0C911CE86}\$FilterClsid" -Force | Out-Null
        New-ItemProperty $key -Name LoadType -PropertyType DWord -Value 0 -Force | Out-Null      # Prefer
        New-ItemProperty $key -Name Merit -PropertyType DWord -Value 0x00200001 -Force | Out-Null
        for ($i = 0; $i -lt $types.Count; $i++) { New-ItemProperty $key -Name ('MT_{0:D3}' -f $i) -PropertyType String -Value $types[$i] -Force | Out-Null }
    }
    Set-StateValue $state 'mpcBeEntry' $name
    $vs = $entries | Where-Object { $_.Props.CLSID -eq $VapourSynthClsid -and $_.Props.Enabled -eq 1 }
    foreach ($e in $vs) {
        Warn "SVP's VapourSynth Filter in MPC-BE (entry $($e.Name)) is disabled: SVP's interpolation and PushMPC's frame generation would stack."
        if ($DryRun) { Dry "would set $($e.Key) Enabled=0" } else { Set-ItemProperty $e.Key -Name Enabled -Value 0 }
        Set-StateValue $state 'mpcBeDisabledVapourSynth' $e.Name
    }
    Set-MpcBeDecoder
}
function Set-MpcBeDecoder {
    $cur = (Get-ItemProperty -Path $MpcBeDecoderKey -Name HwDecoder -ErrorAction SilentlyContinue).HwDecoder
    if ($DryRun) { Dry "would set $MpcBeDecoderKey HwDecoder=2 (D3D11 copy-back; now $(if ($null -ne $cur) { $cur } else { 'absent' })), keeping the old value in $MpcBeBackupKey"; return }
    if (-not (Test-Path $MpcBeBackupKey)) {
        New-Item -Path $MpcBeBackupKey -Force | Out-Null
        if ($null -ne $cur) { New-ItemProperty -Path $MpcBeBackupKey -Name HwDecoder -Value ([int]$cur) -PropertyType DWord -Force | Out-Null }
        else { New-ItemProperty -Path $MpcBeBackupKey -Name HwDecoder -Value 'absent' -PropertyType String -Force | Out-Null }
    }
    if (-not (Test-Path $MpcBeDecoderKey)) { New-Item -Path $MpcBeDecoderKey -Force | Out-Null }
    New-ItemProperty -Path $MpcBeDecoderKey -Name HwDecoder -Value 2 -PropertyType DWord -Force | Out-Null
    Info 'MPC-BE: MPC Video Decoder set to D3D11 copy-back (HwDecoder 2; the previous value is kept for uninstall)'
}
function Remove-MpcBeEntry($state) {
    $entries = Get-MpcBeEntries
    $gone = @()
    foreach ($e in ($entries | Where-Object { $_.Props.CLSID -eq $FilterClsid })) {
        if ($DryRun) { Dry "would remove $($e.Key)" } else { Remove-Item $e.Key -Recurse -Force }
        $gone += [int]$e.Name
        Info "MPC-BE External Filters: entry $($e.Name) removed"
    }
    $vsName = Get-StateValue $state 'mpcBeDisabledVapourSynth'
    if ($gone.Count) {
        # MPC-BE reads the entries until the first missing number: the later ones move down to close the gap
        $next = 0
        foreach ($e in $entries) {
            if ($gone -contains [int]$e.Name) { continue }
            $want = '{0:D3}' -f $next; $next++
            if ($e.Name -ne $want) {
                if ($DryRun) { Dry "would renumber $($e.Name) -> $want" } else { Rename-Item -Path $e.Key -NewName $want }
                if ($vsName -eq $e.Name) { $vsName = $want }
                Info "MPC-BE External Filters: entry $($e.Name) renumbered to $want"
            }
        }
    }
    if ($vsName) {
        $k = Join-Path $MpcBeFilters $vsName
        $p = Get-ItemProperty $k -ErrorAction SilentlyContinue
        if ($p -and $p.CLSID -eq $VapourSynthClsid -and $p.Enabled -eq 0) {
            if ($DryRun) { Dry "would set $k Enabled=1" } else { Set-ItemProperty $k -Name Enabled -Value 1 }
            Info "SVP's VapourSynth Filter in MPC-BE enabled again (this installer had disabled it)"
        }
    }
    Restore-MpcBeDecoder
}
function Restore-MpcBeDecoder {
    if (-not (Test-Path $MpcBeBackupKey)) { return }
    $old = (Get-ItemProperty -Path $MpcBeBackupKey -Name HwDecoder -ErrorAction SilentlyContinue).HwDecoder
    $cur = (Get-ItemProperty -Path $MpcBeDecoderKey -Name HwDecoder -ErrorAction SilentlyContinue).HwDecoder
    if ($DryRun) { Dry "would restore $MpcBeDecoderKey HwDecoder to $old (now $cur)"; return }
    if ($null -ne $cur -and [int]$cur -ne 2) { Info "MPC-BE: HwDecoder left at $cur (changed after PushMPC set it)" }
    elseif ($old -is [string] -and $old -eq 'absent') { Remove-ItemProperty -Path $MpcBeDecoderKey -Name HwDecoder -ErrorAction SilentlyContinue; Info 'MPC-BE: MPC Video Decoder hardware mode back to its default' }
    elseif ($null -ne $old) { New-ItemProperty -Path $MpcBeDecoderKey -Name HwDecoder -Value ([int]$old) -PropertyType DWord -Force | Out-Null; Info "MPC-BE: MPC Video Decoder hardware mode restored ($old)" }
    Remove-Item -Path $MpcBeBackupKey -Recurse -Force
}
# ------------------------------------------------------------------------------------------------ PotPlayer
# PotPlayer (x64, PotPlayerMini64.exe) keeps its settings in HKCU\Software\DAUM\PotPlayerMini64 unless
# PotPlayerMini64.ini exists next to the exe (INI mode: the user adds the filter by hand, see docs\user-guide.md).
# Its filter priority list ("Filter control > Filter priority (overall)") is Override\NNNN (4 digits, read in order;
# the last subkey holds only Type = -2, the end marker) with Type (1 = filter file, Path), Name, CLSID, Disabled,
# FilterType (0 = Prefer), Merit, MeritHi and mod0000... (media types: major type and subtype GUIDs in turn). The layout
# is checked on a hosted runner against a real PotPlayer (integrations.yml, job potplayer): with this entry PotPlayer
# loads PushMpcFilter.ax into its graph. Install adds the entry in front of the end marker; uninstall removes it, moves
# the later entries down and puts the end marker back where it was, so the key is as before. SVP's VapourSynth entry
# (when present and enabled) is disabled and enabled again on uninstall. PotPlayer must be closed (it rewrites its
# settings when it closes). The renderer (MPC Video Renderer) and the decoder (copy-back) stay the user's choice:
# Setup tells them (docs\user-guide.md, PotPlayer).
# Tests: -PotRegRoot (a fake root), -PotPlayerExe (a fake PotPlayerMini64.exe that counts as installed); actions
# potplayer-apply / potplayer-remove run only these steps.
$PotOverride = Join-Path $PotRegRoot 'Override'
$PotEndType = -2
function Get-PotPlayerProcesses { @(Get-Process -Name 'PotPlayerMini64', 'PotPlayer64', 'PotPlayerMini', 'PotPlayer' -ErrorAction SilentlyContinue) }
function Find-PotPlayerExe {
    if ($PotPlayerExe) { if (Test-Path -LiteralPath $PotPlayerExe -PathType Leaf) { return $PotPlayerExe }; return '' }
    foreach ($root in @('HKCU:', 'HKLM:')) {
        foreach ($k in @("$root\Software\DAUM\PotPlayer64", "$root\Software\DAUM\PotPlayer")) {
            $pp = (Get-ItemProperty -Path $k -Name ProgramPath -ErrorAction SilentlyContinue).ProgramPath
            if ($pp -and (Test-Path -LiteralPath $pp -PathType Leaf) -and $pp -match '64\.exe$') { return $pp }
        }
        foreach ($u in @("$root\Software\Microsoft\Windows\CurrentVersion\Uninstall")) {
            if (-not (Test-Path $u)) { continue }
            foreach ($k in (Get-ChildItem $u -ErrorAction SilentlyContinue)) {
                $pr = Get-ItemProperty $k.PSPath -ErrorAction SilentlyContinue
                if ($pr.PSObject.Properties['DisplayName'] -and $pr.DisplayName -like 'PotPlayer*' -and $pr.PSObject.Properties['UninstallString'] -and $pr.UninstallString) {
                    $d = Split-Path -Parent ($pr.UninstallString.Trim('"'))
                    $p = Join-Path $d 'PotPlayerMini64.exe'; if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
                }
            }
        }
    }
    foreach ($pf in @($env:ProgramW6432, $env:ProgramFiles) | Where-Object { $_ }) {
        $p = Join-Path $pf 'DAUM\PotPlayer\PotPlayerMini64.exe'; if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
    }
    return ''
}
function Get-PotEntries {
    if (-not (Test-Path $PotOverride)) { return @() }
    @(Get-ChildItem $PotOverride | Where-Object { $_.PSChildName -match '^\d{4}$' } | Sort-Object PSChildName | ForEach-Object {
        $p = Get-ItemProperty $_.PSPath
        $t = if ($p.PSObject.Properties['Type']) { [int64]$p.Type } else { $null }
        if ($null -ne $t -and $t -gt 2147483647) { $t -= 4294967296 }   # a DWORD read back unsigned (Windows PowerShell)
        [pscustomobject]@{ Key = $_.PSPath; Name = $_.PSChildName; Props = $p; End = ($t -eq $PotEndType); Clsid = $(if ($p.PSObject.Properties['CLSID']) { [string]$p.CLSID } else { '' }) } })
}
function Set-PotPlayerEntry($state) {
    if ($NoPotPlayer) { Info 'PotPlayer skipped (-NoPotPlayer)'; return }
    $exe = Find-PotPlayerExe
    if (-not $exe) { Info 'PotPlayer is not installed: nothing changed for it'; return }
    $ini = Join-Path (Split-Path -Parent $exe) 'PotPlayerMini64.ini'
    if (Test-Path -LiteralPath $ini) {
        Warn "PotPlayer keeps its settings in $ini (INI mode): add PushMPC Filter by hand (Preferences > Filter control > Filter priority (overall) > Add external filter, Prefer); see docs\user-guide.md."
        return
    }
    $ax = Join-Path $InstallDir 'filter\PushMpcFilter.ax'
    $entries = Get-PotEntries
    $ours = $entries | Where-Object { $_.Clsid -eq $FilterClsid } | Select-Object -First 1
    $created = -not (Test-Path $PotOverride)
    if ($ours) { $key = $ours.Key; $name = $ours.Name; Info "PotPlayer filter priority: updating entry $name" }
    else {
        $end = $entries | Where-Object End | Select-Object -First 1
        if ($end) {
            $name = $end.Name; $key = $end.Key
            $endName = '{0:D4}' -f ([int]$end.Name + 1)
            if ($DryRun) { Dry "would move the end marker $($end.Name) -> $endName" }
            else {
                if (Test-Path (Join-Path $PotOverride $endName)) { Remove-Item (Join-Path $PotOverride $endName) -Recurse -Force }
                Rename-Item -Path $end.Key -NewName $endName
            }
        } else {
            $n = 0; while ($entries | Where-Object { $_.Name -eq ('{0:D4}' -f $n) }) { $n++ }
            $name = '{0:D4}' -f $n; $key = Join-Path $PotOverride $name; $endName = '{0:D4}' -f ($n + 1)
            if ($DryRun) { Dry "would write the end marker $endName" }
            else { $ek = Join-Path $PotOverride $endName; New-Item -Path $ek -Force | Out-Null; New-ItemProperty $ek -Name Type -PropertyType DWord -Value $PotEndType -Force | Out-Null }
        }
        Info "PotPlayer filter priority: adding entry $name"
    }
    $vids = '{73646976-0000-0010-8000-00AA00389B71}'
    $types = @($vids, '{3231564E-0000-0010-8000-00AA00389B71}', $vids, '{30313050-0000-0010-8000-00AA00389B71}')   # NV12, P010
    if ($DryRun) { Dry "would write $key (PushMPC Filter, file $ax, Prefer, NV12/P010)" }
    else {
        if (Test-Path $key) { Remove-Item $key -Recurse -Force }
        New-Item -Path $key -Force | Out-Null
        New-ItemProperty $key -Name Type -PropertyType DWord -Value 1 -Force | Out-Null           # a filter file
        New-ItemProperty $key -Name Disabled -PropertyType DWord -Value 0 -Force | Out-Null
        New-ItemProperty $key -Name Name -PropertyType String -Value 'PushMPC Filter' -Force | Out-Null
        New-ItemProperty $key -Name CLSID -PropertyType String -Value $FilterClsid -Force | Out-Null
        New-ItemProperty $key -Name Path -PropertyType String -Value $ax -Force | Out-Null
        for ($i = 0; $i -lt $types.Count; $i++) { New-ItemProperty $key -Name ('mod{0:D4}' -f $i) -PropertyType String -Value $types[$i] -Force | Out-Null }
        New-ItemProperty $key -Name FilterType -PropertyType DWord -Value 0 -Force | Out-Null     # Prefer
        New-ItemProperty $key -Name Merit -PropertyType DWord -Value 0x00200001 -Force | Out-Null
        New-ItemProperty $key -Name MeritHi -PropertyType DWord -Value 0 -Force | Out-Null
    }
    Set-StateValue $state 'potEntry' $name
    if ($created -and -not (Get-StateValue $state 'potOverrideCreated')) { Set-StateValue $state 'potOverrideCreated' $true }
    $vs = $entries | Where-Object { $_.Clsid -eq $VapourSynthClsid -and $_.Props.PSObject.Properties['Disabled'] -and [int64]$_.Props.Disabled -eq 0 }
    foreach ($e in $vs) {
        Warn "SVP's VapourSynth Filter in PotPlayer (entry $($e.Name)) is disabled: SVP's interpolation and PushMPC's frame generation would stack."
        if ($DryRun) { Dry "would set $($e.Key) Disabled=1" } else { Set-ItemProperty $e.Key -Name Disabled -Value 1 }
        Set-StateValue $state 'potDisabledVapourSynth' $e.Name
    }
    Set-PotPlayerLav $state
    Info 'PotPlayer: choose MPC Video Renderer (Preferences > Video > Video renderer) for PushMPC to process video.'
}
# LAV Video as PotPlayer's decoder (a Prefer entry in the same list, after PushMPC's): LAV hands HDR mastering data and
# MaxCLL to the filter as side data, which PotPlayer's built-in decoder does not. Only when LAV Filters is installed
# (registered) and the list has no LAV Video entry of the user's; removed again on uninstall. -PotLavPath overrides the
# registered path (tests).
$PotLavClsid = '{EE30215D-164F-4A92-A4EB-9D4C13390F9F}'
function Find-LavVideo {
    if ($PotLavPath -eq 'none') { return '' }
    if ($PotLavPath) { return $PotLavPath }
    foreach ($root in @('HKCU:\Software\Classes', 'HKLM:\Software\Classes')) {
        $v = (Get-ItemProperty -Path "$root\CLSID\$PotLavClsid\InprocServer32" -ErrorAction SilentlyContinue).'(default)'
        if ($v -and (Test-Path -LiteralPath $v)) { return $v }
    }
    return ''
}
function Set-PotPlayerLav($state) {
    $entries = Get-PotEntries
    if ($entries | Where-Object { $_.Clsid -eq $PotLavClsid }) { if (-not (Get-StateValue $state 'potLavEntry')) { Info 'PotPlayer: your LAV Video entry is kept as it is' }; return }
    $lav = Find-LavVideo
    if (-not $lav) { Info 'PotPlayer: LAV Filters is not installed. With LAV Video as its decoder PotPlayer passes HDR mastering data to PushMPC: install LAV Filters (github.com/Nevcairiel/LAVFilters), then run setup again.'; return }
    $end = $entries | Where-Object End | Select-Object -First 1
    if (-not $end) { return }
    $name = $end.Name; $key = $end.Key; $endName = '{0:D4}' -f ([int]$end.Name + 1)
    $vids = '{73646976-0000-0010-8000-00AA00389B71}'
    $subs = @('{34363248-0000-0010-8000-00AA00389B71}', '{31435641-0000-0010-8000-00AA00389B71}', '{43564548-0000-0010-8000-00AA00389B71}', '{31435648-0000-0010-8000-00AA00389B71}',
              '{31305641-0000-0010-8000-00AA00389B71}', '{30395056-0000-0010-8000-00AA00389B71}', '{E06D8026-DB46-11CF-B4D1-00805F6CBBEA}', '{31435657-0000-0010-8000-00AA00389B71}')   # H264 AVC1 HEVC HVC1 AV01 VP90 MPEG2 WVC1
    if ($DryRun) { Dry "would add LAV Video ($lav) as PotPlayer's decoder (entry $name, Prefer)"; return }
    if (Test-Path (Join-Path $PotOverride $endName)) { Remove-Item (Join-Path $PotOverride $endName) -Recurse -Force }
    Rename-Item -Path $key -NewName $endName
    $key = Join-Path $PotOverride $name
    New-Item -Path $key -Force | Out-Null
    New-ItemProperty $key -Name Type -PropertyType DWord -Value 1 -Force | Out-Null
    New-ItemProperty $key -Name Disabled -PropertyType DWord -Value 0 -Force | Out-Null
    New-ItemProperty $key -Name Name -PropertyType String -Value 'LAV Video Decoder' -Force | Out-Null
    New-ItemProperty $key -Name CLSID -PropertyType String -Value $PotLavClsid -Force | Out-Null
    New-ItemProperty $key -Name Path -PropertyType String -Value $lav -Force | Out-Null
    $i = 0; foreach ($sub in $subs) { New-ItemProperty $key -Name ('mod{0:D4}' -f $i) -PropertyType String -Value $vids -Force | Out-Null; New-ItemProperty $key -Name ('mod{0:D4}' -f ($i + 1)) -PropertyType String -Value $sub -Force | Out-Null; $i += 2 }
    New-ItemProperty $key -Name FilterType -PropertyType DWord -Value 0 -Force | Out-Null
    New-ItemProperty $key -Name Merit -PropertyType DWord -Value 0x00800003 -Force | Out-Null
    New-ItemProperty $key -Name MeritHi -PropertyType DWord -Value 0 -Force | Out-Null
    Set-StateValue $state 'potLavEntry' $name
    Info "PotPlayer: LAV Video ($lav) set as its decoder (entry $name, Prefer): HDR mastering data reaches PushMPC"
}
function Remove-PotPlayerEntry($state) {
    $entries = Get-PotEntries
    $gone = @()
    $lavName = Get-StateValue $state 'potLavEntry'
    foreach ($e in ($entries | Where-Object { $_.Clsid -eq $FilterClsid -or ($lavName -and $_.Name -eq $lavName -and $_.Clsid -eq $PotLavClsid) })) {
        if ($DryRun) { Dry "would remove $($e.Key)" } else { Remove-Item $e.Key -Recurse -Force }
        $gone += $e.Name
        Info "PotPlayer filter priority: entry $($e.Name) ($($e.Props.Name)) removed"
    }
    $vsName = Get-StateValue $state 'potDisabledVapourSynth'
    if ($gone.Count) {
        if ((Get-StateValue $state 'potOverrideCreated') -and -not ($entries | Where-Object { -not $_.End -and $gone -notcontains $_.Name })) {
            if ($DryRun) { Dry "would remove $PotOverride (PushMPC created it)" } else { Remove-Item $PotOverride -Recurse -Force }
            Info 'PotPlayer filter priority: the list PushMPC created is removed'
        } else {
            # PotPlayer reads the entries until the end marker: the later ones (and the marker) move down to close the gap
            $next = 0
            foreach ($e in $entries) {
                if ($gone -contains $e.Name) { continue }
                $want = '{0:D4}' -f $next; $next++
                if ($e.Name -ne $want) {
                    if ($DryRun) { Dry "would renumber $($e.Name) -> $want" } else { Rename-Item -Path $e.Key -NewName $want }
                    if ($vsName -eq $e.Name) { $vsName = $want }
                }
            }
        }
    }
    if ($vsName) {
        $k = Join-Path $PotOverride $vsName
        $p = Get-ItemProperty $k -ErrorAction SilentlyContinue
        if ($p -and $p.PSObject.Properties['CLSID'] -and $p.CLSID -eq $VapourSynthClsid -and [int64]$p.Disabled -eq 1) {
            if ($DryRun) { Dry "would set $k Disabled=0" } else { Set-ItemProperty $k -Name Disabled -Value 0 }
            Info "SVP's VapourSynth Filter in PotPlayer enabled again (this installer had disabled it)"
        }
    }
}

function Register-Filter([string]$ax, [switch]$Unregister) {
    if ($NoRegister) { Info "regsvr32 skipped (-NoRegister): $ax"; return }
    $rargs = @("/s"); if ($Unregister) { $rargs += "/u" }
    $rargs += "`"$ax`""
    if ($DryRun) { Dry "would run regsvr32 $($rargs -join ' ') (per user: the filter writes HKCU\Software\Classes)"; return }
    $p = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\regsvr32.exe') -ArgumentList $rargs -Wait -PassThru
    if ($p.ExitCode -ne 0) { Fail "regsvr32 failed with exit code $($p.ExitCode) for $ax" }
}

# ------------------------------------------------------------------------------------------------ mpv route (preview)
# The vsplugin component in <InstallDir>\vsplugin, the VapourSynth runtime from vapoursynth-pin.json in
# <LOCALAPPDATA>\PushMPC\vapoursynth (the folder mpv-integration.ps1 puts on the user PATH), and the mpv.conf /
# smplayer.ini changes, which mpv-integration.ps1 makes and records in <LOCALAPPDATA>\PushMPC\mpv-integration.json.
# -MpvRoot: a fake root for all of it (mpv-integration.ps1 -Root), as its own tests use.
$MpvLocal = if ($MpvRoot) { Join-Path $MpvRoot.TrimEnd('\') 'AppData\Local' } else { $env:LOCALAPPDATA }
$VsRuntimeDir = Join-Path $MpvLocal 'PushMPC\vapoursynth'
$MpvStateFile = Join-Path $MpvLocal 'PushMPC\mpv-integration.json'
function Get-MpvIntegrationScript {
    foreach ($p in @((Join-Path $InstallDir 'vsplugin\mpv-integration.ps1'), $(if ($StageDir) { Join-Path $StageDir 'vsplugin\mpv-integration.ps1' }), (Join-Path $repo 'integrations\mpv\mpv-integration.ps1'))) {
        if ($p -and (Test-Path -LiteralPath $p)) { return $p }
    }
    return ''
}
# Runs mpv-integration.ps1 in this process (dot-sourced here, so its param variables stay in this function: the
# parameters below must not share their names: Action, Root, ScriptPath, MpvExe, NoSmplayer, RuntimeOnPath).
function Invoke-MpvIntegration([string]$mpvAction, [switch]$onPath, [switch]$noSm) {
    $mi = Get-MpvIntegrationScript
    if (-not $mi) { Warn 'mpv-integration.ps1 not found'; return @() }
    . $mi
    if ($mpvAction -eq 'Detect') { return (Find-PmPlayers (Get-PmEnv $MpvRoot) @()) }
    return @(Invoke-PmMpvIntegration -Action $mpvAction -Root $MpvRoot -ScriptPath (Join-Path $InstallDir 'vsplugin\pushmpc.vpy') -MpvExe @() `
        -NoSmplayer:$noSm -RuntimeOnPath:$onPath -WhatIf:$DryRun)
}
function Test-PinTodo($o) { return ($null -eq $o) -or ("$($o.url) $($o.sha256) $($o.version) $($o.tag)" -match 'TODO') }
# 'ours' (installed or fetched), 'user' (a VapourSynth already on the PATH), 'dry', or '' (not available)
# the folder with VSScript.dll in a runtime folder: its root (older portables) or the wheel's package folder (R7x+)
function Get-VsScriptDir([string]$root) {
    foreach ($d in @($root, (Join-Path $root 'lib\site-packages\vapoursynth'))) { if (Test-Path -LiteralPath (Join-Path $d 'VSScript.dll')) { return $d } }
    return ''
}
# RC2 F5: SVP puts its mpv64 folder (with SVP's own VSScript.dll and Python) on the user PATH. That VapourSynth belongs
# to SVP (its version, its plugins, updated by SVP): it never counts as the user's own; PushMPC fetches its own runtime.
function Test-InSvpFolder([string]$dir) {
    $d = $dir.Trim().Trim('"').TrimEnd('\')
    if (-not $d) { return $false }
    $roots = if ($MpvRoot) { @("$MpvRoot\Program Files\SVP 4", "$MpvRoot\Program Files (x86)\SVP 4") } else { @("$env:ProgramFiles\SVP 4", "${env:ProgramFiles(x86)}\SVP 4") }
    foreach ($r in $roots) { if ($r -and ($d -ieq $r -or $d.StartsWith("$r\", [StringComparison]::OrdinalIgnoreCase))) { return $true } }
    return $d -match '\\SVP ?4(\\|$)'
}
# the PATH searched for a VapourSynth of the user's own: the process PATH; with -MpvRoot (tests) the fake root's
# user-path.txt, as mpv-integration.ps1 keeps it
function Get-VsSearchPath {
    if (-not $MpvRoot) { return @($env:PATH -split ';' | Where-Object { $_ }) }
    $f = Join-Path $MpvRoot 'user-path.txt'
    if (-not (Test-Path -LiteralPath $f)) { return @() }
    return @([IO.File]::ReadAllText($f) -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}
function Get-VapourSynthRuntime {
    if (Get-VsScriptDir $VsRuntimeDir) { Info "VapourSynth runtime: $VsRuntimeDir"; return 'ours' }
    foreach ($d in (Get-VsSearchPath)) {
        if (-not (Test-Path -LiteralPath (Join-Path $d 'VSScript.dll'))) { continue }
        if ((Join-Path $d '.').TrimEnd('.\') -ieq $VsRuntimeDir.TrimEnd('\')) { continue }
        if (Test-InSvpFolder $d) { Info "VapourSynth on the PATH in $d belongs to SVP (left alone): PushMPC uses its own runtime"; continue }
        Info "VapourSynth runtime: your own installation ($d)"; return 'user'
    }
    $pinPath = @($VsPinFile, (Join-Path $InstallDir 'vsplugin\vapoursynth-pin.json'), $(if ($StageDir) { Join-Path $StageDir 'vsplugin\vapoursynth-pin.json' }), (Join-Path $here 'vapoursynth-pin.json')) |
        Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (-not $pinPath) { Warn 'VapourSynth runtime not fetched: vapoursynth-pin.json not found'; return '' }
    $pin = Get-Content -LiteralPath $pinPath -Raw | ConvertFrom-Json
    if ((Test-PinTodo $pin.python) -or (Test-PinTodo $pin.vapoursynth)) {
        Warn "VapourSynth runtime not fetched: $pinPath is not filled in yet (its versions, URLs and SHA-256 still say TODO)."
        return ''
    }
    $stage = Join-Path $InstallDir 'staging\vapoursynth'
    if ($DryRun) { Dry "would download $($pin.python.url) and $($pin.vapoursynth.url), check their pinned SHA-256 and unpack them into $VsRuntimeDir"; return 'dry' }
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force $stage, (Join-Path $InstallDir 'download') | Out-Null
    foreach ($part in @($pin.python, $pin.vapoursynth)) {
        $leaf = Split-Path $part.url -Leaf
        $file = @($(if ($PackageCache) { Join-Path $PackageCache $leaf }), (Join-Path $InstallDir "download\$leaf")) | Where-Object { $_ -and (Test-Path -LiteralPath $_) -and (Get-Sha256 $_) -ieq $part.sha256 } | Select-Object -First 1
        if (-not $file) {
            $file = Join-Path $InstallDir "download\$leaf"
            Info "Downloading $($part.url)"
            & (Join-Path $env:SystemRoot 'System32\curl.exe') -L --fail --retry 3 -s -o $file $part.url
            if ($LASTEXITCODE -ne 0) { Warn "Download of $($part.url) failed: the mpv route is not set up."; return '' }
            if ((Get-Sha256 $file) -ine $part.sha256) { Remove-Item $file -Force; Warn "$leaf does not match its pinned SHA-256: not used."; return '' }
        }
        & (Join-Path $env:SystemRoot 'System32\tar.exe') -xf $file -C $stage
        if ($LASTEXITCODE -ne 0) { Warn "$leaf could not be unpacked."; return '' }
    }
    # VapourSynth R7x+ portable: the runtime is a wheel inside the zip, unpacked to lib\site-packages (where its
    # vspipe.bat looks) and that folder added to the embeddable Python's ._pth
    if ($pin.vapoursynth.wheel) {
        $sp = Join-Path $stage ($pin.vapoursynth.wheelTo.Replace('/', '\'))
        New-Item -ItemType Directory -Force $sp | Out-Null
        foreach ($w in @(Get-ChildItem (Join-Path $stage ($pin.vapoursynth.wheel.Replace('/', '\'))) -ErrorAction SilentlyContinue)) {
            & (Join-Path $env:SystemRoot 'System32\tar.exe') -xf $w.FullName -C $sp
            if ($LASTEXITCODE -ne 0) { Warn "$($w.Name) could not be unpacked."; return '' }
        }
        foreach ($pth in @(Get-ChildItem $stage -Filter 'python*._pth')) { Add-Content -LiteralPath $pth.FullName $pin.vapoursynth.wheelTo.Replace('/', '\') }
    }
    if (-not (Get-VsScriptDir $stage)) { Warn 'The VapourSynth runtime has no VSScript.dll after unpacking: not used.'; return '' }
    Write-Manifest $stage @{ component = 'vapoursynth'; version = "$($pin.vapoursynth.tag)+python$($pin.python.version)"; 'vapoursynth-sha256' = $pin.vapoursynth.sha256; 'python-sha256' = $pin.python.sha256 }
    if (Test-Path $VsRuntimeDir) { Remove-Item $VsRuntimeDir -Recurse -Force }
    New-Item -ItemType Directory -Force (Split-Path $VsRuntimeDir) | Out-Null
    Move-Item $stage $VsRuntimeDir
    Info "VapourSynth runtime installed: $VsRuntimeDir"
    return 'ours'
}
function Install-MpvRoute($state) {
    if ($NoMpv) { Info 'mpv route skipped (-NoMpv)'; return }
    $src = Join-Path $StageDir 'vsplugin'
    if (-not (Read-Manifest $src)) { Info 'mpv route: not in this build (the vsplugin component is not staged; pushmpc_vs.dll is not built yet)'; return }
    $bad = Test-ComponentFiles $src
    if ($bad) { Warn "mpv route skipped: $src is not complete ($bad)"; return }
    $players = @(Invoke-MpvIntegration 'Detect')
    foreach ($p in $players) { Info ("{0}: {1} ({2})" -f $p.Player, $p.Exe, $p.Note) }
    if (-not @($players | Where-Object { $_.Managed }).Count) { Info 'mpv route: no mpv or SMPlayer found, nothing changed (run setup again after installing one)'; return }
    $new = Read-Manifest $src
    $diff = Get-ComponentDifference (Join-Path $InstallDir 'vsplugin') $src
    if (-not $diff) { Info "vsplugin $($new.kv['version']) is already installed (every file matches this setup's)" }
    elseif (Swap-Component 'vsplugin' $src) { Info "vsplugin $($new.kv['version']) installed" }
    else { Warn 'vsplugin is in use (close mpv / SMPlayer): the mpv route is not updated'; return }
    $rt = Get-VapourSynthRuntime
    if (-not $rt) { Warn 'mpv route: mpv.conf / smplayer.ini not changed until the VapourSynth runtime is there (mpv would fail to load the filter).'; return }
    $smRunning = -not $DryRun -and @(Get-Process -Name 'smplayer' -ErrorAction SilentlyContinue).Count
    if ($smRunning) { Warn 'SMPlayer is running: its smplayer.ini is left alone (SMPlayer rewrites it when it closes); run setup again with SMPlayer closed.' }
    $rows = Invoke-MpvIntegration 'Install' -onPath:($rt -ne 'user') -noSm:$smRunning
    foreach ($r in $rows) { Info ("{0}: {1} ({2}){3}" -f $r.Player, $r.Result, $r.Config, $(if ($r.Note) { "; $($r.Note)" } else { '' })) }
    if (-not $DryRun) { Set-StateValue $state 'mpvRoute' (Get-Date).ToString('s') }
}
function Uninstall-MpvRoute($state) {
    if ($NoMpv) { Info 'mpv and SMPlayer settings left as they are (-NoMpv)'; return }
    $vs = Join-Path $InstallDir 'vsplugin'
    $ours = Read-Manifest $VsRuntimeDir
    $ours = $ours -and $ours.kv['component'] -eq 'vapoursynth'
    if (-not (Test-Path $MpvStateFile) -and -not (Test-Path $vs) -and -not $ours) { return }   # the mpv route was never set up
    if (Get-MpvIntegrationScript) {
        foreach ($r in (Invoke-MpvIntegration 'Uninstall')) { Info ("{0}: {1} ({2})" -f $r.Player, $r.Result, $r.Config) }
    } else { Warn "mpv-integration.ps1 not found: mpv.conf / smplayer.ini changes (recorded in $MpvStateFile) are not undone" }
    foreach ($d in @($vs, $(if ($ours) { $VsRuntimeDir }))) {
        if (-not $d -or -not (Test-Path $d)) { continue }
        if (@(Get-Holders $d).Count) { Warn "$d is in use (close mpv / SMPlayer): not removed"; continue }
        if ($DryRun) { Dry "would remove $d" } else { Remove-Item $d -Recurse -Force; Info "removed $d" }
    }
    if ($state.PSObject.Properties['mpvRoute']) { $state.PSObject.Properties.Remove('mpvRoute') }
}

# ------------------------------------------------------------------------------------------------ Auto plan (Setup's engine choice)
# What Auto starts with on this PC, per player route, by the same rules the players' Auto uses: pushcore's GPU family
# (PushCoreGpu.cpp familyOf), the filter's frame generation order (PushMpcAuto.h FgOrder), the unlocks (unlocks.h) and
# the mpv route's engine (vsplugin backend.h SelectBackend). Setup logs it, writes <InstallDir>\auto-plan.json, and
# writes PushFlow's pmml.txt from it. During playback Auto still measures this GPU and steps down where a method does
# not fit; nothing here is a fixed setting.
# Tests: PUSHMPC_TEST_GPU="<vendor hex>:<device hex>:<adapter name>" stands in for the video controllers (CI has no GPU).
function Get-GpuInfo {
    $t = $env:PUSHMPC_TEST_GPU
    if ($t) {
        $p = $t.Split(':', 3)
        return [pscustomobject]@{ Name = $p[2]; Vendor = [Convert]::ToUInt32($p[0], 16); Device = [Convert]::ToUInt32($p[1], 16); Driver = 'test'; Source = 'PUSHMPC_TEST_GPU' }
    }
    $list = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object {
        $v = 0; $d = 0
        if ("$($_.PNPDeviceID)" -match 'VEN_([0-9A-Fa-f]{4})&DEV_([0-9A-Fa-f]{4})') { $v = [Convert]::ToUInt32($Matches[1], 16); $d = [Convert]::ToUInt32($Matches[2], 16) }
        [pscustomobject]@{ Name = "$($_.Name)".Trim(); Vendor = $v; Device = $d; Driver = "$($_.DriverVersion)"; Source = 'Win32_VideoController' } })
    # the adapter pushcore takes is DXGI's high-performance one: the discrete GPU of a laptop with two
    $rank = @{ 0x10DE = 0; 0x1002 = 1; 0x8086 = 2; 0x5143 = 3 }
    $hw = @($list | Where-Object { $_.Vendor -and $_.Vendor -ne 0x1414 } | Sort-Object { if ($rank.ContainsKey([int]$_.Vendor)) { $rank[[int]$_.Vendor] } else { 9 } })
    if ($hw.Count) { return $hw[0] }
    if ($list.Count) { return $list[0] }   # e.g. Microsoft Basic Display Adapter (a runner, a VM)
    return [pscustomobject]@{ Name = ''; Vendor = 0; Device = 0; Driver = ''; Source = 'none' }
}
function Get-GpuFamily($g) {
    $n = "$($g.Name)"
    switch ([int]$g.Vendor) {
        0x10DE {
            if ($n -match 'RTX 50|Blackwell|RTX PRO') { return 'RTX 50' }
            if ($n -match 'RTX 40|Ada') { return 'RTX 40' }
            if ($n -match 'RTX 30|RTX A') { return 'RTX 30' }
            if ($n -match 'RTX 20|TITAN RTX|Quadro RTX') { return 'RTX 20' }
            if ($n -match 'RTX') { return 'RTX 40' }   # an unknown RTX name: a newer card (pushcore's rule)
            return 'GTX'
        }
        0x1002 {
            foreach ($m in [regex]::Matches($n, '(?i)rx\s*(\d)')) { switch ($m.Groups[1].Value) { '9' { return 'RX 9000' } '7' { return 'RX 7000' } '6' { return 'RX 6000' } '5' { return 'RX 5000' } } }
            return 'Radeon'
        }
        0x8086 { return 'Intel' }
        0 { return 'none' }
        0x1414 { return 'none' }   # Microsoft Basic Display Adapter / WARP: no hardware GPU
        default { return 'other' }
    }
}
function Test-Gtx16($g) { $g.Vendor -eq 0x10DE -and (($g.Device -ge 0x2180 -and $g.Device -le 0x21DF) -or ($g.Device -ge 0x1F80 -and $g.Device -le 0x1FBF) -or "$($g.Name)" -match 'GTX ?16|MX[45]50') }
function Test-Nvofa($g, [string]$fam) {
    if ($g.Vendor -ne 0x10DE) { return $false }
    if ($fam -like 'RTX*') { return $true }
    if ($g.Device -ge 0x1F80 -and $g.Device -le 0x1FBF) { return $false }   # TU117 has no optical-flow unit
    return ($g.Device -ge 0x2180 -and $g.Device -le 0x21DF) -or "$($g.Name)" -match 'GTX 1660|1650 SUPER'
}
# the pmml runtime (PushFlow on other ONNX Runtime EPs; optional): -PmmlRuntime, PMML_RUNTIME, HKCU MlRuntime, <InstallDir>\pmml
function Find-PmmlRuntime {
    $c = @($PmmlRuntime, $env:PMML_RUNTIME, (Get-ItemProperty 'HKCU:\Software\PushMPC' -Name MlRuntime -ErrorAction SilentlyContinue).MlRuntime, (Join-Path $InstallDir 'pmml'))
    # pushcore needs ONNX Runtime and the EP (ort\core); the mpv route's ml backend also its graphs
    foreach ($d in $c) { if ($d -and ((Test-Path -LiteralPath (Join-Path $d 'ort\core\onnxruntime.dll')) -or (Test-Path -LiteralPath (Join-Path $d 'graphs\synth-yuv.onnx')))) { return $d } }
    return ''
}
# The pmml runtime Setup installs for this GPU (the quality path for PushFlow; default on, -Pmml No declines it):
# TensorRT-RTX on an NVIDIA RTX card, OpenVINO on Intel. Every other GPU keeps PushFlow on DirectML, pushcore's own
# zero-copy session: Radeon (the AMD GPU EP is not published as a plugin EP; MIGraphX is Linux / ROCm only),
# GeForce GTX (TensorRT-RTX needs RTX), Snapdragon (QNN's NPU path for PushFlow is not built yet).
function Get-PmmlEps([string]$fam) {
    switch -Wildcard ($fam) { 'RTX*' { return @('trt-rtx') } 'Intel' { return @('openvino') } default { return @() } }
}
function Get-PmmlFetchFiles { foreach ($d in @((Join-Path $InstallDir 'filter'), (Join-Path $StageDir 'filter'), $here)) { $s = Join-Path $d 'pmml-fetch.ps1'; $p = Join-Path $d 'ml-runtime-pin.json'; if ((Test-Path -LiteralPath $s) -and (Test-Path -LiteralPath $p)) { return @($s, $p) } }; return @() }
function Install-PmmlRuntime($state) {
    $dir = Join-Path $InstallDir 'pmml'
    $fam = Get-GpuFamily (Get-GpuInfo)
    $eps = @(Get-PmmlEps $fam)
    $ours = [bool](Get-StateValue $state 'pmmlInstalled')
    if ($Pmml -eq 'No' -or -not $eps.Count) {
        Info $(if ($Pmml -eq 'No') { 'PushFlow runtime: not installed (-Pmml No); PushFlow runs on DirectML' } else { "PushFlow runtime: none needed for $fam (PushFlow runs on DirectML, pushcore's own session)" })
        if ($ours -and (Test-Path -LiteralPath $dir)) { if ($DryRun) { Dry "would remove $dir" } else { Remove-Item -LiteralPath $dir -Recurse -Force; $state.PSObject.Properties.Remove('pmmlInstalled'); Info "removed $dir" } }
        return
    }
    $ff = @(Get-PmmlFetchFiles)
    if (-not $ff.Count) { Warn 'PushFlow runtime: pmml-fetch.ps1 / ml-runtime-pin.json not found: not installed'; return }
    $pins = Get-Content -LiteralPath $ff[1] -Raw | ConvertFrom-Json
    $want = (@("ort/core $($pins.onnxruntime.core.version)") + @($eps | ForEach-Object { "eps/$_ $($pins.eps.$_.version)" })) -join ', '
    $have = ''
    if (Test-Path -LiteralPath (Join-Path $dir 'fetched.json')) {
        try { $f = Get-Content -LiteralPath (Join-Path $dir 'fetched.json') -Raw | ConvertFrom-Json; $have = (@($f.ort.PSObject.Properties | ForEach-Object { "ort/$($_.Name) $($_.Value.version)" }) + @($f.eps.PSObject.Properties | ForEach-Object { "eps/$($_.Name) $($_.Value.version)" })) -join ', ' } catch { }
    }
    if ($have -eq $want) { Info "PushFlow runtime ($want) is already installed"; if (-not $DryRun) { Set-StateValue $state 'pmmlInstalled' $want }; return }
    Info "PushFlow runtime for $fam`: $want (ONNX Runtime and the EP from nuget.org / pypi.org, pinned by SHA-256)"
    Progress 93 'Installing PushFlow''s runtime for this GPU'
    if ($DryRun) { Dry "would run pmml-fetch.ps1 -Out $dir -Flavors core -Eps $($eps -join ',')"; return }
    $out = & $ff[0] -Pin $ff[1] -Out $dir -Platform 'win-x64' -Flavors core -Eps $eps -Cache (Join-Path $InstallDir 'download\pmml') 2>&1
    $rc = $LASTEXITCODE
    foreach ($l in @($out)) { if ("$l" -match '^(ok|fetch|cached|skip|pmml-fetch)') { Info "  $l" } }
    Remove-Item -LiteralPath (Join-Path $InstallDir 'download\pmml') -Recurse -Force -ErrorAction SilentlyContinue
    if ($rc -ne 0) { Warn "PushFlow runtime not installed (pmml-fetch exit $rc): PushFlow runs on DirectML; run Repair to try again"; return }
    Set-StateValue $state 'pmmlInstalled' $want
    Info "PushFlow runtime installed in $dir"
}
# PushFlow's EP for pushcore: the pmml Auto benchmark's measurement when this card has one (pmml-autobench.json: the
# backend with the fastest network among the candidates that ran), else pmml's order without a measurement (trt-rtx,
# then dml, ...): TensorRT-RTX on an NVIDIA RTX card when its EP is installed. DirectML is pushcore's own default (no
# pmml.txt then).
function Get-PmmlChoice([string]$rt, [string]$fam, $g) {
    if (-not $rt) { return @{ backend = ''; why = 'no pmml runtime installed: PushFlow on DirectML (pushcore''s own session)' } }
    $cache = Join-Path $rt 'pmml-autobench.json'
    if (Test-Path -LiteralPath $cache) {
        try {
            $j = Get-Content -LiteralPath $cache -Raw | ConvertFrom-Json
            $entries = @($j.entries.PSObject.Properties | Where-Object { -not $g.Name -or $_.Name -like "*$($g.Name)*" })
            $best = $null
            foreach ($e in $entries) { foreach ($r in $e.Value.results.PSObject.Properties) { $v = $r.Value; if ($v.ok -and $v.netMs -gt 0 -and (-not $best -or $v.netMs -lt $best.netMs)) { $best = $v } } }
            if ($best) {
                $b = ($best.candidate -split '\|')[0]
                return @{ backend = $(if ($b -match ':dml:') { '' } else { $b }); why = "pmml's measurement on this card: $($best.candidate) ($([math]::Round($best.netMs, 1)) ms per network run)" }
            }
        } catch { }
    }
    if ($fam -like 'RTX*' -and (Test-Path -LiteralPath (Join-Path $rt 'eps\trt-rtx\ep.json'))) { return @{ backend = 'ort:core:trt-rtx:0'; why = 'NVIDIA RTX with the TensorRT-RTX EP installed (pmml''s order without a measurement: trt-rtx before dml)' } }
    # Intel: OpenVINO's GPU plugin uses the XMX engines DirectML does not reach (Auto's default table for Arc / Xe)
    if ($fam -eq 'Intel' -and (Test-Path -LiteralPath (Join-Path $rt 'eps\openvino\ep.json'))) { return @{ backend = 'ort:core:openvino:0'; why = 'Intel GPU with the OpenVINO EP installed (Auto''s default for Arc / Xe: OpenVINO uses the XMX engines)' } }
    return @{ backend = ''; why = 'pmml''s order without a measurement keeps DirectML on this GPU' }
}
# The plan: one row per route, plus what Setup offers (FSR SDK 2.4) and pmml.txt
function Get-AutoPlan {
    $g = Get-GpuInfo
    $fam = Get-GpuFamily $g
    $rtx = $fam -like 'RTX*'
    $nvofa = Test-Nvofa $g $fam
    $fg = switch ($fam) {
        'RTX 50' { 'NVIDIA VFG (Medium, High when it fits; up to 8x), then DLSS-G, PushFlow, NVIDIA optical flow + warp, FSR 3.1 FG' }
        'RTX 40' { 'NVIDIA VFG (Medium, High when it fits; up to 8x), then DLSS-G, PushFlow, NVIDIA optical flow + warp, FSR 3.1 FG' }
        'RTX 30' { 'NVIDIA VFG (RTX 30 port, on by default), then DLSS-G (unlocked), PushFlow, NVIDIA optical flow + warp, FSR 3.1 FG' }
        'RTX 20' { 'DLSS-G, then PushFlow, NVIDIA optical flow + warp, FSR 3.1 FG' }
        'GTX' { if ($nvofa) { 'PushFlow, then NVIDIA optical flow + warp, FSR 3.1 FG (a slower card starts with the optical flow / FSR until PushFlow''s speed test has run)' } else { 'PushFlow, then FSR 3.1 FG (a slower card starts with FSR until PushFlow''s speed test has run)' } }
        'RX 9000' { 'PushFlow, then FSR 3.1 / FSR 4 frame generation, AMD FRC' }
        { $_ -in 'RX 7000', 'RX 6000', 'RX 5000', 'Radeon' } { 'PushFlow, then FSR 3.1 FG, AMD FRC' }
        'Intel' { 'PushFlow, then FSR 3.1 FG (XeSS-FG and Intel VPL interpolation after their self-tests)' }
        'other' { 'PushFlow, then FSR 3.1 FG' }
        default { '' }
    }
    $sr = switch ($fam) {
        { $_ -like 'RTX*' } { 'RTX Video Super Resolution (and RTX Video HDR for SDR video on an HDR display)' }
        'GTX' { if (Test-Gtx16 $g) { 'RTX Video Super Resolution on GTX 16 (unlock, after its self-test), else NIS' } else { 'NIS' } }
        'RX 9000' { 'FSR 4.1 (RTX VSR through ZLUDA after its self-test)' }
        'RX 7000' { 'FSR 4.1 INT8 (RTX VSR through ZLUDA after its self-test)' }
        'RX 6000' { 'FSR 4.1 INT8 through AMD FSR SDK 2.4 (offered by Setup), else FSR' }
        { $_ -in 'RX 5000', 'Radeon' } { 'FSR' }
        'Intel' { 'NIS (Intel VPL super resolution after its self-test)' }
        'other' { 'NIS' }
        default { '' }
    }
    $pushflow = Test-Path -LiteralPath (Join-Path (Join-Path $InstallDir 'runtime') $PushFlow.Rel)
    $rt = Find-PmmlRuntime
    $pm = Get-PmmlChoice $rt $fam $g
    $hwGpu = $fam -ne 'none'
    # pushcore on this GPU (backend.h PushcoreUsable: RTX, or any DirectX 12 hardware GPU with a GPU frame generation method)
    $dshow = if ($hwGpu) { "pushcore: $fg; upscaling: $sr" } else { 'pushcore needs a DirectX 12 hardware GPU: none found (the filter passes video through)' }
    $mpv = if ($hwGpu) { "pushcore through VapourSynth (the same Auto as the filter): $fg; upscaling: $sr" }
           elseif ($rt) { 'ml: PushFlow through pmml' }
           else { 'portable: the GPU engine on a Vulkan device, else the CPU engine (about 2x at 1080p)' }
    $fsrOffer = $fam -in 'RX 6000', 'RX 7000', 'RX 9000'
    return [pscustomobject][ordered]@{
        gpu = $(if ($g.Name) { $g.Name } else { 'none' }); gpuVendor = ('{0:X4}' -f [int]$g.Vendor); gpuDevice = ('{0:X4}' -f [int]$g.Device); gpuDriver = $g.Driver; gpuSource = $g.Source
        family = $fam; runtimePackage = $GpuVendor
        directshow = $dshow; mpv = $mpv
        vlc = $(if ($hwGpu) { 'the VLC module on pushcore (frame generation; VLC''s own RTX / Intel super resolution)' } else { 'the VLC module on the portable engine' })
        pushflow = $(if ($pushflow) { 'installed' } else { 'not installed' })
        pmmlRuntime = $rt; pmmlBackend = $pm.backend; pmmlWhy = $pm.why
        fsrSdk240 = $(if ($fsrOffer) { if ($FidelityFx -eq 'Package') { 'offered, declined (the package''s FSR SDK 2.3 set)' } else { 'on: AMD FSR SDK 2.4.0 (FSR 4.1 INT8 upscaling)' } } else { 'not for this GPU' })
    }
}
function Show-AutoPlan($p) {
    Info "GPU: $($p.gpu) [$($p.gpuVendor):$($p.gpuDevice)] (family $($p.family); from $($p.gpuSource))"
    Info "Engine for MPC-HC / MPC-BE / PotPlayer: $($p.directshow)"
    Info "Engine for mpv and mpv-based players: $($p.mpv)"
    Info "PushFlow's ONNX Runtime EP: $(if ($p.pmmlBackend) { $p.pmmlBackend } else { 'DirectML' }) ($($p.pmmlWhy))"
    Info "AMD FSR SDK 2.4: $($p.fsrSdk240)"
}
# pmml.txt beside PushFlow's models (<InstallDir>\runtime\runtime\experimental\pushflow: the runtime component keeps the
# package's runtime\ folder): pushcore reads runtime= and backend= from it (patches/pushfps-4.5.3-pushmpc.patch,
# pmmlChoice); without it, or with dml, PushFlow runs on its own DirectML session.
function Write-PmmlChoice($p, $state) {
    $dir = Join-Path (Join-Path $InstallDir 'runtime') $PushFlow.Rel
    $file = Join-Path $dir 'pmml.txt'
    if (-not $p.pmmlBackend) {
        if (Test-Path -LiteralPath $file) { if ($DryRun) { Dry "would remove $file" } else { Remove-Item -LiteralPath $file -Force; Info 'pmml.txt removed (DirectML is the choice now)' } }
        Info "pmml.txt: not written ($($p.pmmlWhy))"
        return
    }
    if (-not (Test-Path -LiteralPath $dir)) { Info "pmml.txt: not written (PushFlow is not installed in $dir)"; return }
    $text = "# written by PushMPC Setup from Auto's choice: $($p.pmmlWhy)`r`nruntime=$($p.pmmlRuntime)`r`nbackend=$($p.pmmlBackend)`r`n"
    if ($DryRun) { Dry "would write $file (backend=$($p.pmmlBackend))"; return }
    [IO.File]::WriteAllText($file, $text, (New-Object System.Text.UTF8Encoding($false)))
    Info "pmml.txt written: backend=$($p.pmmlBackend), runtime=$($p.pmmlRuntime)"
    # the mpv route's ml backend finds the same runtime (vsplugin ReadSetting MlRuntime)
    if (-not $NoRegister) {
        $cur = (Get-ItemProperty 'HKCU:\Software\PushMPC' -Name MlRuntime -ErrorAction SilentlyContinue).MlRuntime
        if (-not $cur) { New-Item 'HKCU:\Software\PushMPC' -Force | Out-Null; Set-ItemProperty 'HKCU:\Software\PushMPC' -Name MlRuntime -Value $p.pmmlRuntime; Set-StateValue $state 'mlRuntimeSet' $p.pmmlRuntime }
    }
}
function Save-AutoPlan($p) {
    if ($DryRun) { return }
    $p | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $InstallDir 'auto-plan.json') -Encoding UTF8
}

# ------------------------------------------------------------------------------------------------ VLC (optional component)
# The VLC 3 module (vlcplugin\, staged as vlc\ with libpushmpc_plugin.dll and pushmpc-vlc.ps1) is installed by its own
# installer when VLC is found; when this build has no VLC component, VLC is handed to capture mode.
function Find-VlcExe {
    foreach ($k in 'HKLM:\SOFTWARE\VideoLAN\VLC', 'HKCU:\SOFTWARE\VideoLAN\VLC') { try { $d = (Get-ItemProperty -Path $k -ErrorAction Stop).InstallDir; if ($d -and (Test-Path -LiteralPath (Join-Path $d 'vlc.exe'))) { return (Join-Path $d 'vlc.exe') } } catch { } }
    foreach ($d in @((Join-Path "$env:ProgramFiles" 'VideoLAN\VLC'), $(if (${env:ProgramFiles(x86)}) { Join-Path ${env:ProgramFiles(x86)} 'VideoLAN\VLC' }))) { if ($d -and (Test-Path -LiteralPath (Join-Path $d 'vlc.exe'))) { return (Join-Path $d 'vlc.exe') } }
    return ''
}
function Get-VlcInstaller { foreach ($d in @((Join-Path $InstallDir 'vlc'), (Join-Path $StageDir 'vlc'))) { $s = Join-Path $d 'pushmpc-vlc.ps1'; if ((Test-Path -LiteralPath $s) -and (Test-Path -LiteralPath (Join-Path $d 'libpushmpc_plugin.dll'))) { return $d } }; return '' }
function Install-VlcRoute($state) {
    if ($NoVlc) { Info 'VLC skipped (-NoVlc)'; return $true }
    $exe = Find-VlcExe
    if (-not $exe) { return $true }
    $d = Get-VlcInstaller
    if (-not $d) { Info "VLC found ($exe): this build has no VLC module (capture mode covers it)"; return $false }
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if ($DryRun) { Dry "would run pushmpc-vlc.ps1 -Action Install -Plugin $d\libpushmpc_plugin.dll"; return $true }
    $pd = Join-Path $InstallDir 'vlc'
    # Windows PowerShell 5.1 turns a child's stderr into a terminating error under Stop: the child's messages are only reported
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $out = @(& $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $d 'pushmpc-vlc.ps1') -Action Install -Plugin (Join-Path $d 'libpushmpc_plugin.dll') -PluginDir $pd 2>&1 | ForEach-Object { "$_" })
    $ErrorActionPreference = $eap
    $rc = $LASTEXITCODE
    foreach ($l in $out) { Info "VLC: $l" }
    # its installer stays beside the plugin (uninstall)
    if ($rc -eq 0) { Copy-Item -LiteralPath (Join-Path $d 'pushmpc-vlc.ps1') $pd -Force -ErrorAction SilentlyContinue; Set-StateValue $state 'vlcRoute' $pd; return $true }
    Warn "VLC: its installer returned $rc (capture mode covers VLC)"; return $false
}
function Uninstall-VlcRoute($state) {
    $d = Get-StateValue $state 'vlcRoute'
    if (-not $d -or $NoVlc) { return }
    $s = Join-Path $d 'pushmpc-vlc.ps1'
    if (-not (Test-Path -LiteralPath $s)) { $s = Join-Path $StageDir 'vlc\pushmpc-vlc.ps1' }
    if (-not (Test-Path -LiteralPath $s)) { Warn 'pushmpc-vlc.ps1 not found: VLC''s settings are not put back'; return }
    if ($DryRun) { Dry 'would run pushmpc-vlc.ps1 -Action Uninstall'; return }
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    foreach ($l in @(& $ps -NoProfile -ExecutionPolicy Bypass -File $s -Action Uninstall -PluginDir $d 2>&1 | ForEach-Object { "$_" })) { Info "VLC: $l" }
    $ErrorActionPreference = $eap
    if ((Test-Path -LiteralPath $d) -and -not @(Get-ChildItem -LiteralPath $d -Recurse -File | Where-Object Name -ne 'pushmpc-vlc.ps1').Count) { Remove-Item -LiteralPath $d -Recurse -Force }
    $state.PSObject.Properties.Remove('vlcRoute')
}

# ------------------------------------------------------------------------------------------------ Plex (libmpv swap)
function Get-PlexScript { foreach ($p in @((Join-Path $InstallDir 'filter\pushmpc-plex.ps1'), (Join-Path $StageDir 'filter\pushmpc-plex.ps1'), (Join-Path $here 'pushmpc-plex.ps1'))) { if ($p -and (Test-Path -LiteralPath $p)) { return $p } }; return '' }
function Install-PlexRoute($state) {
    $s = Get-PlexScript
    if (-not $s) { return }
    if ($PlexSwap -eq 'No') { Info 'Plex: libmpv swap declined (-PlexSwap No): capture mode gives Plex frame generation'; return }
    if ($NoMpv) { Info 'Plex skipped (-NoMpv)'; return }
    if (-not (Read-Manifest (Join-Path $StageDir 'vsplugin'))) { Info 'Plex: this build has no mpv route (vsplugin): Plex is not changed'; return }
    $a = @{ InstallDir = $InstallDir; Machine = $Machine; DryRun = $DryRun }
    if ($PlexRoot) { $a.PlexRoot = $PlexRoot }
    if ($NoTask) { $a.NoTask = $true }
    & $s apply @a
    if (Test-Path -LiteralPath (Join-Path $InstallDir 'plex\plex.json')) { Set-StateValue $state 'plexSwap' (Get-Date).ToString('s') }
}
function Uninstall-PlexRoute($state) {
    if (-not (Get-StateValue $state 'plexSwap') -and -not (Test-Path -LiteralPath (Join-Path $InstallDir 'plex'))) { return }
    $s = Get-PlexScript
    if (-not $s) { Warn "pushmpc-plex.ps1 not found: Plex's own libmpv is not put back (its backup is in $InstallDir\plex\backup)"; return }
    $a = @{ InstallDir = $InstallDir; Machine = $Machine; DryRun = $DryRun }
    if ($PlexRoot) { $a.PlexRoot = $PlexRoot }
    if ($NoTask) { $a.NoTask = $true }
    & $s restore @a
    if ($state.PSObject.Properties['plexSwap']) { $state.PSObject.Properties.Remove('plexSwap') }
}

# ------------------------------------------------------------------------------------------------ capture mode (fallback)
# Players with no PushMPC frame generation route: capture mode (pushmpc-capture.ps1, PushFPS's window capture) is
# switched on for them, and only for them. -Capture Auto (default) | On | Off.
function Find-CaptureOnlyPlayers([object[]]$mpvRows) {
    $out = @()
    $pf = @("$env:ProgramFiles", ${env:ProgramFiles(x86)}, (Join-Path $env:LOCALAPPDATA 'Programs')) | Where-Object { $_ }
    $probe = [ordered]@{
        'KMPlayer 64X' = @('KMPlayer 64X\KMPlayer64.exe'); 'KMPlayer' = @('KMPlayer\KMPlayer.exe')
        'GOM Player' = @('GRETECH\GomPlayer\GOM.exe', 'GRETECH\GOMPlayer\GOM.exe', 'GOM\GOMPlayer\GOM.exe')
        'Emby Theater' = @('Emby Theater\Emby.Theater.exe', 'Emby-Theater\Emby.Theater.exe', 'emby-theater\electron\Emby Theater.exe')
    }
    foreach ($k in $probe.Keys) { foreach ($rel in $probe[$k]) { foreach ($root in $pf) { if (Test-Path -LiteralPath (Join-Path $root $rel)) { $out += $k; break } }; if ($out -contains $k) { break } } }
    if (Get-AppxPackage -Name 'Microsoft.ZuneMusic' -ErrorAction SilentlyContinue) { $out += 'Media Player (Windows)' }
    # mpv-integration's rows: a player it found but cannot give frame generation (Plex: upscaling only; Stremio before
    # 5.0.28: no config setting)
    foreach ($r in @($mpvRows)) {
        if ($r.Player -match '^(Plex|Stremio)' -and ($r.Kind -in 'plex', 'none')) { $out += "$($r.Player) ($(if ($r.Kind -eq 'plex') { 'native upscaling only' } else { 'no native route' }))" }
        elseif ($r.Kind -in 'kodi', 'shim' -and -not $r.Managed) { $out += "$($r.Player) (needs mpv for the native route)" }
    }
    return @($out | Select-Object -Unique)
}
function Get-CaptureScript { foreach ($p in @((Join-Path $InstallDir 'filter\pushmpc-capture.ps1'), (Join-Path $StageDir 'filter\pushmpc-capture.ps1'), (Join-Path $here 'pushmpc-capture.ps1'))) { if ($p -and (Test-Path -LiteralPath $p)) { return $p } }; return '' }
function Invoke-Capture([string]$act, [string[]]$players) {
    $s = Get-CaptureScript
    if (-not $s) { Warn 'pushmpc-capture.ps1 not found: capture mode not changed'; return }
    $a = @{ InstallDir = $InstallDir; Machine = $Machine; DryRun = $DryRun }
    if ($players) { $a.Players = $players }
    if ($CaptureStartMenu) { $a.StartMenu = $CaptureStartMenu }
    $out = & $s $act @a
    foreach ($l in @($out)) { if ($l) { [Console]::Out.WriteLine($l) } }
}
function Install-CaptureRoute($state, [string[]]$players) {
    if ($Capture -eq 'Off' -or ($Capture -eq 'Auto' -and -not $players.Count)) {
        if (Get-StateValue $state 'capture') { Invoke-Capture 'disable'; $state.PSObject.Properties.Remove('capture') }
        Info $(if ($Capture -eq 'Off') { 'Capture mode: off (-Capture Off)' } else { 'Capture mode: off (every player found has a native PushMPC route)' })
        return
    }
    Info "Players without a native PushMPC route: $(if ($players.Count) { $players -join ', ' } else { 'none (-Capture On)' })"
    Invoke-Capture 'enable' $players
    if (-not $DryRun) { Set-StateValue $state 'capture' ($players -join ', ') }
}

# ------------------------------------------------------------------------------------------------ swap (shared by install and update)
# Replaces $InstallDir\<name> with $src; the old one goes to previous\<name> (rollback). False = held by MPC-HC.
function Swap-Component([string]$name, [string]$src) {
    $dst = Join-Path $InstallDir $name
    $prev = Join-Path $InstallDir "previous\$name"
    $holders = Get-Holders $dst
    if ($holders.Count) { Warn "$name is in use by MPC-HC (pid $(($holders | ForEach-Object Id) -join ', '))"; return $false }
    if ($DryRun) { Dry "would move $dst to $prev and copy $src to $dst"; return $true }
    if (Test-Path $dst) {
        if (Test-Path $prev) { Remove-Item $prev -Recurse -Force }
        New-Item -ItemType Directory -Force (Split-Path $prev) | Out-Null
        Move-Item $dst $prev
    }
    New-Item -ItemType Directory -Force $dst | Out-Null
    & robocopy $src $dst /E /R:2 /W:2 /NFL /NDL /NJH /NJS /NP | Out-Null
    $why = ''
    if ($LASTEXITCODE -ge 8) { $why = "copying failed (robocopy exit $LASTEXITCODE)" }
    else { $b = Test-ComponentFiles $dst -Hash; if ($b) { $why = "copy check failed: $b" } }
    if ($why) {
        # rollback, as the native updater does: the previous files come back, then the failure is reported
        Remove-Item $dst -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path $prev) { Move-Item $prev $dst; Warn "$name : $why; the previous version was restored" }
        Fail "$name : $why."
    }
    return $true
}

function Show-Selection([string]$from) {
    Step "Files of each component (from $from)"
    $total = 0
    foreach ($n in $Components) {
        $m = Read-Manifest (Join-Path $from $n)
        if (-not $m -and $n -eq 'runtime') {
            Info ("runtime {0} : not part of the release; {1} files + licence texts extracted from Veyra's {2}" -f $Runtime.Version, $RuntimeFiles.Count, $Runtime.Asset)
            continue
        }
        if (-not $m) { Warn "$n : not staged"; continue }
        $sum = ($m.files | Measure-Object Size -Sum).Sum
        $total += $sum
        Info ("{0} {1} : {2} files, {3:N1} MB" -f $n, $m.kv['version'], $m.files.Count, ($sum / 1MB))
        $groups = $m.files | Group-Object { if ($_.Rel -like 'shaders\*') { 'shaders\*.dxil' } elseif ($_.Rel -like 'licenses\*') { 'licenses\*' } else { $_.Rel } }
        foreach ($g in $groups) { Info ("      {0,-60} {1,10:N0} bytes{2}" -f $g.Name, (($g.Group | Measure-Object Size -Sum).Sum), $(if ($g.Count -gt 1) { " ($($g.Count) files)" } else { '' })) }
    }
    Info ("total {0:N1} MB" -f ($total / 1MB))
}

# ------------------------------------------------------------------------------------------------ actions
switch ($Action) {
    'stage' { Invoke-Stage }

    'package' { Invoke-Package }

    'install' {
        Write-Host ''; Write-Host '  PushMPC installer' -ForegroundColor Green; Write-Host "  $PushMpcRepo"
        Write-Host "  Runtime: Veyra's package $($Runtime.Asset) (https://github.com/$($Runtime.Repo)/releases/tag/$($Runtime.Tag)), SHA-256 $($Runtime.Sha256)"
        foreach ($n in @('filter', 'core')) { $bad = Test-ComponentFiles (Join-Path $StageDir $n); if ($bad) { Fail "$StageDir\$n is not complete ($bad). From a source checkout run: pushmpc-install.ps1 stage" } }
        Show-Selection $StageDir
        if (-not $DryRun -and -not $NoMpcHc -and (Get-MpcHcProcesses).Count) { Fail 'MPC-HC is running. Close it, then run the installer again (MPC-HC also rewrites its settings when it closes).' }
        if (-not $DryRun -and -not $NoMpcBe -and (Get-MpcBeProcesses).Count) { Fail 'MPC-BE is running. Close it, then run the installer again (MPC-BE also rewrites its settings when it closes).' }
        if (-not $DryRun -and -not $NoPotPlayer -and (Get-PotPlayerProcesses).Count) { Fail 'PotPlayer is running. Close it, then run the installer again (PotPlayer also rewrites its settings when it closes).' }
        Progress 2 'Preparing the runtime'
        Step "Installing to $InstallDir"
        if (-not $DryRun) { New-Item -ItemType Directory -Force $InstallDir, (Join-Path $InstallDir 'logs') | Out-Null }
        $state = Read-State
        # the runtime first: a download that fails leaves the installed filter and core untouched
        $rtDir = Join-Path $InstallDir 'runtime'
        if (Test-RuntimeCurrent $rtDir) { Info "runtime $($Runtime.Version) (subset $($Runtime.Subset)) is already installed" }
        else {
            $cur = Read-Manifest $rtDir
            if ($cur) { Info "runtime: installed $($cur.kv['version']) subset $($cur.kv['subset']) -> $($Runtime.Version) subset $($Runtime.Subset)" }
            $src = Prepare-Runtime
            if ($src) {
                Progress 82 'Installing the runtime'
                if (-not (Swap-Component 'runtime' $src)) { Fail 'runtime is in use: close MPC-HC first.' }
                if ($src -like (Join-Path $InstallDir 'staging\*')) { Remove-Item $src -Recurse -Force }
                Info "runtime $($Runtime.Version) installed"
            } elseif (-not $DryRun) { Fail 'The runtime could not be prepared.' }
        }
        Progress 88 'Installing the filter and the core'
        foreach ($n in @('filter', 'core')) {
            $new = Read-Manifest (Join-Path $StageDir $n)
            $diff = Get-ComponentDifference (Join-Path $InstallDir $n) (Join-Path $StageDir $n)
            if (-not $diff) { Info "$n $($new.kv['version']) is already installed (every file matches this setup's)"; continue }
            Info "$n : $diff"
            if (-not (Swap-Component $n (Join-Path $StageDir $n))) { Fail "$n is in use: close MPC-HC first." }
            Info "$n $($new.kv['version']) installed (previous kept for rollback)"
        }
        Progress 92 'Registering the filter'
        Step 'Registering the filter (per user)'
        Register-Filter (Join-Path $InstallDir 'filter\PushMpcFilter.ax')
        Step 'MPC-HC'
        Progress 95 'Adding PushMPC to MPC-HC'
        Set-MpcHcEntry $state
        if ($NoMpcHc) { Info 'MPC Video Renderer settings skipped (-NoMpcHc)' } else { Set-MpcVrHdr }
        Step 'MPC-BE'
        Set-MpcBeEntry $state
        Step 'PotPlayer'
        Set-PotPlayerEntry $state
        Step 'Plex (frame generation through a VapourSynth-enabled libmpv)'
        try { Install-PlexRoute $state } catch { Warn "Plex: not changed ($($_.Exception.Message))" }
        Step 'mpv, SMPlayer, MPC-QT, Stremio, Plex, Jellyfin, Kodi'
        Install-MpvRoute $state
        Step 'VLC'
        $vlcOk = $false; try { $vlcOk = Install-VlcRoute $state } catch { Warn "VLC: not set up ($($_.Exception.Message)); capture mode covers VLC" }
        Step 'PushFlow runtime for this GPU (pmml)'
        try { Install-PmmlRuntime $state } catch { Warn "PushFlow runtime: not installed ($($_.Exception.Message)); PushFlow runs on DirectML" }
        # Auto's engine for this PC (logged; auto-plan.json; PushFlow's pmml.txt from it)
        Step 'Auto: the engine for this PC'
        Progress 96 'Choosing the engine'
        $plan = Get-AutoPlan
        Show-AutoPlan $plan
        Write-PmmlChoice $plan $state
        Save-AutoPlan $plan
        # capture mode only for players without a native route
        Step 'Capture mode (fallback for other players)'
        $mpvRows = @(if (-not $NoMpv -and (Get-MpvIntegrationScript)) { Invoke-MpvIntegration 'Detect' })
        $cap = @(Find-CaptureOnlyPlayers $mpvRows)
        if (-not $vlcOk) { $cap += 'VLC (no VLC module in this build)' }
        try { Install-CaptureRoute $state $cap } catch { Warn "Capture mode: not set up ($($_.Exception.Message))" }
        # the updater: <InstallDir>\PushMpcUpdater.exe (the filter starts "--auto" at most once a day) and the logon
        # task \PushMPC\Apply pending update (installs a downloaded update when MPC-HC is not running)
        $upd = Join-Path $InstallDir 'filter\PushMpcUpdater.exe'
        if (Test-Path $upd) {
            Progress 97 'Setting up updates'
            $rootUpd = Join-Path $InstallDir 'PushMpcUpdater.exe'
            if ($DryRun) { Dry "would copy $upd to $rootUpd and register the logon task \PushMPC\Apply pending update" }
            else {
                Copy-Item $upd $rootUpd -Force
                if ($NoTask) { Info 'Logon task skipped (-NoTask)' }
                else {
                    $p = Start-Process -FilePath $rootUpd -ArgumentList @('--register-task', '--install-dir', "`"$InstallDir`"") -Wait -PassThru -WindowStyle Hidden
                    if ($p.ExitCode -eq 0) { Info 'Updates: PushMpcUpdater.exe and the logon task \PushMPC\Apply pending update' } else { Warn "The logon task could not be registered (exit $($p.ExitCode)); updates still install when the filter next checks." }
                }
            }
        }
        $state.installed = (Get-Date).ToString('s')
        Write-State $state
        Progress 100 'Installed'
        if ($DryRun) { Step 'Dry run: nothing was changed.'; break }
        Step 'PushMPC is installed.'
        Info 'Settings: MPC-HC > Play > Filters > PushMPC Filter (Auto is the default for everything).'
        Info "Remove it with: powershell -File `"$InstallDir\filter\pushmpc-install.ps1`" uninstall"
    }

    'update' {
        if (-not $ReleaseManifest) { Fail "update needs -ReleaseManifest <release.json> (the online feed $UpdateFeed is not used yet)" }
        $rmPath = (Resolve-Path $ReleaseManifest).Path
        # Signature hook: release.json.sig next to it (like PushFPS's SHA256SUMS.txt.sig), RSA-SHA256 over the file
        # bytes, checked with the public key in release-key.xml beside this script once PushMPC publishes one.
        $sig = "$rmPath.sig"; $key = Join-Path $here 'release-key.xml'; if (-not (Test-Path $key)) { $key = Join-Path $repo 'updater\release-key.xml' }
        if ((Test-Path $sig) -and (Test-Path $key)) {
            $rsa = New-Object System.Security.Cryptography.RSACryptoServiceProvider; $rsa.FromXmlString([IO.File]::ReadAllText($key))
            $ok = $rsa.VerifyData([IO.File]::ReadAllBytes($rmPath), 'SHA256', [Convert]::FromBase64String([IO.File]::ReadAllText($sig).Trim()))
            if (-not $ok) { Fail 'The release manifest signature does not verify: not updated.' }
            Info 'Release manifest signature verified.'
        } elseif (-not $AllowUnsigned) { Fail 'The release manifest is not signed (no .sig or no release key): pass -AllowUnsigned for a local build.' }
        else { Warn 'Unsigned release manifest accepted (-AllowUnsigned).' }
        $rel = Get-Content $rmPath -Raw | ConvertFrom-Json
        Step "Release $($rel.release)"
        $pending = @()
        # the runtime first (a failed download leaves filter and core as they are)
        $relRuntime = Get-ReleaseRuntime $rel
        $comps = @($rel.components | Where-Object { $_.name -ne 'runtime' })
        if ($relRuntime) { $comps = @(($rel.components | Where-Object { $_.name -eq 'runtime' -and (-not $_.vendor -or $_.vendor -eq $GpuVendor) } | Select-Object -First 1)) + $comps }
        foreach ($c in $comps) {
            $cur = Read-Manifest (Join-Path $InstallDir $c.name)
            $curVer = ''; if ($cur) { $curVer = $cur.kv['version'] }
            # the runtime is always a reference to Veyra's package: PushMPC hosts no NVIDIA file
            if ($c.name -eq 'runtime') {
                if ($c.format -ne 'veyra-package' -or $c.url -notmatch '^https://github\.com/[^/]+/[^/]+/releases/download/') {
                    Fail 'The release manifest''s runtime is not a Veyra package reference (format veyra-package, a GitHub release URL): refused.'
                }
                $pkg = $relRuntime
                $rtDir = Join-Path $InstallDir 'runtime'
                if (Test-RuntimeCurrent $rtDir $pkg) { Info "runtime $($c.version) is current"; continue }
                Info "runtime: $curVer -> $($c.version) (Veyra's package $($pkg.Asset))"
                $src = Prepare-Runtime $pkg
                if (-not $src) { continue }   # dry run
                if (Swap-Component 'runtime' $src) { if ($src -like (Join-Path $InstallDir 'staging\*')) { Remove-Item $src -Recurse -Force }; Info "runtime $($c.version) installed (previous kept for rollback)" }
                else { $pending += [pscustomobject]@{ name = 'runtime'; version = $c.version; staging = $src } }
                continue
            }
            if ($curVer -eq $c.version) {
                # RC2 F3: the same version built again (other files) is not current. release.json carries the SHA-256 of
                # each component's manifest.txt (manifestSha256, since 0.6.0); an older feed compares versions only.
                $have = Join-Path $InstallDir "$($c.name)\manifest.txt"
                $same = -not $c.manifestSha256 -or ((Test-Path $have) -and (Get-Sha256 $have) -ieq $c.manifestSha256)
                if ($same -and -not (Test-ComponentFiles (Join-Path $InstallDir $c.name) -Hash)) { Info "$($c.name) $curVer is current"; continue }
                Info "$($c.name) $curVer : the installed files differ from the release's, replaced"
            } else { Info "$($c.name): $curVer -> $($c.version)" }
            $staging = Join-Path $InstallDir "staging\$($c.name)-$($c.version)"
            if ($DryRun) { Dry "would fetch $($c.url), verify SHA-256 $($c.sha256), unpack to $staging and swap"; continue }
            if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
            New-Item -ItemType Directory -Force $staging | Out-Null
            $src = $c.url
            if ($src -match '^https?://') {
                $dl = Join-Path $InstallDir "download\$(Split-Path $src -Leaf)"
                New-Item -ItemType Directory -Force (Split-Path $dl) | Out-Null
                & (Join-Path $env:SystemRoot 'System32\curl.exe') -L --fail --retry 3 -C - -o $dl $src
                if ($LASTEXITCODE -ne 0) { Fail "Download of $src failed." }
                $src = $dl
            } elseif (-not [IO.Path]::IsPathRooted($src)) { $src = Join-Path (Split-Path $rmPath) $src }
            if ((Get-Sha256 $src) -ine $c.sha256) { Fail "$src does not match its SHA-256 in the release manifest." }
            Expand-Archive -Path $src -DestinationPath $staging -Force
            $bad = Test-ComponentFiles $staging -Hash
            if ($bad) { Fail "$($c.name) package check failed: $bad" }
            if ($c.manifestSha256 -and (Get-Sha256 (Join-Path $staging 'manifest.txt')) -ine $c.manifestSha256) { Fail "$($c.name) package: its manifest.txt does not match manifestSha256 in the release manifest." }
            if (Swap-Component $c.name $staging) { Remove-Item $staging -Recurse -Force; Info "$($c.name) $($c.version) installed (previous kept for rollback)" }
            else { $pending += [pscustomobject]@{ name = $c.name; version = $c.version; staging = $staging } }
        }
        if ($pending.Count -and -not $DryRun) {
            $pending | ConvertTo-Json | Set-Content (Join-Path $InstallDir 'pending.json') -Encoding UTF8
            Warn "Staged until MPC-HC closes: $(($pending | ForEach-Object name) -join ', '). Run apply-pending then (a later version does it automatically)."
        }
    }

    'apply-pending' {
        $p = Join-Path $InstallDir 'pending.json'
        if (-not (Test-Path $p)) { Info 'Nothing pending.'; break }
        $left = @()
        # (Windows PowerShell 5.1's ConvertFrom-Json returns a JSON array as one object: unrolled)
        foreach ($c in @(Get-Content $p -Raw | ConvertFrom-Json | ForEach-Object { $_ })) {
            if (Swap-Component $c.name $c.staging) { if (-not $DryRun) { Remove-Item $c.staging -Recurse -Force }; Info "$($c.name) $($c.version) installed" } else { $left += $c }
        }
        if ($DryRun) { break }
        if ($left.Count) { $left | ConvertTo-Json | Set-Content $p -Encoding UTF8; Warn 'Still in use; try again after closing MPC-HC.' } else { Remove-Item $p -Force }
    }

    'rollback' {
        $prev = Join-Path $InstallDir "previous\$Component"
        if (-not (Test-Path $prev)) { Fail "No previous $Component to go back to." }
        $tmp = Join-Path $InstallDir "staging\rollback-$Component"
        if (-not $DryRun) { if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }; Copy-Item $prev $tmp -Recurse }
        if (Swap-Component $Component $tmp) { if (-not $DryRun) { Remove-Item $tmp -Recurse -Force }; Info "$Component rolled back to $((Read-Manifest (Join-Path $InstallDir $Component)).kv['version'])" }
        else { Fail "$Component is in use: close MPC-HC first." }
    }

    'uninstall' {
        if (-not $DryRun -and -not $NoMpcHc -and (Get-MpcHcProcesses).Count) { Fail 'MPC-HC is running. Close it first.' }
        if (-not $DryRun -and -not $NoMpcBe -and (Get-MpcBeProcesses).Count) { Fail 'MPC-BE is running. Close it first.' }
        if (-not $DryRun -and -not $NoPotPlayer -and (Get-PotPlayerProcesses).Count) { Fail 'PotPlayer is running. Close it first.' }
        $state = Read-State
        $ax = Join-Path $InstallDir 'filter\PushMpcFilter.ax'
        Progress 5 'Restoring the MPC-HC settings'
        # MPC-HC's / MPC-BE's NVIDIA driver profiles (G-SYNC), if the filter changed them: restored first, by the filter
        # itself (backup in HKCU\Software\PushMPC\NvBackup, which the restore removes)
        if ((Test-Path $ax) -and (Test-Path 'HKCU:\Software\PushMPC\NvBackup') -and -not $NoMpcHc) {
            if ($DryRun) { Dry "would run rundll32 $ax,RestoreNvProfile" }
            else {
                $p = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\rundll32.exe') -ArgumentList @("`"$ax`",RestoreNvProfile") -Wait -PassThru
                if ($p.ExitCode -eq 0) { Info 'NVIDIA profile of MPC-HC restored (G-SYNC setting as before PushMPC)' } else { Warn "The NVIDIA profile restore returned $($p.ExitCode); its backup stays in HKCU\Software\PushMPC\NvBackup" }
            }
        }
        if (Test-Path $ax) { Register-Filter $ax -Unregister }
        if ($NoMpcHc) { Info 'MPC-HC and MPC Video Renderer settings left as they are (-NoMpcHc)' }
        else { Remove-MpcHcEntry $state; Restore-MpcVrHdr }
        if ($NoMpcBe) { Info 'MPC-BE settings left as they are (-NoMpcBe)' } else { Remove-MpcBeEntry $state }
        if ($NoPotPlayer) { Info 'PotPlayer settings left as they are (-NoPotPlayer)' } else { Remove-PotPlayerEntry $state }
        try { Uninstall-PlexRoute $state } catch { Warn "Plex: its own libmpv was not put back ($($_.Exception.Message)); the backup is in $InstallDir\plex\backup" }
        Uninstall-MpvRoute $state
        Uninstall-VlcRoute $state
        if ((Get-StateValue $state 'capture') -or (Test-Path (Join-Path $InstallDir 'capture'))) { Invoke-Capture 'disable' }
        # the pmml runtime path Setup gave the mpv route (only when Setup set it)
        $ml = Get-StateValue $state 'mlRuntimeSet'
        if ($ml -and -not $DryRun -and (Get-ItemProperty 'HKCU:\Software\PushMPC' -Name MlRuntime -ErrorAction SilentlyContinue).MlRuntime -eq $ml) { Remove-ItemProperty 'HKCU:\Software\PushMPC' -Name MlRuntime -ErrorAction SilentlyContinue }
        # the updater and its logon task
        Progress 40 'Removing the updater'
        $rootUpd = Join-Path $InstallDir 'PushMpcUpdater.exe'
        if (-not $NoTask) {
            $tool = @($rootUpd, (Join-Path $InstallDir 'filter\PushMpcUpdater.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
            if ($DryRun) { Dry 'would remove the logon task \PushMPC\Apply pending update' }
            elseif ($tool) { Start-Process -FilePath $tool -ArgumentList @('--unregister-task', '--install-dir', "`"$InstallDir`"") -Wait -WindowStyle Hidden | Out-Null; Info 'Logon task removed' }
        }
        foreach ($u in @($rootUpd, (Join-Path $InstallDir 'PushMpcUpdater.old.exe'))) { if ((Test-Path $u) -and -not $DryRun) { Remove-Item $u -Force -ErrorAction SilentlyContinue } }
        if (-not $NoRegister -and -not $DryRun -and (Test-Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\PushMPC')) {
            Remove-Item 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\PushMPC' -Recurse -Force; Info 'Windows Apps entry removed'
        }
        if (-not $KeepSettings) {
            if ($DryRun) { Dry 'would remove HKCU\Software\PushMPC' }
            elseif (Test-Path 'HKCU:\Software\PushMPC') {
                # an NVIDIA profile backup that could not be restored stays (after a restore it is gone already)
                if (Test-Path 'HKCU:\Software\PushMPC\NvBackup') {
                    Get-ChildItem 'HKCU:\Software\PushMPC' | Where-Object { $_.PSChildName -ne 'NvBackup' } | Remove-Item -Recurse -Force
                    Get-Item 'HKCU:\Software\PushMPC' | Select-Object -ExpandProperty Property | ForEach-Object { Remove-ItemProperty 'HKCU:\Software\PushMPC' -Name $_ }
                    Warn 'Settings removed; HKCU\Software\PushMPC\NvBackup kept (the NVIDIA profile was not restored)'
                } else { Remove-Item 'HKCU:\Software\PushMPC' -Recurse -Force; Info 'Settings removed' }
            }
        }
        Progress 60 'Removing the files'
        # pmml: the PushFlow runtime Setup installed (a folder someone else put there stays)
        foreach ($d in @('filter', 'core', 'runtime', 'previous', 'staging', 'download') + @(if (Get-StateValue $state 'pmmlInstalled') { 'pmml' })) {
            $p = Join-Path $InstallDir $d
            if (Test-Path $p) { if ($DryRun) { Dry "would remove $p" } else { Remove-Item $p -Recurse -Force } }
        }
        foreach ($f in @('state.json', 'pending.json', 'auto-plan.json')) { $p = Join-Path $InstallDir $f; if ((Test-Path $p) -and -not $DryRun) { Remove-Item $p -Force } }
        Progress 100 'Removed'
        Info "Removed (logs are kept in $InstallDir\logs)."
    }

    'autoplan' {
        # Auto's engine choice for this PC (nothing is changed); with -Machine "@@plan|key=value" lines
        $p = Get-AutoPlan
        if ($Machine) { foreach ($pr in $p.PSObject.Properties) { M "plan|$($pr.Name)=$($pr.Value)" } } else { Show-AutoPlan $p }
        # -Apply: what install does with it (auto-plan.json, pmml.txt), e.g. after a GPU change
        if ($Apply) { $state = Read-State; Write-PmmlChoice $p $state; Save-AutoPlan $p; Write-State $state }
    }

    'plan' {
        # What install (or an update to -ReleaseManifest) needs; nothing is changed. Lines "@@plan|key=value" with -Machine.
        $pkg = $Runtime
        if ($ReleaseManifest) { $r = Get-ReleaseRuntime (Get-Content (Resolve-Path $ReleaseManifest).Path -Raw | ConvertFrom-Json); if ($r) { $pkg = $r } }
        $out = [ordered]@{ vendor = $GpuVendor; 'runtime-version' = $pkg.Version; 'runtime-asset' = $pkg.Asset; 'runtime-url' = $pkg.Url; 'runtime-sha256' = $pkg.Sha256
                           'runtime-file' = (Join-Path $InstallDir "download\$($pkg.Asset)") }
        if (Test-RuntimeCurrent (Join-Path $InstallDir 'runtime') $pkg) { $out.runtime = 'current' }
        elseif (Test-RuntimeCurrent (Join-Path $StageDir 'runtime') $pkg) { $out.runtime = 'staged' }
        else {
            $ex = Find-ExistingRuntimeDir
            if ($ex) { $out.runtime = 'existing'; $out['runtime-existing'] = $ex }
            else {
                $found = ''
                foreach ($zip in @($RuntimePackage, $(if ($PackageCache) { Join-Path $PackageCache $pkg.Asset }), $out['runtime-file'])) {
                    if ($zip -and (Test-Path $zip) -and (Get-Sha256 $zip) -ieq $pkg.Sha256) { $found = $zip; break }
                }
                if ($found) { $out.runtime = 'package'; $out['runtime-package'] = $found } else { $out.runtime = 'download' }
            }
            $out.pushflow = Find-PushFlowSource
            $out['pushflow-url'] = $PushFlow.Url
            $out['pushflow-file'] = (Join-Path $InstallDir "download\$($PushFlow.Asset)")
        }
        $ap = Get-AutoPlan
        $out['gpu'] = $ap.gpu; $out['gpu-family'] = $ap.family; $out['engine'] = $ap.directshow; $out['fsr-sdk240'] = $ap.fsrSdk240
        foreach ($k in $out.Keys) { if ($Machine) { M "plan|$k=$($out[$k])" } else { Info ("{0,-18} {1}" -f $k, $out[$k]) } }
    }

    'prepare-runtime' {
        # For PushMpcUpdater.exe: the release's runtime in staging\runtime-<version> (the updater swaps it in later).
        if (-not $ReleaseManifest) { Fail 'prepare-runtime needs -ReleaseManifest <release.json>' }
        $pkg = Get-ReleaseRuntime (Get-Content (Resolve-Path $ReleaseManifest).Path -Raw | ConvertFrom-Json)
        if (-not $pkg) { Fail 'The release manifest has no runtime for this GPU.' }
        if (Test-RuntimeCurrent (Join-Path $InstallDir 'runtime') $pkg) { Info "runtime $($pkg.Version) is current"; if ($Machine) { M 'result|current=1' }; break }
        if (-not $DryRun) { New-Item -ItemType Directory -Force (Join-Path $InstallDir 'staging') | Out-Null }
        $src = Prepare-Runtime $pkg
        if ($src) { Info "runtime staged in $src"; if ($Machine) { M "result|staging=$src" } }
        elseif (-not $DryRun) { Fail 'The runtime could not be prepared.' }
    }

    'status' {
        Step "PushMPC in $InstallDir"
        foreach ($n in $Components) {
            $d = Join-Path $InstallDir $n
            $line = Component-Summary $d
            if (Read-Manifest $d) { $bad = Test-ComponentFiles $d -Hash:$Verify; if ($bad) { $line += " - PROBLEM: $bad" } elseif ($Verify) { $line += ' - all hashes match' } }
            Info ("{0,-8} {1}" -f $n, $line)
            $pv = Join-Path $InstallDir "previous\$n"
            if (Test-Path $pv) { Info ("         previous: {0}" -f (Component-Summary $pv)) }
        }
        $core = Read-Manifest (Join-Path $InstallDir 'core'); $run = Read-Manifest (Join-Path $InstallDir 'runtime')
        if ($core -and $run -and ($core.kv['requires-runtime'] -split '\|') -notcontains $run.kv['version']) { Warn "The core needs runtime $($core.kv['requires-runtime']) but $($run.kv['version']) is installed." }
        $reg = "HKCU:\Software\Classes\CLSID\$FilterClsid\InprocServer32"
        if (Test-Path $reg) { Info "registered: $((Get-ItemProperty $reg).'(default)')" } else { Info 'registered: no' }
        $e = Get-MpcEntries | Where-Object { $_.Props.CLSID -eq $FilterClsid }
        if ($e) { Info "MPC-HC entry: $($e.Name) (enabled $($e.Props.Enabled), load type $($e.Props.LoadType): 0 = Prefer)" } else { Info 'MPC-HC entry: none' }
        if (Test-Path (Join-Path $InstallDir 'pending.json')) { Warn 'An update is waiting for MPC-HC to close (apply-pending).' }
        $st = Read-State
        if ($st.disabledVapourSynth) { Info "SVP's VapourSynth entry $($st.disabledVapourSynth) was disabled by this installer (enabled again on uninstall)." }
        $be = Find-MpcBeExe
        if (-not $be) { Info 'MPC-BE: not installed' }
        else {
            $e = Get-MpcBeEntries | Where-Object { $_.Props.CLSID -eq $FilterClsid }
            if ($e) { Info "MPC-BE entry: $($e.Name) (enabled $($e.Props.Enabled), load type $($e.Props.LoadType): 0 = Prefer)" } else { Info 'MPC-BE entry: none' }
            Info "MPC-BE decoder HwDecoder: $((Get-ItemProperty -Path $MpcBeDecoderKey -Name HwDecoder -ErrorAction SilentlyContinue).HwDecoder) (2 = D3D11 copy-back)"
        }
        $pot = Find-PotPlayerExe
        if (-not $pot) { Info 'PotPlayer: not installed' }
        else { $e = Get-PotEntries | Where-Object { $_.Clsid -eq $FilterClsid }; if ($e) { Info "PotPlayer entry: $($e.Name) (disabled $($e.Props.Disabled), filter type $($e.Props.FilterType): 0 = Prefer)" } else { Info 'PotPlayer entry: none' } }
        $vs = Join-Path $InstallDir 'vsplugin'
        if (Read-Manifest $vs) {
            $line = Component-Summary $vs; $bad = Test-ComponentFiles $vs -Hash:$Verify; if ($bad) { $line += " - PROBLEM: $bad" }
            Info ("{0,-8} {1}" -f 'vsplugin', $line)
            Info "VapourSynth runtime: $(if (Get-VsScriptDir $VsRuntimeDir) { $VsRuntimeDir } else { 'not installed' })"
            if (Get-MpvIntegrationScript) { foreach ($r in (Invoke-MpvIntegration 'Status')) { Info ("{0}: installed {1} ({2}){3}" -f $r.Player, $r.Installed, $(if ($r.Config) { $r.Config } else { $r.Exe }), $(if ($r.Note) { "; $($r.Note)" } else { '' })) } }
        } else { Info 'vsplugin (mpv route): not installed' }
    }

    'mpv-apply' {
        # test path: only the mpv steps of install (state.json in -InstallDir; use -MpvRoot for a fake root)
        if (-not $DryRun) { New-Item -ItemType Directory -Force $InstallDir | Out-Null }
        $state = Read-State
        Install-MpvRoute $state
        Write-State $state
    }
    'mpv-remove' {
        $state = Read-State
        Uninstall-MpvRoute $state
        Write-State $state
    }

    'mpcbe-apply' {
        # test path: only the MPC-BE steps of install (state.json in -InstallDir)
        if (-not $DryRun) { New-Item -ItemType Directory -Force $InstallDir | Out-Null }
        $state = Read-State
        Set-MpcBeEntry $state
        Write-State $state
    }
    'potplayer-apply' {
        # test path: only the PotPlayer steps of install (state.json in -InstallDir)
        if (-not $DryRun) { New-Item -ItemType Directory -Force $InstallDir | Out-Null }
        $state = Read-State
        Set-PotPlayerEntry $state
        Write-State $state
    }
    'potplayer-remove' {
        $state = Read-State
        Remove-PotPlayerEntry $state
        foreach ($n in @('potEntry', 'potOverrideCreated', 'potDisabledVapourSynth', 'potLavEntry')) { if ($state.PSObject.Properties[$n]) { $state.PSObject.Properties.Remove($n) } }
        Write-State $state
    }
    'mpcbe-remove' {
        $state = Read-State
        Remove-MpcBeEntry $state
        foreach ($n in @('mpcBeEntry', 'mpcBeDisabledVapourSynth')) { if ($state.PSObject.Properties[$n]) { $state.PSObject.Properties.Remove($n) } }
        Write-State $state
    }
}
exit 0
