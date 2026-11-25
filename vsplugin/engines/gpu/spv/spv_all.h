// SPIR-V of the portable GPU engine's compute shaders (engines/gpu/shaders), one uint32_t array per shader. Made by the
// pinned glslang (gpu-pins.json) with --vn kSpv_<name>; a build with -DPMVS_GLSLANG=<glslangValidator> makes them anew
// in the build folder, CI checks that these match.
#pragma once
#include <cstdint>
#include "pyr0.h"
#include "pyrdown.h"
#include "dedup.h"
#include "me.h"
#include "mefinal.h"
#include "mad.h"
#include "fbcheck.h"
#include "interp.h"
#include "nis.h"
#include "easu.h"
#include "rcas.h"
#include "pack.h"
#include "ofconv.h"
#include "blur.h"
