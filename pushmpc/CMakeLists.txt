# PushMPC: PushFPS's enhancement graph as a library for an MPC-HC filter.
# Built inside PushFPS's CMake tree (it needs veyra_engine): configure PushFPS with
#   -DPUSHMPC_SOURCE_DIR=<this folder>
add_library(pushcore STATIC src/PushCore.cpp include/pushmpc/PushCore.h)
target_compile_features(pushcore PUBLIC cxx_std_20)
target_compile_options(pushcore PRIVATE /utf-8)
target_include_directories(pushcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(pushcore PUBLIC veyra_engine)

add_executable(pushcore_harness tools/pushcore_harness.cpp)
target_compile_features(pushcore_harness PRIVATE cxx_std_20)
target_compile_options(pushcore_harness PRIVATE /utf-8)
target_include_directories(pushcore_harness PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(pushcore_harness PRIVATE pushcore ${FFMPEG_LIBRARIES})
target_link_options(pushcore_harness PRIVATE /STACK:8388608)

# pushcore.dll: GPU-in, GPU-out core behind the C ABI (include/pushmpc/pushcore_abi.h).
# Static CRT (/MT) like the rest of the tree; loaded by full path from the PushFPS folder.
# The PushFPS version this core is built against goes into the DLL's version resource ("PushFPSVersion");
# the renderer compares it with the installed PushFPS.exe before loading anything (version skew guard).
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" PUSHMPC_PUSHFPS_VERSION "${VEYRA_DISPLAY_VERSION}")
if(NOT PUSHMPC_PUSHFPS_VERSION)
  set(PUSHMPC_PUSHFPS_VERSION "0.0.0")
endif()
string(REPLACE "." "," PUSHMPC_VER_COMMA "${PUSHMPC_PUSHFPS_VERSION},0")
set(PUSHMPC_ABI 3)
# what this core is built with (version resource "PushCoreFeatures"): "nvof" when the NVIDIA Optical Flow SDK headers were
# found (the same test as PushFPS's veyra_nvof target), else "no-nvof" (NVIDIA VFG cannot start; release.yml refuses it)
if(VEYRA_NVOF_SDK_ROOT AND EXISTS "${VEYRA_NVOF_SDK_ROOT}/NvOFInterface/nvOpticalFlowCommon.h")
  set(PUSHMPC_FEATURES "nvof")
else()
  set(PUSHMPC_FEATURES "no-nvof")
endif()
execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short=7 HEAD OUTPUT_VARIABLE PUSHMPC_PUSHFPS_COMMIT
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
configure_file(res/pushcore.rc.in ${CMAKE_CURRENT_BINARY_DIR}/pushcore.rc @ONLY)
add_library(pushcore_dll SHARED src/PushCoreGpu.cpp src/PushCoreDlssgPort.cpp src/PushCoreDlssgPort.h src/PushCoreDeband.h src/PushCoreAnimeSr.h include/pushmpc/pushcore_abi.h ${CMAKE_CURRENT_BINARY_DIR}/pushcore.rc
            src/PushCoreZludaVsr.cpp src/PushCoreZludaVsr.h src/ZludaVsrClient.h include/pushmpc/zluda_vsr.h)
set_target_properties(pushcore_dll PROPERTIES OUTPUT_NAME pushcore ARCHIVE_OUTPUT_NAME pushcore_import)
target_compile_features(pushcore_dll PRIVATE cxx_std_20)
target_compile_options(pushcore_dll PRIVATE /utf-8)
target_include_directories(pushcore_dll PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${FFMPEG_INCLUDE_DIRS})
# d3dcompiler: PushMPC's deband compute shader (PushCoreDeband.h) is compiled at open with the system's d3dcompiler_47.dll
target_link_libraries(pushcore_dll PRIVATE veyra_engine ${FFMPEG_LIBRARIES} d3d11 d3d12 dxgi d3dcompiler)
# Vendor self-tests (pushcore_selfTest; PushCoreSelfTest.h: each method registers its own test and feature bits) and their
# test tool (CI on WARP, testers on hardware: tests/xess-fg-manual.ps1, tests/intel-vsr-manual.ps1, tests/intel-vpl-manual.ps1)
target_sources(pushcore_dll PRIVATE src/PushCoreSelfTest.cpp src/PushCoreSelfTest.h)
add_executable(pushcore_selftest tools/pushcore_selftest.cpp)
target_compile_features(pushcore_selftest PRIVATE cxx_std_20)
target_compile_options(pushcore_selftest PRIVATE /utf-8)
target_include_directories(pushcore_selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# the anime upscalers' TensorRT-RTX zero copy: the CUDA driver API types (nvcuda.dll is loaded at run time)
if(VEYRA_CUDA_DRIVER_INCLUDE_DIR)
  target_include_directories(pushcore_dll PRIVATE "${VEYRA_CUDA_DRIVER_INCLUDE_DIR}")
endif()
# anime upscalers (src/PushCoreAnimeSr.h): pmml, when PushFPS's tree builds it (PUSHMPC_PMML_ORT_INCLUDE); without it the
# capability says so and srMethod PMML falls back to RTX VSR / FSR
if(TARGET pmml)
  target_link_libraries(pushcore_dll PRIVATE pmml)
  target_compile_definitions(pushcore_dll PRIVATE PUSHMPC_HAVE_PMML=1)
endif()

# GPU harness: loads pushcore.dll by path, decodes with D3D11VA on its own D3D11 device (as LAV does),
# feeds the textures in and "presents" the outputs in D3D11. FFmpeg is delay-loaded so the
# exe can run from anywhere: it resolves to the PushFPS folder's DLLs that pushcore.dll pulled in.
add_executable(pushcore_gpu_harness tools/pushcore_gpu_harness.cpp)
target_compile_features(pushcore_gpu_harness PRIVATE cxx_std_20)
target_compile_options(pushcore_gpu_harness PRIVATE /utf-8)
target_include_directories(pushcore_gpu_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(pushcore_gpu_harness PRIVATE ${FFMPEG_LIBRARIES} d3d11 dxgi windowscodecs ole32 delayimp)
target_link_options(pushcore_gpu_harness PRIVATE /DELAYLOAD:avformat-63.dll /DELAYLOAD:avcodec-63.dll
  /DELAYLOAD:avutil-61.dll /DELAYLOAD:avfilter-12.dll /DELAYLOAD:swscale-10.dll /DELAYLOAD:swresample-7.dll)

# G10: Intel XeSS frame generation (headless swap chain) and XeSS SR need the XeSS SDK headers (PushFPS's VEYRA_XESS_ROOT,
# fetched by release.yml); without them pushcore builds and reports both as "built without the Intel XeSS SDK"
# (PushCoreXessStub.cpp). XeSS SR also needs the FsrSrBackend provider slot (the XeSS hunks of patches/pushfps-4.5.8-pushmpc.patch).
if(VEYRA_XESS_ROOT AND EXISTS "${VEYRA_XESS_ROOT}/inc/xess_fg/xefg_swapchain_d3d12.h" AND EXISTS "${VEYRA_XESS_ROOT}/inc/xess/xess_d3d12.h")
  target_sources(pushcore_dll PRIVATE src/PushCoreXefg.cpp src/PushCoreXessSr.cpp)
  target_include_directories(pushcore_dll PRIVATE "${VEYRA_XESS_ROOT}/inc")
else()
  target_sources(pushcore_dll PRIVATE src/PushCoreXessStub.cpp)
  message(STATUS "pushcore: XeSS SDK headers not found (VEYRA_XESS_ROOT): XeFG and XeSS SR are built as unavailable")
endif()
target_sources(pushcore_dll PRIVATE src/PushCoreXess.h src/PushCoreXessCaps.cpp)
target_link_libraries(pushcore_dll PRIVATE bcrypt wintrust crypt32 version)

# Test tool: edits a COPY of a PushFPS chain session (multiplier, SR/FG switches) the way PushFPS saves it,
# for the renderer's live-reload test. Never point it at the user's own data folder.
add_executable(pushcore_session_edit tools/pushcore_session_edit.cpp)
target_compile_features(pushcore_session_edit PRIVATE cxx_std_20)
target_compile_options(pushcore_session_edit PRIVATE /utf-8)
target_include_directories(pushcore_session_edit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(pushcore_session_edit PRIVATE veyra_engine ${FFMPEG_LIBRARIES})

# Test tool: PushFlow end to end on a D3D12 device (WARP on CI) with the network on DirectML or on pmml
# (VEYRA_PUSHFLOW_PMML), the generated frame read back for a pixel comparison (.github/workflows/release.yml).
add_executable(pushflow_pmml_check tools/pushflow_pmml_check.cpp)
target_compile_features(pushflow_pmml_check PRIVATE cxx_std_20)
target_compile_options(pushflow_pmml_check PRIVATE /utf-8)
target_include_directories(pushflow_pmml_check PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(pushflow_pmml_check PRIVATE veyra_engine ${FFMPEG_LIBRARIES} d3d12 dxgi)

# The AMD HIP self-test child (PushFPS's AmdHipPreflight; PushFPS.exe --amd-hip-preflight): shipped in core\ next to
# pushcore.dll, which points AmdHipPreflight at it on AMD GPUs (RX 9000 NR, lmxxf).
add_executable(pushcore_hip_preflight tools/pushcore_hip_preflight.cpp)
target_compile_features(pushcore_hip_preflight PRIVATE cxx_std_20)
target_compile_options(pushcore_hip_preflight PRIVATE /utf-8)
target_include_directories(pushcore_hip_preflight PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(pushcore_hip_preflight PRIVATE veyra_engine ${FFMPEG_LIBRARIES} shell32)

# pushmpc_release: everything a release takes from this tree (every PushFPS shader target, pushcore.dll, the HIP
# preflight child, the GPU harness), one target for "scripts\build-pushmpc.cmd release". The shader targets are
# collected once PushFPS's own CMakeLists.txt is done (a deferred call), so the list follows PushFPS without a patch hunk.
function(pushmpc_collect_shader_targets dir out)
  get_property(_t DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  list(FILTER _t INCLUDE REGEX "^veyra_shader_[A-Za-z0-9_]+$")
  get_property(_subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(_s IN LISTS _subs)
    pushmpc_collect_shader_targets("${_s}" _st)
    list(APPEND _t ${_st})
  endforeach()
  set(${out} ${_t} PARENT_SCOPE)
endfunction()
function(pushmpc_add_release_target)
  pushmpc_collect_shader_targets("${CMAKE_SOURCE_DIR}" _shaders)
  add_custom_target(pushmpc_release)
  add_dependencies(pushmpc_release ${_shaders} pushcore_dll pushcore_hip_preflight pushcore_gpu_harness pushcore_zluda_vsr_worker pmml_winml_ep)
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL pushmpc_add_release_target)

# G10 Intel video: the D3D11 bridge device beside the graph, Intel VSR (driver video processor) before it, Intel VPL frame
# interpolation after it (PushCoreIntel.h)
target_sources(pushcore_dll PRIVATE src/PushCoreD3D11Bridge.cpp src/PushCoreD3D11Bridge.h src/PushCoreIntelVsr.cpp src/PushCoreIntelVsr.h
  src/PushCoreVplFi.cpp src/PushCoreVplFi.h src/PushCoreIntel.cpp src/PushCoreIntel.h)
# Intel VPL (Intel Video Processing Library): its MIT dispatcher, static with the static CRT, linked into pushcore.dll for
# PUSHCORE_FG_VPL (AI frame interpolation; the runtime itself comes with Intel's graphics driver). PUSHMPC_VPL_ROOT = a
# libvpl install (.github/workflows/intel-video.yml and release.yml build it from the pinned tag). Without it pushcore
# builds and reports VPL as "built without Intel VPL".
set(PUSHMPC_VPL_ROOT "" CACHE PATH "libvpl install (include/vpl, lib/vpl.lib) for Intel VPL frame interpolation")
if(PUSHMPC_VPL_ROOT AND EXISTS "${PUSHMPC_VPL_ROOT}/include/vpl/mfx.h" AND EXISTS "${PUSHMPC_VPL_ROOT}/lib/vpl.lib")
  message(STATUS "pushcore: Intel VPL dispatcher from ${PUSHMPC_VPL_ROOT}")
  target_include_directories(pushcore_dll PRIVATE "${PUSHMPC_VPL_ROOT}/include")
  target_compile_definitions(pushcore_dll PRIVATE PUSHMPC_HAVE_VPL ONEVPL_EXPERIMENTAL)
  target_link_libraries(pushcore_dll PRIVATE "${PUSHMPC_VPL_ROOT}/lib/vpl.lib" ole32 uuid advapi32)
else()
  message(STATUS "pushcore: no Intel VPL dispatcher (PUSHMPC_VPL_ROOT): VPL frame interpolation not built")
endif()

# G11: RTX VSR on AMD RX 7000 / 9000 through ZLUDA. pushcore starts this worker (shipped in core\ next to pushcore.dll);
# it loads ZLUDA (runtime\amd-nr-rdna3\zluda) and nvngx_vsr.dll in its own process. Needs only the NGX SDK headers
# (VEYRA_DLSS_SDK_ROOT/include) and NVIDIA's cuda.h (VEYRA_CUDA_DRIVER_INCLUDE_DIR); everything else is loaded at run time.
# zluda_vsr_test (CPU + WARP, the stub worker built with PUSHMPC_ZLUDA_VSR_STUB) checks the protocol and the refusals.
add_executable(pushcore_zluda_vsr_worker tools/zluda_vsr_worker.cpp)
target_compile_features(pushcore_zluda_vsr_worker PRIVATE cxx_std_20)
target_compile_options(pushcore_zluda_vsr_worker PRIVATE /utf-8)
target_compile_definitions(pushcore_zluda_vsr_worker PRIVATE UNICODE _UNICODE)
target_include_directories(pushcore_zluda_vsr_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${VEYRA_DLSS_SDK_ROOT}/include ${VEYRA_CUDA_DRIVER_INCLUDE_DIR})
target_link_libraries(pushcore_zluda_vsr_worker PRIVATE d3d12 dxgi advapi32 user32)
add_executable(zluda_vsr_stub_worker tools/zluda_vsr_worker.cpp)
target_compile_features(zluda_vsr_stub_worker PRIVATE cxx_std_20)
target_compile_options(zluda_vsr_stub_worker PRIVATE /utf-8)
target_compile_definitions(zluda_vsr_stub_worker PRIVATE UNICODE _UNICODE PUSHMPC_ZLUDA_VSR_STUB=1)
target_include_directories(zluda_vsr_stub_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zluda_vsr_stub_worker PRIVATE d3d12 dxgi advapi32 user32)
add_executable(zluda_vsr_test tools/zluda_vsr_test.cpp)
target_compile_features(zluda_vsr_test PRIVATE cxx_std_20)
target_compile_options(zluda_vsr_test PRIVATE /utf-8)
target_compile_definitions(zluda_vsr_test PRIVATE UNICODE _UNICODE)
target_include_directories(zluda_vsr_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zluda_vsr_test PRIVATE d3d12 dxgi advapi32 user32)

# G11: AMD's GPU EP for PushFlow (pmml): asks Windows ML's EP catalog for AMDGPUExecutionProvider and writes
# eps\amdgpu\ep.json (Setup runs it; shipped in core\). No build dependency: the catalog's C API is loaded at run time.
add_executable(pmml_winml_ep ${CMAKE_CURRENT_SOURCE_DIR}/../pmml/tools/pmml_winml_ep.cpp)
target_compile_features(pmml_winml_ep PRIVATE cxx_std_17)
target_compile_options(pmml_winml_ep PRIVATE /utf-8)
target_compile_definitions(pmml_winml_ep PRIVATE UNICODE _UNICODE)
