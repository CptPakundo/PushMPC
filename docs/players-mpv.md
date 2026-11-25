# PushMPC in mpv, SMPlayer and MPC-QT

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
| SMPlayer (with its bundled mpv) | extra mpv options in SMPlayer's settings | automatic (experimental) |
| MPC-QT | libmpv; does not read `mpv.conf` | by hand, see below |
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

### MPC-QT (by hand)

MPC-QT uses libmpv and does not read `mpv.conf`. If your MPC-QT build has an mpv options field in its settings, add:

```
vf-append=@pushmpc:vapoursynth=file=[%LOCALAPPDATA%/PushMPC/vsplugin/pushmpc.vpy]
hwdec=auto-copy
hr-seek-framedrop=no
```

Write the real path in place of `%LOCALAPPDATA%`. Its bundled libmpv must also have VapourSynth support.

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
| `portable` | PushFPS's motion-compensated frame generation on the GPU through Vulkan (any vendor; MoltenVK on macOS), with NIS or FSR1 upscaling; on the CPU when there is no Vulkan GPU | anything: Windows with AMD or Intel, Linux, macOS | good; light on a GPU, CPU-bound without one (see below) |

**Auto:** an NVIDIA RTX GPU with PushMPC's engine installed gets `nvidia`; otherwise `rife` when the `rife` plugin is
loaded in VapourSynth; otherwise `portable`. To choose yourself, add `backend=portable` (or `rife`, `nvidia`) to the
`user-data` of the vf line, or set `Backend` in the settings.

### What the portable backend does

- **Engine.** On the best Vulkan GPU it finds (a discrete card first), the GPU engine runs; without one (or with
  `PortableDevice = cpu`) the CPU engine. The log says which, and why; every frame carries `PushMPCEngine` = `gpu` or `cpu`.
- Motion both ways between two real frames: PushFPS's block matcher on the GPU (8 px vectors from the full-size picture by
  default), or NVIDIA's optical-flow hardware where the driver offers it (`VK_NV_optical_flow`; `motion=blocks` turns it
  off). Scene cuts: PushFPS's rule plus PushMPC's guard (no blend across a cut: the nearer real frame is shown). The
  generated frames sit at 1/k, 2/k, ... between the real ones.
- **Anime on twos and threes (Dedup, on by default):** a repeated frame is recognised (PushFPS's rule) and the generated
  frames are spread between the different drawings, so a drawing held for two or three frames moves smoothly instead of
  stepping. The plugin looks two source frames ahead for this (`dedup=0` turns it off).
- **Subtitle Hold (on by default in this engine):** burned-in subtitles stay sharp and in place while the picture behind
  them moves (PushFPS's rule; `subtitle_hold=0` turns it off). Soft subtitles are drawn by mpv afterwards anyway.
- **Upscaling (GPU engine):** to the screen when mpv passes its size (or `target=1440p|4K|5K`), with FSR1 (EASU + RCAS) by
  default or NIS (`upscaler=nis`); `upscaler=off` leaves scaling to mpv. Chroma is scaled bicubic.
- The output keeps the source's format and colour (YUV 4:2:0, 8 or 10 bit, SDR or HDR). Without upscaling, real frames
  are passed through unchanged. No RTX features.
- **Auto and the GPU.** The plugin times one source frame and one generated frame on the GPU at the video's size and
  picks the largest multiplier the display wants that fits in 60% of a source frame's time.
- **Auto and the CPU.** When the script loads, the plugin times one motion estimation and one generated frame at the
  video's size, then picks the largest multiplier the display wants that fits in 60% of a source frame's time. Above
  1080p, Auto goes no higher than x2. On a 16-thread desktop CPU, 1080p at 24 fps gets about x2; 4K stays at x1 (the
  CPU is too slow for 4K). `smoother=4` forces x4 even when the CPU cannot keep up (mpv then drops frames). The log
  records the measurement and the choice.

### With the rife backend

Install vs-rife-ncnn-vulkan in your VapourSynth (your distribution's package, the AUR, or its release page) so that
`core.rife` exists, and optionally VapourSynth's `misc` plugin for scene-change detection. `pushmpc.vpy` then converts to
RGB, runs RIFE at the multiplier from the display (or `smoother=N`), and converts back. Options: `rife_model=N`,
`rife_gpu=N`.

### Windows with an AMD or Intel GPU

Same setup as above (`mpv-integration.ps1`). Auto picks `rife` when it is installed, else `portable`.

### Linux

You need mpv built with VapourSynth (`mpv --vf=help` lists `vapoursynth`), VapourSynth itself (API 4, R55 or newer),
the plugin `pushmpc_vs.so` and `pushmpc.vpy` in `~/.local/share/pushmpc/vsplugin/`. Then:

```sh
sh mpv-integration.sh install     # adds the PushMPC block to ~/.config/mpv/mpv.conf (backup kept)
sh mpv-integration.sh status      # what is set up, conflicts, which players were found
sh mpv-integration.sh uninstall   # removes it; restores the original byte for byte when you changed nothing else
```

- **mpv:** reads `~/.config/mpv/mpv.conf` (or `$XDG_CONFIG_HOME/mpv`).
- **Celluloid:** turn on Preferences > Config Files > "Load mpv configuration file" and choose `~/.config/mpv/mpv.conf`.
- **Haruna:** not documented whether it reads `mpv.conf`; if it does not, put the block's options into Haruna's own mpv
  settings.
- **SMPlayer and MPC-QT:** as on Windows (SMPlayer's mpv options; MPC-QT by hand).
- **Flatpak and Snap players** have their own config folder and often lack VapourSynth. The script reports them and
  leaves them alone.
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
