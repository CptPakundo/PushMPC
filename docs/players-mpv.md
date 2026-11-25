# PushMPC in mpv and mpv-based players

PushMPC's engine also runs outside MPC-HC, as a VapourSynth filter (`pushmpc.Enhance`) for players built on mpv. It is
the same engine as in MPC-HC (RTX Video Super Resolution, NVIDIA frame generation and the other frame generation
methods, the scene-cut guard, the colour chain), with the same settings and the same Auto rules.

> Status: **preview** in PushMPC 0.6.0, an optional component of Setup. Built and tested in CI (VapourSynth's vspipe
> and mpv, 8 and 10 bit, warm reloads) against a stand-in core; the first test pass with real players and a real GPU is
> still to come. The updater does not update this component yet: run a newer Setup.

## Supported players

| Player | How | Setup |
|---|---|---|
| mpv (Windows builds with VapourSynth, e.g. shinchiro's) | `vf=vapoursynth` in `mpv.conf` | automatic |
| mpv.net | the same block in mpv.net's own `mpv.conf` (its bundled libmpv has VapourSynth) | automatic |
| SMPlayer (with its bundled mpv) | extra mpv options in SMPlayer's settings | automatic (experimental) |
| MPC-QT | libmpv; its Options > Tweaks "mpv options file" | automatic |
| Kodi | Kodi's own player has no filter hook: videos play in mpv as Kodi's external player (`playercorefactory.xml`) | automatic when mpv is installed |
| Jellyfin MPV Shim, Plex-MPV-Shim | their bundled mpv has no VapourSynth on Windows: they are switched to the installed mpv (`conf.json` `mpv_ext`) with the block in their `mpv.conf` | automatic when mpv is installed |
| Jellyfin Desktop 2.x | its libmpv (shinchiro's build on Windows) reads `mpv.conf` in each profile folder | automatic |
| Plex for Windows, legacy Emby Theater | libmpv without VapourSynth: a VapourSynth line would stop playback | not set up (use Plex-MPV-Shim) |
| Stremio | its built-in mpv resets the video filters itself | by hand: play in mpv, see below |
| SVP's own mpv | | left alone: SVP manages it. Do not run both filters at once |

The mpv build must include VapourSynth support. Most Windows builds do; `mpv --vf=help` lists `vapoursynth` when it is
there.

## Setup

PushMPC's installer does this when it finds mpv or SMPlayer (without one, it changes nothing for this route; run Setup
again after installing one):

1. It installs the filter (`pushmpc_vs.dll`, `pushmpc.vpy`) in `%LOCALAPPDATA%\PushMPC\vsplugin`.
2. It installs a portable VapourSynth runtime (VapourSynth R82 portable plus Python 3.12's embeddable package) in
   `%LOCALAPPDATA%\PushMPC\vapoursynth`, unless you already have VapourSynth on your PATH, and puts its VSScript folder
   (`vapoursynth\lib\site-packages\vapoursynth`, where R82 keeps `VSScript.dll`) on your user PATH so mpv finds it. The
   installer downloads the official packages and checks each against a pinned SHA-256, as it does for the
   runtime. Uninstalling removes the folder and the PATH entry.
3. It adds a marked block to the top of each `mpv.conf` (mpv's `portable_config` folder when mpv has one, else
   `%APPDATA%\mpv`):

   ```
   # >>> PushMPC managed: do not edit between these lines (PushMPC uninstall removes them)
   vf-append=@pushmpc:vapoursynth=file=[<path>/pushmpc.vpy]:buffered-frames=4:concurrent-frames=2
   hwdec=auto-copy
   hr-seek-framedrop=no
   interpolation=no
   # <<< PushMPC managed
   ```

   - `hwdec=auto-copy`: VapourSynth only takes frames in system memory, so hardware decoding must copy back.
   - `hr-seek-framedrop=no`: no frames are dropped around a seek.
   - `interpolation=no`: mpv's own frame blending would fight the generated frames.

   Your `mpv.conf` is first copied to `mpv.conf.pushmpc-backup`. Uninstalling removes the block. If you did not change
   the file since, it is then byte-identical to the backup.

To switch the filter off for one session, use `mpv --vf-remove=@pushmpc`, or bind a key in `input.conf`:
`P vf toggle @pushmpc`.

### SMPlayer

The installer appends the same options to SMPlayer's *Preferences → Advanced → Options* field
(`mplayer_additional_options` in `smplayer.ini`). Start SMPlayer once before installing, so that the file exists.

### mpv.net

mpv.net reads its own folder, `%APPDATA%\mpv.net\mpv.conf` (or `portable_config\mpv.conf` next to `mpvnet.exe`). The
installer puts the same block there, with the same backup.

### MPC-QT

MPC-QT uses libmpv and does not read `mpv.conf`, but it can load an options file (Options > Tweaks > "mpv options
file"). The installer turns that on and points it at `%LOCALAPPDATA%\PushMPC\mpc-qt-mpv.conf`, which holds the block.
If you already use an options file, the block goes into yours instead. MPC-QT must be closed during setup (it saves its
settings, `%APPDATA%\mpc-qt\settings.json`, when it closes); uninstalling puts both settings back. MPC-QT splits that
file at spaces, so the path of `pushmpc.vpy` must not contain one.

### Kodi

Kodi's own player cannot take a video filter. When mpv is installed, the installer makes mpv Kodi's external player
for videos: `%APPDATA%\Kodi\userdata\playercorefactory.xml` gets a player "PushMPC mpv" (fullscreen) and a rule that
sends every video to it, both between `PushMPC managed` comments. Your own players and rules stay. mpv then plays with
its `mpv.conf`, so PushMPC runs. Kodi's interface is not shown while mpv plays; closing mpv returns to Kodi.
Uninstalling removes the marked parts (and the file, if PushMPC created it).

### Jellyfin MPV Shim and Plex-MPV-Shim

Their bundled mpv has no VapourSynth on Windows. When mpv is installed, the installer sets `mpv_ext` = true and
`mpv_ext_path` = that mpv in the shim's `conf.json` (`%APPDATA%\jellyfin-mpv-shim` or `%APPDATA%\plex-mpv-shim`), and
puts the block in the `mpv.conf` beside it. Close the shim first. Uninstalling puts both files back.

### Jellyfin Desktop

Jellyfin Desktop 2.x plays through libmpv (on Windows the same shinchiro build as mpv, with VapourSynth) and reads an
`mpv.conf` in each profile folder: `%LOCALAPPDATA%\Jellyfin Desktop\profiles\<id>\` on Windows,
`~/.local/share/jellyfin-desktop/profiles/<id>/` on Linux. The installer puts the block into each profile's `mpv.conf`
(created when there is none) and takes it out again on uninstall. Run setup again after adding a profile. The Flatpak
build has its own mpv and is left alone.

### Stremio (by hand)

Stremio's built-in mpv sets its video filter list itself whenever the display state changes, so a filter in a config
file does not last. Play in mpv instead: in Stremio, Settings > Player > "Play in external player" (choose mpv), or
copy the stream link and open it in mpv (where your Stremio version has no such setting). mpv then runs PushMPC as set
up above.

## Settings

The filter reads PushMPC's own settings (the same as the MPC-HC filter's settings page). The script's `user-data` can
override them for mpv alone, for example `...:user-data=[smoother=4,sharper=2]`. The keys are `smoother`, `sharper`,
`cleanup`, `sdr_to_hdr`, `deband`, `subtitle_hold`, `fg_quality`, `fg_method`, `vrr`, `upscaler`, `target`, `vsr_style`, `chroma`
(`auto`/`444`/`420`) and `display_hdr`.

**Auto** chooses the multiplier from the display's refresh rate (mpv's `display_fps`) and the GPU budget, with the same
rules as in MPC-HC.

**VRR:** when mpv runs in its own window, G-SYNC is detected as in MPC-HC. When the filter cannot see the player's window
(SMPlayer and MPC-QT embed mpv), it assumes a fixed refresh rate. Set `Vrr` to `On` if you use G-SYNC there.

## Costs and limits

- **Copies.** mpv hands VapourSynth CPU frames. Each frame is uploaded to the GPU, enhanced, and read back, and mpv then
  uploads it again to draw it. This costs PCIe and memory bandwidth that MPC-HC's GPU-only path does not:

  | Output | Readback (one direction) |
  |---|---|
  | 1080p at 120 fps, SDR | 0.75 GB/s |
  | 4K at 120 fps, SDR | 3.0 GB/s |
  | 4K at 120 fps, HDR (10-bit 4:4:4) | 6.0 GB/s |
  | 4K at 240 fps, HDR (10-bit 4:4:4) | 11.9 GB/s |

  Above about 6 GB/s, Auto converts the output to 4:2:0 on the GPU before the readback, which halves the bandwidth at a
  small chroma cost (`chroma=444` keeps full chroma). The log says which it chose.
- **Latency.** The filter buffers a few frames (mpv's `buffered-frames`, plus one source pair for frame generation):
  about 150–250 ms at 24 fps. This is fine for films; there is no lip-sync issue, because mpv times audio against the
  filter's output.
- **Seeking.** mpv reloads the script on every seek. The engine stays loaded in the player's process, so a seek only
  flushes it. The first file after starting the player takes longer, while the models load.
- **Not available here:** PushMPC's own SDR-to-HDR curve, and the on-screen status, both of which are part of the MPC-HC
  filter. RTX Video HDR works.
- **HDR.** HDR sources come out as 10-bit PQ BT.2020 (RGB, or 4:2:0 at high rates). Use mpv's
  `target-colorspace-hint=yes` with Windows HDR on for HDR passthrough.
- **Frame drops.** When the GPU cannot keep up, a generated frame is replaced by the nearest real frame, and mpv drops
  frames as usual.

## Logs

`%LOCALAPPDATA%\PushMPC\logs\vsplugin-*.log` (one per player process) records the settings, the display, Auto's choice,
the output format and the bandwidth decision, the warm-core reuse on each seek, and frame counts. The engine's own log
is `pushcore-engine-vsplugin.log` in the same folder.

## Other GPUs, Linux and macOS (preview, untested on real hardware)

> Status: preview. Built and unit-tested on synthetic frames on Windows; the Linux and macOS builds and the players
> below have not run on real hardware yet.

Without an NVIDIA RTX GPU, `pushmpc.Enhance` still generates frames, with one of two other engines. `pushmpc.vpy` picks
one for you:

| Backend | What it is | Where | Quality and cost |
|---|---|---|---|
| `nvidia` | PushMPC's engine (everything above) | Windows, NVIDIA RTX | best; GPU |
| `rife` | vs-rife-ncnn-vulkan, **if you have it installed** (PushMPC never downloads it) | any Vulkan GPU: AMD, Intel, Apple (MoltenVK), NVIDIA | very good; GPU, heavy at 4K |
| `portable` | PushFPS's motion-compensated frame generation, on the CPU | anything: Windows with AMD or Intel, Linux, macOS | simpler; CPU-bound (see below) |

**Auto:** an NVIDIA RTX GPU with PushMPC's engine installed gets `nvidia`; otherwise `rife` when the `rife` plugin is
loaded in VapourSynth; otherwise `portable`. To choose yourself, add `backend=portable` (or `rife`, `nvidia`) to the
`user-data` of the vf line, or set `Backend` in the settings.

### What the portable backend does

- Motion both ways between two real frames, PushFPS's scene-cut rule (no blend across a cut: the nearer real frame is
  shown), and the generated frames at 1/k, 2/k, ... between them.
- The output keeps the source's format and colour (YUV 4:2:0, 8 or 10 bit, SDR or HDR). Real frames are passed through
  unchanged. No upscaling, no RTX features.
- **Auto and the CPU.** When the script loads, the plugin times one motion estimation and one generated frame at the
  video's size, then picks the largest multiplier the display wants that fits in 60% of a source frame's time. Above
  1080p, Auto goes no higher than x2. On a 16-thread desktop CPU, 1080p at 24 fps gets about x2; 4K stays at x1 (the
  CPU is too slow for 4K). `smoother=4` forces x4 even when the CPU cannot keep up (mpv then drops frames). The log
  records the measurement and the choice.
- A GPU version of the portable engine (Vulkan compute) is planned. It is not part of this preview.

### With the rife backend

Install vs-rife-ncnn-vulkan in your VapourSynth (your distribution's package, the AUR, or its release page) so that
`core.rife` exists, and optionally VapourSynth's `misc` plugin for scene-change detection. `pushmpc.vpy` then converts to
RGB, runs RIFE at the multiplier from the display (or `smoother=N`), and converts back. Options: `rife_model=N`,
`rife_gpu=N`.

### Windows with an AMD or Intel GPU

Same setup as above (`mpv-integration.ps1`). Auto picks `rife` when it is installed, else `portable`.

### Linux

Download `pushmpc-linux-x86_64-<version>.tar.gz` (or `-aarch64-`), unpack it and run its installer as your user (no
administrator password; nothing outside your home folder changes):

```sh
sh install.sh                               # set up every player found
sh ~/.local/share/pushmpc/install.sh status
sh ~/.local/share/pushmpc/install.sh uninstall   # puts every changed file back
```

mpv needs VapourSynth. Arch, Manjaro and EndeavourOS build mpv with it (Arch users can also use the AUR package
`pushmpc` and run `pushmpc-setup`). **Ubuntu, Debian and Mint build mpv without it**: the installer then offers, once,
to install mpv from Flathub for your user (`io.mpv.Mpv`, which bundles VapourSynth; about 400 MB the first time). Answer
ahead with `--flatpak yes` or `--flatpak no`. A Flathub mpv that PushMPC installed is removed again by the uninstall
(`--keep-flatpak-mpv` keeps it).

What the installer sets up, each with a backup:

- **mpv** (only when it has VapourSynth): the block in `~/.config/mpv/mpv.conf`. With VapourSynth R73 or newer the
  installer also runs `vapoursynth config`, which tells VapourSynth's script library inside mpv where Python is
  (`~/.config/vapoursynth/vapoursynth.toml`; without it mpv stops with "Python executable and library path couldn't be
  determined").
- **Flathub mpv**: the block in `~/.var/app/io.mpv.Mpv/config/mpv/mpv.conf`, and a Flatpak override that lets it read
  `~/.local/share/pushmpc` (read-only). Its settings file is `~/.var/app/io.mpv.Mpv/config/pushmpc/settings.ini`.
- **Celluloid:** its "Load mpv configuration file" setting, pointed at `~/.config/mpv/mpv.conf`.
- **Haruna:** startup custom commands (Settings > Custom commands, `PushMPC_1` to `PushMPC_4`) that add the filter.
- **SMPlayer:** its mpv options (`~/.config/smplayer/smplayer.ini`; start SMPlayer once first).
- **MPC-QT:** its "mpv options file" (`~/.config/mpc-qt/settings.json`).
- **Jellyfin MPV Shim, Plex-MPV-Shim:** the block in the `mpv.conf` beside their `conf.json`.
- **Kodi** (native package): mpv, or the Flathub mpv, as Kodi's external player for videos.
- Celluloid, Haruna, SMPlayer and MPC-QT use the system's mpv library, so they need the distribution's mpv to have
  VapourSynth. Flatpak versions of these players and Flatpak Kodi cannot load the plugin and are left alone.
- Settings: `~/.config/pushmpc/settings.ini` with the same names as on Windows, for example:

  ```ini
  [PushMPC]
  Smoother=Auto        ; Auto, Off or 2..8
  Backend=auto         ; auto, portable, rife
  PortableQuality=balanced   ; or fast
  PortableThreads=0    ; 0 = all
  ```
- Logs: `~/.local/state/pushmpc/logs/` (or `$XDG_STATE_HOME/pushmpc/logs`).

### macOS

The same as Linux, with `pushmpc_vs.dylib` and `pushmpc.vpy` in `~/Library/Application Support/PushMPC/vsplugin/`
(check that your mpv lists `vapoursynth` in `mpv --vf=help`).

- **mpv:** `~/.config/mpv/mpv.conf`, set up with `mpv-integration.sh`.
- **IINA:** turn on Settings > Advanced > "Use config directory" so IINA reads `~/.config/mpv`. IINA's own mpv must have
  VapourSynth support; this has not been checked.
- Settings: `~/.config/pushmpc/settings.ini`. Logs: `~/Library/Logs/PushMPC/`.
