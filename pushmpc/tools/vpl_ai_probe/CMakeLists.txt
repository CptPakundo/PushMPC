# Standalone build of vpl_ai_probe (not part of the pushcore build).
# With the Intel VPL dispatcher: cmake -S . -B build -DCMAKE_PREFIX_PATH=<libvpl install>  (find_package(VPL))
# Without it the probe still builds and runs its baselines, and reports VplFi/VplSr "unsupported".
cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
project(vpl_ai_probe CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/msvc-loop-guard.cmake")   # MSVC loop optimiser bug guard (/d2NoLUR)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # one self-contained exe for testers

add_executable(vpl_ai_probe ${CMAKE_CURRENT_SOURCE_DIR}/../vpl_ai_probe.cpp)
target_compile_options(vpl_ai_probe PRIVATE /utf-8 /W3)
target_compile_definitions(vpl_ai_probe PRIVATE UNICODE _UNICODE)
target_link_libraries(vpl_ai_probe PRIVATE dxgi)

option(VPL_PROBE_REQUIRE_VPL "Fail when the VPL dispatcher is not found" OFF)
find_package(VPL CONFIG QUIET)
if(VPL_FOUND)
  message(STATUS "VPL dispatcher ${VPL_VERSION}: building with HAVE_VPL")
  target_link_libraries(vpl_ai_probe PRIVATE VPL::dispatcher)
  # ONEVPL_EXPERIMENTAL exposes MFX_AI_FRAME_INTERPOLATION_MODE_BEST_SPEED/BEST_QUALITY and MFX_FRCALGM_AI_FRAME_INTERPOLATION
  target_compile_definitions(vpl_ai_probe PRIVATE HAVE_VPL ONEVPL_EXPERIMENTAL)
elseif(VPL_PROBE_REQUIRE_VPL)
  message(FATAL_ERROR "VPL dispatcher not found (set CMAKE_PREFIX_PATH to a libvpl install)")
else()
  message(WARNING "VPL dispatcher not found: building the baseline-only probe")
endif()
