@echo off
rem Builds bin\pushmpc_vs.dll (when third_party\vapoursynth has the SDK headers), bin\stub\pushcore.dll (the stub core),
rem build\sched_test.exe, build\core_test.exe and build\vs_harness.exe (paths relative to this script), then runs the tests
rem that need no VapourSynth runtime: build.cmd [notest]
rem Incremental: CMake runs only on a new tree. Settings (Visual Studio, compiler cache, parallel jobs):
rem scripts\build-env.cmd.
setlocal
set NoDefaultCurrentDirectoryInExePath=
set HERE=%~dp0
set HERE=%HERE:~0,-1%
call "%HERE%\..\scripts\build-env.cmd" || exit /b 1
set "PUSHMPC_CFG=-DCMAKE_BUILD_TYPE=Release"
call "%HERE%\..\scripts\cmake-configure.cmd" "%HERE%" "%HERE%\build" || exit /b 1
cmake --build "%HERE%\build" %PUSHMPC_PARALLEL% || exit /b 1
if /i "%~1"=="notest" exit /b 0
"%HERE%\build\portable_test.exe" || exit /b 1
"%HERE%\build\sched_test.exe" || exit /b 1
set PUSHMPC_VS_LOG_DIR=%HERE%\build\logs
"%HERE%\build\core_test.exe" "%HERE%\bin\stub" || exit /b 1
