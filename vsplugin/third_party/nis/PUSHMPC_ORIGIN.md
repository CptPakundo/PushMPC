# NVIDIA Image Scaling SDK 1.0.3

Upstream: https://github.com/NVIDIAGameWorks/NVIDIAImageScaling, commit 35e13ba316c98eeecf16f37eae70ce88019911f6 (2022-08-22),
taken unchanged from PushFPS v4.5.3 `third_party/nis` (which records the same commit). Licence: MIT (`LICENSE.txt`).

| File | SHA-256 |
|---|---|
| NIS_Scaler.h | 2d3bd5b45d6a0c4169fe36dfe349e7ff5a099dff6fb6876f8a3286489f74d3bf |
| NIS_Config.h | f33c5dd82bb63f3c1cc7cefd86690053de0bd3fe0e730463a5e63e90e7778754 |

Used by the portable GPU engine: `engines/gpu/shaders/nis.comp` compiles `NIS_Scaler.h` in its GLSL form; `gpu_engine.cpp` uses
`NIS_Config.h` for the shader constants and the half-float coefficient tables. Source only: no NVIDIA binary.
