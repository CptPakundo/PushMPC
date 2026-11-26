# PushMpcUpdater.exe: PushMPC's auto-updater (see src/updater.cpp). Static CRT, no dependencies beyond Windows.
#   -DPUSHMPC_RELEASE_KEY=<public key xml>   embedded release key (default: release-key.xml next to this file)
#   -DPUSHMPC_UPDATER_DEV=0                  release build: unsigned feeds refused unless AllowUnsigned=1
cmake_minimum_required(VERSION 3.24)
project(PushMpcUpdater CXX RC)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/msvc-loop-guard.cmake")   # MSVC loop optimiser bug guard (/d2NoLUR)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(PUSHMPC_RELEASE_KEY "${CMAKE_CURRENT_SOURCE_DIR}/release-key.xml" CACHE FILEPATH "public release key (RSAKeyValue XML)")
option(PUSHMPC_UPDATER_DEV "developer build: unsigned release manifests allowed by default" ON)
set(PUSHMPC_UPDATER_SRC "${CMAKE_CURRENT_SOURCE_DIR}")
string(REPLACE "\\" "/" PUSHMPC_RELEASE_KEY "${PUSHMPC_RELEASE_KEY}")
configure_file(res/updater.rc.in ${CMAKE_CURRENT_BINARY_DIR}/updater.rc @ONLY)

add_library(pushmpc_common STATIC src/common/pm_common.cpp src/common/pm_net.cpp src/common/pm_release.cpp src/common/pm_common.h)
target_compile_definitions(pushmpc_common PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(pushmpc_common PUBLIC /W3 /EHsc /O2 /utf-8 /Z7)
target_include_directories(pushmpc_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(PushMpcUpdater WIN32 src/updater.cpp ${CMAKE_CURRENT_BINARY_DIR}/updater.rc)
target_compile_definitions(PushMpcUpdater PRIVATE PUSHMPC_UPDATER_DEV=$<IF:$<BOOL:${PUSHMPC_UPDATER_DEV}>,1,0>)
target_link_libraries(PushMpcUpdater PRIVATE pushmpc_common)
target_link_options(PushMpcUpdater PRIVATE /DEBUG /OPT:REF /PDBALTPATH:%_PDB% /MANIFEST:NO)
set_target_properties(PushMpcUpdater PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin")
