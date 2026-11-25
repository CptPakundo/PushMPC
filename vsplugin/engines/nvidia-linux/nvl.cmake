# The Linux NVIDIA backend (engines/nvidia-linux), included by vsplugin/CMakeLists.txt:
#   nvl_test           every OS: colour conversions, cut guard, Auto, fatbin retarget, SHA-256, settings (pure C++)
#   nvl_got_test       Linux: the GOT hooks of the RTX 30 compatibility on a demo library pair (no GPU)
#   pmvs_nvl           Linux, PMVS_NVIDIA_LINUX: the CUDA engine (libcuda and the Video Effects runtime loaded at run
#                      time; only nvcc is needed at build time, for the kernels' fatbin)
#   pushmpc-nvvfx-selftest   the RTX 30 VFG gate and smoke test (next to the plugin)
#   nvl_struct_test    Linux: the pinned runtime's libraries loaded against a stub libcuda (CI, no GPU)
set(NVL "${CMAKE_SOURCE_DIR}/engines/nvidia-linux")

add_executable(nvl_test tests/nvl_test.cpp)
set_target_properties(nvl_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
target_include_directories(nvl_test PRIVATE "${NVL}" src)
add_test(NAME nvl_test COMMAND nvl_test)

set(PMVS_NVIDIA_LINUX_DEFAULT OFF)
if(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|aarch64|arm64")
  set(PMVS_NVIDIA_LINUX_DEFAULT ON)
  # the GOT hooks on a demo pair: libnvl_cudademo.so (cuDeviceGetAttribute, cuModuleLoadData, cuCtxGetDevice) and
  # libnvl_gotdemo.so importing them, as libnvVFXVideoFrameGeneration.so imports them from libcuda.so.1
  add_library(nvl_cudademo SHARED tests/nvidia-linux/cudademo.cpp)
  add_library(nvl_gotdemo SHARED tests/nvidia-linux/gotdemo.cpp)
  target_link_libraries(nvl_gotdemo PRIVATE nvl_cudademo)
  set_target_properties(nvl_cudademo nvl_gotdemo PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_executable(nvl_got_test tests/nvidia-linux/got_test.cpp "${NVL}/nvl_ampere.cpp")
  set_target_properties(nvl_got_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  target_include_directories(nvl_got_test PRIVATE "${NVL}")
  target_link_libraries(nvl_got_test PRIVATE ${CMAKE_DL_LIBS})
  add_dependencies(nvl_got_test nvl_gotdemo nvl_cudademo)
  add_test(NAME nvl_got_test COMMAND nvl_got_test "${CMAKE_BINARY_DIR}/libnvl_gotdemo.so" "${CMAKE_BINARY_DIR}/libnvl_cudademo.so")
endif()
option(PMVS_NVIDIA_LINUX "the nvidia backend on Linux (CUDA + NVIDIA's Video Effects runtime; needs nvcc at build time)" ${PMVS_NVIDIA_LINUX_DEFAULT})

if(PMVS_NVIDIA_LINUX)
  find_program(PMVS_NVCC nvcc HINTS "$ENV{CUDA_PATH}/bin" /usr/local/cuda/bin)
  if(NOT PMVS_NVCC)
    message(WARNING "PMVS_NVIDIA_LINUX: nvcc not found (CUDA toolkit): the Linux nvidia backend is not built")
    set(PMVS_NVIDIA_LINUX OFF)
  endif()
endif()

if(PMVS_NVIDIA_LINUX)
  # SASS for Turing, Ampere, Ada, Blackwell (consumer); PTX 7.5 (JIT on anything else: Hopper, Blackwell data centre) and 12.0
  set(PMVS_CUDA_GENCODE "-gencode;arch=compute_75,code=sm_75;-gencode;arch=compute_86,code=sm_86;-gencode;arch=compute_89,code=sm_89;-gencode;arch=compute_120,code=sm_120;-gencode;arch=compute_75,code=compute_75;-gencode;arch=compute_120,code=compute_120"
      CACHE STRING "nvcc -gencode list for the kernels' fatbin")
  set(NVL_FATBIN "${CMAKE_BINARY_DIR}/nvl_kernels.fatbin")
  set(NVL_FATBIN_CPP "${CMAKE_BINARY_DIR}/nvl_kernels_fatbin.cpp")
  add_custom_command(OUTPUT "${NVL_FATBIN}"
    COMMAND "${PMVS_NVCC}" -fatbin -O3 -std=c++17 -I "${NVL}" ${PMVS_CUDA_GENCODE} -o "${NVL_FATBIN}" "${NVL}/nvl_kernels.cu"
    DEPENDS "${NVL}/nvl_kernels.cu" "${NVL}/nvl_colour.h" VERBATIM COMMAND_EXPAND_LISTS)
  add_custom_command(OUTPUT "${NVL_FATBIN_CPP}"
    COMMAND "${CMAKE_COMMAND}" -DIN=${NVL_FATBIN} -DOUT=${NVL_FATBIN_CPP} -P "${NVL}/bin2cpp.cmake"
    DEPENDS "${NVL_FATBIN}" "${NVL}/bin2cpp.cmake" VERBATIM)

  add_library(pmvs_nvl STATIC "${NVL}/nvl_loader.cpp" "${NVL}/nvl_engine.cpp" "${NVL}/nvl_ampere.cpp" "${NVL}/nvl_selftest.cpp" "${NVL_FATBIN_CPP}")
  target_include_directories(pmvs_nvl PUBLIC "${NVL}" src)
  target_link_libraries(pmvs_nvl PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
  set_target_properties(pmvs_nvl PROPERTIES CXX_VISIBILITY_PRESET hidden)

  add_executable(pushmpc-nvvfx-selftest "${NVL}/nvl_selftest_main.cpp")
  target_link_libraries(pushmpc-nvvfx-selftest PRIVATE pmvs_nvl)
  set_target_properties(pushmpc-nvvfx-selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PMVS_BIN}")

  add_executable(nvl_struct_test tests/nvidia-linux/struct_test.cpp)
  target_link_libraries(nvl_struct_test PRIVATE pmvs_nvl)
  set_target_properties(nvl_struct_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  # run by CI with the pinned runtime and a stub libcuda: nvl_struct_test <runtime-dir> (see vsplugin-nvidia-linux.yml)
endif()
