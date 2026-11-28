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
- **YADIF** ("yet another deinterlacing filter", Michael Niedermayer, the original in FFmpeg's `vf_yadif`, LGPL): VLC's
  module deinterlaces with the YADIF algorithm, written anew for PushMPC from the algorithm (`vlcplugin/src/pm_deint.cpp`,
  GPL-3.0-or-later); no FFmpeg code is copied. VLC's 2:3 pulldown removal (`vlcplugin/src/pm_ivtc.cpp`) is PushMPC's own. In mpv, the deinterlacing and field matching are
  mpv's and FFmpeg's own.
- **[LAV Filters](https://github.com/Nevcairiel/LAVFilters)** (Hendrik Leppkes): the settings interface of LAV Video
  (`LAVVideoSettings.h`, GPL-2.0-or-later), at the tag `scripts/lav-pin.json` pins. Only this header is used: the
  DirectShow filter turns on LAV's double-rate deinterlacer for files measured interlaced. The build fetches it and
  checks its SHA-256.
- **VapourSynth** SDK headers: LGPL-2.1 (`vsplugin/third_party/vapoursynth/`).
- **NVIDIA Image Scaling** 1.0.3: MIT (`vsplugin/third_party/nis/`).
- **AMD FidelityFX Super Resolution 1** (FidelityFX SDK 1.1.4): MIT (`vsplugin/third_party/fidelityfx-fsr1/`).
- **ncnn** (Tencent), in the Linux builds of the PushFlow route: BSD-3-Clause.
- **Intel VPL dispatcher** (libvpl 2.17.0, Intel): MIT, linked into pushcore.dll for Intel's AI frame interpolation.
- **Intel XeSS SDK** 3.0.2 headers (Intel): Intel Simplified Software License, with Intel's third-party notices.
- **RIFE** (Practical-RIFE v4.25 lite and v4.26 weights by hzwer, MIT; ONNX conversions by
  [vs-mlrt](https://github.com/AmusementClub/vs-mlrt), GPL-3.0; prepared by PushFPS's `prepare_runtime.py`, GPL-3.0),
  in PushMPC's mpv for Windows on Arm only: the int8 versions of PushFlow's networks for the Snapdragon NPU
  (`pmml-qdq\`), made from those models by `pmml/scripts/quantize_qnn.py` (`.github/workflows/pmml-qdq.yml`). The MIT
  licence text and a notice of where every file comes from are next to them (`RIFE_LICENSE.txt`, `NOTICE.txt`).

The licence texts of these two are installed with pushcore in `core\licenses\` (`libvpl-LICENSE.txt`,
`intel-xess-LICENSE.txt`, `intel-xess-third-party-programs.txt`).
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
- **Intel XeSS** 3.0.2 runtime (XeSS super resolution, XeSS frame generation, XeLL), from Intel's GitHub repository, on
  Intel graphics cards: Intel Simplified Software License. Intel's licence and third-party notices are installed with
  the files in `runtime\licenses\intel-xess\`.
- **DLSS frame generation on older GeForce cards** (RTX 20, GTX 16 and RTX 30 only): the community port sdli1995/dlssg_for_sm86 0.4.2
  from its GitHub release, checked against fixed hashes. Its code is MIT; it carries NVIDIA's unmodified DLSS frame
  generation runtime (under NVIDIA's terms) and its own RTX 30 and Turing kernels (the Turing ones from Coldwood1026/dlssg_for_sm75; binary only).
  Its `LICENSE.md` and `THIRD_PARTY_NOTICES.txt` are installed with it.
- **HelixSR** 3.4.1 (on GTX, Radeon and Intel cards): frame generation and, on RX 5000 and GTX cards, HLX FSR 4 upscaling,
  from HelixSR's GitHub release, checked against fixed hashes, unmodified. HelixSR is free to use under the HelixSR Freeware
  License (its source is not published); it contains AMD FidelityFX code and AMD's FSR 4 network under AMD's MIT licence.
  Its `LICENSE`, `THIRD_PARTY_NOTICES.md` and `SOURCE.md` are installed with it. PushMPC does not use its DLSS part.
- **VapourSynth** R82 portable (LGPL-2.1) and Python's embeddable package (PSF licence), for the mpv route on Windows.
- **mpv** for Plex, when Plex's own player can't take PushMPC's filter: shinchiro's libmpv build (GPL/LGPL).
- **ArtCNN** weights (MIT), for the Mac's Homebrew package.
- **Anime upscalers** (on by default; Setup's `-AnimeUpscalers No` leaves them out): ArtCNN (MIT) from its GitHub
  release, CuNNy (LGPL-3.0) from its authors' repository and AnimeSR v2 (Apache-2.0) from its authors' Google Drive.
  Each file is checked against its pinned SHA-256 before it is used, and each project's licence text is installed next
  to its files. CuNNy stays a separate, replaceable component: PushMPC reads its weights straight from CuNNy's own file,
  so you can swap in your own CuNNy weights and remake the graph with `pmml/scripts/anime_sr_graphs.py`.
  For the mpv-based players Setup also fetches ArtCNN's and CuNNy's own mpv shaders (same licences, the licence text is
  in each file's header); each one stays its own file you can replace.

See [release model](release-model.md) for how downloads are pinned and checked.
