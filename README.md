<div align="center">

<img src="assets/logo/pushmpc-logo-1-clapper-push.svg" width="112" height="112" alt="">

# PushMPC

**PushFPS's video enhancement for MPC-HC, MPC-BE, PotPlayer, mpv, VLC, Kodi, Stremio, Plex and more.**<br>
RTX Video Super Resolution, up to 8x frame generation with a scene-cut guard, SDR to HDR and lossless HDR handling,
set up by itself for your GPU and display. Your player stays your player.

<p>
<a href="https://github.com/CptPakundo/PushMPC/releases/latest/download/PushMPC-Setup.exe"><img alt="Download for Windows" src="https://img.shields.io/badge/Download_for_Windows-8EF02E?style=for-the-badge&logo=windows&logoColor=0D1A04&labelColor=8EF02E"></a>
</p>

<sub><a href="https://github.com/CptPakundo/PushMPC/releases/latest">All files and what's new</a> · Windows 10/11 64-bit, Linux, macOS · one Setup, no PushFPS install needed</sub>

<p>
<a href="https://github.com/CptPakundo/PushMPC/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/CptPakundo/PushMPC?include_prereleases&color=444&label=release"></a>
<a href="https://github.com/CptPakundo/PushMPC/releases"><img alt="Downloads" title="GitHub's count of release downloads" src="https://img.shields.io/github/downloads/CptPakundo/PushMPC/total?color=444"></a>
<a href="LICENSE"><img alt="GPL-3.0 license" src="https://img.shields.io/github/license/CptPakundo/PushMPC?color=444"></a>
</p>

<!-- Screenshots: put the files in assets/screenshots/ (list at the end of this file), then remove this comment's markers.
<p>
<img src="assets/screenshots/before-after.jpg" width="760" alt="The same frame without and with PushMPC: sharper detail from RTX Video Super Resolution">
</p>
<p>
<img src="assets/screenshots/property-page.jpg" width="374" alt="PushMPC's settings in MPC-HC: Auto for everything">
<img src="assets/screenshots/status.jpg" width="374" alt="The status: frame rate, GPU time, method, cut guard and display">
</p>
-->

</div>

