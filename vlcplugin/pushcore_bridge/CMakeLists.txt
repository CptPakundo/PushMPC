# pm_pushcore.dll (MSVC, Windows x64): pushcore for the VLC module. Next to libpushmpc_plugin.dll; the module loads it
# at run time and falls back to the portable engine when it or pushcore.dll is missing.
cmake_minimum_required(VERSION 3.20)
project(pm_pushcore CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/msvc-loop-guard.cmake")   # MSVC loop optimiser bug guard (/d2NoLUR)
if(NOT MSVC)
  message(FATAL_ERROR "pm_pushcore is built with MSVC (pushcore host uses structured exception handling)")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: one self-contained DLL
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_library(pm_pushcore SHARED pm_pushcore.cpp "${ROOT}/vsplugin/src/corehost.cpp")
target_include_directories(pm_pushcore PRIVATE . "${ROOT}/vsplugin/src" "${ROOT}/filter/src" "${ROOT}/pushmpc/include" "${ROOT}/mpvnative/include")
target_compile_definitions(pm_pushcore PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(pm_pushcore PRIVATE /W3 /EHsc /O2 /utf-8)
target_link_libraries(pm_pushcore PRIVATE d3d11 dxgi d3dcompiler user32 advapi32 shell32 ole32)

# the zero-copy path on WARP with the stub core (CI): pm_pc_zc_test <pm_pushcore.dll> <stub folder>
add_executable(pm_pc_zc_test pm_pc_zc_test.cpp)
target_compile_definitions(pm_pc_zc_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pm_pc_zc_test PRIVATE /W3 /EHsc /O2 /utf-8)
target_link_libraries(pm_pc_zc_test PRIVATE d3d11)

# the configuration ABI (ABI 2 callers, ABI 3 sizes, upscaler, updates) on WARP with the stub core (PUSHCORE_STUB_SR):
# pm_pc_abi_test <pm_pushcore.dll> <stub folder> <scratch folder>
add_executable(pm_pc_abi_test pm_pc_abi_test.cpp)
target_compile_definitions(pm_pc_abi_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pm_pc_abi_test PRIVATE /W3 /EHsc /O2 /utf-8)
target_include_directories(pm_pc_abi_test PRIVATE "${ROOT}/pushmpc/include")
target_link_libraries(pm_pc_abi_test PRIVATE d3d11)
