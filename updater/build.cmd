@echo off
rem Builds bin\PushMpcUpdater.exe (paths relative to this script). Options: PUSHMPC_VS (Visual Studio folder),
rem PUSHMPC_UPDATER_CMAKE_ARGS (for a release: -DPUSHMPC_UPDATER_DEV=OFF, optionally -DPUSHMPC_RELEASE_KEY=<xml>).
setlocal
set NoDefaultCurrentDirectoryInExePath=
set HERE=%~dp0
set HERE=%HERE:~0,-1%
if not defined PUSHMPC_VS for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set PUSHMPC_VS=%%i
call "%PUSHMPC_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%PUSHMPC_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PUSHMPC_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cmake -S "%HERE%" -B "%HERE%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release %PUSHMPC_UPDATER_CMAKE_ARGS% >nul || exit /b 1
cmake --build "%HERE%\build" --parallel || exit /b 1
