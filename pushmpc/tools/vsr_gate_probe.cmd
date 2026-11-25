@echo off
rem Builds vsr_gate_probe.exe (the Gtx16Vsr / ZludaVsr self-test) without the PushFPS tree.
rem Usage: vsr_gate_probe.cmd <DLSS SDK root (NVIDIA/DLSS: include\, lib\Windows_x86_64\x64\)> [output dir]
setlocal
set NoDefaultCurrentDirectoryInExePath=
set "NGX=%~1"
set "OUT=%~2"
if "%NGX%"=="" (echo usage: %~nx0 ^<dlss_sdk_root^> [out_dir] & exit /b 2)
if "%OUT%"=="" set "OUT=%~dp0..\..\build\vsr-gate-probe"
set "VS="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if "%VS%"=="" (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /O2 /EHsc /W3 /MT /DUNICODE /D_UNICODE /I"%NGX%\include" "%~dp0vsr_gate_probe.cpp" /Fo"%OUT%\\" /Fe"%OUT%\vsr_gate_probe.exe" ^
  /link /SUBSYSTEM:CONSOLE "%NGX%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" d3d12.lib dxgi.lib advapi32.lib user32.lib || exit /b 1
echo built "%OUT%\vsr_gate_probe.exe"
