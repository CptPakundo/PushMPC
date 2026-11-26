@echo off
rem Builds bin\PushMpcFilter.ax, bin\PushMpcExit.dll and build\auto_test.exe (paths relative to this script).
rem Incremental: CMake runs only on a new tree or changed options; a compiler cache is used when one is installed.
rem Settings (Visual Studio, compiler cache, parallel jobs): scripts\build-env.cmd. PUSHMPC_FILTER_CMAKE_ARGS: more
rem CMake options (for example -DPUSHMPC_MPCVR_DIR=<MPC Video Renderer source>).
setlocal
set NoDefaultCurrentDirectoryInExePath=
set HERE=%~dp0
set HERE=%HERE:~0,-1%
call "%HERE%\..\scripts\build-env.cmd" || exit /b 1
set "PUSHMPC_CFG=-DCMAKE_BUILD_TYPE=Release %PUSHMPC_FILTER_CMAKE_ARGS%"
call "%HERE%\..\scripts\cmake-configure.cmd" "%HERE%" "%HERE%\build" || exit /b 1
cmake --build "%HERE%\build" %PUSHMPC_PARALLEL% || exit /b 1
