# Release model: PushMPC never ships NVIDIA's files

A PushMPC release contains only PushMPC's own work:

| In the release | What |
|---|---|
| `PushMPC-Setup.exe` | the installer for end users: one file with the `filter` and `core` components (and the optional `vsplugin`) inside |
| `PushMPC-<v>.zip` | the same for a script install: `pushmpc-install.ps1`, `Install-PushMPC.cmd`, `filter\`, `core\` |
| `filter\` | `PushMpcFilter.ax`, `PushMpcExit.dll`, `PushMpcUpdater.exe`, the installer script and `release-key.xml` (used by update, repair and uninstall), `manifest.txt` |
| `core\` | `pushcore.dll` and its compiled shaders, `pushcore_hip_preflight.exe`, `manifest.txt` |
| `vsplugin\` (optional, preview) | the mpv route: `pushmpc_vs.dll`, `pushmpc.vpy`, `mpv-integration.ps1`, `vapoursynth-pin.json`, `manifest.txt`; marked `optional` in `release.json`, installed by Setup only when it finds mpv or SMPlayer |
| `PushMPC-filter-<v>.zip`, `PushMPC-core-<v>.zip`, `PushMPC-vsplugin-<v>.zip` | the components (the updater downloads filter and core; vsplugin is updated by a newer Setup) |
| `release.json`, `release.json.sig` | the release manifest (component versions, relative URLs, SHA-256, Veyra's package reference) and its signature |

The **runtime** (FFmpeg, the NVIDIA or AMD components that pushcore loads, and PushFlow) is never part of a release. It
comes from **Veyra's official runtime package** for the GPU (NVIDIA or AMD, `-Gpu`), the same packages PushFPS's own
installer uses, and PushFlow from PushFPS's own release: reused from an installed PushFPS / Veyra when every file
matches its pinned SHA-256, else downloaded at install time.

| Source | Pinned | Gives |
|---|---|---|
| `Veyra-2.2.1-NVIDIA-win64-portable.zip` (Veyra's release v2.2.1) | zip SHA-256 `E09150D1...544386B` and every file's SHA-256 | NVIDIA runtime, FidelityFX |
| `Veyra-2.2.1-AMD-win64-portable.zip` (Veyra's release v2.2.1) | zip SHA-256 `B6440744...0077BF8` and every file's SHA-256 | FidelityFX, AMD AI enhancement runtimes |
| `PushFPS-4.5.3-win64.zip` (PushFPS's release v4.5.3), or an installed PushFPS (4.2.0 or newer: the same PushFlow files) | every PushFlow file's SHA-256 | PushFlow (ONNX Runtime, DirectML, RIFE models) |
| `VapourSynth64-Portable-R82.zip` (VapourSynth's release R82) and `python-3.12.10-embed-amd64.zip` (python.org), only for the mpv route | each zip's SHA-256 (`scripts/vapoursynth-pin.json`) | the VapourSynth runtime in `%LOCALAPPDATA%\PushMPC\vapoursynth` (R82's wheel unpacked to `lib\site-packages`) |

## Install

`PushMPC-Setup.exe` (a small native program, per user, no administrator rights) unpacks its `filter` and `core` into a
temporary folder and runs the bundled `pushmpc-install.ps1` hidden, reading its progress: the script is the one
implementation of the steps below. Setup adds what a script cannot do well: it closes MPC-HC on request, downloads
Veyra's package itself (progress, cancel, resume, SHA-256) before the script runs, writes the Windows Apps entry
(`HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\PushMPC`: name, version, publisher "PushMPC (community)",
icon, size, `UninstallString`) and keeps a copy of itself as `%LOCALAPPDATA%\PushMPC\PushMpcSetup.exe`, which runs as
Repair / Update / Uninstall. The script install still works from the release zip:

    Install-PushMPC.cmd            (or: powershell -File pushmpc-install.ps1 install)

1. The installer names the pinned package: asset, Veyra's release page and its SHA-256 (pinned when this PushMPC release
   was built, as PushFPS pins it).
2. It looks for that zip in `-RuntimePackage <zip>`, then in `-PackageCache <folder>`, then in its own download folder
   (`%LOCALAPPDATA%\PushMPC\download`). Anything found must match the SHA-256; a mismatch is not used.
3. Otherwise it downloads the zip from Veyra's GitHub release (about 750 MB, resumable with curl), checks the SHA-256
   and deletes the file on a mismatch.
4. It extracts only the files PushMPC uses (NVIDIA: 20 files and four folders; AMD: 8 files and two folders; plus
   the package's licence texts) into `runtime\`. No other file of the package is unpacked. Before any download it
   looks for an installed PushFPS or Veyra holding exactly these files (`-NoExisting` skips that).
5. Licence handling, as in PushFPS's installer:
   - every NVIDIA file PushMPC needs must be an original with a valid NVIDIA signature, or the installer stops (the
     package is not the expected one);
   - NVIDIA files that are not signed originals (community builds and unlocks, such as the SF AI enhancement slot for
     RTX 20-50 or the DLSS-G files for RTX 30/40) and the AMD AI enhancement runtimes are installed by default;
     `-ExperimentalExtras No` leaves them out;
   - the package's `LICENSE`, `THIRD_PARTY_NOTICES.md` and `licenses\` (NVIDIA's licences under `licenses\nvidia-*`)
     go into the runtime folder with the files.
6. `runtime\manifest.txt` records the package (asset, URL, SHA-256), the file-list revision and the extras choice.
   The filter checks it against the core's `requires-runtime` before loading anything.

The runtime is prepared before the filter and core are replaced, so a failed download leaves an installed PushMPC as
it was.

## Update

The updater, `%LOCALAPPDATA%\PushMPC\PushMpcUpdater.exe`, reads the feed:

| Channel | Feed |
|---|---|
| Stable (default) | `https://github.com/CptPakundo/PushMPC/releases/latest/download/release.json` (GitHub's latest release, never a pre-release) |
| Beta | the newest release in GitHub's release list that carries a `release.json`, pre-release or not (tags `vX.Y.Z-beta.N`); if GitHub's API is unavailable, the stable feed |

