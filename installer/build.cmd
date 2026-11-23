@echo off
rem Builds bin\PushMPC-Setup.exe. Usage: build.cmd [<setup-payload.bin>]
rem The payload (the filter and core components) is written by "pushmpc-install.ps1 package", which calls this script;
rem without one the exe is a developer build (repair / update / uninstall only). PUSHMPC_VS overrides Visual Studio.
setlocal
set NoDefaultCurrentDirectoryInExePath=
set HERE=%~dp0
set HERE=%HERE:~0,-1%
set PAYLOAD=%~f1
if not defined PUSHMPC_VS for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set PUSHMPC_VS=%%i
call "%PUSHMPC_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%PUSHMPC_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PUSHMPC_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cmake -S "%HERE%" -B "%HERE%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DPUSHMPC_PAYLOAD=%PAYLOAD%" >nul || exit /b 1
cmake --build "%HERE%\build" --parallel || exit /b 1
