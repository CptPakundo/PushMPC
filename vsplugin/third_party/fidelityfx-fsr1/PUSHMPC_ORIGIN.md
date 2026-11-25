# AMD FidelityFX Super Resolution 1 (from the FidelityFX SDK 1.1.4)

From the FidelityFX SDK 1.1.4 (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), `sdk/include/FidelityFX/gpu/`: the GPU
headers `ffx_core*.h`, `ffx_common_types.h` and `fsr1/ffx_fsr1.h` (FSR1 1.2.0), copied unchanged. Licence: MIT (`LICENSE.txt`).

Used by the portable GPU engine: `engines/gpu/shaders/easu.comp` and `rcas.comp` compile them in their GLSL, 32-bit form
(EASU upscaling, then RCAS sharpening, on the luma plane).
