# PushMPC user guide

This guide goes a step further than the [README](../README.md): what Setup does, how each player is set up, what every
setting means, and what to do when something doesn't look right.

- [Installing on Windows](#installing-on-windows)
- [Installing on Linux](#installing-on-linux)
- [Installing on a Mac](#installing-on-a-mac)
- [Players](#players)
- [Capture mode](#capture-mode)
- [Graphics cards](#graphics-cards)
- [Settings](#settings)
- [HDR, scene cuts and G-SYNC](#hdr-scene-cuts-and-g-sync)
- [Updates](#updates)
- [Repair and uninstall](#repair-and-uninstall)
- [Troubleshooting](#troubleshooting)

## Installing on Windows

Download **PushMPC-Setup.exe** from the [latest release](https://github.com/CptPakundo/PushMPC/releases/latest) and run
it. It installs for your Windows account only, without administrator rights.

Setup isn't code-signed yet, so Windows may show "Windows protected your PC" the first time: click **More info**, then
**Run anyway**.

The first page lists the players Setup found and how it will handle each one. A few boxes let you choose, depending on
your PC:

| Box | When you see it | What it does |
|---|---|---|
| Download faster AI frame generation | NVIDIA RTX, GTX 16 and Intel cards | a faster version of PushFlow for your card (about 220-255 MB) |
| Download AMD's newest FSR upscaling | Radeon RX 6000, 7000 and 9000 | FSR 4.1 from AMD |
| Give Plex smoother motion | Plex is installed | sets up Plex (see [Plex](#plex)); Windows asks for permission once |
| Also install PushMPC's own mpv player | always (off by default) | a ready-made mpv with PushMPC built in; Start menu: **PushMPC mpv** |
| Check for updates automatically | always | at most once a day, in the background |

Then Setup downloads what your graphics card needs (about 750 MB the first time). The download shows its progress, can
be cancelled, and picks up where it stopped when you run Setup again. If PushFPS is already installed, Setup reuses its
files instead.

If MPC-HC, MPC-BE or PotPlayer is open, Setup offers to close it first, because they save their settings when they
close. MPC-HC must have been started and closed once before, so it has settings PushMPC can be added to.

**Installed a new player later?** Run Setup again (or Settings > Apps > PushMPC > Modify > Repair) and it sets that one
up too. Repair keeps the choices you made the first time.

## Installing on Linux

Download `pushmpc-linux-x86_64-<version>.tar.gz` (or `aarch64`) from the
[latest release](https://github.com/CptPakundo/PushMPC/releases/latest), unpack it and run:

```sh
sh setup.sh
```

Setup installs for your user only, never touches anything outside your home folder and doesn't need `sudo`. It finds
your graphics card and your players and sets each one up. It only asks when a download needs your yes:

- **mpv from Flathub**, when your distribution's mpv can't run PushMPC (Ubuntu and Debian);
- **NVIDIA's video effects**, on NVIDIA cards, because NVIDIA's licence has to be accepted.

`sh setup.sh --yes` answers yes to both ahead of time. Other options: `--no-pmml` skips the PushFlow download, `plan`
shows what Setup would choose without changing anything, and `status` shows what is set up.

What Setup sets up:

- mpv (your distribution's or Flathub's), Celluloid, Haruna, SMPlayer, MPC-QT, Jellyfin Desktop and the Jellyfin and
  Plex MPV Shims;
- VLC 3 (also from Flathub or Snap) and the Kodi add-on, in the x86_64 download;
- Plex and Plex HTPC from Flathub (the Snap version of Plex can't be set up and is left alone);
- PushFlow for your card: NVIDIA RTX, Intel, and AMD (with or without ROCm).

To remove everything: `sh ~/.local/share/pushmpc/setup.sh uninstall`. To update: download the new release and run
`sh setup.sh` again.

PushMPC's settings on Linux live in `~/.config/pushmpc/settings.ini`, with the same names as the [settings](#settings)
below, for example `Smoother=Auto` (or `Off`, or `2` to `8`).

## Installing on a Mac

PushMPC on the Mac is a preview. It works in mpv and in Plex (Plex for Mac and Plex HTPC). You need
[Homebrew](https://brew.sh). Open Terminal and run:

```sh
brew tap CptPakundo/pushmpc https://github.com/CptPakundo/PushMPC
brew install CptPakundo/pushmpc/pushmpc
pushmpc-setup install
```

`pushmpc-setup install` adds PushMPC to mpv. If Plex for Mac or Plex HTPC is in your Applications folder, it adds
PushMPC to Plex's own player too, checks with a short test picture that it works, and keeps a copy of what it changed.
When Plex updates itself, PushMPC sets itself up again by itself.

- If macOS says Terminal isn't allowed to change Plex, open **System Settings > Privacy & Security > App Management**,
  turn Terminal on, and run `pushmpc-setup install` again.
- On a Mac with macOS 15.4 or newer, PushMPC uses Apple's own frame interpolation when your Mac can run it. Otherwise
  it uses the graphics chip, and if that isn't possible, the processor.
- To remove everything: `pushmpc-setup uninstall`, then `brew uninstall pushmpc`. Plex and your mpv settings go back
  exactly as they were.
- To update: `brew upgrade pushmpc`, then `pushmpc-setup install` again.

IINA and VLC on the Mac are coming later.

## Players

PushMPC needs nothing from you in most players: Setup sets them up and Auto does the rest.

| Player | Windows | Linux | Mac | Notes |
|---|:-:|:-:|:-:|---|
| MPC-HC, MPC-BE | ✅ | – | – | the most complete: settings page and on-screen info |
| PotPlayer | ✅ | – | – | needs MPC Video Renderer, see [below](#potplayer) |
| mpv, mpv.net, SMPlayer, MPC-QT | ✅ | ✅ | 🧪 mpv | see [mpv and mpv-based players](players-mpv.md) for details |
| PushMPC's own mpv | ✅ | – | – | optional box in Setup; on Linux, your distribution's mpv gets the same built-in route |
| Celluloid, Haruna | – | ✅ | – | |
| VLC 3 | ✅ | ✅ | 🔜 | see [VLC](#vlc) |
| Kodi 21 and 22 | ✅ | ✅ | – | see [Kodi](#kodi) |
| Plex for Windows, Plex HTPC, Plex for Mac | ✅ | ✅ | 🧪 | on Linux from Flathub; see [Plex](#plex) |
| Jellyfin Desktop, Jellyfin MPV Shim, Plex MPV Shim | ✅ | ✅ | – | the shims play through mpv |
| Stremio 5.0.28 and newer | ✅ | – | – | older versions: [capture mode](#capture-mode) |
| KMPlayer, GOM Player, Emby Theater, other players | ✅ | – | – | [capture mode](#capture-mode) |
| Zoom Player | by hand | – | – | see [below](#zoom-player-by-hand) |
| SVP's own mpv | – | – | – | left alone: SVP manages it |

### MPC-HC and MPC-BE

Setup adds PushMPC to the player and sets up MPC Video Renderer's HDR options; in MPC-BE it also switches the video
decoder to **D3D11 copy-back**, which PushMPC needs. Everything is put back when you uninstall.

PushMPC works with MPC Video Renderer (the default in both players). With another renderer, the video plays as
before, without PushMPC.

If MPC-BE keeps its settings in an INI file instead of the registry, Setup leaves it alone and says so. Add PushMPC by
hand: Options > External Filters > Add Filter > **PushMPC Filter**, set to Prefer; and Options > Internal Filters > MPC
Video Decoder > hardware decoding: **D3D11 copy-back**. 32-bit MPC-BE can't load PushMPC.

### PotPlayer

Setup adds PushMPC to PotPlayer's filter list. If LAV Filters is installed, it also adds LAV Video Decoder, which passes
HDR information on to PushMPC. Close PotPlayer before running Setup.

Three choices stay yours, because they change how PotPlayer plays everything:

1. **Renderer.** Preferences (F5) > Video > Video Renderer: **MPC Video Renderer**. PotPlayer doesn't include it:
   install it (for example the copy that comes with MPC-HC). With PotPlayer's own renderers or madVR, PushMPC stays out
   of the way.
2. **Decoder.** Preferences > Filter Control > Video Decoder: the built-in decoder with **D3D11 (or DXVA2) copy-back**,
   or LAV Video in a copy-back mode.
3. **PotPlayer's own frame interpolation:** turn it off, so the two don't stack.

If PotPlayer keeps its settings in an INI file, add PushMPC yourself: Preferences > Filter Control > Filter Priority
(Overall) > Add external filter, pick `%LOCALAPPDATA%\PushMPC\filter\PushMpcFilter.ax`, set to **Prefer**.

### VLC

Setup adds PushMPC to VLC 3 and checks that it loads. Its settings are under **Tools > Preferences**, *Show settings:
All*, **Video > Filters > PushMPC**. On Linux, Setup also tests VLC's hardware decoding with PushMPC and keeps the
fastest setting that works.

### Kodi

On Windows and Linux, Setup installs the **PushMPC** add-on into Kodi 21 or 22 and switches it on. It plays your local
video files with smoother motion, while audio, subtitles, chapters and seeking stay Kodi's. Its settings are in Kodi
under Add-ons > My add-ons > VideoPlayer InputStream > PushMPC. Without the add-on, Kodi can still use mpv as its external player, and Setup sets
that up when mpv is installed.

### Plex

Plex for Windows and Plex HTPC get smoother motion too. Setup adds PushMPC to Plex's own player and plays a short test
picture through Plex to check that it works. Plex's files are backed up first and put back exactly as they were when
you uninstall. Because Plex is installed for all users, Windows asks for permission once.

- If the test fails, Setup takes its files out again and tries a second way: it swaps in a player library that PushMPC
  can work with (on Plex versions it knows). It never does both.
- When Plex updates itself, PushMPC sets it up again at your next sign-in.
- Don't want this? Untick the Plex box on Setup's first page.
- On Linux, `setup.sh` does the same for Plex and Plex HTPC from Flathub, without changing anything inside Plex.
- On a Mac, `pushmpc-setup install` does the same for Plex for Mac and Plex HTPC (preview), see
  [Installing on a Mac](#installing-on-a-mac).

### Stremio, Jellyfin and the MPV Shims

Stremio 5.0.28 and newer reads an mpv settings file, which Setup fills in. Jellyfin Desktop is set up the same way. The
Jellyfin and Plex MPV Shims play through your installed mpv, so they need mpv (Setup tells you if it's missing).

### Zoom Player (by hand)

Not tested. Zoom Player works like MPC-HC, so the same filter can work:

1. Install PushMPC with Setup.
2. Options (Ctrl+O) > Advanced Options > Playback > Filter Control: add **PushMPC Filter** as a preferred video filter.
3. Use MPC Video Renderer, and LAV Video or Zoom Player's decoder in a copy-back mode.

Remove the filter there again when you uninstall PushMPC.

## Capture mode

Capture mode is for players PushMPC can't plug into, such as KMPlayer, GOM Player or Emby Theater. Setup turns it on
only if you have one of those players, and adds **PushMPC Capture** to the Start menu.

1. Start your player and play a video.
2. Open **PushMPC Capture** from the Start menu.
3. A second window shows the video with smoother motion. Put it fullscreen.

Good to know:

- The first time, PushMPC Capture asks to download PushFPS (about 130 MB), which does the capturing. It goes into
  PushMPC's own folder; a PushFPS you already have isn't touched.
- The picture comes about 1/20 of a second after the player's sound. If the sound seems early, set your player's audio
  delay to about +80 ms (KMPlayer and GOM: audio settings; VLC: Tools > Track Synchronization). For Plex, PushMPC does
  this for you.
- Copy-protected video (Netflix and similar) stays black.
- A player PushMPC plugs into directly always looks better than capture mode.

### Capture mode on Linux and Mac

A built-in capture mode for Linux and Mac is planned. Until then, OBS Studio can turn your player's window into a
virtual camera that PushFPS can show:

1. Install OBS Studio. On Linux also install `v4l2loopback` (for example your distribution's `v4l2loopback-dkms`). On a
   Mac, allow OBS's camera extension when OBS asks.
2. In OBS, add a source for the player: *Window Capture (PipeWire)* on Linux, *macOS Screen Capture* on a Mac. Set the
   canvas to the video's size and frame rate.
3. Click *Start Virtual Camera*.
4. Install [PushFPS](https://github.com/DosukaSOL/PushFPS), choose *OBS Virtual Camera* as the capture card, and turn on
   frame generation.

## Graphics cards

Auto tries the methods in this order and uses the best one your card keeps up with:

| Graphics card | Smoother motion | Sharper picture | Clean-up |
|---|---|---|---|
| GeForce RTX 50 / 40 | NVIDIA frame generation, then DLSS frame generation, PushFlow, FSR | RTX Video Super Resolution | yes |
| GeForce RTX 30 | NVIDIA frame generation, then DLSS frame generation, PushFlow, FSR | RTX Video Super Resolution | when there's time |
| GeForce RTX 20 | DLSS frame generation, then PushFlow, NVIDIA optical flow, FSR | RTX Video Super Resolution | when there's time |
| GeForce GTX 16 / 10 | PushFlow, then NVIDIA optical flow (GTX 16), FSR | RTX Video Super Resolution on GTX 16, else NIS | – |
| Intel | PushFlow, then FSR | NIS | – |
| Radeon RX 9000 | PushFlow, then FSR 4, AMD Fluid Motion | FSR | yes |
| Radeon RX 5000-7000 | PushFlow, then FSR 3.1, AMD Fluid Motion | FSR | RX 7000, when there's time |

SDR to HDR works on every card with PushMPC's own curve; RTX cards can also use RTX Video HDR.

On Linux, NVIDIA RTX cards use NVIDIA's frame generation and Super Resolution (with NVIDIA's own driver), and other
cards use PushFlow or PushMPC's own engine with NIS or FSR upscaling.

## Settings

In MPC-HC and MPC-BE: **Play > Filters > PushMPC Filter**. The "Now:" lines show what Auto chose and why it stopped
where it did. Changes apply within a couple of seconds, without restarting the video. On Windows, mpv-based players use the same settings.

| Setting | Choices | What Auto does |
|---|---|---|
| Enable PushMPC | on / off | |
| Smoother | Auto, Off, 2x-16x | the highest multiplier your card keeps up with and your display can show. On a fixed-refresh display, an even rhythm wins a tie |
| Sharper | Auto, Off, level 1-4 | upscales to your screen size with the time left over |
| Clean up | Auto, Off, On | AI clean-up for low-quality video, when your card has time for it |
| SDR to HDR | Auto, Off, On | when Windows HDR is on and the video is SDR, shows it as HDR |
| HDR boost | Auto, Off, Low, Medium, High | how bright SDR highlights get. Off keeps SDR exactly as it is; Low, Medium and High lift highlights up to 2x, 3.2x or 5x, never above what your display can show. Auto: High for clean sources, Medium for web video, Low for low-quality video |

**Advanced** (tick *Show advanced settings*):

| Setting | Choices | What it does |
|---|---|---|
| FG quality | Auto, Low, Medium, High | frame generation quality. Auto uses High only where it fits with time to spare |
| FG method | Auto, NVIDIA VFG, DLSS-G, PushFlow, FSR, AMD FRC | the frame generation method; methods your card can't run are marked |
| Refresh (VRR) | Auto, Assume VRR, Assume fixed | whether your display runs G-SYNC or FreeSync for the player. Auto reads it from the driver |
| Upscaler | Auto, RTX VSR, FSR, NIS | Auto: RTX VSR on NVIDIA RTX, otherwise FSR |
| HDR method | Auto (PushMPC curve), RTX Video HDR | how SDR becomes HDR; RTX Video HDR needs an RTX card |
| Limit brightness on bright scenes | on / off | on (default): very bright scenes are calmed slowly so your display can hold them |
| Display peak | Auto, 400, 600, 1000, or a number | your display's peak brightness. Auto uses what Windows reports. On OLED monitors in "True Black 400" mode, choose 400 |
| Upscale to | Screen, 1440p, 4K, 5K | the target size |
| Upscale style | Auto, Standard, Keep detail, Clean hard | the look of RTX Video Super Resolution; Auto picks by the video's quality |
| Clean-up strength | Subtle, Balanced, Strong | how strong the clean-up is |
| Deband | Auto, Off, On | smooths banding in skies and gradients |
| Deband strength | Low, Medium, High | how strong the smoothing is |
| Subtitle hold | Auto (on text), Off, On | keeps subtitles that are burned into the picture steady, so they don't warp. Normal subtitles never need it |
| Animation | Auto, Off, On | cartoons and anime are often drawn for every second or third frame. PushMPC finds the repeated drawings and spreads the new frames evenly between the drawings, so motion glides instead of stepping. Auto also tunes the picture for cartoons when it finds one. Off plays every frame as before |
| On-screen status | Brief at start, Always, Off | the info line in the top-left corner: for 3 seconds at the start, always, or never |
| On battery | Light, Same as plugged in | on a laptop's battery, Light uses less power: up to 2x, lighter upscaling, no clean-up |

Also on the page: **Turn on G-SYNC for MPC-HC** (see below), **Import my PushFPS settings** when PushFPS is installed,
and **Write a frame log** (only turn it on when asked for one in a bug report).

## HDR, scene cuts and G-SYNC

**HDR video on an HDR display.** HDR10, HDR10+, HLG and Dolby Vision (profiles 5, 7 and 8) pass through untouched. Setup
sets MPC Video Renderer so that only content brighter than your display is toned down, and puts your old settings back
when you uninstall.

**HDR video on an SDR display.** MPC Video Renderer converts it to fit your screen. With HDR10+ and Dolby Vision, it
follows each scene's brightness.

**SDR to HDR.** With Windows HDR on, SDR video can be shown as HDR. PushMPC keeps shadows and mid-tones where they are
and only lifts the highlights, as far as your display allows.

**Scene cuts.** At a hard cut, frame generation would blend two shots into a smear. PushMPC spots cuts and lets the new
shot start exactly on its own frame. Fast pans, zooms, flashes and fades are left alone. When a film switches between
two picture shapes (for example a taller picture in some scenes), that change is treated like a cut too. Large effects
like fire or energy beams can still look a little torn with every method.

**G-SYNC and FreeSync.** Auto reads whether variable refresh is on for the player and picks the frame rate to match. If
your NVIDIA display supports G-SYNC but it's off for MPC-HC, the settings page offers **Turn on G-SYNC for MPC-HC**. It
only changes MPC-HC's own profile in the NVIDIA driver, keeps a backup, and uninstalling puts it back. On AMD, FreeSync
switched on in AMD Software with the player fullscreen counts as variable refresh.

**Picture and sound stay in sync.** Every frame, real or generated, keeps its own timestamp. After a seek, the first
generated frame appears in about 1/20 of a second.

## Updates

On Windows, the **About** tab of the settings page shows the update status. **Check for updates** checks now;
**Automatic updates** lets PushMPC check at most once a day during playback (it never delays the start of a video);
**Channel** picks Stable or Beta.

Updates download only what changed, are checked against the release's signature, and install when your player is
closed (at the latest at your next sign-in). The previous version is kept: if a new one fails to load, PushMPC goes back
to it by itself.

You can also update by running a newer PushMPC-Setup.exe, or with Settings > Apps > PushMPC > Modify > Update. On
Linux, download the new release and run `sh setup.sh` again.

## Repair and uninstall

- **Repair, update or uninstall on Windows:** Settings > Apps > Installed apps > PushMPC > Modify, or Uninstall.
  Uninstalling removes PushMPC and puts back what it changed: the players' settings, MPC Video Renderer's HDR settings,
  SVP's filter (if PushMPC had switched it off), MPC-HC's G-SYNC setting, and Plex's files. Your logs stay in
  `%LOCALAPPDATA%\PushMPC\logs`.
- **Uninstall on Linux:** `sh ~/.local/share/pushmpc/setup.sh uninstall`.

## Troubleshooting

**The video plays, but nothing changes.** The settings page's "Now:" lines and status say why PushMPC is resting. The
usual reasons:

- another video renderer than MPC Video Renderer is in use;
- the decoder is in a "native" hardware mode: choose a copy-back mode;
- the video is interlaced (MPC Video Renderer handles it; PushMPC steps aside);
- the player was installed after PushMPC: run Setup again.

If a frame generation method doesn't start on your card, PushMPC moves on to the next one and says so.

**Copy diagnostics.** About > Copy diagnostics puts a report (versions, graphics card, displays, settings, recent log)
on the clipboard and saves it in the logs folder. Your user name, folders and video file names are removed. Paste it
into a [bug report](https://github.com/CptPakundo/PushMPC/issues).

**Logs.** Windows: `%LOCALAPPDATA%\PushMPC\logs` (Setup's log is `setup.log` there). Linux: `~/.local/state/pushmpc/logs`, and Setup's notes in
`~/.local/share/pushmpc`.

**Stuttering.** Check that your display runs at its full refresh rate, and close other heavy GPU programs. Auto steps
down when your card can't keep up, so a lower multiplier than you expected usually means it's working as intended.
The "Now:" line says where it stopped.

**SmartScreen.** "Windows protected your PC" appears because Setup isn't code-signed yet. Click **More info**, then
**Run anyway**.
