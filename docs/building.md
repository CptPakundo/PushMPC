# Building PushMPC

Clone into a folder **without spaces** in its path (FFmpeg/vcpkg and MSYS tools break on spaces). Layout:

    <repo>\PushFPS            PushFPS 4.4.0 source (tag v4.4.0) with patches\pushfps-4.4.0-pushmpc.patch applied
    <repo>\MpcVideoRenderer   MPC Video Renderer source (DirectShow base classes and interfaces)
    <repo>\pushmpc            pushcore (C ABI, built inside PushFPS's CMake tree)
    <repo>\filter             the DirectShow filter
    <repo>\updater            PushMpcUpdater.exe (and the helpers Setup shares), release-key.xml (the public release key)
    <repo>\installer          PushMPC-Setup.exe
    <repo>\deps               PushFPS's dependencies (Qt, vcpkg FFmpeg, SDKs), see PushFPS's build workflow
    <repo>\scripts            build and install scripts

Tools: Visual Studio 2022 or newer with C++ (found with vswhere; `PUSHMPC_VS` overrides), CMake and Ninja (bundled with VS).

    git -C PushFPS apply ..\patches\pushfps-4.4.0-pushmpc.patch
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
