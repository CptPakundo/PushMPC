# Licences

PushMPC itself (the filter, pushcore, the players' modules, Setup and the scripts) is free software under the
**GPL-3.0-or-later** ([LICENSE](../LICENSE)). It is built on, or works with, the projects below. Thank you to all of
them.

## Built in

- **[PushFPS](https://github.com/DosukaSOL/PushFPS)** (DosukaSOL and contributors) and
  **[Veyra](https://github.com/Likely7/Veyra-NRVideo)** (Likely7): GPL-3.0-or-later. pushcore is built from PushFPS's
  source with the small patch set in `patches/`; the portable engine uses files of PushFPS's portable core
  (`vsplugin/third_party/pushfps-portable/`).
- **MPC Video Renderer**'s DirectShow base classes and interface headers: GPL.
- **VapourSynth** SDK headers: LGPL-2.1 (`vsplugin/third_party/vapoursynth/`).
- **NVIDIA Image Scaling** 1.0.3: MIT (`vsplugin/third_party/nis/`).
- **AMD FidelityFX Super Resolution 1** (FidelityFX SDK 1.1.4): MIT (`vsplugin/third_party/fidelityfx-fsr1/`).
- **ncnn** (Tencent), in the Linux builds of the PushFlow route: BSD-3-Clause.
- The deband pass is PushMPC's own implementation of the well-known random-offset deband method (as used by
  libplacebo and mpv); no code was taken from them.

## Downloaded by Setup, never shipped by PushMPC

- **Veyra's runtime package** (from Veyra's GitHub release, SHA-256 checked): FFmpeg (LGPL/GPL) and the NVIDIA and AMD
  components. NVIDIA's parts (RTX Video Super Resolution, RTX Video HDR, the Video Effects frame generation, the AI
  enhancement runtime, the CUDA runtime) are under NVIDIA's licences, AMD's under AMD's. The package's `LICENSE`,
  `THIRD_PARTY_NOTICES.md` and `licenses\` are installed with them. The community unlocks and ports in the package are
  installed by default, as PushMPC's aim is to give every card everything it can run; see
  [building](building.md#how-setup-works) to leave them out.
- **PushFlow**: ONNX Runtime and its execution providers (MIT, Microsoft; TensorRT-RTX under NVIDIA's licence,
  OpenVINO under Apache-2.0) from nuget.org and pypi.org, and the RIFE models (MIT) from PushFPS's release.
- **AMD FidelityFX SDK** runtime (FSR 4.1 on Radeon RX 6000-9000), from AMD's GitHub release, under AMD's licence.
- **DLSS frame generation for RTX 20** (on GeForce RTX 20 cards only): the community port sdli1995/dlssg_for_sm86 0.4.2
  from its GitHub release, checked against fixed hashes. Its code is MIT; it carries NVIDIA's unmodified DLSS frame
  generation runtime (under NVIDIA's terms) and its own Turing kernels (from Coldwood1026/dlssg_for_sm75, binary only).
  Its `LICENSE.md` and `THIRD_PARTY_NOTICES.txt` are installed with it.
- **VapourSynth** R82 portable (LGPL-2.1) and Python's embeddable package (PSF licence), for the mpv route on Windows.
- **mpv** for Plex, when Plex's own player can't take PushMPC's filter: shinchiro's libmpv build (GPL/LGPL).
- **ArtCNN** weights (MIT), for the Mac's Homebrew package.

See [release model](release-model.md) for how downloads are pinned and checked.
