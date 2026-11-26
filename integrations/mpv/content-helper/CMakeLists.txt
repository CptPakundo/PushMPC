# pushmpc-content: the image model for plain mpv (pushmpc.lua without PushMPC's engine), next to pushmpc.lua.
#   -DPMML_ORT_INCLUDE=<dir>  ONNX Runtime C API headers (pmml loads ONNX Runtime at run time; no import library)
# Without PMML_ORT_INCLUDE only the model-free unit test is built.
cmake_minimum_required(VERSION 3.20)
project(pushmpc_content CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/msvc-loop-guard.cmake")   # MSVC loop optimiser bug guard (/d2NoLUR)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(PM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
if(MSVC)
  # one file to ship: the static C++ runtime
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /W3)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

enable_testing()
# the model-free parts (protocol, integrator, cut meter, status): no pmml, no runtime
add_executable(content_helper_test content_helper_test.cpp)
target_include_directories(content_helper_test PRIVATE "${PM_ROOT}/pushmpc/include")
add_test(NAME content_helper_test COMMAND content_helper_test)

set(PMML_ORT_INCLUDE "" CACHE STRING "ONNX Runtime C API headers for pmml")
if(PMML_ORT_INCLUDE)
  add_subdirectory("${PM_ROOT}/pmml" pmml EXCLUDE_FROM_ALL)
  add_executable(pushmpc-content main.cpp)
  target_include_directories(pushmpc-content PRIVATE "${PM_ROOT}/pushmpc/include")
  target_compile_definitions(pushmpc-content PRIVATE PUSHMPC_CONTENT_MODEL=1)
  target_link_libraries(pushmpc-content PRIVATE pmml)
  # the helper without a model or runtime: it must say so in its status and exit (the lua then falls back)
  add_test(NAME content_helper_no_model COMMAND "${CMAKE_COMMAND}" -DHELPER=$<TARGET_FILE:pushmpc-content>
           -DWORK=${CMAKE_CURRENT_BINARY_DIR}/no-model -P "${CMAKE_CURRENT_SOURCE_DIR}/no_model_test.cmake")
  message(STATUS "pushmpc-content: the helper with the image model (pmml)")
else()
  message(STATUS "pushmpc-content: no PMML_ORT_INCLUDE: only the unit test")
endif()
