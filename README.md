<div align="center">

<img src="assets/logo/pushmpc-logo-1-clapper-push.svg" width="112" height="112" alt="">

# PushMPC

**Smoother, sharper video in the player you already use.**<br>
PushMPC brings PushFPS's frame generation and upscaling to MPC-HC, mpv, VLC, Kodi, Plex and more,
and sets itself up for your graphics card and display.

<p>
<a href="https://github.com/CptPakundo/PushMPC/releases/latest/download/PushMPC-Setup.exe"><img alt="Download for Windows" src="https://img.shields.io/badge/Download_for_Windows-8EF02E?style=for-the-badge&logo=windows&logoColor=0D1A04&labelColor=8EF02E"></a>
<a href="https://github.com/CptPakundo/PushMPC/releases/latest"><img alt="Download for Linux" src="https://img.shields.io/badge/Download_for_Linux-8EF02E?style=for-the-badge&logo=linux&logoColor=0D1A04&labelColor=8EF02E"></a>
</p>

<p>
<a href="https://github.com/CptPakundo/PushMPC/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/CptPakundo/PushMPC?color=444&label=release"></a>
<a href="LICENSE"><img alt="Licence: GPL-3.0" src="https://img.shields.io/github/license/CptPakundo/PushMPC?color=444"></a>
</p>

</div>

## What you get

- **Smoother motion.** Up to 16 frames for every frame of the video, so a 24 fps film fills a 120 Hz or 240 Hz screen.
- **Clean scene cuts.** A new shot starts on its own frame instead of a smeared blend.
- **A sharper picture.** Video is upscaled to your screen with RTX Video Super Resolution, FSR or NIS, with an optional
  clean-up for low-quality streams.
- **HDR done right.** HDR10, HDR10+, HLG and Dolby Vision play untouched, and SDR video can be shown as HDR.
- **No fiddling.** Auto picks every setting for your graphics card and screen, and keeps adjusting while you watch.
  You can still change anything.
- **Your player stays your player.** PushMPC plugs into it, and uninstalling puts its settings back the way they were.

In our tests on a GeForce RTX 5090, MPC-HC played a 1080p film at 10x (240 fps, one new frame for every refresh) on a 4K
240 Hz screen without dropping a single frame.

## Players

| Player | Windows | Linux | Mac |
|---|:-:|:-:|:-:|
| MPC-HC, MPC-BE | ✅ | – | – |
| PotPlayer | ✅ | – | – |
| mpv, and PushMPC's own mpv | ✅ | ✅ | 🧪 |
| mpv.net | ✅ | – | – |
| SMPlayer, MPC-QT | ✅ | ✅ | – |
| Celluloid, Haruna | – | ✅ | – |
| IINA | – | – | 🧪 |
| VLC 3 | ✅ | ✅ | 🧪 |
| Kodi 21 and 22 | ✅ | ✅ | – |
| Plex (Plex for Windows, Plex HTPC, Plex for Mac; on Linux from Flathub) | ✅ | ✅ | 🧪 |
| Jellyfin Desktop, Jellyfin MPV Shim, Plex MPV Shim | ✅ | ✅ | – |
| Stremio 5.0.28 and newer | ✅ | – | – |
| Any other player (KMPlayer, GOM, Emby Theater, ...) through **capture mode** | ✅ | – | – |

✅ Works · 🧪 Preview (works, still being tested) · 🔜 Coming soon · – not available

Setup finds the players on your PC and sets up each one. MPC-HC has had the most testing on real hardware; the
others are checked automatically with every build.

