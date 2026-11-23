# Licences

- PushMPC (filter, pushcore, scripts): GPL-3.0-or-later.
- PushFPS (DosukaSOL and contributors) and Veyra (Likely7): GPL-3.0-or-later. pushcore is built from PushFPS's source
  with the patches in `patches/`.
- MPC Video Renderer's DirectShow base classes and interface headers: under their own licences (GPL).
- FFmpeg (avutil, swscale) from Veyra's runtime package: LGPL/GPL, see the package's notices.
- NVIDIA components (RTX Video Super Resolution, RTX Video HDR, the Video Effects SDK frame generation and
  VideoSuperRes, the AI enhancement runtime, the CUDA runtime): NVIDIA's licences. PushMPC does not distribute them: the
  installer downloads Veyra's official package from Veyra's GitHub release and installs them only as NVIDIA ships them
  (valid NVIDIA signature); community-modified NVIDIA files are opt-in only, as in PushFPS's installer. The package's
  `LICENSE`, `THIRD_PARTY_NOTICES.md` and `licenses\` go into the runtime folder with them. See
  [release model](release-model.md).
- The deband pass is PushMPC's own implementation of the well-known random-offset deband method (as used by
  libplacebo / mpv); no code was taken from them.