Settings in `HKCU\Software\PushMPC\Update` (all optional): `Enabled` (DWORD, 1), `Channel` (`Stable` / `Beta`),
`FeedUrl` (another feed: a URL, a `file://` URL or a folder), `BetaFeedUrl`, `AllowUnsigned` (DWORD, 0 in release
builds). Status for the property page: `LastCheck` (UTC, ISO 8601), `Status` (`UpToDate`, `Available`, `Downloaded`,
`Error`), `AvailableVersion`, `Message`; also `LatestVersion`, `InstalledVersion`, `LastApply`, `FailedVersion`.

| Command | Does |
|---|---|
| `--check` | fetches `release.json` and `release.json.sig`, verifies the signature with the release key built into the updater, compares the component versions with the installed `manifest.txt` files, writes the status |
| `--download` | `--check`, then each changed component: its zip (relative URLs resolve to `<repository>/releases/download/<tag>/`), resumable, SHA-256 checked, unpacked into `staging\<component>-<version>` and checked file by file against its manifest; a new runtime pin: Veyra's package (resumable, SHA-256), then the release's own installer script builds `staging\runtime-<version>` (`pushmpc-install.ps1 plan` / `prepare-runtime`) |
| `--apply` | when no process has a file of `filter`, `core` or `runtime` loaded (MPC-HC, MPC-BE or any other): `<component>` to `previous\<component>`, staging to `<component>`, runtime first, filter last; then the filter is registered and loaded once in a child process (`DllGetClassObject`), and the core image is checked; any failure puts all of them back and marks the release as failed. Otherwise the update stays in `pending.json` (the same file `pushmpc-install.ps1 apply-pending` reads) |
| `--auto` | what the filter starts, at most once a day after stable playback, detached and at background priority: apply a pending update, then (if the last check is 24 h old) check, download, apply |

