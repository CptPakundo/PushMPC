@echo off
rem Builds bin\PushMPC-Setup.exe. Usage: build.cmd [<setup-payload.bin>]
rem The payload (the filter and core components) is written by "pushmpc-install.ps1 package", which calls this script;
rem without one the exe is a developer build (repair / update / uninstall only).
rem Incremental: CMake runs only on a new tree or a different payload path. Settings (Visual Studio, compiler cache,
rem parallel jobs): scripts\build-env.cmd.
setlocal
set NoDefaultCurrentDirectoryInExePath=
set HERE=%~dp0
set HERE=%HERE:~0,-1%
set PAYLOAD=%~f1
call "%HERE%\..\scripts\build-env.cmd" || exit /b 1
set "PUSHMPC_CFG=-DCMAKE_BUILD_TYPE=Release "-DPUSHMPC_PAYLOAD=%PAYLOAD%""
call "%HERE%\..\scripts\cmake-configure.cmd" "%HERE%" "%HERE%\build" || exit /b 1
cmake --build "%HERE%\build" %PUSHMPC_PARALLEL% || exit /b 1
