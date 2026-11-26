@echo off
rem Builds the ZLUDA VSR worker, its stub and the unit test without the PushFPS tree (the release builds the worker in
rem PushFPS's CMake tree as pushcore_zluda_vsr_worker; pushmpc\CMakeLists.txt).
rem Usage: zluda_vsr_build.cmd <DLSS SDK root (include\nvsdk_ngx*.h)> <folder with NVIDIA's cuda.h> [output dir]
setlocal
set NoDefaultCurrentDirectoryInExePath=
set "NGX=%~1"
set "CUDAINC=%~2"
set "OUT=%~3"
if "%CUDAINC%"=="" (echo usage: %~nx0 ^<dlss_sdk_root^> ^<cuda_include^> [out_dir] & exit /b 2)
if "%OUT%"=="" set "OUT=%~dp0..\..\build\zluda-vsr"
set "VS="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if "%VS%"=="" (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%\real" mkdir "%OUT%\real"
if not exist "%OUT%\stub" mkdir "%OUT%\stub"
if not exist "%OUT%\test" mkdir "%OUT%\test"
set "FLAGS=/nologo /std:c++20 /O2 /EHsc /W3 /MT /DUNICODE /D_UNICODE /utf-8 /I"%~dp0..\include""
cl %FLAGS% /I"%NGX%\include" /I"%CUDAINC%" "%~dp0zluda_vsr_worker.cpp" /Fo"%OUT%\real\\" /Fe"%OUT%\pushcore_zluda_vsr_worker.exe" ^
  /link /SUBSYSTEM:CONSOLE d3d12.lib dxgi.lib advapi32.lib user32.lib || exit /b 1
cl %FLAGS% /DPUSHMPC_ZLUDA_VSR_STUB=1 "%~dp0zluda_vsr_worker.cpp" /Fo"%OUT%\stub\\" /Fe"%OUT%\zluda_vsr_stub_worker.exe" ^
  /link /SUBSYSTEM:CONSOLE d3d12.lib dxgi.lib advapi32.lib user32.lib || exit /b 1
cl %FLAGS% "%~dp0zluda_vsr_test.cpp" /Fo"%OUT%\test\\" /Fe"%OUT%\zluda_vsr_test.exe" ^
  /link /SUBSYSTEM:CONSOLE d3d12.lib dxgi.lib advapi32.lib user32.lib || exit /b 1
echo built "%OUT%"
