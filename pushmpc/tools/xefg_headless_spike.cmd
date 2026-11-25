@echo off
rem Builds the XeFG headless swapchain spike without the PushFPS tree.
rem Usage: xefg_headless_spike.cmd <XeSS SDK root (has inc\ and bin\)> [output dir]
setlocal
set "XESS=%~1"
set "OUT=%~2"
if "%XESS%"=="" (echo usage: %~nx0 ^<xess_sdk_root^> [out_dir] & exit /b 2)
if "%OUT%"=="" set "OUT=%~dp0..\..\build\xefg-spike"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if "%VS%"=="" (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /O2 /EHsc /W3 /MD /DUNICODE /D_UNICODE /I"%XESS%\inc" "%~dp0xefg_headless_spike.cpp" /Fo"%OUT%\\" /Fe"%OUT%\xefg_headless_spike.exe" /link /SUBSYSTEM:CONSOLE || exit /b 1
copy /y "%XESS%\bin\libxess_fg.dll" "%OUT%\" >nul || exit /b 1
copy /y "%XESS%\bin\libxell.dll" "%OUT%\" >nul || exit /b 1
echo built "%OUT%\xefg_headless_spike.exe"
