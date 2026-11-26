# PushMPC capture mode for Linux and macOS (capture/), included at the end of vsplugin/CMakeLists.txt:
#   pmc_core          every OS: the timeline, pacer, converter, protection detector, lip sync, options, host engines
#   capture_test      every OS, no GPU: cap_core and the CPU engine (ctest)
#   pushmpc-capture   Linux / macOS, with the portable GPU engine (PMVS_GPU_ON) and glslang for its two shaders: the
#                     capture host. Linux sources and presenters by what is installed (pkg-config):
#                       X11        x11 xcomposite xdamage xext xrandr          -> source_x11.cpp + present_x11.cpp
#                       portal     dbus-1 libpipewire-0.3                       -> source_portal.cpp
#                       Wayland    wayland-client wayland-scanner wayland-protocols -> present_wayland.cpp
#                     macOS: ScreenCaptureKit + AppKit + MoltenVK (source_sck.mm, present_mac.mm).
#                     Engines: gpu always; ml when pmml is built (PMML_ORT_INCLUDE); apple on macOS (pmvs_apple).
#   pmc_vk_check      every OS with PMVS_GPU_ON (Windows included; by name): cap_vk.cpp and cap_gpu.cpp compiled (not linked) so
#                     a Windows build checks the Vulkan code too; stub shader headers when glslang is missing.
# Ubuntu / Debian packages: libx11-dev libxcomposite-dev libxdamage-dev libxext-dev libxrandr-dev libdbus-1-dev
#   libpipewire-0.3-dev libwayland-dev wayland-protocols glslang-tools; Fedora / Alma: libX11-devel libXcomposite-devel
#   libXdamage-devel libXext-devel libXrandr-devel dbus-devel pipewire-devel wayland-devel wayland-protocols-devel glslang
set(PMC_DIR "${CMAKE_SOURCE_DIR}/../capture")
option(PMC_CAPTURE "build pushmpc-capture (Linux, macOS) when its dependencies are there" ON)

add_library(pmc_core STATIC "${PMC_DIR}/src/cap_core.cpp" "${PMC_DIR}/src/cap_engines.cpp")
target_include_directories(pmc_core PUBLIC "${PMC_DIR}/src" src engines/portable)
target_link_libraries(pmc_core PUBLIC pmvs_portable Threads::Threads)
if(WIN32)
  target_link_libraries(pmc_core PUBLIC advapi32)   # platform.h reads the settings from the registry
endif()
# the ml engine (pmml) when pmml is configured: here or by the plugin block above
if(PMML_ORT_INCLUDE AND NOT TARGET pmml)
  add_subdirectory("${CMAKE_SOURCE_DIR}/../pmml" pmml)
endif()
if(TARGET pmml)
  target_link_libraries(pmc_core PUBLIC pmml)
  target_compile_definitions(pmc_core PUBLIC PMC_HAVE_PMML)
endif()
if(APPLE AND TARGET pmvs_apple)
  target_link_libraries(pmc_core PUBLIC pmvs_apple)
  target_compile_definitions(pmc_core PUBLIC PMC_HAVE_APPLE)
endif()

