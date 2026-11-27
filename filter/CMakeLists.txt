# PushMpcFilter.ax: the PushMPC DirectShow filter (LAV Video -> PushMpcFilter (pushcore, GPU in / GPU out) -> stock MPC-VR),
# PushMpcExit.dll (fast process exit with the warm NGX core) and auto_test (headless check of the Auto rules).
cmake_minimum_required(VERSION 3.24)
project(PushMpcFilter CXX RC)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/msvc-loop-guard.cmake")   # MSVC loop optimiser bug guard (/d2NoLUR)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # /MT like pushcore.dll and MPC-VR

# Paths relative to this folder (repository layout: filter/, pushmpc/, MpcVideoRenderer/); override on the command line.
set(PUSHMPC_CORE_INCLUDE "${CMAKE_SOURCE_DIR}/../pushmpc/include" CACHE PATH "pushcore ABI headers")
set(PUSHMPC_MPCVR_DIR "${CMAKE_SOURCE_DIR}/../MpcVideoRenderer" CACHE PATH "MPC Video Renderer source (BaseClasses, ID3DVideoMemoryConfiguration.h, IMediaSideData.h)")
set(BASECLASSES_DIR "${PUSHMPC_MPCVR_DIR}/external/BaseClasses")
file(GLOB BASECLASSES_SRC "${BASECLASSES_DIR}/*.cpp")
list(FILTER BASECLASSES_SRC EXCLUDE REGEX "stdafx\\.cpp$")
add_library(baseclasses STATIC ${BASECLASSES_SRC})
target_include_directories(baseclasses PUBLIC "${BASECLASSES_DIR}")
target_compile_definitions(baseclasses PUBLIC UNICODE _UNICODE NOMINMAX WIN64 _WIN64 _CRT_SECURE_NO_WARNINGS)
target_compile_options(baseclasses PRIVATE /W0 /wd4996)

add_library(PushMpcFilter SHARED src/PushMpcFilter.cpp src/PushMpcFilter.def src/PushMpcFilter.rc)
set_target_properties(PushMpcFilter PROPERTIES SUFFIX ".ax" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin")
target_include_directories(PushMpcFilter PRIVATE "${PUSHMPC_CORE_INCLUDE}" "${PUSHMPC_MPCVR_DIR}/Include" "${PUSHMPC_MPCVR_DIR}/Source")
# /Z7: debug info in the objects (the linker still writes the PDB), so compiler caches (ccache, sccache) can store them
target_compile_options(PushMpcFilter PRIVATE /W3 /EHsc /O2 /Z7 /utf-8)
target_link_options(PushMpcFilter PRIVATE /DEBUG /OPT:REF /PDBALTPATH:%_PDB%)  # only the PDB file name in the binary, no build path
target_link_libraries(PushMpcFilter PRIVATE baseclasses strmiids winmm avrt version d3d11 dxgi d3dcompiler dxguid advapi32 ole32 oleaut32 user32 shell32 uuid comctl32 bcrypt gdi32)

# The optional image model behind ContentDetect (PushMpcContentModel.h): pmml (../pmml) when the ONNX Runtime headers are
# given (-DPMML_ORT_INCLUDE=...); without them the filter uses the hand-built cues only
if(PMML_ORT_INCLUDE)
  add_subdirectory("${CMAKE_SOURCE_DIR}/../pmml" pmml)
  target_link_libraries(PushMpcFilter PRIVATE pmml)
  target_compile_definitions(PushMpcFilter PRIVATE PUSHMPC_CONTENT_MODEL=1)
endif()

# PushMpcExit.dll: skips the DLL detach handlers of an exiting process (see src/PushMpcExit.cpp); deployed next to the .ax
add_library(PushMpcExit SHARED src/PushMpcExit.cpp)
set_target_properties(PushMpcExit PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin")
target_compile_options(PushMpcExit PRIVATE /W3 /O2)

# Headless check of the Auto rules (prints decisions; no GPU, no MPC-HC)
add_executable(auto_test tools/auto_test.cpp)
set_target_properties(auto_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
target_compile_definitions(auto_test PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(auto_test PRIVATE /W3 /EHsc /O2 /utf-8)
target_link_libraries(auto_test PRIVATE user32 advapi32 shell32)

# ContentDetect (pushmpc/content_detect.h): what the picture is, interlacing and the frame interval, measured in the
# picture and the timestamps (synthetic clips, replay of measured feature traces; CPU only)
add_executable(content_test tools/content_test.cpp)
set_target_properties(content_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
target_include_directories(content_test PRIVATE "${PUSHMPC_CORE_INCLUDE}")
target_compile_options(content_test PRIVATE /W3 /EHsc /O2 /utf-8)

# Held-frame re-timing on the pushcore route (pushmpc/src/HeldFrames.h, PushCoreHeld.h): the rules, the GPU test pass on
# WARP with a synthetic animation clip on twos and threes, and the output spacing (no GPU needed)
add_executable(held_test tools/held_test.cpp)
set_target_properties(held_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
target_include_directories(held_test PRIVATE "${CMAKE_SOURCE_DIR}/../pushmpc/src")
target_compile_definitions(held_test PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(held_test PRIVATE /W3 /EHsc /O2 /utf-8)
target_link_libraries(held_test PRIVATE d3d11 dxgi d3dcompiler)

# The settings reload (src/PushMpcReload.h, CPushMpcFilter::DoReload) against a stub pushcore API whose reload / close
# are slow, blocked or hang beside each other (F-T1): frames keep flowing, Stop returns in bound (CPU only)
add_executable(reload_test tools/reload_test.cpp)
set_target_properties(reload_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
target_include_directories(reload_test PRIVATE "${PUSHMPC_CORE_INCLUDE}" "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(reload_test PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(reload_test PRIVATE /W3 /EHsc /O2 /utf-8)

# The log rotation (src/PushMpcLogs.h): per-process engine logs kept with their sessions, count / size bounds (CPU only)
add_executable(logs_test tools/logs_test.cpp)
set_target_properties(logs_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
target_include_directories(logs_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(logs_test PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(logs_test PRIVATE /W3 /EHsc /O2 /utf-8)
