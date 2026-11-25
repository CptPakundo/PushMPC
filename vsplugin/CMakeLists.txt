# pushmpc_vs: the VapourSynth plugin pushmpc.Enhance (R1 and R3, players-all.md) with its backends, the stub pushcore.dll
# for tests and CI, and the tests:
#   portable_test  every OS: backend selection, the CPU budget, settings.ini and folders, the portable engine on
#                  synthetic frames (no VapourSynth, no GPU);
#   sched_test     Windows: scheduling, rates, reuse, bandwidth, Auto glue;
#   core_test      Windows: the D3D11 host and the pipeline against the stub on WARP.
# Backends: nvidia (pushcore, Windows only) and portable (PushFPS's portable core, third_party/pushfps-portable, GPL-3)
# are compiled in; rife is a script preset in pushmpc.vpy. The plugin needs VapourSynth's SDK headers (VapourSynth4.h,
# VSHelper4.h, LGPL) in third_party/vapoursynth; without them only the stub and the tests are built.
# The same tree builds with MSVC on Windows, gcc / clang on Linux and clang on macOS:
#   cmake -S vsplugin -B vsplugin/build -DCMAKE_BUILD_TYPE=Release && cmake --build vsplugin/build && ctest --test-dir vsplugin/build
cmake_minimum_required(VERSION 3.21)
project(PushMpcVsPlugin CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)        # the portable library goes into a shared plugin on Linux / macOS
enable_testing()

set(PUSHMPC_CORE_INCLUDE "${CMAKE_SOURCE_DIR}/../pushmpc/include" CACHE PATH "pushcore ABI headers")
set(PUSHMPC_FILTER_SRC "${CMAKE_SOURCE_DIR}/../filter/src" CACHE PATH "the filter's Auto rules (PushMpcAuto.h, PushMpcSettings.h; reused header-only)")
set(VAPOURSYNTH_SDK "${CMAKE_SOURCE_DIR}/third_party/vapoursynth" CACHE PATH "VapourSynth4.h and VSHelper4.h")
set(PMVS_BIN "${CMAKE_SOURCE_DIR}/bin")
set(PFP "${CMAKE_SOURCE_DIR}/third_party/pushfps-portable")
find_package(Threads REQUIRED)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # /MT like pushcore.dll
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  add_compile_options(/W3 /EHsc /O2 /Zi /utf-8)
  add_link_options(/DEBUG /OPT:REF /PDBALTPATH:%_PDB%)   # only the PDB file name in the binaries
else()
  add_compile_options(-O2 -Wall -Wextra -Wno-unused-parameter)
endif()

# PushFPS's portable core (motion estimation, interpolation reference), GPL-3, copied unmodified
add_library(pushfps_portable STATIC "${PFP}/src/Motion.cpp" "${PFP}/src/Interpolate.cpp")
target_include_directories(pushfps_portable PUBLIC "${PFP}/include")
target_link_libraries(pushfps_portable PUBLIC Threads::Threads)
# the plugin's portable engine (planar YUV interpolation, scene cuts, the CPU budget's calibration)
add_library(pmvs_portable STATIC engines/portable/portable_engine.cpp)
target_include_directories(pmvs_portable PUBLIC engines/portable src)
target_link_libraries(pmvs_portable PUBLIC pushfps_portable)

add_executable(portable_test tests/portable_test.cpp)
set_target_properties(portable_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
target_link_libraries(portable_test PRIVATE pmvs_portable)
add_test(NAME portable_test COMMAND portable_test)

if(WIN32)
  # the stub pushcore.dll (same exports as the real one; CPU blend, runs on WARP)
  add_library(pushcore_stub SHARED stub/pushcore_stub.cpp)
  set_target_properties(pushcore_stub PROPERTIES OUTPUT_NAME pushcore RUNTIME_OUTPUT_DIRECTORY "${PMVS_BIN}/stub" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PMVS_BIN}/stub")
  target_include_directories(pushcore_stub PRIVATE "${PUSHMPC_CORE_INCLUDE}")
  target_link_libraries(pushcore_stub PRIVATE d3d11 dxgi)

  # unit tests of the scheduling and Auto glue
  add_executable(sched_test tests/sched_test.cpp)
  set_target_properties(sched_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
  target_include_directories(sched_test PRIVATE src "${PUSHMPC_CORE_INCLUDE}" "${PUSHMPC_FILTER_SRC}")
  target_link_libraries(sched_test PRIVATE user32 advapi32 shell32 ole32)
  add_test(NAME sched_test COMMAND sched_test)

  # the D3D11 host against the stub
  add_executable(core_test tests/core_test.cpp src/corehost.cpp)
  set_target_properties(core_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
  target_include_directories(core_test PRIVATE src "${PUSHMPC_CORE_INCLUDE}")
  target_link_libraries(core_test PRIVATE d3d11 dxgi d3dcompiler user32 advapi32 shell32 ole32)
endif()

if(EXISTS "${VAPOURSYNTH_SDK}/VapourSynth4.h")
  # Windows: pushmpc_vs.dll with both backends; Linux / macOS: pushmpc_vs.so / .dylib with the portable backend
  set(PMVS_SOURCES src/pushmpc_vs.cpp src/portable_vs.cpp)
  if(WIN32)
    list(APPEND PMVS_SOURCES src/corehost.cpp)
  endif()
  # the ml backend: PushFlow through pmml (../pmml), when the ONNX Runtime headers are given (-DPMML_ORT_INCLUDE=...)
  set(PMVS_HAVE_PMML 0)
  if(PMML_ORT_INCLUDE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/../pmml" pmml)
    list(APPEND PMVS_SOURCES src/ml_vs.cpp)
    set(PMVS_HAVE_PMML 1)
  endif()
  add_library(pushmpc_vs SHARED ${PMVS_SOURCES})
  target_compile_definitions(pushmpc_vs PRIVATE PMVS_HAVE_PMML=${PMVS_HAVE_PMML})
  if(PMVS_HAVE_PMML)
    target_link_libraries(pushmpc_vs PRIVATE pmml)
    target_include_directories(pushmpc_vs PRIVATE engines/ml)
  endif()
  set_target_properties(pushmpc_vs PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${PMVS_BIN}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PMVS_BIN}"
                                              LIBRARY_OUTPUT_DIRECTORY "${PMVS_BIN}" LIBRARY_OUTPUT_DIRECTORY_RELEASE "${PMVS_BIN}")
  target_include_directories(pushmpc_vs PRIVATE src "${VAPOURSYNTH_SDK}" "${PUSHMPC_CORE_INCLUDE}")
  target_link_libraries(pushmpc_vs PRIVATE pmvs_portable)
  if(WIN32)
    target_include_directories(pushmpc_vs PRIVATE "${PUSHMPC_FILTER_SRC}")
    target_link_libraries(pushmpc_vs PRIVATE d3d11 dxgi d3dcompiler user32 advapi32 shell32 ole32)
  else()
    set_target_properties(pushmpc_vs PROPERTIES CXX_VISIBILITY_PRESET hidden)   # only VapourSynthPluginInit2 (VS_EXTERNAL_API)
  endif()
  # the harness that loads the plugin through the VapourSynth API (needs a VapourSynth runtime to run)
  if(WIN32)
    add_executable(vs_harness tests/vs_harness.cpp)
    set_target_properties(vs_harness PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
    target_include_directories(vs_harness PRIVATE "${VAPOURSYNTH_SDK}")
  endif()
else()
  message(WARNING "VapourSynth SDK headers not found in ${VAPOURSYNTH_SDK}: pushmpc_vs and vs_harness are not built (see third_party/README.md)")
endif()
