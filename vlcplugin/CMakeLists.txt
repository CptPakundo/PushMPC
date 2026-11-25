# PushMPC for VLC 3.0.x: libpushmpc_plugin.{so,dll,dylib}
#   -DVLC_PLUGIN_INCLUDE=<dir with vlc_common.h>   (Linux: /usr/include/vlc/plugins; Windows/macOS: <vlc-win64-7z>/sdk/include/vlc/plugins)
#   -DVLC_CORE_LIB=<libvlccore import library>      (Windows: <sdk>/lib/libvlccore.dll.a; Linux: libvlccore.so; macOS: none,
#                                                    symbols resolve from VLC's libvlccore at load)
#   -DPM_ALT_ENTRY=<entry name>                     (the plug-in entry name the headers do not define; see pushmpc_vlc.c)
# The engine is vsplugin's portable engine (PushFPS's portable core, GPL-3), linked statically, with the C++ runtime
# linked statically too, so the plugin only depends on libvlccore and the C library.
cmake_minimum_required(VERSION 3.20)
project(pushmpc_vlc C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)          # VLC's headers want gnu11
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(VLC_PLUGIN_INCLUDE "" CACHE PATH "VLC 3 plugin headers")
set(VLC_CORE_LIB "" CACHE FILEPATH "libvlccore to link (optional on macOS)")
set(PM_ALT_ENTRY "" CACHE STRING "second plug-in entry name to export")
if(NOT EXISTS "${VLC_PLUGIN_INCLUDE}/vlc_common.h")
  message(FATAL_ERROR "VLC_PLUGIN_INCLUDE must point at the VLC 3 plugin headers (vlc_common.h)")
endif()

set(VS "${CMAKE_CURRENT_SOURCE_DIR}/../vsplugin")
find_package(Threads REQUIRED)
add_library(pm_pushfps_portable STATIC "${VS}/third_party/pushfps-portable/src/Motion.cpp" "${VS}/third_party/pushfps-portable/src/Interpolate.cpp")
target_include_directories(pm_pushfps_portable PUBLIC "${VS}/third_party/pushfps-portable/include")
add_library(pm_engine_portable STATIC "${VS}/engines/portable/portable_engine.cpp" src/engine_portable.cpp)
target_include_directories(pm_engine_portable PUBLIC "${VS}/engines/portable" "${VS}/src" src)
target_link_libraries(pm_engine_portable PUBLIC pm_pushfps_portable Threads::Threads)

