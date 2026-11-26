# Building PushMPC

Clone into a folder **without spaces** in its path (FFmpeg/vcpkg and MSYS tools break on spaces). Layout:

    <repo>\PushFPS            PushFPS 4.5.3 source (tag v4.5.3) with patches\pushfps-4.5.3-pushmpc.patch applied
    <repo>\MpcVideoRenderer   MPC Video Renderer source (DirectShow base classes and interfaces)
    <repo>\pushmpc            pushcore (C ABI, built inside PushFPS's CMake tree)
    <repo>\filter             the DirectShow filter
    <repo>\updater            PushMpcUpdater.exe (and the helpers Setup shares), release-key.xml (the public release key)
    <repo>\installer          PushMPC-Setup.exe
    <repo>\deps               PushFPS's dependencies (Qt, vcpkg FFmpeg, SDKs), see PushFPS's build workflow
    <repo>\scripts            build and install scripts

Tools: Visual Studio 2022 or newer with C++ (found with vswhere; `PUSHMPC_VS` overrides), CMake and Ninja (bundled with VS).

    git -C PushFPS apply ..\patches\pushfps-4.5.3-pushmpc.patch
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

`.github/workflows/release.yml` builds a release on `windows-latest` from a tag (`v0.6.1`, or `v0.6.1-beta.1` for the
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

## How Setup works (for developers)

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
- **Plex:** `scripts/pushmpc-plex.ps1` replaces Plex's `libmpv-2.dll` with a VapourSynth-enabled libmpv (shinchiro's mpv
  build, SHA-256 pinned) after a self-test, on tested Plex versions only (`$Tested`), with a backup, elevated once; a
  logon task (`\PushMPC\Plex libmpv check`) re-applies it after Plex updates; uninstall restores the original.
  mpv-integration then writes PushMPC's block and `tls-ca-file=<Plex>\resources\cacert.pem` into Plex's `mpv.conf`.
  `-PlexSwap No` declines it.
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