**Capture mode** covers players PushMPC can't plug into. It shows the player's picture, made smoother, in a second
window. It is a little behind the sound, and copy-protected video (Netflix and the like) stays black, so a player from
the list above always looks better. See [capture mode](docs/user-guide.md#capture-mode).

## Graphics cards

PushMPC works on any graphics card that runs DirectX 12 (Windows) or Vulkan (Linux). What you get depends on the card:

| Graphics card | Smoother motion | Sharper picture |
|---|---|---|
| **GeForce RTX 50 / 40** | NVIDIA frame generation up to 16x | RTX Video Super Resolution, RTX Video HDR |
| **GeForce RTX 30 / 20** | NVIDIA frame generation (RTX 30), DLSS frame generation, PushFlow | RTX Video Super Resolution, RTX Video HDR |
| **GeForce GTX 16 / 10** | PushFlow, FSR | RTX Video Super Resolution (GTX 16) or NIS |
| **Radeon RX 6000 / 7000 / 9000** | PushFlow, FSR, AMD Fluid Motion | FSR |
| **Radeon RX 5000, Intel Arc, Snapdragon X** | PushFlow, FSR | FSR or NIS |

PushFlow is PushFPS's AI frame generation. On Linux, RTX cards get NVIDIA's frame generation and Super Resolution, and
other cards get PushFlow or PushMPC's own engine. If your card can run a feature, you get it; Auto picks the best mix.

## Install

### Windows 10 and 11

1. Download **[PushMPC-Setup.exe](https://github.com/CptPakundo/PushMPC/releases/latest/download/PushMPC-Setup.exe)**
   and run it.
2. If Windows says "Windows protected your PC", click **More info**, then **Run anyway**. (Setup isn't code-signed
   yet.)
3. Click through the pages. Setup sets up every player it finds and downloads what your graphics card needs: about
   750 MB the first time, less if PushFPS is already installed.

No administrator rights are needed. Only Plex asks for permission once, because Plex is installed for all users.

### Linux

1. Download `pushmpc-linux-x86_64-<version>.tar.gz` (or `aarch64` for ARM) from the
   [latest release](https://github.com/CptPakundo/PushMPC/releases/latest).
2. Unpack it and run Setup from the folder:

   ```sh
   tar xf pushmpc-linux-x86_64-*.tar.gz
   cd pushmpc-linux-x86_64-*/
   sh setup.sh
   ```

Setup installs for your user only and sets up every player it finds. It may ask two questions: whether to install mpv
from Flathub (Ubuntu and Debian need it), and on NVIDIA cards whether to accept NVIDIA's licence for their video
effects. VLC and the Kodi add-on are in the x86_64 download.

### Mac

Preview, for mpv, IINA, VLC and Plex (Plex for Mac and Plex HTPC). You need [Homebrew](https://brew.sh). In Terminal:

```sh
brew tap CptPakundo/pushmpc https://github.com/CptPakundo/PushMPC
brew install CptPakundo/pushmpc/pushmpc
pushmpc-setup install
```

To remove it: `pushmpc-setup uninstall`, then `brew uninstall pushmpc`. More in the [user guide](docs/user-guide.md#installing-on-a-mac).

## Use

Just open a video in your player. PushMPC switches on by itself.

- **On-screen info.** In MPC-HC, MPC-BE and PotPlayer, a small line in the top-left corner shows what PushMPC is doing
  for the first few seconds, for example `PushMPC · 10x Medium · VSR 4 · 240 fps · VRR`.
- **Settings.** In MPC-HC and MPC-BE: **Play > Filters > PushMPC Filter**. In Kodi: the PushMPC add-on's settings. In
  VLC: **Tools > Preferences > All > Video > Filters > PushMPC**. Everything starts on Auto.
- **Plex, mpv and the others** use Auto with the same rules and need nothing from you.

The [user guide](docs/user-guide.md) explains every setting.

## Troubleshooting and FAQ

<details><summary><b>Nothing seems to change in my player.</b></summary>

Open the settings page (MPC-HC: Play > Filters > PushMPC Filter). Its "Now:" lines say what PushMPC is doing, or why
it is resting: for example interlaced video, another video renderer, or a hardware decoder mode that leaves no room for
PushMPC. In PotPlayer, choose MPC Video Renderer and a "copy-back" decoder (see the
[user guide](docs/user-guide.md#potplayer)). After installing a new player, run Setup again so it sets that one up too.
</details>

<details><summary><b>Do I need an HDR display?</b></summary>

No. On an SDR display you get smoother motion and upscaling, and HDR video is converted to fit your screen. With Windows
HDR on, HDR10, HDR10+, HLG and Dolby Vision pass through untouched, and SDR video can be shown as HDR. If you use an
OLED monitor's "True Black 400" mode, set **Display peak** to 400 in the settings.
</details>

<details><summary><b>G-SYNC, FreeSync and VRR</b></summary>

PushMPC reads whether G-SYNC or FreeSync is on for your player and chooses the frame rate to match. If your screen
supports G-SYNC but it is off for MPC-HC, the settings page offers **Turn on G-SYNC for MPC-HC**. It changes only
MPC-HC's own profile in the NVIDIA driver, and uninstalling puts it back.
</details>

<details><summary><b>The sound is early in capture mode.</b></summary>

Capture mode shows the picture about 1/20 of a second after the player. Set your player's audio delay to about
+80 ms (KMPlayer and GOM: audio settings; VLC: Tools > Track Synchronization). For Plex, PushMPC does this for you.
This only affects capture mode; every other player keeps picture and sound in sync.
</details>

<details><summary><b>How do I uninstall?</b></summary>

- **Windows:** Settings > Apps > Installed apps > PushMPC > Uninstall.
- **Linux:** `sh ~/.local/share/pushmpc/setup.sh uninstall`

Your players' settings go back to how they were before PushMPC.
</details>

<details><summary><b>Where are the logs?</b></summary>

- **Windows:** `%LOCALAPPDATA%\PushMPC\logs` (paste that into the Explorer address bar). The settings page's About tab
  opens the folder, and **Copy diagnostics** puts a report on the clipboard with your user name, folders and file
  names removed. Attach it to a bug report.
- **Linux:** `~/.local/state/pushmpc/logs`. Setup keeps its notes in `~/.local/share/pushmpc`.
</details>

<details><summary><b>Does it send anything over the internet?</b></summary>

Nothing about you or what you watch. Setup downloads the parts your graphics card needs from GitHub and the official
package sites, and PushMPC checks for updates at most once a day (you can switch that off on the About tab). There is
no tracking of any kind.
</details>

<details><summary><b>How do updates work?</b></summary>

On Windows, PushMPC checks at most once a day, downloads only what changed, and installs it when your player is
closed. If a new version doesn't work, it goes back to the previous one by itself. To update on Linux, download the new
release and run `sh setup.sh` again.
</details>

<details><summary><b>Do I need PushFPS or SVP?</b></summary>

No. PushMPC brings everything it needs. If PushFPS is already installed, Setup reuses its files to save the download.
PushMPC leaves SVP's own mpv alone. If SVP's filter is on in MPC-HC, Setup switches it off so the two don't stack, and
uninstalling turns it back on.
</details>

## Roadmap

- Capture mode on Linux and Mac (for now, there's a [workaround with OBS](docs/user-guide.md#capture-mode-on-linux-and-mac)).
- Faster AI frame generation on more graphics cards, including AMD Radeon and Snapdragon on Windows.

## Credits

PushMPC stands on the work of others:

- **[PushFPS](https://github.com/DosukaSOL/PushFPS)** by DosukaSOL: the video engine at PushMPC's heart (GPL-3.0).
- **[Veyra](https://github.com/Likely7/Veyra-NRVideo)** by Likely7: the runtime package with the NVIDIA and AMD parts
  (GPL-3.0). Setup downloads it from Veyra's own release.
- **[MPC-HC](https://github.com/clsid2/mpc-hc)**, **[MPC Video Renderer](https://github.com/Aleksoid1978/VideoRenderer)**,
  **[LAV Filters](https://github.com/Nevcairiel/LAVFilters)**, **[mpv](https://mpv.io)**, **[VLC](https://www.videolan.org)**,
  **[Kodi](https://kodi.tv)** and **[VapourSynth](https://www.vapoursynth.com)**: the players and tools PushMPC plugs
  into.
- **[FFmpeg](https://ffmpeg.org)**, **[ONNX Runtime](https://onnxruntime.ai)**, **[ncnn](https://github.com/Tencent/ncnn)**,
  the RIFE models, **NVIDIA Image Scaling** and **AMD FidelityFX**.

PushMPC was written with the help of AI (Claude, by Anthropic).

## Licence

PushMPC is free software under the [GPL-3.0-or-later](LICENSE). The NVIDIA and AMD runtimes keep their own licences:
PushMPC doesn't ship them, Setup downloads them. Details: [licences](docs/licences.md).

---

Developers: see [building PushMPC](docs/building.md) for how to build it, how Setup works and how releases are made.
