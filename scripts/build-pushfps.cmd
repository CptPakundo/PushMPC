@echo off
rem Configure and build PushFPS.exe, mirroring the Configure and Build steps of .github/workflows/build-windows.yml.
rem Only needed for the PushFPS app itself: build-pushmpc.cmd builds pushcore on its own. Incremental: CMake runs only on
rem a new tree or a new version; a compiler cache is used when one is installed (scripts\build-env.cmd).
setlocal
rem Paths (defaults relative to this repository, no spaces): PUSHMPC_DEPS (<repo>\deps), PUSHMPC_PUSHFPS_SRC (<repo>\PushFPS), PUSHMPC_BUILD_DIR (<repo>\build)
set NoDefaultCurrentDirectoryInExePath=
call "%~dp0build-env.cmd" || exit /b 1
set "ROOT=%PUSHMPC_REPO%"
if not defined PUSHMPC_PUSHFPS_SRC set PUSHMPC_PUSHFPS_SRC=%ROOT%\PushFPS
if not defined PUSHMPC_BUILD_DIR set PUSHMPC_BUILD_DIR=%ROOT%\build
set DEPS=%PUSHMPC_DEPS%
set REPO=%PUSHMPC_PUSHFPS_SRC%
set BUILD=%PUSHMPC_BUILD_DIR%
rem The PushFPS version this tree is (tag vX.Y.Z of PushFPS) plus our suffix; also stamps pushcore.dll's PushFPSVersion
set PUSHFPS_DISPLAY_VERSION=4.5.4-pushmpc
set FFMPEG=%DEPS%\vcpkg\installed\x64-windows
set STREAM=%DEPS%\stream-installed\x64-windows-static
set PKG_CONFIG_PATH=%STREAM%\lib\pkgconfig
set PATH=%STREAM%\tools\protobuf;%LOCALAPPDATA%\Programs\Python\Python312;%PATH%

rem a new tree gets every option; an existing tree keeps its cache and only takes a new version
set PUSHMPC_CFG_NEW=-DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_PREFIX_PATH=%DEPS%\Qt\6.8.3\msvc2022_64;%FFMPEG%;%STREAM%" ^
  -DVEYRA_BUILD_QML_UI=ON -DVEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON -DVEYRA_ENABLE_D3D12_DEBUG=OFF ^
  -DVEYRA_ENABLE_REMOTEPLAY=ON -DVEYRA_ENABLE_MOONLIGHT=ON -DVEYRA_ENABLE_XBOX=ON ^
  "-DVEYRA_RP_CHIAKI_SOURCE_DIR=%DEPS%\chiaki-stage" "-DVEYRA_RP_CHIAKI_VERIFY_DIR=%DEPS%\chiaki-clean" ^
  "-DVEYRA_MOONLIGHT_SOURCE_DIR=%DEPS%\moonlight-common-c" ^
  "-DPROTOC=%STREAM%\tools\protobuf\protoc.exe" "-DPKG_CONFIG_EXECUTABLE=%STREAM%\tools\pkgconf\pkgconf.exe" ^
  -DOPENSSL_USE_STATIC_LIBS=ON -DOPENSSL_MSVC_STATIC_RT=ON ^
  "-DVEYRA_FFMPEG_ROOT=%FFMPEG%" ^
  "-DVEYRA_DLSS_SDK_ROOT=%DEPS%\dlss" ^
  "-DVEYRA_NVENC_HEADERS_ROOT=%DEPS%\nv-codec-headers" ^
  "-DVEYRA_CUDA_DRIVER_INCLUDE_DIR=%DEPS%\cudart-root\include" ^
  "-DVEYRA_XESS_ROOT=%DEPS%\xess" ^
  "-DVEYRA_FIDELITYFX_ROOT=%DEPS%\ffx1\sdk" ^
  "-DVEYRA_FIDELITYFX2_ROOT=%DEPS%\ffx2\Kits\FidelityFX" ^
  "-DVEYRA_AMF_SDK_ROOT=%DEPS%\amf\amf\public" ^
  "-DVEYRA_ONNXRUNTIME_ROOT=%DEPS%\pushflow\ort" "-DVEYRA_DIRECTML_ROOT=%DEPS%\pushflow\dml" ^
  -DVEYRA_ENABLE_LIBDOVI=ON "-DVEYRA_LIBDOVI_ROOT=%DEPS%\libdovi"
set "PUSHMPC_CFG="-DVEYRA_DISPLAY_VERSION=%PUSHFPS_DISPLAY_VERSION%""
call "%~dp0cmake-configure.cmd" "%REPO%" "%BUILD%" || exit /b 1
cmake --build "%BUILD%" --target veyra_qml_ui %PUSHMPC_PARALLEL% -k 0 || exit /b 1
dir "%BUILD%\PushFPS.exe"
