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
add_library(pushcore_dll SHARED src/PushCoreGpu.cpp src/PushCoreDeband.h include/pushmpc/pushcore_abi.h ${CMAKE_CURRENT_BINARY_DIR}/pushcore.rc
            src/PushCoreZludaVsr.cpp src/PushCoreZludaVsr.h src/ZludaVsrClient.h include/pushmpc/zluda_vsr.h)
set_target_properties(pushcore_dll PROPERTIES OUTPUT_NAME pushcore ARCHIVE_OUTPUT_NAME pushcore_import)
target_compile_features(pushcore_dll PRIVATE cxx_std_20)
target_compile_options(pushcore_dll PRIVATE /utf-8)
target_include_directories(pushcore_dll PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${FFMPEG_INCLUDE_DIRS})
# d3dcompiler: PushMPC's deband compute shader (PushCoreDeband.h) is compiled at open with the system's d3dcompiler_47.dll
target_link_libraries(pushcore_dll PRIVATE veyra_engine ${FFMPEG_LIBRARIES} d3d11 d3d12 dxgi d3dcompiler)

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
