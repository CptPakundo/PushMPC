# PushMPC for IINA

IINA plays video through its own copy of libmpv, and that copy is built without VapourSynth, so IINA cannot load the
PushMPC filter (`pushmpc.Enhance`). Two tiers:

- **Frame generation and super resolution inside IINA (the bridge).** `brew install pushmpc` also installs
  `pushmpc-iina.so`, an mpv C plugin. IINA 1.5.1's libmpv has C plugins enabled and IINA's entitlements turn library
  validation off, so the plugin loads it with `load-script`. It patches libmpv's imports of a few public libavfilter
  functions (`av_buffersrc_add_frame`, `av_buffersink_get_frame_flags`, ...) so that one marked no-op filter,
  `vf=@pushmpc:lavfi=[null@pushmpc]`, runs PushMPC's apple engine: VideoToolbox interpolation and super resolution, or
  ArtCNN on Metal, zero copy on IINA's VideoToolbox frames; where VideoToolbox cannot run, PushMPC's engine module from
  the Mac bundle (`pushmpc-setup install` unpacks it): VideoToolbox on downloaded frames, then the GPU engine on
  MoltenVK, then the CPU. `pushmpc-setup install` also installs this plugin into IINA and switches it on. IINA.app is not modified (no re-signing, IINA's updates
  keep working). When the bridge cannot run (IINA's FFmpeg major changed, an arm64e build, no Homebrew package), the
  filter passes frames through unchanged and the shader tier below applies. Log: `~/Library/Logs/PushMPC/iina-bridge.log`.
  Status: built and smoke-tested in CI inside IINA's own libmpv on a VM (portable engine); the VideoToolbox path
  needs a real Mac.
- **Shaders only**, when the bridge is off or missing:

- **Upscaling shaders, picked for your chip.** IINA renders through OpenGL 4.1, so only fragment and textureGather
  shaders work, not compute shaders.
  - Light: FSR 1. Default on base M1 and M2.
  - Balanced: RAVU-Lite r3. Default on base M3 and newer.
  - High: FSRCNNX 8, or ArtCNN C4F16 for anime. Default on Pro chips.
  - Ultra: FSRCNNX 16, or ArtCNN C4F32 for anime. Default on Max, Ultra, and M4 or newer Pro chips.

  ArtCNN (MIT) ships upstream only as compute shaders. `build.py` generates fragment-pass versions from the pinned
  ONNX weights (`vsplugin/tools/artcnn_tool.py`), so they run on IINA's OpenGL 4.1.

  A shader runs only when the video is drawn larger than its own size.
- **Smooth motion:** `video-sync=display-resample`, `interpolation` and `tscale=oversample`. Each frame stays on screen
  for its exact share of display refreshes, so a 24 fps film does not judder on a 60 Hz, 120 Hz or ProMotion display.
  This is not frame generation.
- **HDR is left alone.** IINA's own EDR handling stays in charge.

For real frame generation on a Mac (Apple's VideoToolbox interpolation and super resolution, or PushFPS's motion
interpolation), use mpv with the PushMPC filter: `brew tap CptPakundo/pushmpc https://github.com/CptPakundo/PushMPC`,
`brew install CptPakundo/pushmpc/pushmpc`, then `pushmpc-setup install`. See `packaging/homebrew`.

## Install

1. Download `PushMPC-<version>.iinaplgz` from the release, or from the `iina-plugin` artifact of the `apple` CI
   workflow.
2. Double-click it, or go to IINA > Settings > Plugins > Install from local package. IINA 1.4 or newer is required.
3. IINA asks for two permissions:
   - **Show OSD:** a one-line notice when the preset changes.
   - **File system:** writes the bundled shaders to the plugin's own data folder (mpv needs real files), and reads
     the chip name with `sysctl`.
4. Change settings in IINA > Settings > Plugins > PushMPC, or from the Plugin > PushMPC menu while a video plays.

## Building the package

`python3 build.py build --out dist` does the following:
- fetches the shaders pinned in `shaders.lock.json` (each URL is immutable, and each file is checked against its git
  blob SHA-1 or SHA-256);
- refuses any shader with a compute pass;
- generates `shaders.js` and checks the JavaScript with `node --check`;
- packs `dist/PushMPC-<version>.iinaplgz`, a zip of the plugin folder's contents, the same as `iina-plugin pack`.

`python3 build.py validate` checks only the plugin folder: `Info.json`, the files it names, and the preference keys.
CI runs both (`.github/workflows/apple.yml`, job `iina`).

Shader licences: FSR port MIT, RAVU LGPL-3.0, FSRCNNX GPL-3.0. All are compatible with PushMPC's GPL-3.0. Each file
keeps its own header, and the package lists them in `SHADER-LICENSES.txt`.

## Not pursued: swapping IINA's libmpv

Swapping `IINA.app/Contents/Frameworks/libmpv.2.dylib` for a VapourSynth-enabled one (SVP's route) breaks IINA's code
signature, drifts with every IINA update and is undone by IINA's updater. The bridge above reaches the same engine
without touching the bundle.
