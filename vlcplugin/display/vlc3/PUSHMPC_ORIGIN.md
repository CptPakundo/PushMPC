# The Direct3D 11 output fork: what comes from VLC

The fork is built from VideoLAN's VLC **3.0.24** release (https://download.videolan.org/pub/videolan/vlc/3.0.24/,
LGPL-2.1-or-later). Its sources are not kept in this repository: `fetch-vlc-sources.sh` downloads the release tarball
(SHA-256 checked), extracts the files the output needs below `modules/` at their upstream paths, and applies
`pushmpc-vlc-3.0.24.patch`.

PushMPC's changes (the patch, marked "PushMPC" in the code):
- `video_output/win32/direct3d11.c`: module name `pushmpc_d3d11` (score 0, chosen with `vout=pushmpc_d3d11`), its own
  option names (`pushmpc-d3d11-*`), upscale modes `nis`, `anime` (pushcore's upscaler, else NIS) and `auto` (pushcore's
  upscaler where the anime upscaler can run, else the driver's Super Resolution, else NIS); `Prepare` hands each picture
  to pushcore before it takes the context lock.
- `video_output/win32/d3d11_scaler.{cpp,h}`: a NIS path next to the video processor's, and pushcore's upscaler on it.

PushMPC's own files (GPL-3.0-or-later):
- `modules/video_output/win32/pm_nis_d3d11.{cpp,h}`: NVIDIA Image Scaling 1.0.3 (MIT, `vsplugin/third_party/nis`) on the
  decoder's textures, in compute shaders; its colour conversion alone for pictures that are already upscaled.
- `modules/video_output/win32/pm_pcsr_d3d11.{cpp,h}`: pushcore's upscaler through `pm_pushcore.dll` (loaded at run time
  from the plugin's folder, ABI 3): the decoder's texture slices in on the output's own device, pushcore's picture at the
  screen size out (Video Super Resolution; the anime upscaler while ContentDetect measures the picture as drawn, switched
  at most once per 30 s as in the Kodi add-on), converted into NIS's window-size texture. NIS draws every picture
  pushcore does not deliver in time, and the rest of the stream when pushcore fails. Not used while PushMPC's frame
  generation filter may hold pushcore (one pushcore per player), nor for SD (BT.601) sources.

## The OpenGL output fork (Linux)

Built from the same VLC 3.0.24 tarball (`modules/video_output/opengl/`), with `pushmpc-vlc-3.0.24-gl.patch`:
- `display.c`: module name `pushmpc_gl` (score 0, chosen with `vout=pushmpc_gl`); it declares only `pushmpc-gl-upscale`
  (`fsr`, default, or `off`) and `pushmpc-gl-sharpness`, and reads VLC's own `gl`, `glconv` and colour options.
- `vout_helper.c`: flat 2D pictures that are upscaled are drawn by VLC's shaders into a source-size texture, then go
  through `pm_glscale` into the window. 360°, stereo and rotated pictures, and downscaling, keep VLC's path.

PushMPC's own files (GPL-3.0-or-later): `modules/video_output/opengl/pm_glscale.{c,h}`: AMD FidelityFX Super Resolution 1
(EASU + RCAS, after AMD's MIT-licensed reference) as GLSL 1.20 / ES 1.00 fragment shaders.
