@echo off
rem Builds PushMPC targets in PushFPS's build tree: pushcore_dll (pushcore.dll, C ABI), pushcore_gpu_harness,
rem pushcore_session_edit, pushcore, pushcore_harness. Run build-pushfps.cmd once first.
rem Paths (defaults relative to this repository; the path must not contain spaces, FFmpeg/vcpkg break on them):
rem   PUSHMPC_PUSHFPS_SRC  PushFPS source with the PushMPC patches   (default <repo>\PushFPS)
rem   PUSHMPC_BUILD_DIR    the build tree                           (default <repo>\build)
rem   PUSHMPC_TEST_COPY    optional folder that gets a copy of a newer pushcore.dll
setlocal
set NoDefaultCurrentDirectoryInExePath=
for %%i in ("%~dp0..") do set REPO=%%~fi
if not defined PUSHMPC_PUSHFPS_SRC set PUSHMPC_PUSHFPS_SRC=%REPO%\PushFPS
if not defined PUSHMPC_BUILD_DIR set PUSHMPC_BUILD_DIR=%REPO%\build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined PUSHMPC_VS for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set PUSHMPC_VS=%%i
call "%PUSHMPC_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%PUSHMPC_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PUSHMPC_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
set "CORE=%REPO%\pushmpc"
rem NVIDIA Optical Flow SDK interface headers (developer builds only, for the DLSS-G comparison; never published):
rem PUSHMPC_NVOF_SDK = a folder with NvOFInterface\nvOpticalFlowCommon.h and nvOpticalFlowD3D12.h (default <repo>\deps\nvof-sdk)
if not defined PUSHMPC_NVOF_SDK if exist "%REPO%\deps\nvof-sdk\NvOFInterface\nvOpticalFlowD3D12.h" set PUSHMPC_NVOF_SDK=%REPO%\deps\nvof-sdk
set NVOF_ARG=
if defined PUSHMPC_NVOF_SDK set "NVOF_ARG=-DVEYRA_NVOF_SDK_ROOT=%PUSHMPC_NVOF_SDK:\=/%"
cmake -S "%PUSHMPC_PUSHFPS_SRC%" -B "%PUSHMPC_BUILD_DIR%" "-DPUSHMPC_SOURCE_DIR=%CORE:\=/%" %NVOF_ARG% >nul || exit /b 1
cmake --build "%PUSHMPC_BUILD_DIR%" --target %* --parallel || exit /b 1
if defined PUSHMPC_TEST_COPY if exist "%PUSHMPC_BUILD_DIR%\pushmpc\pushcore.dll" xcopy /d /y "%PUSHMPC_BUILD_DIR%\pushmpc\pushcore.dll" "%PUSHMPC_TEST_COPY%\" >nul
