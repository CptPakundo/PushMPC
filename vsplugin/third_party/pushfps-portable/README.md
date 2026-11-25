# PushFPS portable core (motion estimation, interpolation, Subtitle Hold)

- **What:** `include/veyra/portable/{Image,Motion,Interpolate,CrosshairHold,Dedup,HudLock}.h`, `src/{Motion,Interpolate,CrosshairHold}.cpp` (directory `portable/core`) and `include/veyra/pipeline/PushOverlay.h` (the Crosshair / Subtitle Hold rule, from the repository's `include/`), copied unmodified (byte for byte, upstream line endings) from PushFPS **v4.5.3** (commit `731589e2`, portable core 0.3.0).
- **License:** GPL-3.0-or-later. Copyright (C) 2026 DosukaSOL and the PushFPS contributors; PushFPS is a modified version of Veyra. The full licence text is in `LICENSE`.
- **Used by:** the `portable` backend of `pushmpc.Enhance`:
  - the CPU engine (`vsplugin/engines/portable/`) uses the hierarchical block motion estimation and its scene-cut rule as they are; `Interpolate.cpp` is the reference its planar YUV port follows (`portable_test` compares the two);
  - the GPU engine (`vsplugin/engines/gpu/`) ports the block matcher and the interpolation to Vulkan compute shaders (`gpu_test` compares them with these files), takes Dedup's duplicate test, and runs Subtitle Hold with `PushOverlay.h`'s rule and `CrosshairHold.cpp`'s `stepFast` as they are.
- **Not copied:** `Engine.cpp` (real-time display pacing) and `Convert.cpp` (capture formats to BGRA). A pull-model VapourSynth filter needs neither.
- **Updating:** copy the same files from a newer PushFPS tag, update the version line above, and run `portable_test` and `gpu_test`.
