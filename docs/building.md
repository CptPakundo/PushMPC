# Building PushMPC

This page is for developers: how PushMPC is put together, how to build it, how Setup and the updater work, and how
releases are made. For using PushMPC, see the [README](../README.md) and the [user guide](user-guide.md).

- [Architecture](#architecture)
- [Building on Windows](#building-on-windows)
- [The signing key](#the-signing-key)
- [GitHub Actions](#github-actions)
- [How Setup works](#how-setup-works)
- [Setup and script options](#setup-and-script-options)
- [What Setup changes in each player](#what-setup-changes-in-each-player)
- [The updater](#the-updater)
- [Linux and macOS](#linux-and-macos)

## Architecture

| Folder | What |
|---|---|
| `pushmpc/` | **pushcore**: PushFPS's engine behind a C interface, built inside PushFPS's CMake tree (`patches/` holds the small patch set on the pinned PushFPS tag, `scripts/pushfps-pin.json`) |
| `filter/` | the DirectShow filter for MPC-HC, MPC-BE and PotPlayer (settings page, on-screen status, Auto) |
| `vsplugin/` | `pushmpc.Enhance`, the VapourSynth filter for the mpv family, with its backends: pushcore (Windows), NVIDIA Video Effects (Linux), the portable CPU and Vulkan engines, the ML backend (pmml), Apple VideoToolbox |
| `mpvnative/` | the native mpv route: `vf_pushmpc` in PushMPC's own mpv build (Windows) and a runtime hook for distro mpv and Plex's libmpv (Linux, Windows proxy, macOS) |
| `vlcplugin/` | the VLC 3 module (Windows, Linux, macOS) |
| `kodiaddon/` | `inputstream.pushmpc`, the Kodi 21/22 add-on |
| `pmml/` | the ML runtime layer for PushFlow (ONNX Runtime with the best execution provider per GPU, or ncnn on Vulkan) |
| `installer/`, `updater/`, `scripts/` | Setup (`PushMPC-Setup.exe`), `PushMpcUpdater.exe`, the PowerShell installer and helpers |
| `integrations/` | Linux `setup.sh` / `install.sh`, mpv configuration writers, the IINA bridge |
| `packaging/` | the Linux tarball, the Homebrew formula, AUR, Scoop and winget manifests |
| `tests/` | cross-component tests |

Auto's rules (multiplier from the GPU's measured 95th-percentile cost and the display cadence or VRR, then quality,
upscaling and clean-up with the time left) are the same on every route;
`filter\build\auto_test.exe` prints Auto's decisions for sample sources and displays without a GPU.

## Building on Windows

Clone into a folder **without spaces** in its path (FFmpeg/vcpkg and MSYS tools break on spaces). Layout:

    <repo>\PushFPS            PushFPS 4.5.4 source (tag v4.5.4) with patches\pushfps-4.5.4-pushmpc.patch applied
    <repo>\MpcVideoRenderer   MPC Video Renderer source (DirectShow base classes and interfaces)
    <repo>\pushmpc            pushcore (C ABI, built inside PushFPS's CMake tree)
    <repo>\filter             the DirectShow filter
    <repo>\updater            PushMpcUpdater.exe (and the helpers Setup shares), release-key.xml (the public release key)
    <repo>\installer          PushMPC-Setup.exe
    <repo>\deps               PushFPS's dependencies (Qt, vcpkg FFmpeg, SDKs), see PushFPS's build workflow
    <repo>\scripts            build and install scripts

Tools: Visual Studio 2022 or newer with C++ (found with vswhere; `PUSHMPC_VS` overrides), CMake and Ninja (bundled with VS).

    git -C PushFPS apply ..\patches\pushfps-4.5.4-pushmpc.patch
    scripts\build-pushfps.cmd                 (once: configures and builds PushFPS, which also builds the shaders)
    scripts\build-pushmpc.cmd pushcore_dll
    filter\build.cmd                          (PushMpcFilter.ax, PushMpcExit.dll, auto_test.exe)
    updater\build.cmd                         (PushMpcUpdater.exe; developer build: unsigned feeds allowed)
    powershell -File scripts\pushmpc-install.ps1 stage
    powershell -File scripts\pushmpc-install.ps1 install -DryRun            (shows the download of Veyra's package)
    powershell -File scripts\pushmpc-install.ps1 package                    (release\: PushMPC-Setup.exe, zips, release.json + .sig)

`package` builds Setup itself (`installer\build.cmd <payload>`; `-NoSetup` skips it) and signs `release.json` with
`-SigningKey <xml>`, the environment variable `PUSHMPC_SIGNING_KEY` (the key's XML text) or
`deps\signing\release-private.xml`. A release build of the updater: `set PUSHMPC_UPDATER_CMAKE_ARGS=-DPUSHMPC_UPDATER_DEV=OFF`
before `updater\build.cmd`. Code signing of the exes: set `PUSHMPC_CODESIGN` (see [release model](release-model.md#signing)).

Test switches that keep a test install away from your real one: `pushmpc-install.ps1 install|uninstall -InstallDir <scratch>
-NoMpcHc -NoRegister -NoTask -KeepSettings`; `PushMPC-Setup.exe --sandbox --install-dir <scratch>` (the same, plus its own
Apps entry "PushMPC-Sandbox" and settings key `Software\PushMPC-Sandbox`); `PushMpcUpdater.exe --install-dir <scratch>
--reg-key Software\PushMPC-Test\Update --no-register --feed <folder or http://127.0.0.1:...>`.

## The signing key

    powershell -File scripts\new-signing-key.ps1

writes the private key to `deps\signing\release-private.xml` (git-ignored; keep a backup somewhere safe: an updater in
the field trusts only this key's public half) and the public key to `updater\release-key.xml`, which is committed and
built into the updater. For GitHub Actions, copy the private XML's text into the repository secret
`PUSHMPC_SIGNING_KEY`. Replacing the key later means every installed updater refuses new releases until it is updated
with a release signed by the old key that ships the new public key.

## GitHub Actions

`.github/workflows/release.yml` builds a release on `windows-latest` from a tag (`v1.0.1`, or `v1.0.1-beta.1` for the
Beta channel) and publishes it; `pushfps-watch.yml` follows PushFPS's releases daily (see
[release model](release-model.md#build-pipeline-github-actions)). Repository settings they use:

| Kind | Name | What |
|---|---|---|
| secret | `PUSHMPC_SIGNING_KEY` | the private release key (XML), required to publish |
| secret | `NVOF_SDK_HEADERS` | required for a release, as in PushFPS's CI: base64 of a zip with NVIDIA's Optical Flow SDK 5 headers. Without it the core has no NVIDIA optical flow and NVIDIA VFG cannot start: a release run fails, a dry run warns in its summary (core manifest `nvof=0`) |
| secret | `CODESIGN_PFX_BASE64`, `CODESIGN_PFX_PASSWORD` | optional Authenticode certificate |
| variable | `PUSHMPC_GPU_RUNNER` | `true` runs the GPU smoke test on a self-hosted runner labelled `gpu` |
| variable | `PUSHFPS_WATCH_BETA` | `true` publishes a passing PushFPS bump as a beta release |

Workflow permissions (Settings > Actions > General): "Read and write permissions" and "Allow GitHub Actions to create
and approve pull requests", for the watch's branch and pull request.

### Workflows

| Workflow | What it checks or builds |
|---|---|
| `release.yml` | the release: Windows build, Setup, component zips, signed manifest, Linux tarballs; builds the VLC, mpv-native and Kodi pieces through their workflows (build jobs and one smoke test each; their full test suites run on their own pushes) |
| `dev-build.yml` | a quick Windows test build (below) |
| `release-ux.yml` | Setup on Windows and `setup.sh` on Linux with real players, including a clean uninstall |
| `integrations.yml`, `players-layout.yml` | the player configuration writers, MPC-BE and PotPlayer settings layouts |
| `vsplugin*.yml` | the VapourSynth route: the pushcore, portable CPU, Vulkan and Linux NVIDIA backends |
| `mpv-native.yml` | PushMPC's own mpv build, the distro-mpv hook for every FFmpeg 6.0-9.1 release, the Plex proxies |
| `vlc-plugin.yml`, `kodi-addon.yml` | the VLC module and the Kodi add-on, built and played headless |
| `pmml.yml`, `nvidia-vfx.yml`, `vendor-paths.yml` | the PushFlow runtime layer, NVIDIA Video Effects on Linux, the vendor routes |
| `apple.yml`, `apple-metal.yml` | the Homebrew formula, IINA bridge and Apple backends on macOS |
| `pins.yml`, `runtime-pins.yml` | the pinned downloads (VapourSynth, Veyra's packages) against their official sources |
| `pushfps-watch.yml` | follows PushFPS's releases and tries the patch on each new one |

Hosted runners have no GPU, so these run on synthetic clips, stand-in GPUs or software Vulkan (lavapipe); the real-GPU
checks run on a self-hosted runner (below) or by hand.

### Test builds

For a Windows build to try out, use `dev-build.yml`, not a release dry run. It builds pushcore, the filter, the
updater, the VapourSynth plugin and `PushMPC-Setup.exe` with release.yml's own jobs, without the VLC, mpv, Plex, Kodi
and Linux pieces, in about 10 minutes on a warm cache. It runs on every push to a `track/**` branch that touches the
Windows build, or by hand: `gh workflow run dev-build.yml --ref <branch>`. The result is the `pushmpc-release` artifact
(Setup, the component zips, `release.json`), the same as a dry run's: `gh run download <run id> -n pushmpc-release`.

The caches are keyed by what goes into them (pins, build scripts, sources) and saved right after each build step.
Caches made on `main` are the ones every branch can restore; `dev-build.yml` runs weekly on `main` to keep them.

### GPU runner

Hosted runners have no GPU, so the GPU smoke test needs your own Windows PC with an NVIDIA RTX card:

1. Repository > Settings > Actions > Runners > New self-hosted runner > Windows x64; follow the shown commands in a
   folder without spaces (they download the runner and register it with a one-time token).
2. When `config.cmd` asks for labels, add `gpu` (the runner also has `self-hosted` and `windows`).
3. Run it as a service or with `run.cmd` while you want tests to run. It needs Git, PowerShell, an NVIDIA driver and
   internet access (Veyra's package, about 750 MB, is downloaded once into the runner's tool cache; the test clip too).
4. Set the repository variable `PUSHMPC_GPU_RUNNER` = `true`.

The job installs the build into a scratch folder with `-NoMpcHc -NoRegister -NoTask` (nothing of the PC's MPC-HC or
PushMPC changes), downloads a public test clip (Big Buck Bunny, 10 s, 1080p) and runs `pushcore_gpu_harness` with
VFG at 2x and 4x; it fails on any count, order, timestamp or size error. A self-hosted runner runs code from the
repository's workflows: keep it for this repository only and do not enable it for forks' pull requests.

A release never contains the runtime; see [release model](release-model.md). On a developer machine that already has
Veyra's zip: `stage -WithRuntime -PackageCache <folder>` (or `install -PackageCache <folder>`).

NVIDIA's Optical Flow SDK interface headers (`NvOFInterface\nvOpticalFlowCommon.h`, `nvOpticalFlowD3D12.h`, version
5.0.7 as PushFPS expects; from NVIDIA's developer site) in `deps\nvof-sdk` or `PUSHMPC_NVOF_SDK` build PushFPS's
hardware optical flow in. NVIDIA VFG needs it: without it VFG fails with "failed backend 2" and Auto falls back to
DLSS-G. `pushmpc-install.ps1 stage` records it in the core manifest (`nvof=1` or `nvof=0`; `pushcore.dll`'s version
resource has `PushCoreFeatures` = `nvof` or `no-nvof`), and `stage -RequireNvof` refuses a core without it. The
headers are never published.

Environment overrides: `PUSHMPC_DEPS`, `PUSHMPC_PUSHFPS_SRC`, `PUSHMPC_BUILD_DIR`. `filter\build\auto_test.exe` prints
the Auto decisions for a few sources and displays (no GPU needed).

Before publishing anything: `scripts\check-public.ps1` (needs a local `scripts\check-public.patterns.local`, never
published) scans the files listed in `scripts\public-files.txt` for personal data.

## How Setup works

Setup (`PushMPC-Setup.exe`) runs `scripts/pushmpc-install.ps1 install` from its payload; on Linux `setup.sh` runs
`install.sh` and the other installers. What they do besides the player settings:

- **Auto plan:** the GPU family (pushcore's rules), the frame generation order and upscaler per route, logged and saved
  as `auto-plan.json` (`pushmpc-install.ps1 autoplan [-Apply]`; Linux `setup.sh plan`, `auto-plan.txt`).
  `PUSHMPC_TEST_GPU="<vendor>:<device>:<name>"` stands in for the GPU in tests.
- **PushFlow runtime (pmml):** `scripts/pmml-fetch.ps1` (Windows) and `pmml/scripts/fetch_runtime.py` (Linux) fetch ONNX
  Runtime and the execution provider for the GPU by the pins in `scripts/ml-runtime-pin.json` (nuget.org, pypi.org,
  SHA-256 checked) into `%LOCALAPPDATA%\PushMPC\pmml` / `~/.local/share/pushmpc/pmml`: TensorRT-RTX on RTX, OpenVINO on
  Intel, MIGraphX on Linux AMD with ROCm. Other GPUs keep DirectML. `pmml.txt` in the runtime's
  `runtime\experimental\pushflow` folder tells pushcore which provider to use (a measurement in `pmml-autobench.json`
  wins). Switches: `-Pmml No`, `--no-pmml`.
- **AMD FSR SDK 2.4:** `scripts/fidelityfx-fetch.ps1` on Radeon RX 6000-9000 (`-FidelityFx Package` declines it).
- **Repair and update keep the first install's choices:** every install records `choices` (pmml, fsr, plex) in its
  state; a later run whose options are still `Auto` (Setup's Repair and Update, quiet or not) follows them, so it never
  adds a download the first install did not have. `-Pmml Yes` / `-PlexSwap Yes` ask for one explicitly.
- **Plex:** two routes, never both (state `plexNative` / `plexSwap`). First the proxy: the optional `plexnative`
  component (mpv-native.yml's `plexnative` artifact: proxy `avfilter-8.dll`, `pushmpc-avfilter-8.*.dll`,
  `pushmpc_engine.dll`, staged with `-PlexNative`) installed by `mpvnative/install/pushmpc-plex-native.ps1 install -Json
  -AllowBoth` (elevated once), which renames Plex's `avfilter-8.dll`, writes its own `mpv.conf` block and self-tests
  through Plex's own libmpv; mpv-integration then leaves that Plex alone. If the self-test fails, the proxy is
  uninstalled and `scripts/pushmpc-plex.ps1` replaces Plex's `libmpv-2.dll` with a VapourSynth-enabled libmpv
  (shinchiro's mpv build, SHA-256 pinned) after its own self-test, on tested Plex versions only (`$Tested`), with a
  backup; a logon task (`\PushMPC\Plex libmpv check`) re-applies it after Plex updates; mpv-integration then writes
  PushMPC's block and `tls-ca-file=<Plex>\resources\cacert.pem` into Plex's `mpv.conf`. Uninstall reverses whichever
  ran. `-PlexSwap No` declines both; `PUSHMPC_TEST_PLEX_PROXY=fail` forces the fallback in tests.
- **Plex on Linux (Flathub):** the tarball's `plex/` holds `pushmpc_preload.so` and `pushmpc-plex-selftest` (built in
  the release's manylinux job from `mpvnative/hook/pushmpc_preload.c` and `libmpv_host.c`), `pushmpc-plex-linux.sh`, and
  symlinks to the native hook's `libpushmpc-avfilter-8.*.so` and `libpushmpc_engine.so` (`PUSHMPC_PLEX_DIR` for
  make-tarball.sh). setup.sh runs `pushmpc-plex-linux.sh install --from plex --json` when a Plex Flatpak is installed;
  a target that is not `installed` is uninstalled again; state `plex`. Snap Plex is skipped.
- **VLC, Kodi:** optional `vlc` and `kodi` components (`stage -VlcPlugin`, `-KodiPackage`), installed by
  `vlcplugin/install/pushmpc-vlc.ps1` and `kodiaddon/install/install.ps1` when the player is found.
- **Linux mpv:** `setup.sh` tries the native hook first (`mpvnative/install/pushmpc-native.sh`); when it does not activate
  in the distro's mpv, the VapourSynth route (`install.sh`) is used instead, never both.
- **Capture mode:** `scripts/pushmpc-capture.ps1` installs its own PushFPS into `%LOCALAPPDATA%\PushMPC\capture\PushFPS`
  (PushFPS's installer with `-InstallDir`, `-NoShortcuts` and its own TEMP), so logs, caches and `runtime_local` stay in
  that folder; it starts PushFPS with `--data-dir`, `--no-update-check`, `QML_DISK_CACHE_PATH` and TEMP inside it, and
  opens the player's window directly (`screen:0:<hwnd>:0:0:2:0:0:0:0`) on tested PushFPS versions. `-Capture On|Off`.
- **CI:** `.github/workflows/release-ux.yml` runs Setup headless on windows-2025 and `setup.sh` in Arch with players from
  their packages, and checks that uninstall leaves nothing behind.
- **Community unlocks:** the unlocks and ports in Veyra's package (the SF AI enhancement build for RTX 20-50, the DLSS-G
  unlocks for RTX 30/40, the AMD AI enhancement runtimes) are installed by default; `-ExperimentalExtras No` leaves them
  out.

## Setup and script options

Setup runs the same steps as the script install, which also works from a release zip: `Install-PushMPC.cmd`, or
`powershell -ExecutionPolicy Bypass -File pushmpc-install.ps1 install`.

`PushMPC-Setup.exe` options for scripted installs: `--quiet` (no window), `--install-dir <folder>`,
`--package-cache <folder>` (a folder that already holds Veyra's zip), `--no-existing` (never reuse an installed PushFPS
or Veyra), `--repair`, `--uninstall`, `--dry-run`, `--mpv-build`.

The installer picks Veyra's runtime package for the GPU (`-Gpu Auto|NVIDIA|AMD`; Auto: NVIDIA when an NVIDIA card is
present, else the AMD package, which also serves Intel), and takes the runtime files, in this order, from:

1. an installed PushFPS or Veyra with exactly the pinned files (every file's SHA-256 is checked; copied, or hard-linked
   with `-LinkExisting`; `-NoExisting` skips this);
2. the package zip in `-RuntimePackage <zip>`, `-PackageCache <folder>` or its own download folder;
3. a download from Veyra's GitHub release (about 750 MB, resumable), checked against the pinned SHA-256.

PushFlow is not in Veyra's packages: it comes from an installed PushFPS, a PushFPS release zip in `-PackageCache`, or
PushFPS's GitHub release, every file checked. See [release model](release-model.md).

Install layout, all under `%LOCALAPPDATA%\PushMPC`:

| Folder | What |
|---|---|
| `filter` | `PushMpcFilter.ax`, `PushMpcExit.dll`, the installer script and helpers |
| (top level) | `PushMpcUpdater.exe`, `PushMpcSetup.exe` (repair, update, uninstall from Windows Apps), `auto-plan.json` |
| `core` | `pushcore.dll`, its shaders, `pushcore_hip_preflight.exe` (AMD) |
| `runtime` | FFmpeg, the NVIDIA or AMD components and PushFlow |
| `pmml` | the PushFlow runtime for the GPU (when chosen) |
| `vsplugin`, `vapoursynth` | the VapourSynth filter and its runtime (only when an mpv-family player was found) |
| `vlc`, `mpv-native`, `plex`, `capture` | the VLC module, PushMPC's own mpv, Plex backups, capture mode's own PushFPS |
| `previous` | the version before the last update (rollback) |
| `logs` | logs (kept on uninstall; the last 20 playbacks, at most 50 MB) |

Other script actions: `status [-Verify]`, `update -ReleaseManifest release.json`, `uninstall [-KeepSettings]`,
`rollback -Component filter|core|runtime`, `plan`, `autoplan [-Apply]`.

## What Setup changes in each player

- **MPC-HC:** "PushMPC Filter" in External Filters (Prefer). MPC Video Renderer's HDR options (local tone mapping on,
  type Spline, display nits = the display's peak), with the previous values kept in `MpcVrBackup` and restored on
  uninstall; `ManageMpcVr` = 0 (DWORD) under `HKCU\Software\PushMPC` stops PushMPC from managing them. SVP's VapourSynth
  filter is disabled while PushMPC is installed (its interpolation would stack) and enabled again on uninstall.
- **MPC-BE:** `HKCU\Software\MPC-BE\ExternalFilters\NNN` "PushMPC Filter" (Prefer, NV12 and P010 inputs), and MPC Video
  Decoder's `HwDecoder` = 2 (D3D11 copy-back; D3D11 native leaves no room for a filter) under
  `HKCU\Software\MPC-BE\Filters\MPC Video Decoder`, previous value restored on uninstall unless changed since. INI mode
  (`mpc-be64.ini`) is detected and left alone. MPC-HC and MPC-BE share one MPC Video Renderer registration.
- **PotPlayer:** "PushMPC Filter" in `HKCU\Software\DAUM\PotPlayerMini64\Override` (Prefer) in front of the list's end
  marker, plus LAV Video Decoder after it when LAV Filters is installed (it passes HDR mastering data and MaxCLL on).
  Uninstall restores the list exactly. INI mode (`PotPlayerMini64.ini`) is left alone. PushMPC cannot ask PotPlayer to
  reopen a file, and the G-SYNC button covers MPC-HC and MPC-BE only.
- **mpv family:** see [mpv and mpv-based players](players-mpv.md).
- **NVIDIA profile:** the settings page's G-SYNC button changes only MPC-HC's driver profile, with a backup restored on
  uninstall.

## The updater

`PushMpcUpdater.exe` reads the release list of this repository, checks `release.json` against `release.json.sig` with
the public key built into it, and downloads only the components whose version changed (and Veyra's package when a
release pins a new one), each checked by SHA-256 and resumable. It installs at once when no player has PushMPC loaded;
otherwise the task `\PushMPC\Apply pending update` (current user only) installs it when the player is closed or at the
next sign-in. A new filter is registered and loaded once as a test; if that fails, the previous version in `previous\`
is put back and that release is not tried again.

    PushMpcUpdater.exe --check | --download | --apply | --status

Settings and status under `HKCU\Software\PushMPC\Update`: `Enabled` (DWORD, 0 = off), `Status`, `Message`,
`AvailableVersion`, `LastCheck`; log `%LOCALAPPDATA%\PushMPC\logs\updater.log`.

## Linux and macOS

- **Linux tarball:** `packaging/linux/make-tarball.sh` (built by release.yml in manylinux_2_28 for x86_64 and aarch64).
  `setup.sh --help` lists its actions; `PUSHMPC_TEST_GPU`, `PUSHMPC_TEST_VULKAN`, `PUSHMPC_TEST_NVIDIA_DRIVER` and
  `PUSHMPC_TEST_UNAME` stand in for the hardware in tests (`integrations/linux/tests/setup.tests.sh`).
- **VLC on Linux, hardware decoding:** the installer tests VLC's hardware decoding with PushMPC and writes the result to
  `hw-selftest.txt` in the plugin folder (`pushmpc-vlc.sh selftest-hw` runs it again). Zero-copy VA-API display needs
  Mesa 25.1 or newer, or an X server with DRI3; otherwise the installer sets `avcodec-hw=vaapi_drm`; if no hardware path
  passes, software decoding stays on.
- **macOS (Homebrew):** this repository is the tap; `Formula/pushmpc.rb` builds the VapourSynth plugin with the
  VideoToolbox and ML backends, the mpv helper, the IINA bridge and `pushmpc-setup`, and carries the Plex for Mac
  archive. The release fills in its asset URLs (`scripts/update-formula.py`); `.github/workflows/apple.yml` (job
  `homebrew`) tests the tap route from the checkout; see [packaging/homebrew/README.md](../packaging/homebrew/README.md).
