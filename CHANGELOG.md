# Changelog

All notable changes to PushMPC. Versions follow [semantic versioning](https://semver.org/); the core's version also
names the PushFPS release it is built from (`0.6.0+pushfps4.5.3`).

## 0.6.0: first release

The first public build of PushMPC: PushFPS's video enhancement engine as a DirectShow filter for MPC-HC and MPC-BE,
with its own Setup and updater, plus the mpv route as a preview.

### Inside

| Component | Version | Built from |
|---|---|---|
| Filter (`PushMpcFilter.ax`, updater, installer script) | 0.6.0 | this repository |
| Core (`pushcore.dll` and its shaders) | 0.6.0+pushfps4.5.3 | [PushFPS](https://github.com/DosukaSOL/PushFPS) v4.5.3 with the patches in `patches/` |
| Runtime (not in the release; Setup fetches or reuses it) | veyra-2.2.1 | [Veyra](https://github.com/Likely7/Veyra-NRVideo)'s v2.2.1 NVIDIA or AMD package, PushFlow from PushFPS v4.5.3 |
| vsplugin (optional, preview) | 0.6.0 | this repository; VapourSynth R82 with Python 3.12 fetched at install |

### Features

- **Frame generation** up to 8x: NVIDIA VFG (RTX 40/50, and the RTX 30 port), DLSS-G (unlocked on RTX 30/40), PushFlow
  (any DirectX 12 GPU), FSR and AMD FRC.
- **Scene-cut guard** for every frame generation method: a new shot starts on its own frame instead of a blend;
  sudden letterbox and aspect-ratio changes are held like cuts.
- **Upscaling**: RTX Video Super Resolution to the screen size with bitrate-matched model styles (FSR or NIS on other
  GPUs), AI clean-up and a deband pass for low-bitrate video.
- **SDR to HDR** with Windows HDR on: PushMPC's calibrated curve on every GPU (HDR boost Auto / Off / Low / Medium /
  High, a bright-scene limiter, the display peak from Windows or set by hand), or RTX Video HDR.
- **HDR kept intact**: HDR10, HDR10+ (per-frame metadata), HLG (converted to HDR10) and Dolby Vision profiles 5, 7 and
  8 (output as HDR10; with profiles 7 and 8 the RPU is passed on every sample). MPC Video Renderer's HDR options are set
  for this and put back on uninstall.
- **Auto** for every setting: the multiplier from the GPU's measured cost (95th percentile) and the display (fixed
  refresh or G-SYNC / VRR, from the NVIDIA driver profile and fullscreen state), then quality and upscaling with the
  time left. Auto learns the GPU's real costs while you watch, follows playback-rate changes and steps down instead of
  dropping frames. The status says what it chose and where it stopped.
- **Engine: PushFPS 4.5.3** (was 4.4.0 in the release candidates) on Veyra's runtime v2.2.1 (the same vendor runtimes,
  FFmpeg rebuilt with a D3D12 decoding fix; the package now keeps its program under `versions\2.2.1\`). It brings:
  **Subtitle Hold** (burned-in subtitles kept out of frame generation; Advanced > Subtitle hold, off under Auto: it costs
  about 0.5 ms per frame at 4K and soft subtitles never need it), FSR upscaling with motion at the right size (non-NVIDIA
  GPUs), and a stronger motion search for AMD FRC. With Subtitle Hold on, the scene-cut guard still tests the real
  frames.
- **AMD FreeSync** is detected for Auto (AMD driver: FreeSync on and the player fullscreen counts as variable refresh),
  as G-SYNC is on NVIDIA.
- **Settings page** in the player (Play > Filters > PushMPC Filter) with live "Now:" lines, an optional on-screen status,
  a G-SYNC button for MPC-HC's driver profile (with backup and restore), and anonymised Copy diagnostics.
- **Setup** per user, without administrator rights: installs the filter and core, fetches Veyra's runtime package
  (resumable, SHA-256 checked) or reuses an installed PushFPS / Veyra, sets up the players it finds, and registers
  Repair / Update / Uninstall in Windows Apps.
- **Updater**: a signed release manifest, at most one check a day during playback, downloads only changed components,
  installs when the player is closed (or at the next sign-in), and rolls back by itself if a new version fails to load.
  Stable and Beta channels.

### Players

| Player | Status in 0.6.0 |
|---|---|
| MPC-HC (x64) | ✅ supported, tested |
| MPC-BE (x64) | 🆕 automatic: Setup adds the filter and sets MPC Video Decoder to D3D11 copy-back (put back on uninstall). Setup's changes are checked against MPC-BE 1.9.3 in CI; playback in MPC-BE itself is not tested yet |
| PotPlayer (x64) | 🆕 automatic: the filter in its priority list; checked in CI with a real PotPlayer |
| mpv, mpv.net, SMPlayer, MPC-QT, Jellyfin, Kodi (external mpv) | 🆕 automatic on Windows and Linux; checked in CI with real players (no GPU there) |
| Stremio 5.0.28+ | 🆕 automatic (its mpv config file setting) |
| Plex for Windows / HTPC | 🧪 frame generation through capture mode; video super resolution inside Plex once its mpv supports it |
| VLC 3 | 🧪 preview: PushMPC's VLC module (Windows, Linux, macOS) |
| IINA, mpv on macOS | 🧪 preview: Homebrew formula (tap not published yet) |
| Any other Windows player | 🧪 capture mode (fallback): PushFPS's window capture, about 50-60 ms of delay, no DRM-protected video |
| SVP's own mpv | left alone (SVP manages it) |

Setup (Windows) and `setup.sh` (Linux) find the players and the graphics card, log the engine Auto starts with
(`auto-plan.json` / `auto-plan.txt`), write PushFlow's `pmml.txt` from it, offer AMD FSR SDK 2.4 on Radeon RX 6000-9000
and NVIDIA's Video Effects on Linux, and turn capture mode on only for players without a native route.

### Known limitations

- **Tested hardware:** one machine with a GeForce RTX 50 card. Other GPUs get what PushFPS's engine supports on them,
  but were not tested with PushMPC.
- **Not code-signed:** Windows SmartScreen warns on the first run of Setup (More info > Run anyway). The release manifest
  is signed and the updater checks it.
- **Interlaced video** plays without PushMPC (MPC Video Renderer deinterlaces); the status says why.
- **Dolby Vision profile 5** and truly interlaced sources had no test files: untested.
- **Scene-cut guard:** large non-rigid effects (fire, energy beams) can still look torn or ghosted with every method, and
  a cut in the middle of a fast zoom of hand-drawn animation can be missed.
- **G-SYNC in fullscreen** with only the driver's global "fullscreen only" setting is predicted from the profile; the
  monitor's own refresh readout has not confirmed it yet.
- **MPC-BE:** INI mode (`mpc-be64.ini`) is detected and left alone (add the filter by hand). MPC-HC and MPC-BE share one
  MPC Video Renderer registration: the copy installed last is the one both use.
- **PotPlayer:** PushMPC cannot ask it to reopen a file, and uninstalling does not remove its filter entry.
- **mpv route:** each frame is copied to the GPU and back (PCIe bandwidth; Auto drops to 4:2:0 above about 6 GB/s);
  PushMPC's own SDR-to-HDR curve and the on-screen status are not available there; the updater does not update the
  vsplugin component yet (run a newer Setup); SMPlayer support is experimental.
- **Linux, macOS, and AMD or Intel GPUs in mpv** (portable engine on the CPU, or vs-rife-ncnn-vulkan): preview, built
  and unit-tested only.
