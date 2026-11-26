The first release of **PushMPC**: PushFPS's video enhancement (frame generation up to 8x with a scene-cut guard, RTX
Video Super Resolution, SDR to HDR, lossless HDR handling) inside MPC-HC and MPC-BE, set up by itself for your GPU and
display.

## Install

Download **PushMPC-Setup.exe** and run it (per user, no administrator rights). It adds PushMPC to MPC-HC and MPC-BE
(whichever is installed) and downloads Veyra's runtime package (about 750 MB, resumable, checked by SHA-256) unless
PushFPS or Veyra is already installed with the same files. Then play a video: Auto is on.

Setup is not code-signed yet: Windows SmartScreen may say "Windows protected your PC". Click **More info**, then
**Run anyway**. The release manifest (`release.json`) is signed, and the updater checks it.

Installed PushMPC updates itself (Windows Settings > Apps > PushMPC > Modify, or Play > Filters > PushMPC Filter >
About in the player).

## Players

| Player | Status |
|---|---|
| MPC-HC (x64) | ✅ supported, tested |
| MPC-BE (x64) | 🆕 automatic (filter + MPC Video Decoder in D3D11 copy-back); playback in MPC-BE not tested yet |
| mpv, SMPlayer | 🧪 **preview**, optional: set up only when Setup finds mpv or SMPlayer; not yet tested with a real GPU |
| MPC-QT, PotPlayer | 🧪 manual setup ([mpv guide](https://github.com/CptPakundo/PushMPC/blob/main/docs/players-mpv.md), [user guide](https://github.com/CptPakundo/PushMPC/blob/main/docs/user-guide.md#players)); PotPlayer untested |

## Inside

- Filter **0.6.0**, core **0.6.0+pushfps4.5.4** (PushFPS v4.5.4 with the patches in `patches/`), runtime from Veyra
  v2.2.1 (never shipped here: Setup fetches it from Veyra's release or reuses an installed copy).
- The optional mpv component (vsplugin 0.6.0) fetches the official VapourSynth R82 portable and Python 3.12 embeddable
  packages, each checked against a pinned SHA-256.

## Known limitations

Tested on one machine with a GeForce RTX 50 card; other GPUs were not tested with PushMPC. Interlaced video plays
without PushMPC. The mpv route copies every frame to the GPU and back, lacks PushMPC's SDR-to-HDR curve and the
on-screen status, and is not updated by the updater yet. The full list is in the
[changelog](https://github.com/CptPakundo/PushMPC/blob/main/CHANGELOG.md).

## Files

| File | What |
|---|---|
| `PushMPC-Setup.exe` | the installer: everything you need |
| `PushMPC-0.6.0.zip` | the same for a script install (`Install-PushMPC.cmd`) |
| `PushMPC-filter-*.zip`, `PushMPC-core-*.zip`, `PushMPC-vsplugin-*.zip` | the components the updater downloads |
| `release.json`, `release.json.sig` | the signed release manifest |

PushMPC is GPL-3.0-or-later. The engine is [PushFPS](https://github.com/DosukaSOL/PushFPS) by DosukaSOL, the runtime
[Veyra](https://github.com/Likely7/Veyra-NRVideo) by Likely7. Made with AI: see the README.