# the portable GPU engine (vsplugin/engines/gpu: Vulkan 1.2 compute, the Vulkan library loaded at run time through volk;
# MoltenVK on macOS). Vulkan-Headers and volk at the commits engines/gpu/gpu-pins.json pins (-DPM_FETCH_GPU_DEPS=ON, CI),
# shaders from the checked-in SPIR-V. Without them: the CPU engine only. At run time the module falls back to the CPU
# engine when there is no usable Vulkan device.
option(PM_GPU "build the portable GPU engine" ON)
option(PM_FETCH_GPU_DEPS "download the pinned Vulkan-Headers and volk" OFF)
set(PM_VULKAN_HEADERS_DIR "" CACHE PATH "Vulkan-Headers checkout")
set(PM_VOLK_DIR "" CACHE PATH "volk checkout")
if(PM_GPU)
  if(PM_FETCH_GPU_DEPS AND (NOT PM_VULKAN_HEADERS_DIR OR NOT PM_VOLK_DIR))
    file(READ "${VS}/engines/gpu/gpu-pins.json" _pins)
    include(FetchContent)
    string(JSON _vh_url GET "${_pins}" vulkanHeaders url)
    string(JSON _vh_sha GET "${_pins}" vulkanHeaders sha256)
    string(JSON _volk_url GET "${_pins}" volk url)
    string(JSON _volk_sha GET "${_pins}" volk sha256)
    FetchContent_Declare(pm_vkh URL "${_vh_url}" URL_HASH SHA256=${_vh_sha} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_Declare(pm_volk URL "${_volk_url}" URL_HASH SHA256=${_volk_sha} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(pm_vkh)
    if(NOT pm_vkh_POPULATED)
      FetchContent_Populate(pm_vkh)
    endif()
    FetchContent_GetProperties(pm_volk)
    if(NOT pm_volk_POPULATED)
      FetchContent_Populate(pm_volk)
    endif()
    set(PM_VULKAN_HEADERS_DIR "${pm_vkh_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(PM_VOLK_DIR "${pm_volk_SOURCE_DIR}" CACHE PATH "" FORCE)
  endif()
  if(EXISTS "${PM_VULKAN_HEADERS_DIR}/include/vulkan/vulkan_core.h" AND EXISTS "${PM_VOLK_DIR}/volk.c")
    add_library(pm_engine_gpu STATIC "${VS}/engines/gpu/vk_context.cpp" "${VS}/engines/gpu/gpu_engine.cpp"
      "${VS}/engines/gpu/subtitle_hold.cpp" "${VS}/third_party/pushfps-portable/src/CrosshairHold.cpp" "${PM_VOLK_DIR}/volk.c")
    target_include_directories(pm_engine_gpu PUBLIC "${VS}/engines/gpu" "${PM_VULKAN_HEADERS_DIR}/include" "${PM_VOLK_DIR}"
      "${VS}/third_party/nis" PRIVATE "${VS}/engines/gpu/spv")
    target_compile_definitions(pm_engine_gpu PUBLIC PMVS_GPU VK_NO_PROTOTYPES)
    target_link_libraries(pm_engine_gpu PUBLIC pm_engine_portable ${CMAKE_DL_LIBS})
    target_link_libraries(pm_engine_portable PUBLIC pm_engine_gpu)   # engine_portable.cpp picks the GPU first
    message(STATUS "PushMPC VLC: portable GPU engine (Vulkan) included")
  else()
    message(STATUS "PushMPC VLC: Vulkan-Headers / volk not found: CPU engine only")
  endif()
endif()

add_library(pushmpc_plugin SHARED src/pushmpc_vlc.c src/display_hz.c src/engine_pushcore.c src/pm_snap_boot.c)
set_target_properties(pushmpc_plugin PROPERTIES PREFIX "lib" OUTPUT_NAME "pushmpc_plugin")
target_include_directories(pushmpc_plugin PRIVATE "${VLC_PLUGIN_INCLUDE}" src)
target_compile_definitions(pushmpc_plugin PRIVATE __PLUGIN__ MODULE_STRING="pushmpc" _FILE_OFFSET_BITS=64 _REENTRANT _THREAD_SAFE)
if(PM_ALT_ENTRY)
  target_compile_definitions(pushmpc_plugin PRIVATE PM_ALT_ENTRY=${PM_ALT_ENTRY})
endif()
target_link_libraries(pushmpc_plugin PRIVATE pm_engine_portable)
if(VLC_CORE_LIB)
  target_link_libraries(pushmpc_plugin PRIVATE "${VLC_CORE_LIB}")
endif()
if(WIN32)
  # MinGW: one self-contained DLL (libstdc++, libgcc, winpthread inside)
  target_link_options(pushmpc_plugin PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_libraries(pushmpc_plugin PRIVATE user32)
elseif(APPLE)
  target_link_options(pushmpc_plugin PRIVATE -undefined dynamic_lookup)
  target_link_libraries(pushmpc_plugin PRIVATE "-framework CoreGraphics")
else()
  target_link_options(pushmpc_plugin PRIVATE -static-libstdc++ -static-libgcc)
  if(VLC_CORE_LIB)
    target_link_options(pushmpc_plugin PRIVATE -Wl,--no-undefined -Wl,-z,defs)
  endif()
  # without VLC_CORE_LIB (the manylinux2014 release build) libvlccore's symbols stay undefined and resolve from the
  # VLC process at load, as on macOS, so one binary fits every distro's VLC 3.0.x (glibc >= 2.17)
  target_link_libraries(pushmpc_plugin PRIVATE ${CMAKE_DL_LIBS})
endif()

# a headless check of the engine through the C interface (no VLC needed)
add_executable(pm_engine_test tests/pm_engine_test.c)
target_link_libraries(pm_engine_test PRIVATE pm_engine_portable)
set_target_properties(pm_engine_test PROPERTIES LINKER_LANGUAGE CXX)
if(WIN32)
  target_link_options(pm_engine_test PRIVATE -static)
endif()

# ---- Windows: PushMPC's fork of VLC 3.0.24's Direct3D 11 output (display/vlc3, LGPL-2.1+) with NIS upscaling on any
# GPU: libpushmpc_d3d11_plugin.dll (vout=pushmpc_d3d11); the VLC sources come from fetch-vlc-sources.sh (tarball +
# PushMPC patch). NIS 1.0.3 (MIT) from vsplugin/third_party/nis; its HLSL is
# embedded at build time and compiled at run time with the system's d3dcompiler_47.dll.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/display/vlc3/modules/video_output/win32/direct3d11.c")   # display/vlc3/fetch-vlc-sources.sh first
  set(D3 "${CMAKE_CURRENT_SOURCE_DIR}/display/vlc3/modules")
  set(NISDIR "${VS}/third_party/nis")
  file(READ "${NISDIR}/NIS_Scaler.h" _nis_src)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen/pm_nis_shader.h"
       "// generated from NIS_Scaler.h (NVIDIA Image Scaling 1.0.3, MIT)\nstatic const char kPmNisScalerHlsl[] = R\"PMNIS(${_nis_src})PMNIS\";\n")
  set(D3D11_FORK_SOURCES
    ${D3}/video_output/win32/direct3d11.c ${D3}/video_output/win32/common.c ${D3}/video_output/win32/events.c
    ${D3}/video_output/win32/pm_sensors_stub.c ${D3}/video_output/win32/win32touch.c ${D3}/video_output/win32/d3d11_quad.c
    ${D3}/video_output/win32/d3d11_shaders.c ${D3}/video_output/win32/d3d11_scaler.cpp
    ${D3}/video_output/win32/d3d11_tonemap.cpp ${D3}/video_output/win32/pm_nis_d3d11.cpp
    ${D3}/video_chroma/d3d11_fmt.c ${D3}/video_chroma/dxgi_fmt.c ${D3}/video_chroma/copy.c)
  add_library(pushmpc_d3d11_plugin SHARED ${D3D11_FORK_SOURCES})
  set_target_properties(pushmpc_d3d11_plugin PROPERTIES PREFIX "lib" OUTPUT_NAME "pushmpc_d3d11_plugin")
  target_include_directories(pushmpc_d3d11_plugin PRIVATE "${VLC_PLUGIN_INCLUDE}" "${D3}/../include" "${D3}/video_output" "${D3}/video_output/win32"
                             "${NISDIR}" "${CMAKE_CURRENT_BINARY_DIR}/gen")
  target_compile_definitions(pushmpc_d3d11_plugin PRIVATE __PLUGIN__ MODULE_STRING="pushmpc_d3d11" MODULE_NAME_IS_direct3d11
                             _WIN32_WINNT=0x0601 WINVER=0x0601 COBJMACROS "gettext_noop(s)=s" "N_(s)=s" "_(s)=vlc_gettext(s)")
  target_compile_options(pushmpc_d3d11_plugin PRIVATE -Wno-unknown-pragmas -Wno-deprecated-declarations
    # vlc_threads.h calls poll(), which VLC's own tree declares in its compat layer: Winsock's WSAPoll for the C++ files
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include winsock2.h>" "$<$<COMPILE_LANGUAGE:CXX>:-Dpoll=WSAPoll>")
  if(VLC_CORE_LIB)
    target_link_libraries(pushmpc_d3d11_plugin PRIVATE "${VLC_CORE_LIB}")
  endif()
  target_link_libraries(pushmpc_d3d11_plugin PRIVATE uuid gdi32 ole32 user32 dxgi d3d11 ws2_32)
  target_link_options(pushmpc_d3d11_plugin PRIVATE -static -static-libgcc -static-libstdc++)

  # the NIS upscaler on WARP without VLC (CI)
  add_executable(pm_nis_test display/tests/pm_nis_test.cpp ${D3}/video_output/win32/pm_nis_d3d11.cpp)
  target_include_directories(pm_nis_test PRIVATE "${VLC_PLUGIN_INCLUDE}" "${D3}/video_output/win32" "${NISDIR}" "${CMAKE_CURRENT_BINARY_DIR}/gen")
  target_compile_definitions(pm_nis_test PRIVATE _WIN32_WINNT=0x0601 COBJMACROS poll=WSAPoll)
  target_compile_options(pm_nis_test PRIVATE "SHELL:-include winsock2.h")
  target_link_libraries(pm_nis_test PRIVATE d3d11 dxgi uuid ws2_32)
  target_link_options(pm_nis_test PRIVATE -static)
endif()
