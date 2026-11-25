# The Direct3D 11 output fork: what comes from VLC

The fork is built from VideoLAN's VLC **3.0.24** release (https://download.videolan.org/pub/videolan/vlc/3.0.24/,
LGPL-2.1-or-later). Its sources are not kept in this repository: `fetch-vlc-sources.sh` downloads the release tarball
(SHA-256 checked), extracts the files the output needs below `modules/` at their upstream paths, and applies
`pushmpc-vlc-3.0.24.patch`.

PushMPC's changes (the patch, marked "PushMPC" in the code):
- `video_output/win32/direct3d11.c`: module name `pushmpc_d3d11` (score 0, chosen with `vout=pushmpc_d3d11`), its own
  option names (`pushmpc-d3d11-*`), upscale modes `nis` and `auto` (the driver's Super Resolution, else NIS).
- `video_output/win32/d3d11_scaler.{cpp,h}`: a NIS path next to the video processor's.

PushMPC's own files (GPL-3.0-or-later): `modules/video_output/win32/pm_nis_d3d11.{cpp,h}`: NVIDIA Image Scaling 1.0.3
(MIT, `vsplugin/third_party/nis`) on the decoder's textures, in compute shaders.
