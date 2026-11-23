// pushcore_hip_preflight.exe: the child process of the AMD HIP self-test (PushFPS's AmdHipPreflight.h, issue 88).
// PushFPS runs it as "PushFPS.exe --amd-hip-preflight ..."; pushcore runs inside MPC-HC, so the same child is this
// small program next to pushcore.dll (core\), set with amdhip::setPreflightExecutable when pushcore opens on AMD.
// No window, no player state: only the HIP calls and their result lines on the pipe.
#include "veyra/pipeline/AmdHipPreflight.h"
#include <windows.h>
#include <shellapi.h>
int main()
{
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    const int code = wide ? veyra::pipeline::amdhip::preflightMain(count, wide) : 2;
    if (wide) LocalFree(wide);
    return code;
}
