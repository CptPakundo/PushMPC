@echo off
rem Configures a CMake build tree (Ninja) for the build scripts, but only when it is needed: when the tree has no
rem build.ninja yet, or when its options differ from the ones it was configured with (kept in
rem <build dir>\pushmpc-configure.txt). Otherwise the build itself reruns CMake when a CMakeLists.txt changed.
rem usage (after build-env.cmd): set "PUSHMPC_CFG=<cmake options>" & call cmake-configure.cmd <source> <build dir>
rem PUSHMPC_CFG_NEW: options for a new tree only (an existing tree keeps what it was configured with; they are not part
rem of the comparison). The compiler cache options from build-env.cmd (PUSHMPC_LAUNCHER) are added here.
setlocal
set "_src=%~1"
set "_bld=%~2"
set _new=
if not exist "%_bld%\build.ninja" set "_new=%PUSHMPC_CFG_NEW%"
if not exist "%_bld%\" mkdir "%_bld%" || exit /b 1
> "%_bld%\pushmpc-configure.new" echo(-S "%_src%" -G Ninja %PUSHMPC_LAUNCHER% %PUSHMPC_CFG%
if exist "%_bld%\build.ninja" if exist "%_bld%\pushmpc-configure.txt" fc /b "%_bld%\pushmpc-configure.new" "%_bld%\pushmpc-configure.txt" >nul 2>&1 && (del "%_bld%\pushmpc-configure.new" & exit /b 0)
del "%_bld%\pushmpc-configure.txt" 2>nul
cmake -S "%_src%" -B "%_bld%" -G Ninja %PUSHMPC_LAUNCHER% %_new% %PUSHMPC_CFG% >nul || exit /b 1
move /y "%_bld%\pushmpc-configure.new" "%_bld%\pushmpc-configure.txt" >nul
exit /b 0
