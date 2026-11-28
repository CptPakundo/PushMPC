The first release of **PushMPC**: smoother, sharper video in the player you already use. PushMPC brings PushFPS's frame
generation and upscaling to MPC-HC, MPC-BE, PotPlayer, mpv, VLC, Kodi, Plex, Jellyfin, Stremio and more, and sets
itself up for your graphics card and display.

PushFPS engine: <version>

## Highlights

- **Smoother motion**, up to 16x on GeForce RTX 50 and 40 cards, so a 24 fps film fills a 120 Hz or 240 Hz screen. On a
  GeForce RTX 5090, MPC-HC played a 1080p film at 10x (240 fps) on a 4K 240 Hz screen without dropping a frame.
- **Starts fast:** in MPC-HC and MPC-BE, smooth motion begins about half a second after the picture appears, even
  for the first video after starting the player.
- **Clean scene cuts:** a new shot starts on its own frame, never a smeared blend.
- **A sharper picture** with RTX Video Super Resolution, FSR or NIS.
- **HDR done right:** HDR10, HDR10+ and HLG play untouched (in MPC-HC also Dolby Vision, profiles 7 and 8). On an SDR
  display, HDR video is converted to fit your screen. In mpv 0.39 and newer, PushMPC reads each video's colour (SDR,
  HDR10 or HLG) from its very first frame.
- **Made for animation:** in MPC-HC, PushMPC tells cartoons and anime from live action by looking at the picture
  itself, and you can switch on an anime upscaler for them (tested with 1080p video).
- **Auto** picks everything for your card and screen, including G-SYNC, and keeps adjusting while you watch.
- **Every player at once:** Setup finds your players and sets up each one. Uninstalling puts them back as they were.

## What to download

| Your computer | Download |
|---|---|
| Windows 11 (Windows 10 should work too) | **PushMPC-Setup.exe**: run it and click through |
| Linux, 64-bit PC | **pushmpc-linux-x86_64-1.0.0.tar.gz**: unpack it and run `sh setup.sh` |
| Linux, ARM | **pushmpc-linux-aarch64-1.0.0.tar.gz**: the same (without VLC and the Kodi add-on) |
| Mac (preview) | no download: install with [Homebrew](https://brew.sh), see the [README](https://github.com/CptPakundo/PushMPC#mac) |

The other files are parts that Setup and the updater download by themselves: you don't need them.

Windows may say "Windows protected your PC" because Setup isn't code-signed yet. Click **More info**, then
**Run anyway**. Once installed, PushMPC keeps itself up to date.

## Known limits

- MPC-HC with a GeForce RTX 50 card has had the most testing on real hardware; the other players and cards are checked
  automatically on every build.
- Interlaced video plays without PushMPC.
- Capture mode (for players PushMPC can't plug into) is a preview. It is a little behind the sound and can't show
  copy-protected video.
- Mac support is a preview.
- In mpv older than 0.39, the first few frames of each video play without PushMPC.
- When saving a video with mpv (`mpv --o=...`), mpv ignores your `mpv.conf`: add `--include=<path to your mpv.conf>`
  to the command to use PushMPC.

How to use it, settings and troubleshooting: see the
[README](https://github.com/CptPakundo/PushMPC#readme) and the
[user guide](https://github.com/CptPakundo/PushMPC/blob/main/docs/user-guide.md). What's inside:
[changelog](https://github.com/CptPakundo/PushMPC/blob/main/CHANGELOG.md).