> **Made with AI ("vibe coded").** The code, tests and documentation were written by an AI (Claude, by Anthropic) from
> the repository owner's prompts and testing. There is no warranty (see `LICENSE`). The video engine is
> [PushFPS](https://github.com/DosukaSOL/PushFPS) by DosukaSOL, built on [Veyra](https://github.com/Likely7/Veyra-NRVideo)
> by Likely7; the credit for that work belongs to its authors.

## ✨ What you get

- 🎞️ **Smoother motion**: NVIDIA VFG frame generation up to **8x**, so a 24 fps film fills a 120-240 Hz display. DLSS-G,
  PushFlow, FSR and AMD FRC where your GPU runs those instead.
- ✂️ **Scene-cut guard**: a new shot starts exactly on its own frame instead of a smeared blend, for every frame
  generation method. Letterbox changes are guarded too.
- 🔍 **Sharper picture**: RTX Video Super Resolution to your screen size (FSR or NIS on other GPUs), plus PushFPS's AI
  clean-up and deband for low-bitrate video.
- 🌗 **SDR to HDR**: SDR video shown as HDR10 with a calibrated curve, or RTX Video HDR, tuned to your display's peak.
- 🎨 **HDR kept intact**: HDR10, HDR10+, HLG and Dolby Vision (profiles 5, 7, 8) pass through losslessly.
- 🤖 **Auto**: every setting picks itself for your GPU, display, refresh rate (VRR or fixed) and video. Change anything
  you like; Auto is only the default.
- 📦 **One Setup, updates itself**: no PushFPS install, no administrator rights, quiet updates while you are not watching,
  and a rollback if a new version fails to load.

## 📺 Players and GPUs

Setup finds your players and your graphics card and sets everything up for you. You don't have to answer any
questions.

✅ tested on a real PC · 🆕 sets itself up · 🧪 preview (works, still being tested)

| Player | Windows | Linux | macOS |
|---|---|---|---|
| **MPC-HC** | ✅ | | |
| **MPC-BE**, **PotPlayer** | 🆕 | | |
| **mpv**, **mpv.net**, **SMPlayer**, **MPC-QT** | 🆕 | 🆕 | 🧪 |
| **Celluloid**, **Haruna** | | 🆕 | |
| **IINA** | | | 🧪 |
| **VLC 3** | 🧪 | 🧪 | 🧪 |
| **Kodi** | 🆕 | 🆕 | |
| **Stremio** (5.0.28 and newer) | 🆕 | | |
| **Plex** (Plex for Windows, Plex HTPC; Linux: from Flathub) | 🧪 | 🧪 | |
| **Jellyfin** (Jellyfin Desktop, Jellyfin MPV Shim, Plex MPV Shim) | 🆕 | 🆕 | |
| **Any other player** (KMPlayer, GOM, Emby Theater, ...) | 🧪 capture mode | | |

**Capture mode** is for players PushMPC can't plug into. Setup turns it on only if you have one of those players: you
then find **PushMPC Capture** in the Start menu. Start your player, then PushMPC Capture, and the picture shows up
smoother in a second window on top. It reacts a little later than the player (about 1/20 of a second), so if the sound
seems early, set your player's audio delay to about +80 ms. Copy-protected streams (like Netflix) stay black. On Linux
and macOS there is a [workaround with OBS](docs/user-guide.md#capture-on-linux-and-macos-workaround).

| Graphics card | Smoother motion | Sharper picture |
|---|---|---|
| **GeForce RTX 50 / 40** | NVIDIA frame generation, up to 8x | RTX Video Super Resolution, RTX Video HDR |
| **GeForce RTX 30 / 20** | NVIDIA frame generation (RTX 30), DLSS-G, PushFlow | RTX Video Super Resolution, RTX Video HDR |
| **GeForce GTX 16 / 10** | PushFlow, FSR | RTX Video Super Resolution (GTX 16) or NIS |
| **Radeon RX 6000 / 7000 / 9000** | PushFlow, FSR, AMD Fluid Motion | FSR 4.1 |
| **Radeon RX 5000**, **Intel Arc**, **Snapdragon X** | PushFlow, FSR | FSR or NIS |
| **Linux** | NVIDIA frame generation on RTX cards, PushFlow, PushMPC's own engine | RTX Video Super Resolution, NIS |
| **Mac** (Apple silicon) | Apple's frame interpolation (macOS 26) | Apple's super resolution |

If your graphics card can run a feature, you get it. Auto picks the best mix for your card and your screen, and adjusts
while you watch. More in the [user guide](docs/user-guide.md#gpus).

## 🚀 Install

**Windows 10/11:** download **[PushMPC-Setup.exe](https://github.com/CptPakundo/PushMPC/releases/latest/download/PushMPC-Setup.exe)**
and run it. It installs for your account only (no administrator rights needed), sets up every player it finds, and
downloads what your graphics card needs (about 750 MB the first time; if PushFPS is already installed it reuses its
files). Then just play a video. In MPC-HC and MPC-BE the settings are under **Play > Filters > PushMPC Filter**.

Windows may say "Windows protected your PC" because Setup isn't signed yet: choose **More info > Run anyway**. To
remove PushMPC, go to *Windows Settings > Apps > Installed apps > PushMPC*; your players' settings go back to how they
were.

**Linux:** download `pushmpc-linux-x86_64-<version>.tar.gz` (or `aarch64`) from the
[releases](https://github.com/CptPakundo/PushMPC/releases/latest), unpack it and run `sh setup.sh`. It sets up every
player it finds. It may ask you twice: whether to install mpv from Flathub (Ubuntu and Debian need it) and, on NVIDIA
cards, whether to accept NVIDIA's licence for their video effects. To remove it: `sh ~/.local/share/pushmpc/setup.sh
uninstall`.

**Mac:** a Homebrew package is on the way. Until then, see [mpv, IINA and other players](docs/players-mpv.md#macos).

## 🤖 How Auto picks

- **Smoother**: the highest multiplier your GPU keeps up with (measured at the 95th percentile) and your display can show.
  On a fixed-refresh display an even cadence wins a tie.
- **Method and quality**: the best frame generation method your GPU runs; High quality only where it fits at the same
  multiplier with time to spare.
- **Sharper**: the upscaler to your screen size, with the GPU time left over; the model style follows the bitrate.
- **HDR**: SDR to HDR only when Windows HDR is on and the video is SDR; HDR video always passes through untouched.
- **It learns**: Auto measures your GPU's real costs while you watch and adjusts. The status shows where it stopped and why.

## ❓ FAQ

<details><summary><b>Does frame generation add delay?</b></summary>

Not between picture and sound: every frame, real or generated, keeps its own timestamp, and the renderer shows it on the
playback clock. A generated frame needs the next source frame, so PushMPC works a little behind the decoder, which
already decodes ahead of the picture. After a seek, the first generated frame appears in about 60 ms (measured on an
RTX 5090).
</details>

<details><summary><b>How heavy is it on my GPU?</b></summary>

Auto only turns on what fits in the time between frames, with headroom, and steps down instead of dropping frames.
An RTX 40/50 handles 4K with VSR and high multipliers; older GPUs get a lower multiplier or PushFlow. The status shows
the GPU time per source frame.
</details>

<details><summary><b>Which GPUs work?</b></summary>

Any DirectX 12 GPU on Windows 10/11 (x64; Windows on Arm through Setup's x64 emulation), any Vulkan GPU on Linux, and
Apple silicon on macOS. NVIDIA RTX 40/50 get everything; the others get what each can run (see the table above).
</details>

<details><summary><b>Do I need an HDR display?</b></summary>

No. On an SDR display you get frame generation and upscaling, and HDR video is tone mapped by MPC Video Renderer. With
Windows HDR on, SDR video can be shown as HDR, and HDR10, HDR10+ and Dolby Vision pass through losslessly.
</details>

<details><summary><b>What does PushMPC send over the internet?</b></summary>

Nothing about you or what you watch. The only connections are downloads from GitHub: Setup fetches the runtime from
Veyra's and PushFPS's releases, and the updater fetches the signed release manifest at most once a day (you can switch
it off), plus the files of an update. Like any download, GitHub sees your IP address and the program's name and
version; PushMPC has no analytics or telemetry. Diagnostics you copy for a bug report leave out your user name, folders
and video file names.
</details>

<details><summary><b>How do updates work?</b></summary>

PushMPC checks at most once a day during playback (never delaying the start of a video), downloads only what changed
and installs when the player is closed (at the latest at your next sign-in). It keeps the previous version and goes back
to it by itself if a new one fails to load. Stable and Beta channels; turn it off on the About tab.
</details>

<details><summary><b>Do I need PushFPS or SVP?</b></summary>

No. PushMPC brings its own copy of PushFPS's engine and Veyra's runtime. If PushFPS or Veyra is installed with the same
files, Setup reuses them instead of downloading.
</details>

## 🗺️ Roadmap (after the first release)

- Capture mode on Linux and Mac without OBS, with the best picture PushMPC can make there.
- Plex on Mac.

## 📚 More

- [User guide](docs/user-guide.md): every setting, players, GPUs, HDR, updates, troubleshooting.
- [mpv, SMPlayer and MPC-QT](docs/players-mpv.md): the VapourSynth route, Linux and macOS (preview).
- [Changelog](CHANGELOG.md): what is in each release and the known limitations.
- [Release model](docs/release-model.md): what a release contains and how updates are verified.
- [Building](docs/building.md) · [Licences](docs/licences.md)

## 🙏 Credits

- **[PushFPS](https://github.com/DosukaSOL/PushFPS)** by DosukaSOL: the enhancement engine (GPL-3.0), built here with a
  small patch set in `patches/`.
- **[Veyra](https://github.com/Likely7/Veyra-NRVideo)** by Likely7: the runtime package (GPL-3.0). PushMPC never ships
  NVIDIA's files; Setup downloads Veyra's package and checks its pinned SHA-256.
- **[MPC-HC](https://github.com/clsid2/mpc-hc)**, **[MPC Video Renderer](https://github.com/Aleksoid1978/VideoRenderer)**
  and **[LAV Filters](https://github.com/Nevcairiel/LAVFilters)**: the player, renderer and decoder PushMPC plugs into.
- **[VapourSynth](https://github.com/vapoursynth/vapoursynth)** (LGPL-2.1): the filter framework of the mpv route.
- NVIDIA's and AMD's runtime components are used under their own licences.

## 📄 Licence

PushMPC is [GPL-3.0-or-later](LICENSE). Third-party licences: [docs/licences.md](docs/licences.md).

<!--
Screenshots to take (anonymised: no file names, titles, paths or user names visible; use a public test clip such as
Big Buck Bunny or Tears of Steel, and crop out MPC-HC's title bar and playlist):
  assets/screenshots/before-after.jpg  1520x? split frame, left original, right PushMPC (VSR on), labels "Original" / "PushMPC"
  assets/screenshots/property-page.jpg the PushMPC property page, main tab, all on Auto
  assets/screenshots/status.jpg        the status panel during 4x-8x playback (fps, GPU time, method, cut guard, VRR)
  optional: assets/screenshots/cut-guard.gif  short loop of a scene cut, guard off vs on
  optional: assets/screenshots/setup.jpg      the Setup wizard's first page
Change a player row to ✅ once it is tested with a real GPU.
-->
