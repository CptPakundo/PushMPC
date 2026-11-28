# Changelog

What changed in each PushMPC release.

## 1.0.0

First release. PushMPC brings PushFPS's frame generation and upscaling into the video players people already use, and
sets itself up for your graphics card and display.

### Players

- **Windows:** MPC-HC, MPC-BE, PotPlayer, mpv, mpv.net, MPC-QT, VLC 3, Kodi 21 and 22 (its own add-on), Plex for
  Windows and Plex HTPC, Jellyfin Desktop, the Jellyfin MPV Shim, and Stremio 5.0.28 and newer. SMPlayer and the Plex
  MPV Shim are a preview.
- **PushMPC's own mpv** for Windows, with PushMPC built in (an optional box in Setup).
- **Capture mode** (preview) for every other Windows player (KMPlayer, GOM Player, Emby Theater and more).
- **Linux:** mpv (your distribution's or Flathub's), Celluloid, Haruna, SMPlayer, MPC-QT, VLC 3, Kodi with its add-on,
  Plex from Flathub, Jellyfin Desktop and the MPV Shims, on x86_64 and ARM.

### Picture

- Frame generation up to 16x: NVIDIA frame generation, DLSS frame generation, PushFlow (PushFPS's AI frame generation),
  FSR and AMD Fluid Motion, whichever your card runs best.
- A scene-cut guard for every method, so a new shot starts on its own frame. Sudden changes of the picture's shape are
  handled like cuts.
- Upscaling to your screen with RTX Video Super Resolution, FSR or NIS, plus an AI clean-up and a deband pass for
  low-quality video.
- HDR10, HDR10+ and HLG stay HDR, and in MPC-HC Dolby Vision (profiles 7 and 8) keeps its Dolby Vision data. On an
  SDR display, HDR video is converted to fit your screen.
- Subtitle Hold keeps subtitles that are burned into the picture steady.
- Smooth motion for cartoons and anime drawn "on twos" or "on threes" (each drawing shown for two or three frames):
  the in-between frames are spread over the whole drawing instead of moving in one step and stopping in the next.
- Interlaced video (TV recordings, many DVDs and broadcasts) gets smooth motion too. PushMPC sees the combing in the
  picture itself (never trusting the file's labels), deinterlaces it into whole frames, one per field, and then
  generates from those. In mpv this uses mpv's own deinterlacer, and film carried in a TV signal (2:3 pulldown) is
  put back into its original 24 frames first, so nothing of the film is lost. In VLC, PushMPC uses its own
  deinterlacer (film is deinterlaced like video there). In MPC-HC, MPC-BE and PotPlayer, PushMPC still steps aside on
  interlaced video and the player deinterlaces it.

### Auto

- Every setting picks itself for your graphics card, display, refresh rate (fixed or G-SYNC) and video, and
  keeps adjusting while you watch. It steps down rather than dropping frames.
- A lighter profile on laptop battery.
- Quick start in MPC-HC: the picture appears sooner and smooth motion starts a few seconds after you
  open a video, even the first one after starting the player. The sharpest upscaling joins a little later, without
  a pause.

### Setup and updates

- One Setup for Windows, without administrator rights, and one `setup.sh` for Linux. Both find your players and set up
  each one, and uninstalling puts everything back.
- Faster PushFlow downloads for NVIDIA RTX, GTX 16 and Intel cards (and AMD on Linux), and AMD's newest FSR for
  Radeon RX 6000-9000, each one optional.
- A settings page in MPC-HC and MPC-BE with live "Now:" lines, an on-screen info line, a G-SYNC button and Copy
  diagnostics.
- Updates on Windows: at most one check a day, only changed parts downloaded, installed when the player is closed, and
  an automatic step back if a new version fails to load.

Built on PushFPS 4.5.8 and Veyra 2.2.1.

### Known limits

- Setup isn't code-signed yet, so Windows SmartScreen warns the first time.
- MPC-HC with a GeForce RTX 50 card has had the most testing on real hardware. The other players and cards are checked
  automatically on every build, but not all of them on real hardware yet.
- Interlaced video: in VLC with hardware decoding kept on the GPU (the default on Linux and with Direct3D 11 on
  Windows), and in MPC-HC or MPC-BE with a decoder other than LAV Video, PushMPC still steps aside and the player
  deinterlaces as before. Film in a TV signal is restored to 24 frames in mpv only; VLC deinterlaces it instead.
- In mpv, deinterlacing or restoring film starts a few seconds into the file, once the picture is measured, and not
  when saving a video with mpv (`--o`).
- Mac noise reduction (`CleanUp=On`, macOS 26) is new and hasn't run on a real Mac yet. It is off by default and not
  used in VLC.
- Capture mode is a little behind the sound (set the player's audio delay to about +80 ms) and can't show
  copy-protected video.
- In mpv through VapourSynth, very high multipliers at 4K can be limited by copying frames; PushMPC's own mpv and the
  built-in Linux route are faster.
- Mac support is a preview (install with Homebrew).
- In mpv older than 0.39, the first few frames of each video play without PushMPC.