add_executable(capture_test "${PMC_DIR}/tests/capture_test.cpp")
set_target_properties(capture_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
target_link_libraries(capture_test PRIVATE pmc_core)
add_test(NAME capture_test COMMAND capture_test)

if(NOT PMVS_GPU_ON)
  message(STATUS "capture mode: no Vulkan headers / volk (PMVS_FETCH_GPU_DEPS=ON): pushmpc-capture is not built")
  return()
endif()

# ---- the two shaders (glslang), or stub headers for the compile check only
find_program(PMC_GLSLANG NAMES glslangValidator glslang HINTS "${PMVS_GLSLANG}")
if(PMVS_GLSLANG)
  set(PMC_GLSLANG "${PMVS_GLSLANG}")
endif()
set(PMC_SPV "${CMAKE_BINARY_DIR}/pmc_spv")
file(MAKE_DIRECTORY "${PMC_SPV}")
set(PMC_SHADERS_REAL OFF)
if(PMC_GLSLANG)
  set(PMC_SHADERS_REAL ON)
  set(_pmc_spv_out)
  foreach(s rgb2yuv present)
    add_custom_command(OUTPUT "${PMC_SPV}/pmc_${s}.h"
      COMMAND "${PMC_GLSLANG}" -V --target-env vulkan1.2 --vn kSpv_pmc_${s} -o "${PMC_SPV}/pmc_${s}.h" "${PMC_DIR}/shaders/${s}.comp"
      DEPENDS "${PMC_DIR}/shaders/${s}.comp"
      COMMENT "glslang: capture/shaders/${s}.comp" VERBATIM)
    list(APPEND _pmc_spv_out "${PMC_SPV}/pmc_${s}.h")
  endforeach()
  add_custom_target(pmc_spv DEPENDS ${_pmc_spv_out})
else()
  set(PMC_SPV "${CMAKE_BINARY_DIR}/pmc_spv_stub")
  file(MAKE_DIRECTORY "${PMC_SPV}")
  foreach(s rgb2yuv present)
    file(WRITE "${PMC_SPV}/pmc_${s}.h" "// stub: no glslang at configure time (compile check only)\n#pragma once\n#include <cstdint>\nstatic const uint32_t kSpv_pmc_${s}[] = { 0x07230203u };\n")
  endforeach()
endif()

# compile check of the Vulkan parts on any OS (not linked into anything)
add_library(pmc_vk_check OBJECT EXCLUDE_FROM_ALL "${PMC_DIR}/src/cap_vk.cpp" "${PMC_DIR}/src/cap_gpu.cpp")
target_include_directories(pmc_vk_check PRIVATE "${PMC_SPV}")
target_link_libraries(pmc_vk_check PRIVATE pmc_core pmvs_gpu)
if(TARGET pmc_spv)
  add_dependencies(pmc_vk_check pmc_spv)
endif()

if(WIN32 OR NOT PMC_CAPTURE)
  return()   # Windows has its own capture mode (scripts/pushmpc-capture.ps1, PushFPS's window capture)
endif()
if(NOT PMC_SHADERS_REAL)
  message(WARNING "capture mode: glslang not found (PMVS_GLSLANG or glslangValidator on PATH): pushmpc-capture is not built")
  return()
endif()

set(PMC_SOURCES "${PMC_DIR}/src/cap_main.cpp" "${PMC_DIR}/src/cap_vk.cpp" "${PMC_DIR}/src/cap_gpu.cpp")
set(PMC_DEFS)
set(PMC_LIBS pmc_core pmvs_gpu Threads::Threads ${CMAKE_DL_LIBS})
set(PMC_INCS "${PMC_SPV}")

if(APPLE)
  enable_language(OBJCXX)
  list(APPEND PMC_SOURCES "${PMC_DIR}/src/source_sck.mm" "${PMC_DIR}/src/present_mac.mm")
  set_source_files_properties("${PMC_DIR}/src/source_sck.mm" "${PMC_DIR}/src/present_mac.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  list(APPEND PMC_LIBS "-framework ScreenCaptureKit" "-framework AppKit" "-framework CoreGraphics" "-framework CoreMedia" "-framework CoreVideo"
                       "-framework QuartzCore" "-framework Foundation")
else()
  find_package(PkgConfig)
  set(_have_source OFF)
  set(_have_present OFF)
  if(PkgConfig_FOUND)
    pkg_check_modules(PMC_X11 IMPORTED_TARGET x11 xcomposite xdamage xext xrandr)
    pkg_check_modules(PMC_PORTAL IMPORTED_TARGET dbus-1 libpipewire-0.3)
    pkg_check_modules(PMC_WL IMPORTED_TARGET wayland-client)
    pkg_check_modules(PMC_WLSCAN wayland-scanner)
    pkg_check_modules(PMC_WLPROT wayland-protocols)
  endif()
  if(PMC_X11_FOUND)
    list(APPEND PMC_SOURCES "${PMC_DIR}/src/source_x11.cpp" "${PMC_DIR}/src/present_x11.cpp")
    list(APPEND PMC_DEFS PMC_HAVE_X11)
    list(APPEND PMC_LIBS PkgConfig::PMC_X11)
    set(_have_source ON)
    set(_have_present ON)
  else()
    message(STATUS "capture mode: no X11 development files (x11 xcomposite xdamage xext xrandr): no X11 capture")
  endif()
  if(PMC_PORTAL_FOUND)
    list(APPEND PMC_SOURCES "${PMC_DIR}/src/source_portal.cpp")
    list(APPEND PMC_DEFS PMC_HAVE_PORTAL)
    list(APPEND PMC_LIBS PkgConfig::PMC_PORTAL)
    set(_have_source ON)
  else()
    message(STATUS "capture mode: no dbus-1 / libpipewire-0.3 development files: no portal capture (Wayland)")
  endif()
  if(PMC_WL_FOUND AND PMC_WLSCAN_FOUND AND PMC_WLPROT_FOUND)
    pkg_get_variable(PMC_WAYLAND_SCANNER wayland-scanner wayland_scanner)
    pkg_get_variable(PMC_WP_DIR wayland-protocols pkgdatadir)
    set(PMC_WL_GEN "${CMAKE_BINARY_DIR}/pmc_wayland")
    file(MAKE_DIRECTORY "${PMC_WL_GEN}")
    set(_xdg "${PMC_WP_DIR}/stable/xdg-shell/xdg-shell.xml")
    set(_layer "${PMC_DIR}/protocols/wlr-layer-shell-unstable-v1.xml")
    set(_wl_out)
    foreach(p xdg-shell wlr-layer-shell-unstable-v1)
      if(p STREQUAL "xdg-shell")
        set(_xml "${_xdg}")
      else()
        set(_xml "${_layer}")
      endif()
      add_custom_command(OUTPUT "${PMC_WL_GEN}/${p}-client-protocol.h" "${PMC_WL_GEN}/${p}-protocol.c"
        COMMAND "${PMC_WAYLAND_SCANNER}" client-header "${_xml}" "${PMC_WL_GEN}/${p}-client-protocol.h"
        COMMAND "${PMC_WAYLAND_SCANNER}" private-code "${_xml}" "${PMC_WL_GEN}/${p}-protocol.c"
        DEPENDS "${_xml}" COMMENT "wayland-scanner: ${p}" VERBATIM)
      list(APPEND _wl_out "${PMC_WL_GEN}/${p}-protocol.c")
    endforeach()
    list(APPEND PMC_SOURCES "${PMC_DIR}/src/present_wayland.cpp" ${_wl_out})
    list(APPEND PMC_DEFS PMC_HAVE_WAYLAND)
    list(APPEND PMC_LIBS PkgConfig::PMC_WL)
    list(APPEND PMC_INCS "${PMC_WL_GEN}")
    set(_have_present ON)
  else()
    message(STATUS "capture mode: no wayland-client / wayland-scanner / wayland-protocols: no Wayland overlay")
  endif()
  if(NOT _have_source OR NOT _have_present)
    message(WARNING "capture mode: needs X11, or the portal plus Wayland, development files: pushmpc-capture is not built")
    return()
  endif()
endif()

# built by name (capture.yml, release.yml, the formula): never part of another workflow's full build
add_executable(pushmpc_capture EXCLUDE_FROM_ALL ${PMC_SOURCES})
set_target_properties(pushmpc_capture PROPERTIES OUTPUT_NAME pushmpc-capture RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_include_directories(pushmpc_capture PRIVATE ${PMC_INCS})
target_compile_definitions(pushmpc_capture PRIVATE ${PMC_DEFS})
target_link_libraries(pushmpc_capture PRIVATE ${PMC_LIBS})
add_dependencies(pushmpc_capture pmc_spv)
message(STATUS "capture mode: pushmpc-capture with ${PMC_DEFS}")
