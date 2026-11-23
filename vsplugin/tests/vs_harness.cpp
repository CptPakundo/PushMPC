// Loads a .vpy through VSScript (API 4), pulls every output frame and checks pushmpc.Enhance's contract:
//   vs_harness <script.vpy> <expected multiplier> [<source frames>] [--seek <n>] [--reload <count>]
// - the output clip has source frames x k frames and k x the source rate;
// - every frame carries _DurationNum/_DurationDen = the source duration / k and PushMPCFrameKind;
// - --seek n: also pulls frames from n on after the sequential run (a jump), and --reload c evaluates the script c more
//   times in this process (mpv's reload on every seek), so the plugin log must show "flush (warm core reused)".
// Needs VSScript.dll (VapourSynth R55+) on the DLL search path and VapourSynth4.h / VSScript4.h to build.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include "VSScript4.h"

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: vs_harness <script.vpy> <k> [<source frames>] [--seek n] [--reload c]\n"); return 2; }
    const char* script = argv[1];
    const int k = atoi(argv[2]);
    int srcFrames = argc > 3 && argv[3][0] != '-' ? atoi(argv[3]) : -1;
    int seek = -1, reloads = 0;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--seek") && i + 1 < argc) seek = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reload") && i + 1 < argc) reloads = atoi(argv[++i]);
    }
    HMODULE vss = LoadLibraryW(L"VSScript.dll");
    if (!vss) { std::printf("VSScript.dll not found\n"); return 2; }
    auto getApi = reinterpret_cast<const VSSCRIPTAPI* (VS_CC*)(int)>(GetProcAddress(vss, "getVSScriptAPI"));
    const VSSCRIPTAPI* sapi = getApi ? getApi(VSSCRIPT_API_VERSION) : nullptr;
    if (!sapi) { std::printf("getVSScriptAPI(%d) failed\n", VSSCRIPT_API_VERSION); return 2; }
    const VSAPI* vsapi = sapi->getVSAPI(VAPOURSYNTH_API_VERSION);
    int fails = 0;
    for (int pass = 0; pass <= reloads; ++pass) {
        VSScript* se = sapi->createScript(nullptr);
        if (sapi->evaluateFile(se, script)) { std::printf("evaluate: %s\n", sapi->getError(se)); sapi->freeScript(se); return 1; }
        VSNode* node = sapi->getOutputNode(se, 0);
        if (!node) { std::printf("no output node\n"); sapi->freeScript(se); return 1; }
        const VSVideoInfo* vi = vsapi->getVideoInfo(node);
        std::printf("pass %d: %dx%d, %d frames, %lld/%lld fps, %d-bit family %d\n", pass, vi->width, vi->height, vi->numFrames, (long long)vi->fpsNum, (long long)vi->fpsDen,
                    vi->format.bitsPerSample, vi->format.colorFamily);
        if (srcFrames > 0 && vi->numFrames != srcFrames * k) { std::printf("FAIL frame count %d != %d x %d\n", vi->numFrames, srcFrames, k); ++fails; }
        const int last = pass == 0 ? vi->numFrames : std::min(vi->numFrames, 3 * k);   // reload passes: a few frames
        int kinds[3] = {};
        auto pull = [&](int from, int to) {
            for (int n = from; n < to; ++n) {
                char err[512] = {};
                const VSFrame* f = vsapi->getFrame(n, node, err, sizeof(err));
                if (!f) { std::printf("FAIL frame %d: %s\n", n, err); ++fails; return; }
                const VSMap* p = vsapi->getFramePropertiesRO(f);
                int e1 = 0, e2 = 0, e3 = 0;
                const int64_t dn = vsapi->mapGetInt(p, "_DurationNum", 0, &e1), dd = vsapi->mapGetInt(p, "_DurationDen", 0, &e2);
                const int64_t kind = vsapi->mapGetInt(p, "PushMPCFrameKind", 0, &e3);
                if (e1 || e2 || e3) { std::printf("FAIL frame %d: missing properties\n", n); ++fails; }
                else {
                    if (kind >= 0 && kind < 3) kinds[kind]++;
                    // duration x k x fps == 1
                    if (vi->fpsNum > 0 && dn * vi->fpsNum != dd * vi->fpsDen) { std::printf("FAIL frame %d: duration %lld/%lld vs %lld/%lld fps\n", n, (long long)dn, (long long)dd, (long long)vi->fpsNum, (long long)vi->fpsDen); ++fails; }
                }
                vsapi->freeFrame(f);
            }
        };
        pull(0, last);
        if (pass == 0 && seek >= 0 && seek < vi->numFrames) pull(seek, std::min(vi->numFrames, seek + 4 * k));
        std::printf("pass %d: real %d, generated %d, repeated %d\n", pass, kinds[0], kinds[1], kinds[2]);
        vsapi->freeNode(node);
        sapi->freeScript(se);
    }
    std::printf("vs_harness: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