The per-user task `\PushMPC\Apply pending update` (at logon, least privilege, no administrator rights) runs
`--apply`, so a downloaded update lands even if MPC-HC is rarely opened. The updater replaces itself from the new
`filter\` component. Log: `%LOCALAPPDATA%\PushMPC\logs\updater.log`.

`release.json` (schema 3) lists `filter` and `core` as PushMPC's zips (URL relative to the release + SHA-256) and the
runtime only as a **reference** to Veyra's package, one per GPU vendor:

    { "name": "runtime", "vendor": "NVIDIA", "format": "veyra-package", "version": "veyra-2.2.1-nvidia", "subset": "3",
      "asset": "Veyra-2.2.1-NVIDIA-win64-portable.zip", "repository": "https://github.com/Likely7/Veyra-NRVideo",
      "tag": "v2.2.1", "url": "https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.2.1/...", "sha256": "..." }

Both the updater and `pushmpc-install.ps1 update -ReleaseManifest release.json` refuse a runtime entry that is not
such a reference (format `veyra-package` and a GitHub release URL). The runtime is prepared again only when the package
or the file list changed.

## Signing

`release.json.sig` is the base64 RSA-SHA256 (PKCS#1 v1.5) signature over the exact bytes of `release.json`. The public
key is `updater/release-key.xml` (an RSAKeyValue XML), built into `PushMpcUpdater.exe` and copied next to the installer
script. A release build of the updater refuses a manifest without a valid signature (`AllowUnsigned` = 1 allows
unsigned ones for local builds); a manifest whose signature does not match is always refused. The private key never
enters the repository: `scripts\new-signing-key.ps1` writes it to `deps\signing\release-private.xml` (git-ignored) and
the public half to `updater\release-key.xml`; GitHub Actions reads the private key from the secret
`PUSHMPC_SIGNING_KEY`. `package` signs with `-SigningKey <xml>`, `PUSHMPC_SIGNING_KEY` or that file, and refuses to
finish if the signature does not verify with `updater\release-key.xml`.

The executables are not Authenticode-signed yet (Windows SmartScreen warns on the first run of Setup). The code-signing
hook: `PUSHMPC_CODESIGN` = a command line with `{file}` (for example a `signtool sign ... "{file}"`); `stage` signs the
binaries before their manifests are written and `package` signs `PushMPC-Setup.exe`. In GitHub Actions the optional
secrets `CODESIGN_PFX_BASE64` and `CODESIGN_PFX_PASSWORD` set it up.

## Build pipeline (GitHub Actions)

| Workflow | When | Does |
|---|---|---|
| `release.yml` | a tag `vX.Y.Z` (stable) or `vX.Y.Z-beta.N` (pre-release), by hand, or called by the watch | on `windows-latest`: PushFPS at the tag pinned in `scripts/pushfps-pin.json` with `patches/` applied (its dependencies fetched as PushFPS's own CI does, with the pins read from PushFPS's workflow at that tag), pushcore, MPC Video Renderer's base classes at the pinned commit, the filter, the updater (release build), `pushmpc_vs.dll` (the optional vsplugin component), `stage` and `package`; then the optional GPU smoke test; then a GitHub release with Setup, the zips, `release.json` and its signature, its body from `docs/release-notes-<version>.md` when that file exists. By hand it runs as a dry run by default (no release, no tag) |
| `pushfps-watch.yml` | daily | compares DosukaSOL/PushFPS's latest release with the pin; if newer, applies our patch to the new tag (`git apply --3way`), commits the moved pin and the regenerated patch to `bot/pushfps-<tag>`, builds it with `release.yml` (published as a beta when the variable `PUSHFPS_WATCH_BETA` is `true`), then opens a pull request; a patch that does not apply or a failed build opens an issue |

GPU test: hosted runners have no GPU. With the variable `PUSHMPC_GPU_RUNNER` = `true`, a self-hosted Windows runner
labelled `gpu` installs the build into a scratch folder (no MPC-HC change), downloads a public test clip (Big Buck
Bunny, 10 s) and runs `pushcore_gpu_harness` at 2x and 4x; a release is published only if it passes. See
[building](building.md#gpu-runner).

## Files taken from the package

| File | For |
|---|---|
| `avutil-61.dll`, `swscale-10.dll` | FFmpeg (pushcore imports) |
| `vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll` | Visual C++ runtime, when the system has none |
| `runtime_local\amd\fidelityfx\*` (loader, upscaler, frame generation) | FidelityFX: FSR upscaling, FSR frame generation, the optical flow DLSS-G uses (both packages) |
| `runtime\config\ngx-local.json` | NGX project id (NVIDIA) |
| `runtime\experimental\nvngx_vsr.dll` | RTX Video Super Resolution (NVIDIA) |
| `runtime\experimental\nvngx_truehdr.dll` | RTX Video HDR (NVIDIA) |
| `runtime\experimental\nvngx_dlssg.dll`, `dlssg-lite\`, `fallback-dlssg-310.7\`, `dlssg-kernels\` | DLSS frame generation, its performance mode (RTX 50) and the RTX 30/40 unlock files (NVIDIA) |
| `runtime\experimental\nr-original\nvngx_dlssnr.dll`, `nr-ampere\` | AI enhancement: NVIDIA's original runtime (RTX 50), the SF build (RTX 20-50) |
| `runtime\nvidia-vfg\*` (7 files) | NVIDIA VFG frame generation and the RTX VSR styles (Video Effects SDK) |
| `runtime\amd-nr\`, `runtime\amd-nr-rdna3\` | AI enhancement on RX 9000 (lmxxf) and RX 7000 (RedDuke worker process) (AMD) |
| `runtime\experimental\pushflow\*` (20 files, from PushFPS) | PushFlow frame generation (any DirectX 12 GPU) |
| `LICENSE`, `NOTICE.md`, `THIRD_PARTY_NOTICES.md`, `licenses\*` | the package's licence texts |
## Building a release

    updater\build.cmd                          (PushMpcUpdater.exe; it goes into the filter component)
    pushmpc-install.ps1 stage                  (filter + core from the build outputs)
    pushmpc-install.ps1 package [-ReleaseDir <folder>] [-ReleaseVersion 0.6.1] [-Prerelease] [-SigningKey <xml>]

`package` writes the component zips, `release.json` and its signature, and builds `PushMPC-Setup.exe` with the
components inside (`installer\build.cmd <payload>`). It refuses to finish if any file in the release folder carries
NVIDIA's company name. For a developer's own
machine, `stage -WithRuntime -PackageCache <folder>` also stages the runtime from a zip already on disk; that staged
runtime is used by `install` and never packaged.
