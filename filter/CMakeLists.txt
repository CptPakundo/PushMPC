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

# LAV Video's settings interface (scripts/lav-pin.json): the double-rate deinterlacer for files measured interlaced. CI
# fetches the pinned header (-DPUSHMPC_FETCH_LAV=ON; its SHA-256 must match the pin), or -DPUSHMPC_LAV_INCLUDE=<dir>
# names a folder that has it; without it the filter builds as before and steps aside for interlaced files.
option(PUSHMPC_FETCH_LAV "download LAV Filters' LAVVideoSettings.h at the tag scripts/lav-pin.json pins" OFF)
set(PUSHMPC_LAV_INCLUDE "" CACHE PATH "a folder with LAVVideoSettings.h")
if(PUSHMPC_FETCH_LAV AND NOT PUSHMPC_LAV_INCLUDE)
  file(READ "${CMAKE_SOURCE_DIR}/../scripts/lav-pin.json" _lav)
  string(JSON _lav_repo GET "${_lav}" repository)
  string(JSON _lav_tag GET "${_lav}" tag)
  string(JSON _lav_path GET "${_lav}" path)
  string(JSON _lav_sha GET "${_lav}" sha256)
  set(_lav_dir "${CMAKE_BINARY_DIR}/lav-include")
  file(DOWNLOAD "https://raw.githubusercontent.com/${_lav_repo}/${_lav_tag}/${_lav_path}" "${_lav_dir}/LAVVideoSettings.h" STATUS _lav_st)
  list(GET _lav_st 0 _lav_rc)
  if(NOT _lav_rc EQUAL 0)
    message(FATAL_ERROR "LAVVideoSettings.h (${_lav_repo} ${_lav_tag}): download failed: ${_lav_st}")
  endif()
  file(SHA256 "${_lav_dir}/LAVVideoSettings.h" _lav_got)
  if(_lav_sha STREQUAL "TODO")
    # not pinned yet: the header is not used (the filter steps aside for interlaced files); the hash to pin is printed
    message(WARNING "LAVVideoSettings.h (${_lav_repo} ${_lav_tag}) is not pinned: its SHA-256 is ${_lav_got}; put it in scripts/lav-pin.json")
    file(REMOVE "${_lav_dir}/LAVVideoSettings.h")
  elseif(NOT _lav_got STREQUAL _lav_sha)
    message(FATAL_ERROR "LAVVideoSettings.h (${_lav_repo} ${_lav_tag}): SHA-256 ${_lav_got}, scripts/lav-pin.json pins ${_lav_sha}")
  else()
    message(STATUS "PushMpcFilter: LAVVideoSettings.h fetched (${_lav_repo} ${_lav_tag}), SHA-256 ${_lav_got} matches scripts/lav-pin.json")
    set(PUSHMPC_LAV_INCLUDE "${_lav_dir}")
  endif()
endif()
if(PUSHMPC_LAV_INCLUDE)
  target_include_directories(PushMpcFilter PRIVATE "${PUSHMPC_LAV_INCLUDE}")
  message(STATUS "PushMpcFilter: LAV Video's settings interface from ${PUSHMPC_LAV_INCLUDE}")
else()
  message(STATUS "PushMpcFilter: no LAVVideoSettings.h: interlaced files pass through for the renderer to deinterlace")
endif()

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

# Live check of the late reload take-over (needs the GPU and an installed MPC Video Renderer; run only through
# tools/late_reload_live.ps1, holding the GPU lock): loads bin\PushMpcFilter.ax directly, no registration
add_executable(late_reload_host tools/late_reload_host.cpp)
set_target_properties(late_reload_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
target_compile_definitions(late_reload_host PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(late_reload_host PRIVATE /W3 /EHsc /O2 /utf-8)
target_link_libraries(late_reload_host PRIVATE strmiids ole32 user32 advapi32 shell32)

# The pass-through's CPU conversions (src/PushMpcPassConvert.h: YUY2, RGB32 for renderers that refuse NV12 / P010)
add_executable(pass_convert_test tools/pass_convert_test.cpp)
set_target_properties(pass_convert_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
target_include_directories(pass_convert_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(pass_convert_test PRIVATE /W3 /EHsc /O2 /utf-8)
