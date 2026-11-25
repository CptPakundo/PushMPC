# VLC 3.0.24 sources (Direct3D 11 video output)

From VideoLAN's VLC, tag `3.0.24` (https://code.videolan.org/videolan/vlc), LGPL-2.1-or-later (`COPYING.LIB`). Kept at their
paths below `modules/` so their relative includes work unchanged.

Taken unchanged: `video_output/win32/{common,events,win32touch}.{c,h}`, `sensors.cpp`, `d3d11_quad.{c,h}`,
`d3d11_shaders.{c,h}`, `d3d11_tonemap.{cpp,h}`, `video_chroma/{d3d11_fmt,dxgi_fmt,copy}.{c,h}`,
`codec/avcodec/va_surface.h`.

Changed by PushMPC (marked "PushMPC" in the code):
- `video_output/win32/direct3d11.c`: module name `pushmpc_d3d11` (score 0, chosen with `vout=pushmpc_d3d11`), its own
  option names (`pushmpc-d3d11-*`), upscale modes `nis` and `auto` (the driver's Super Resolution, else NIS).
- `video_output/win32/d3d11_scaler.{cpp,h}`: a NIS path next to the video processor's.

Added by PushMPC (GPL-3.0-or-later): `video_output/win32/pm_nis_d3d11.{cpp,h}`: NVIDIA Image Scaling 1.0.3 (MIT,
`vsplugin/third_party/nis`) on the decoder's textures, in compute shaders.
