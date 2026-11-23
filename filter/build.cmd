@echo off
rem Builds bin\PushMpcFilter.ax, bin\PushMpcExit.dll and build\auto_test.exe (paths relative to this script).
rem VS: the newest Visual Studio with C++ (vswhere), or the PUSHMPC_VS environment variable.
setlocal
set NoDefaultCurrentDirectoryInExePath=
set HERE=%~dp0
set HERE=%HERE:~0,-1%
if not defined PUSHMPC_VS for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set PUSHMPC_VS=%%i
call "%PUSHMPC_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%PUSHMPC_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PUSHMPC_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cmake -S "%HERE%" -B "%HERE%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release >nul || exit /b 1
cmake --build "%HERE%\build" --parallel || exit /b 1
