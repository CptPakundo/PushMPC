# Changelog

What changed in each PushMPC release.

## 1.0.0

First release. PushMPC brings PushFPS's frame generation and upscaling into the video players people already use, and
sets itself up for your graphics card and display.

### Players

- **Windows:** MPC-HC, MPC-BE, PotPlayer, mpv, mpv.net, SMPlayer, MPC-QT, VLC 3, Kodi 21 and 22 (its own add-on),
  Plex for Windows and Plex HTPC, Jellyfin Desktop, the Jellyfin and Plex MPV Shims, and Stremio 5.0.28 and newer.
- **PushMPC's own mpv** for Windows, with PushMPC built in (an optional box in Setup).
- **Capture mode** for every other Windows player (KMPlayer, GOM Player, Emby Theater and more).
- **Linux:** mpv (your distribution's or Flathub's), Celluloid, Haruna, SMPlayer, MPC-QT, VLC 3, Kodi with its add-on,
  Plex from Flathub, Jellyfin Desktop and the MPV Shims, on x86_64 and ARM.

### Picture

- Frame generation up to 8x: NVIDIA frame generation, DLSS frame generation, PushFlow (PushFPS's AI frame generation),
  FSR and AMD Fluid Motion, whichever your card runs best.
- A scene-cut guard for every method, so a new shot starts on its own frame. Sudden changes of the picture's shape are
  handled like cuts.
- Upscaling to your screen with RTX Video Super Resolution, FSR or NIS, plus an AI clean-up and a deband pass for
  low-quality video.
- SDR to HDR with PushMPC's own curve on every card, or RTX Video HDR, tuned to your display's brightness.
- HDR10, HDR10+, HLG and Dolby Vision (profiles 5, 7 and 8) pass through untouched.
- Subtitle Hold keeps subtitles that are burned into the picture steady.

### Auto

- Every setting picks itself for your graphics card, display, refresh rate (fixed, G-SYNC or FreeSync) and video, and
  keeps adjusting while you watch. It steps down rather than dropping frames.
- A lighter profile on laptop battery.

### Setup and updates

- One Setup for Windows, without administrator rights, and one `setup.sh` for Linux. Both find your players and set up
  each one, and uninstalling puts everything back.
- Faster PushFlow downloads for NVIDIA RTX, GTX 16 and Intel cards (and AMD on Linux), and AMD's newest FSR for
  Radeon RX 6000-9000, each one optional.
- A settings page in MPC-HC and MPC-BE with live "Now:" lines, an on-screen info line, a G-SYNC button and Copy
  diagnostics.
- Updates on Windows: at most one check a day, only changed parts downloaded, installed when the player is closed, and
  an automatic step back if a new version fails to load.

Built on PushFPS 4.5.4 and Veyra 2.2.1.

### Known limits

- Setup isn't code-signed yet, so Windows SmartScreen warns the first time.
- MPC-HC with a GeForce RTX 50 card has had the most testing on real hardware. The other players and cards are checked
  automatically on every build, but not all of them on real hardware yet.
- Interlaced video plays without PushMPC.
- Capture mode is a little behind the sound (set the player's audio delay to about +80 ms) and can't show
  copy-protected video.
- In mpv through VapourSynth, very high multipliers at 4K can be limited by copying frames; PushMPC's own mpv and the
  built-in Linux route are faster.
- Mac support is coming soon.
