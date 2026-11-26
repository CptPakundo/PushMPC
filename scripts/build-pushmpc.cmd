@echo off
rem Builds PushMPC targets in PushFPS's build tree: pushcore_dll (pushcore.dll, C ABI), pushcore_gpu_harness,
rem pushcore_session_edit, pushcore, pushcore_harness, the shader targets.
rem   build-pushmpc.cmd <targets...>      for example: build-pushmpc.cmd pushcore_dll pushcore_gpu_harness
rem   build-pushmpc.cmd release           what a release ships (as release.yml): the target pushmpc_release = every
rem                                       shader target, pushcore_dll, pushcore_hip_preflight, pushcore_gpu_harness
rem Fastest layout for several checkouts (ccache hits across them): <base>\pushfps (PUSHMPC_PUSHFPS_SRC) and
rem <base>\build (PUSHMPC_BUILD_DIR) side by side, one <base> per checkout.
rem A new build tree is configured for pushcore only, as release.yml does (no PushFPS app, Qt or streaming clients), so
rem build-pushfps.cmd is not needed first; a tree that build-pushfps.cmd made works too. Incremental: CMake runs only
rem on a new tree or changed options; a compiler cache is used when one is installed (scripts\build-env.cmd).
rem Paths (defaults relative to this repository; the path must not contain spaces, FFmpeg/vcpkg break on them):
rem   PUSHMPC_PUSHFPS_SRC  PushFPS source with the PushMPC patches   (default <repo>\PushFPS)
rem   PUSHMPC_BUILD_DIR    the build tree                           (default <repo>\build)
rem   PUSHMPC_DEPS         the dependencies, laid out as build-pushfps.cmd expects   (default <repo>\deps)
rem   PUSHMPC_TEST_COPY    optional folder that gets a copy of a newer pushcore.dll
rem   PUSHMPC_PUSHFPS_CMAKE_ARGS   more CMake options for a new tree
setlocal
set NoDefaultCurrentDirectoryInExePath=
call "%~dp0build-env.cmd" || exit /b 1
set "REPO=%PUSHMPC_REPO%"
if not defined PUSHMPC_PUSHFPS_SRC set PUSHMPC_PUSHFPS_SRC=%REPO%\PushFPS
if not defined PUSHMPC_BUILD_DIR set PUSHMPC_BUILD_DIR=%REPO%\build
set "DEPS=%PUSHMPC_DEPS%"
set "CORE=%REPO%\pushmpc"
rem NVIDIA Optical Flow SDK interface headers (developer builds only, for the DLSS-G comparison; never published):
rem PUSHMPC_NVOF_SDK = a folder with NvOFInterface\nvOpticalFlowCommon.h and nvOpticalFlowD3D12.h (default <deps>\nvof-sdk)
if not defined PUSHMPC_NVOF_SDK if exist "%DEPS%\nvof-sdk\NvOFInterface\nvOpticalFlowD3D12.h" set PUSHMPC_NVOF_SDK=%DEPS%\nvof-sdk
set NVOF_ARG=
if defined PUSHMPC_NVOF_SDK set "NVOF_ARG=-DVEYRA_NVOF_SDK_ROOT=%PUSHMPC_NVOF_SDK:\=/%"
rem the MSVC loop optimiser bug guard for PushFPS's own targets too (cmake\msvc-loop-guard.cmake, no patch hunk needed)
set "PUSHMPC_CFG="-DPUSHMPC_SOURCE_DIR=%CORE:\=/%" "-DCMAKE_PROJECT_INCLUDE=%REPO:\=/%/cmake/msvc-loop-guard.cmake" %NVOF_ARG%"
rem a new tree: pushcore only, with the dependencies release.yml uses (options as in its "Configure PushFPS" step)
set PUSHMPC_CFG_NEW=
if exist "%PUSHMPC_BUILD_DIR%\build.ninja" goto :configure
set "FFMPEG=%DEPS%\vcpkg\installed\x64-windows"
set DOVI=-DVEYRA_ENABLE_LIBDOVI=OFF
if exist "%DEPS%\libdovi\bin\dolby_vision.dll" set "DOVI=-DVEYRA_ENABLE_LIBDOVI=ON "-DVEYRA_LIBDOVI_ROOT=%DEPS%\libdovi""
set PMML=
if exist "%DEPS%\pmml\ort\core\include" set "PMML="-DPUSHMPC_PMML_ORT_INCLUDE=%DEPS:\=/%/pmml/ort/core/include""
set PUSHFPS_DISPLAY_VERSION=4.5.4-pushmpc
for /f "usebackq delims=" %%v in (`powershell -NoProfile -Command "(Get-Content '%REPO%\scripts\pushfps-pin.json' -Raw | ConvertFrom-Json).pushfps.displayVersion" 2^>nul`) do set "PUSHFPS_DISPLAY_VERSION=%%v"
set PUSHMPC_CFG_NEW=-DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=%FFMPEG%" ^
  -DVEYRA_BUILD_QML_UI=OFF -DVEYRA_BUILD_QT_PROBE=OFF -DVEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON -DVEYRA_ENABLE_D3D12_DEBUG=OFF ^
  -DVEYRA_ENABLE_REMOTEPLAY=OFF -DVEYRA_ENABLE_MOONLIGHT=OFF -DVEYRA_ENABLE_XBOX=OFF ^
  "-DVEYRA_DISPLAY_VERSION=%PUSHFPS_DISPLAY_VERSION%" "-DVEYRA_FFMPEG_ROOT=%FFMPEG%" "-DVEYRA_DLSS_SDK_ROOT=%DEPS%\dlss" ^
  "-DVEYRA_NVENC_HEADERS_ROOT=%DEPS%\nv-codec-headers" "-DVEYRA_CUDA_DRIVER_INCLUDE_DIR=%DEPS%\cudart-root\include" ^
  "-DVEYRA_XESS_ROOT=%DEPS%\xess" "-DVEYRA_FIDELITYFX_ROOT=%DEPS%\ffx1\sdk" "-DVEYRA_FIDELITYFX2_ROOT=%DEPS%\ffx2\Kits\FidelityFX" ^
  "-DVEYRA_AMF_SDK_ROOT=%DEPS%\amf\amf\public" "-DVEYRA_ONNXRUNTIME_ROOT=%DEPS%\pushflow\ort" "-DVEYRA_DIRECTML_ROOT=%DEPS%\pushflow\dml" ^
  %DOVI% %PMML% %PUSHMPC_PUSHFPS_CMAKE_ARGS%
:configure
call "%~dp0cmake-configure.cmd" "%PUSHMPC_PUSHFPS_SRC%" "%PUSHMPC_BUILD_DIR%" || exit /b 1
rem ccache: paths under the folder that holds the build tree are hashed relative to it, so checkouts laid out alike
rem (<base>\pushfps and <base>\build) share their hits
if not defined PUSHMPC_CCACHE_BASEDIR for %%i in ("%PUSHMPC_BUILD_DIR%\..") do set "CCACHE_BASEDIR=%%~fi"
rem "release": pushmpc_release (pushmpc\CMakeLists.txt), every shader target plus the core and its helpers
set "TARGETS=%*"
if /i "%~1"=="release" set TARGETS=pushmpc_release
cmake --build "%PUSHMPC_BUILD_DIR%" --target %TARGETS% %PUSHMPC_PARALLEL% || exit /b 1
if defined PUSHMPC_TEST_COPY if exist "%PUSHMPC_BUILD_DIR%\pushmpc\pushcore.dll" xcopy /d /y "%PUSHMPC_BUILD_DIR%\pushmpc\pushcore.dll" "%PUSHMPC_TEST_COPY%\" >nul
