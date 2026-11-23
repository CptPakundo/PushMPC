# Changelog

All notable changes to PushMPC. Versions follow [semantic versioning](https://semver.org/); the core's version also
names the PushFPS release it is built from (`0.6.0+pushfps4.4.0`).

## 0.6.0: first release

The first public build of PushMPC: PushFPS's video enhancement engine as a DirectShow filter for MPC-HC and MPC-BE,
with its own Setup and updater, plus the mpv route as a preview.

### Inside

| Component | Version | Built from |
|---|---|---|
| Filter (`PushMpcFilter.ax`, updater, installer script) | 0.6.0 | this repository |
| Core (`pushcore.dll` and its shaders) | 0.6.0+pushfps4.4.0 | [PushFPS](https://github.com/DosukaSOL/PushFPS) v4.4.0 with the patches in `patches/` |
| Runtime (not in the release; Setup fetches or reuses it) | veyra-2.1.9 | [Veyra](https://github.com/Likely7/Veyra-NRVideo)'s v2.1.9 NVIDIA or AMD package, PushFlow from PushFPS v4.4.0 |
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
| mpv, SMPlayer | 🧪 preview, optional: the VapourSynth filter, set up only when Setup finds mpv or SMPlayer. Tested in CI with a stand-in core; not yet with a real GPU |
| MPC-QT | 🧪 preview, manual setup |
| PotPlayer (x64) | 🧪 manual setup, untested |
| SVP's own mpv | left alone (SVP manages it) |

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
