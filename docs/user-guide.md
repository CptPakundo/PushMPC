# PushMPC user guide

## Install

Download **PushMPC-Setup.exe** from the [releases](https://github.com/CptPakundo/PushMPC/releases) and run it. It installs
for your Windows account only, without administrator rights: welcome, licence and credits, then the installation with a
progress bar. The welcome page lists the supported players and the ones it found on this PC. If MPC-HC, MPC-BE or
PotPlayer is running, setup offers to close it (they save their settings when they close).

Setup asks nothing else. It:
- sets up every player it finds (see the table below);
- picks the best settings for your graphics card (Auto keeps adjusting them while you watch);
- downloads a faster version of PushFlow, PushMPC's AI frame generation, for NVIDIA RTX and Intel graphics cards
  (about 220-255 MB; you can untick it on the first page);
- downloads AMD's newest FSR upscaling for Radeon RX 6000, 7000 and 9000 cards (you can untick it too);
- gives Plex smoother motion if you have Plex (see [Plex](#plex));
- turns on [capture mode](#capture-mode-fallback) only if you have a player PushMPC can't plug into.
Setup is not code-signed yet, so Windows SmartScreen may show "Windows protected your PC" the first time: click
**More info**, then **Run anyway**. The download of Veyra's runtime package (about 750 MB) shows its progress, can be
cancelled, and continues where it stopped when you run setup again; it is checked against its pinned SHA-256.

Setup runs the same steps as the script install below, which still works for a release zip:
`Install-PushMPC.cmd` (or `powershell -ExecutionPolicy Bypass -File pushmpc-install.ps1 install`). Setup options for
scripted installs: `PushMPC-Setup.exe --quiet` (no window), `--install-dir <folder>`, `--package-cache <folder>` (a
folder that already holds Veyra's zip), `--no-existing` (never reuse an installed PushFPS or Veyra).

The installer picks Veyra's runtime package for your graphics card (`-Gpu Auto|NVIDIA|AMD`; Auto: NVIDIA when an NVIDIA
card is present, else the AMD package, which also serves Intel). It takes the runtime files, in this order, from:

1. an installed PushFPS or Veyra with exactly the pinned files (every file's SHA-256 is checked; copied, or hard-linked
   with `-LinkExisting`; `-NoExisting` skips this);
2. the package zip in `-RuntimePackage <zip>`, `-PackageCache <folder>` or its own download folder;
3. a download from Veyra's GitHub release (about 750 MB), checked against the pinned SHA-256.

PushFlow (PushFPS's own frame generation) is not in Veyra's packages: it comes from an installed PushFPS, a PushFPS
release zip in `-PackageCache`, or PushFPS's GitHub release, every file checked. See [release model](release-model.md).

Everything goes into `%LOCALAPPDATA%\PushMPC`, per user, without administrator rights:

| Folder | What |
|---|---|
| `filter` | `PushMpcFilter.ax`, `PushMpcExit.dll`, the installer script, the updater |
| (top level) | `PushMpcUpdater.exe` (updates), `PushMpcSetup.exe` (repair, update, uninstall from Windows Apps) |
| `core` | `pushcore.dll`, its shaders, `pushcore_hip_preflight.exe` (AMD) |
| `runtime` | FFmpeg, the NVIDIA or AMD components and PushFlow |
| `vsplugin`, `vapoursynth` | the mpv preview: the VapourSynth filter and its runtime (only when mpv or SMPlayer was found) |
| `previous` | the version before the last update (rollback) |
| `logs` | logs |

The installer registers the filter for your user, adds "PushMPC Filter" to MPC-HC's External Filters (Prefer) and
disables SVP's VapourSynth filter if it is enabled (SVP's interpolation and PushMPC's frame generation would stack;
uninstall enables it again). Community unlocks and ports in the package (the SF AI enhancement build for RTX 20-50,
the DLSS-G unlocks for RTX 30/40, the AMD AI enhancement runtimes) are installed by default (`-ExperimentalExtras No`
leaves them out).

## Players

| Player | Support | What setup does |
|---|---|---|
| MPC-HC (x64) | full, tested | adds "PushMPC Filter" to External Filters (Prefer); MPC Video Renderer HDR options |
| MPC-BE (x64) | automatic; setup's changes checked against MPC-BE 1.9.3 in CI, playback not yet tested in MPC-BE | the same, plus MPC Video Decoder set to D3D11 copy-back |
| PotPlayer (x64) | automatic; checked in CI with a real PotPlayer (it loads the filter), playback with a GPU not yet tested | adds "PushMPC Filter" to its filter priority list (Prefer); renderer and decoder are your choice (see below) |
| mpv, mpv.net, SMPlayer, MPC-QT | **preview**, optional component; not yet tested with a real GPU | when found: the VapourSynth filter in their settings, plus a VapourSynth runtime; see [mpv and mpv-based players](players-mpv.md) |
| Jellyfin Desktop | preview | the VapourSynth filter in each profile's `mpv.conf` |
| Kodi, Jellyfin MPV Shim, Plex-MPV-Shim | preview, when mpv is installed | they play through mpv (Kodi's external player; the shims' external mpv) |
| Stremio 5.0.28 and newer | automatic | its "mpv config file" setting; [details](players-mpv.md#stremio) |
| Plex for Windows, Plex HTPC | preview | smoother motion on tested Plex versions ([details](#plex)); other versions: capture mode |
| VLC 3 | preview | adds PushMPC to VLC |
| KMPlayer, GOM Player, Emby Theater, other players | fallback | [capture mode](#capture-mode-fallback) |
| Zoom Player | by hand, untested | nothing ([steps](#zoom-player-by-hand)) |
| SVP's own mpv | | left alone (SVP manages it) |

PushMPC needs MPC Video Renderer in D3D11 mode after it, and a decoder before it that delivers its frames in system
memory (copy-back). With another renderer, or a decoder in native hardware mode, the player still plays, without
PushMPC.

### MPC-BE

Setup finds MPC-BE x64 (its registry install path, its uninstall entry, or `Program Files\MPC-BE x64`). When it is
there and keeps its settings in the registry, setup:

- adds `HKCU\Software\MPC-BE\ExternalFilters\NNN` "PushMPC Filter" (Prefer, NV12 and P010 inputs);
- sets MPC Video Decoder's hardware decoding to **D3D11 copy-back** (`HKCU\Software\MPC-BE\Filters\MPC Video Decoder`,
  `HwDecoder` = 2). Its default, D3D11 native, cannot feed a filter between the decoder and the renderer (MPC-BE would
  then decode in software). The previous value is kept and put back on uninstall, unless you changed it since;
- disables SVP's VapourSynth entry in MPC-BE if it is enabled (enabled again on uninstall).

Without MPC-BE nothing is written for it. MPC-BE's default renderer is MPC Video Renderer, which PushMPC needs. If
MPC-BE keeps its settings in `mpc-be64.ini` (INI mode), setup changes nothing and says so; add the filter by hand:
Options > External Filters > Add Filter > "PushMPC Filter", Prefer; and Options > Internal Filters > MPC Video Decoder
> hardware decoding: D3D11 copy-back. 32-bit MPC-BE cannot load the 64-bit filter. MPC-HC's and MPC-BE's MPC Video
Renderer share one registration and one settings key: the copy installed last is the one both players use.

### PotPlayer

Setup finds PotPlayer x64 (`Program Files\DAUM\PotPlayer`) and adds "PushMPC Filter" to its filter priority list
(`HKCU\Software\DAUM\PotPlayerMini64\Override`, Prefer, NV12 and P010 inputs), in front of the list's end marker. A
real PotPlayer on a CI runner loads the filter from that entry. SVP's VapourSynth entry, if enabled, is disabled (and
enabled again on uninstall). Uninstalling removes the entry and leaves the list exactly as it was. Close PotPlayer
before setup: it saves its settings when it closes. If PotPlayer keeps its settings in `PotPlayerMini64.ini` (INI mode),
setup changes nothing and says so: add the filter by hand (step 1 below).

When LAV Filters is installed, setup also adds **LAV Video Decoder** to the same list (Prefer, after PushMPC), so
PotPlayer decodes with LAV, which passes HDR mastering data and MaxCLL on to PushMPC (PotPlayer's built-in decoder
does not; PushMPC still sees PQ/HLG and BT.2020 from the stream itself). A LAV entry of your own is left as it is.
Without LAV Filters, setup says so; install it from github.com/Nevcairiel/LAVFilters and run setup again.

The renderer stays your choice, because it changes how PotPlayer plays everything else too:

1. **Filter (INI mode only).** Preferences (F5) > Filter Control > Filter Priority (Overall) > Add external filter,
   pick `%LOCALAPPDATA%\PushMPC\filter\PushMpcFilter.ax`, and set it to **Prefer**.
2. **Renderer.** Preferences > Video > Video Renderer: **MPC Video Renderer**. PotPlayer does not bundle it: install
   MPC Video Renderer (for example the copy that comes with MPC-HC or MPC-BE) and select it. With PotPlayer's built-in
   Direct3D 11 renderer, EVR or madVR, PushMPC passes the video through unchanged.
3. **Decoder.** Preferences > Filter Control > Video Decoder: the built-in decoder with **D3D11 (or DXVA2) copy-back**,
   or LAV Video in a copy-back hardware mode. Native DXVA / D3D11 decoding leaves no room for a filter between the
   decoder and the renderer.
4. Turn off PotPlayer's own frame interpolation, which would stack with PushMPC's frame generation.

Known limits: PushMPC cannot ask PotPlayer to reopen a file (an interlaced file or a lost graphics device then shows
the source frames unprocessed); whether PotPlayer's built-in decoder passes HDR10 metadata on to a filter is unknown;
the G-SYNC button covers MPC-HC and MPC-BE only.

### Zoom Player (by hand)

**Untested.** Zoom Player (MAX or Free, 64-bit) is a DirectShow player like MPC-HC, so the same filter works when MPC
Video Renderer draws the video and the decoder copies back:

1. Install PushMPC with setup (it registers the filter for your user).
2. Options (Ctrl+O) > Advanced Options > Playback > Filter Control: add "PushMPC Filter" as a custom (preferred) video
   filter.
3. Video renderer: MPC Video Renderer (install it, for example the copy that comes with MPC-HC). With other renderers
   PushMPC passes the video through.
4. Decoder: LAV Video or Zoom Player's decoder in a copy-back hardware mode (or software).

Remove the filter there again when you uninstall PushMPC.

### Plex

Plex for Windows and Plex HTPC get smoother motion too. To make that work, Setup replaces one file in Plex's folder with
a version that PushMPC can work with. It keeps Plex's own file and puts it back when you uninstall PushMPC. Because
Plex is installed for all users, Windows asks for administrator permission once.

- This is only done for Plex versions we have tested. With any other version, Plex keeps its own file and you can use
  [capture mode](#capture-mode-fallback) instead.
- When Plex updates itself, PushMPC checks at your next sign-in and sets it up again if the new version is a tested one.
- Don't want this? Untick the Plex box on Setup's first page.

### Capture mode (fallback)

Capture mode is for players PushMPC can't plug into, such as KMPlayer, GOM Player or Emby Theater. Setup turns it on
only if you have one of those players.

1. Start your player and play a video.
2. Open **PushMPC Capture** from the Start menu.
3. A second window shows the video with smoother motion. Put it fullscreen.

Good to know:
- The first time, PushMPC Capture asks to download PushFPS (about 130 MB), which does the capturing. It goes into
  PushMPC's own folder; if you already have PushFPS, yours is not touched.
- The picture comes about 1/20 of a second later than the player's sound. If the sound seems early, set your player's
  audio delay to about +80 ms (KMPlayer and GOM: audio settings; VLC: Tools > Track Synchronization). For Plex,
  PushMPC does this for you while capture mode runs.
- Copy-protected video (Netflix and similar) stays black.
- A player PushMPC plugs into directly (MPC-HC, mpv, VLC, Kodi, ...) always looks better than capture mode.
### Capture on Linux and macOS (workaround)

On Linux and Mac, PushFPS can't capture a window, but it can show a camera. OBS Studio can turn your player's window into
a virtual camera. The picture is a bit simpler than on Windows; a built-in way is planned.

1. Install OBS Studio. Linux: also `v4l2loopback` (OBS's virtual camera needs it; your distribution's package, e.g.
   `v4l2loopback-dkms`). macOS 13 and newer: allow OBS's camera extension when OBS asks.
2. In OBS, add a source for the player: *Window Capture (PipeWire)* or *Screen Capture (PipeWire)* on Linux, *macOS
   Screen Capture* (window) on macOS. Set the canvas to the video's size and frame rate.
3. Click *Start Virtual Camera*.
4. Install PushFPS (github.com/DosukaSOL/PushFPS), choose *OBS Virtual Camera* as the capture card, and turn on frame
   generation.

## GPUs

| GPU | Frame generation (Auto's order) | Upscaling | Clean up | SDR to HDR |
|---|---|---|---|---|
| RTX 50 / 40 | NVIDIA VFG, then DLSS-G, PushFlow, FSR | RTX VSR | yes | PushMPC's curve (RTX Video HDR selectable) |
| RTX 30 | NVIDIA VFG (RTX 30 port), then DLSS-G (unlocked), PushFlow, FSR | RTX VSR | when the GPU time fits | PushMPC's curve (RTX Video HDR selectable) |
| RTX 20 | DLSS-G, then PushFlow, NVIDIA optical flow, FSR | RTX VSR | when it fits | PushMPC's curve (RTX Video HDR selectable) |
| GTX 16 / 10 | PushFlow, then NVIDIA optical flow (GTX 16), FSR | RTX VSR on GTX 16 (unlock, after its self-test), else NIS | - | PushMPC's curve |
| Intel | PushFlow, then FSR | NIS | - | PushMPC's curve |
| Radeon RX 9000 | PushFlow, then FSR 4, AMD FRC | FSR | yes (lmxxf runtime) | PushMPC's curve |
| Radeon RX 5000-7000 | PushFlow, then FSR 3.1, AMD FRC | FSR | RX 7000 (worker process), when it fits | PushMPC's curve |

## Settings

MPC-HC: Play > Filters > PushMPC Filter (or Options > External Filters > PushMPC Filter > Properties).

| Setting | Choices | Auto does |
|---|---|---|
| Enable PushMPC | on / off | |
| Smoother | Auto, Off, 2x-8x | frame generation: the highest multiplier your GPU keeps up with (at p95) and your display can show; on a fixed-refresh display an even cadence wins a tie, but never at the cost of a much lower multiplier (any multiplier with G-SYNC / VRR active) |
| Sharper | Auto, Off, level 1-4 | the upscaler to your screen size, with the GPU time left over |
| Clean up | Auto, Off, On | AI picture enhancement for low-bitrate video, where the GPU has a runtime for it and time to spare |
| SDR to HDR | Auto, Off, On | when Windows HDR is on for the display and the video is SDR: shown as HDR10 with PushMPC's curve (every GPU) |
| HDR boost | Auto, Off, Low, Medium, High | how far SDR highlights are expanded. Off: SDR exactly as it is, at 203 cd/m2 white. Low / Medium / High: highlights up to 2x / 3.2x / 5x of SDR white, never above the display's reported peak; shadows and mid-tones stay where SDR has them. Auto: High for clean sources, Medium for typical web video, Low for low-bitrate video. A slow limiter keeps large bright areas within what the display can hold. |

Advanced:

| Setting | Choices | What it does |
|---|---|---|
| FG quality | Auto, Low, Medium, High | Auto: Medium for the multiplier, High only where it fits at the same multiplier with 3 ms to spare. For PushFlow Low / Medium / High are Fast / Balanced / Quality. |
| FG method | Auto, NVIDIA VFG, DLSS-G, PushFlow, FSR, AMD FRC | the frame generation method; a method this GPU cannot run is marked and the note says why |
| Refresh (VRR) | Auto, Assume VRR, Assume fixed | Auto predicts G-SYNC / VRR for MPC-HC's window from the NVIDIA driver profile (G-SYNC selected for the player and its windowed / fullscreen mode); the driver's display-wide "running" flag only confirms it. On AMD it reads FreeSync from the AMD driver: FreeSync switched on in AMD Software and the player fullscreen counts as VRR. Without an answer it assumes a fixed refresh and says so |
| Upscaler | Auto, RTX VSR, FSR, NIS | Auto: RTX VSR on NVIDIA RTX, else FSR |
| HDR method | Auto (PushMPC curve), RTX Video HDR | the SDR to HDR method; RTX Video HDR needs NVIDIA RTX, and HDR boost then sets its peak (500 / 700 / 1000 cd/m2) |
| Limit brightness on bright scenes | on / off | on (default): when a scene is bright over large areas, the highlight expansion is reduced slowly so the frame average stays within what the display holds over its full area (with RTX Video HDR, the light above paper white of its output is scaled the same way); off: the full expansion always |
| Display peak | Auto, 400, 600, 1000, or type a number (cd/m2) | the peak brightness of your display. Auto uses what Windows reports. Some OLED monitors offer a bright mode (for example "Console HDR", about 1000 cd/m2 on small areas) and a "True Black 400" mode (about 450) but report the same peak in both: in the True Black mode choose 400. Used by SDR to HDR, the bright-scene limiter, the HDR metadata and MPC Video Renderer's tone mapping. |
| Upscale to | Screen, 1440p, 4K, 5K | the target size |
| Upscale style | Auto, Standard, Keep detail, Clean hard | the RTX VSR model family; Auto picks it by the video's bitrate (Clean hard for web video, Keep detail for Blu-ray-class bitrates, else Standard) |
| Clean-up strength | Subtle, Balanced, Strong | the AI enhancement look |
| Deband | Auto, Off, On | PushMPC's own deband pass on 8-bit video, before the upscaler |
| Deband strength | Low, Medium, High | how wide and how strong the smoothing is |
| Subtitle hold | Auto (on text), Off, On | PushFPS's Subtitle Hold for subtitles burned into the picture: text that stands still near the bottom while the picture moves is kept out of frame generation and laid back over the generated frames, so it does not warp or ghost. Only for burned-in text; subtitles the player draws (soft subtitles) are added after PushMPC and never need it. Under Auto it watches for burned-in text and holds it only while text is found (about 0.5 ms per frame at 4K) |

| On-screen status | Brief at start, Always, Off | shows what PushMPC is doing (for example "5x Medium · VSR 4 · 120 fps") in the top-left corner. Brief at start (default): for 3 seconds when playback starts or settings change; Always: kept on; Off: hidden. While it is visible it also appears in MPC-HC's Save Image screenshots. |
| On battery | Light, Same as plugged in | Light (default) uses a lighter profile on laptop battery: frame generation up to 2x, VSR up to level 2, clean up and deband off, SDR to HDR boost Low. Same as plugged in keeps the full profile. |

Also "Write a frame log" (off by default; turn it on only when you are asked for one to troubleshoot), and "Import my
PushFPS settings" when PushFPS is installed. PushMPC keeps the logs of the last 20 playbacks (at most 50 MB) and deletes older
ones.

**G-SYNC.** If your NVIDIA display supports G-SYNC but MPC-HC's driver profile does not have it turned on, Advanced
offers "Turn on G-SYNC for MPC-HC" (for windowed and full screen playback). It changes only MPC-HC's own profile in the NVIDIA
driver, keeps a backup, and the uninstaller puts your previous setting back.

**HDR video on an HDR display.** PushMPC passes HDR10, HDR10+, HLG and Dolby Vision (profiles 5, 7 and 8) to MPC Video
Renderer as HDR10. The installer, and PushMPC on its first HDR playback, set MPC Video Renderer's HDR options so that only
content brighter than your display is tone mapped: local tone mapping on, type Spline, and "display nits" = your display's
peak (Display peak above). The previous values are kept and the uninstaller restores them. To manage them yourself, set
`ManageMpcVr` = 0 (DWORD) under `HKCU\Software\PushMPC`. HLG video is converted to HDR10 with the BT.2100 reference
(1000 cd/m2), also for the first frames while PushMPC starts.

**HDR video on an SDR display.** MPC Video Renderer tone maps it. With HDR10+ or Dolby Vision, PushMPC passes each
scene's peak brightness as the frame's metadata, so MPC Video Renderer's "Metadata" tone mapping follows the scenes.

**Scene cuts.** At a hard cut between two shots, frame generation would blend them. PushMPC detects cuts on the GPU, for
every frame generation method, and shows the last frame of the old shot in the generated slots instead, so the new
shot starts exactly at its own frame. It looks only at the picture (letterbox and pillarbox bars are found and left
out), handles dark scenes, and catches cuts inside fast motion. Fast pans, zooms, flashes and fades are not treated as
cuts. Known limit: large non-rigid effects (fire, energy beams) can still look torn or ghosted with every method, and
a cut in the middle of a fast zoom of hand-drawn animation can be missed. Besides the brightness measures it
compares the frames after the best zoom and shift between them, their colours, the directions of their edges and how
well blocks of the new frame match the old one, so a zoom or a creature crossing the frame is not taken for a cut.
On NVIDIA RTX cards it also uses the GPU's optical flow engine (when the GPU has time for it, about 1 ms per
source frame): where the motion field is consistent in both directions, as in a flash or an effect over a moving scene,
the pair is not treated as a cut.

**Letterbox and aspect-ratio changes.** The bars are measured on every frame. When the picture switches between two
aspect ratios at once (for example films that open up to a taller ratio for some scenes), the pair is held like a cut,
because every frame generation method otherwise smears the bar into the picture. A letterbox that opens slowly is
followed frame by frame, a change during a black frame needs nothing, and an inserted picture (picture-in-picture, a
logo or subtitles in the bar) does not count as a change.

**SDR to HDR details.** SDR video is decoded with a 2.2 power (as SDR displays show it), white at 203 cd/m2 (the HDR
reference white; lower on dim displays), BT.709 colours kept. The display's peak and full-frame brightness come from
Windows. The output carries HDR10 metadata that matches the curve, and the final 10-bit step is dithered.

The status shows the frame rate, the GPU time per source frame, the frame generation method and quality in use, whether
the cut guard runs, the display with its VRR state, and where Auto stopped. Auto learns your GPU's costs while you
watch; on first use of PushFlow it measures PushFlow's speed on your card once.

## Updates

**About > Updates** in the property page shows the update status. "Check for updates" checks now; "Automatic updates"
lets PushMPC check in the background at most once a day, during playback (it never delays the start of a video); and
"Channel" picks Stable or Beta (Beta also gets pre-releases, such as builds for a new PushFPS version).

How it works: `PushMpcUpdater.exe` reads the release list of github.com/CptPakundo/PushMPC, checks the release's
signature, and downloads only the components whose version changed (and Veyra's package when a release pins a new
one), each checked by SHA-256. A download that stops continues where it stopped. The update is installed at once when
no player has PushMPC loaded; otherwise it waits and is installed when MPC-HC is closed, at the latest at your next
sign-in (the task "\PushMPC\Apply pending update" in Task Scheduler, which runs only for your account). The version
before an update stays in `previous\`. A new filter is registered and loaded once as a test; if that fails, the
previous version is put back at once and that release is not tried again.

Turn automatic updates off on the About tab or in setup, or set `Enabled` = 0 (DWORD) under
`HKCU\Software\PushMPC\Update`. The status values there (`Status`, `Message`, `AvailableVersion`, `LastCheck`) and
`%LOCALAPPDATA%\PushMPC\logs\updater.log` say what happened. By hand:

    PushMpcUpdater.exe --check | --download | --apply | --status

To update without the updater: run a newer PushMPC-Setup.exe (it offers Update), or Windows Settings > Apps >
Installed apps > PushMPC > Modify > Update.

## Repair, roll back, uninstall

- **Repair or uninstall:** Windows Settings > Apps > Installed apps > PushMPC > Modify (Repair / Update / Uninstall) or
  Uninstall. Uninstall removes the filter, the core, the runtime, the updater and its task, PushMPC's settings and the
  MPC-HC entry, and puts back what PushMPC changed: MPC Video Renderer's HDR settings, SVP's VapourSynth entry (if
  PushMPC had disabled it) and MPC-HC's NVIDIA profile (G-SYNC). Your logs stay in `%LOCALAPPDATA%\PushMPC\logs`.
- **Roll back** one component to the version before the last update: `pushmpc-install.ps1 rollback -Component filter|core|runtime`.
- Script equivalents: `pushmpc-install.ps1 status [-Verify]`, `update -ReleaseManifest release.json`, `uninstall [-KeepSettings]`.

## Troubleshooting

**Copy diagnostics.** About > Copy diagnostics copies a report (versions, GPU, displays, settings, recent log) to the
clipboard and saves it in the logs folder. Your user name, folders and video file names are removed. Paste it into a
bug report.

The About tab names the installed versions and opens the log folder. If PushMPC cannot run (no DirectX 12 GPU, a
missing or mismatched component, an interlaced video, another renderer), the video plays unchanged and the status says
why. If a frame generation method does not start on your GPU, PushMPC falls back to the next one and says so.

**VLC on Linux: hardware-decoded video.** The installer tests VLC's hardware decoding with PushMPC on and writes what
it found to `hw-selftest.txt` in the plugin folder (run `pushmpc-vlc.sh selftest-hw` to test again, for example after a
driver update). Zero-copy VA-API display (VLC's OpenGL output importing the decoder's surfaces) needs Mesa 25.1 or newer,
or an X server with DRI3. Without either, the installer sets `avcodec-hw=vaapi_drm`: hardware decoding stays, and VLC
copies the frames for the display. If no hardware path passes, software decoding stays on, with PushMPC still working.
