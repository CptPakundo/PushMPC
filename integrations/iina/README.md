# PushMPC for IINA

IINA plays video through its own copy of libmpv, and that copy is built without VapourSynth. So IINA cannot load the
PushMPC filter (`pushmpc.Enhance`), and it has no frame generation. This plugin gives IINA the best picture it can
manage without a different libmpv:

- **Upscaling shaders, picked for your chip.** IINA renders through OpenGL 4.1, so only fragment and textureGather
  shaders work, not compute shaders.
  - Light: FSR 1. Default on base M1 and M2.
  - Balanced: RAVU-Lite r3. Default on base M3 and newer.
  - High: FSRCNNX 8, or RAVU-Lite r4 for anime. Default on Pro chips.
  - Ultra: FSRCNNX 16, or RAVU-Lite r4 for anime. Default on Max, Ultra, and M4 or newer Pro chips.

  A shader runs only when the video is drawn larger than its own size.
- **Smooth motion:** `video-sync=display-resample`, `interpolation` and `tscale=oversample`. Each frame stays on screen
  for its exact share of display refreshes, so a 24 fps film does not judder on a 60 Hz, 120 Hz or ProMotion display.
  This is not frame generation.
- **HDR is left alone.** IINA's own EDR handling stays in charge.

For real frame generation on a Mac (Apple's VideoToolbox interpolation and super resolution, or PushFPS's motion
interpolation), use mpv with the PushMPC filter: `brew install pushmpc`, then `pushmpc-setup install`. See
`packaging/homebrew`.

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

## Later: IINA with a VapourSynth-enabled libmpv (not implemented)

Swapping IINA's `IINA.app/Contents/Frameworks/libmpv.2.dylib` for Homebrew's (which has VapourSynth) is SVP's route.
It would let IINA run the full PushMPC filter, including the VideoToolbox backend. It has costs:
- it breaks IINA's code signature, which then needs an ad-hoc re-sign;
- the ABI can drift with every IINA update;
- IINA's updater overwrites the swap.

A cleaner fix would be upstream: IINA's `deps-buildscripts` building mpv with `-Dvapoursynth=enabled`, linked weakly.
Both routes are listed in the internal plan as later work.
