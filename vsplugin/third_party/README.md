# third_party

`vapoursynth/` holds VapourSynth's SDK headers, which the plugin and `vs_harness` need at build time:
`VapourSynth4.h`, `VSHelper4.h`, `VSScript4.h` (from the `include/` folder of the VapourSynth release the runtime pin in
`scripts/vapoursynth-pin.json` names) and their licence notice (LGPL-2.1, `COPYING.LGPLv2.1`).

Without them `build.cmd` builds only the stub core and the tests that need no VapourSynth (`sched_test`, `core_test`).

`pushfps-portable/` holds the files of PushFPS's portable core that the `portable` backend compiles (motion estimation,
its scene-cut rule, the interpolation reference, Subtitle Hold), copied unmodified from PushFPS v4.5.3 under
GPL-3.0-or-later. See its own README.md; `portable_test` builds from it on every OS with no VapourSynth needed.

`nis/` (NVIDIA Image Scaling 1.0.3, MIT) and `fidelityfx-fsr1/` (AMD FSR1 from the FidelityFX SDK 1.1.4, MIT) are the
upscalers of the portable GPU engine (`engines/gpu`), compiled into its shaders; see their PUSHMPC_ORIGIN.md.
