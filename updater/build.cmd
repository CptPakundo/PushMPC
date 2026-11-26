@echo off
rem Builds bin\PushMpcUpdater.exe (paths relative to this script). Options: PUSHMPC_UPDATER_CMAKE_ARGS (for a release:
rem -DPUSHMPC_UPDATER_DEV=OFF, optionally -DPUSHMPC_RELEASE_KEY=<xml>).
rem Incremental: CMake runs only on a new tree or changed options. Settings (Visual Studio, compiler cache, parallel
rem jobs): scripts\build-env.cmd.
setlocal
set NoDefaultCurrentDirectoryInExePath=
set HERE=%~dp0
set HERE=%HERE:~0,-1%
call "%HERE%\..\scripts\build-env.cmd" || exit /b 1
set "PUSHMPC_CFG=-DCMAKE_BUILD_TYPE=Release %PUSHMPC_UPDATER_CMAKE_ARGS%"
call "%HERE%\..\scripts\cmake-configure.cmd" "%HERE%" "%HERE%\build" || exit /b 1
cmake --build "%HERE%\build" %PUSHMPC_PARALLEL% || exit /b 1
