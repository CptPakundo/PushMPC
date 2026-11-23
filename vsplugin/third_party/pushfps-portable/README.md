# PushFPS portable core (motion estimation and interpolation)

- **What:** `include/veyra/portable/{Image,Motion,Interpolate}.h` and `src/{Motion,Interpolate}.cpp`, copied unmodified from PushFPS **v4.3.1** (commit `b5e7a140`), directory `portable/core`.
- **License:** GPL-3.0-or-later. Copyright (C) 2026 DosukaSOL and the PushFPS contributors; PushFPS is a modified version of Veyra. The full licence text is in `LICENSE`.
- **Used by:** the `portable` backend of `pushmpc.Enhance` (`vsplugin/engines/portable/`). The plugin uses the hierarchical block motion estimation and its scene-cut rule as they are. `Interpolate.cpp` is the reference the plugin's planar YUV port follows; `portable_test` compares the two.
- **Not copied:** `Engine.cpp` (real-time display pacing, duplicate removal, Live HUD Lock) and `Convert.cpp` (capture formats to BGRA). A pull-model VapourSynth filter needs neither.
- **Updating:** copy the same files from a newer PushFPS tag, update the version line above, and run `portable_test`.
