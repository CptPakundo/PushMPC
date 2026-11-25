// iina_bridge_host (CI, macOS): IINA's own libmpv (from the release DMG, unmodified) with the PushMPC bridge loaded
// as a C plugin, playing a synthetic clip headless:
//   iina_bridge_host <path to IINA's libmpv.2.dylib> <pushmpc-iina.so> <seconds>
// It sets vo=null, hwdec=no (software frames: the portable engine runs in a VM where VideoToolbox's ML effects fail),
// loads the bridge with load-script, adds vf=@pushmpc:lavfi=[null@pushmpc] and plays lavfi testsrc at 24 fps with
// PUSHMPC_IINA_K=2. It prints the bridge state (user-data/pushmpc/bridge), the filter's output rate
// (estimated-vf-fps) and the decoded / output frame counts. Exit 0 when the bridge is active and the output rate is
// about twice the source rate.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mpv/client.h>

#define SYM(name) typeof(&name) p_##name = (typeof(&name))dlsym(lib, #name); if (!p_##name) { fprintf(stderr, "no %s\n", #name); return 2; }

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: iina_bridge_host <libmpv.2.dylib> <pushmpc-iina.so> <seconds>\n"); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "dlopen %s: %s\n", argv[1], dlerror()); return 2; }
    SYM(mpv_create) SYM(mpv_initialize) SYM(mpv_set_option_string) SYM(mpv_command) SYM(mpv_get_property_string) SYM(mpv_get_property)
    SYM(mpv_wait_event) SYM(mpv_terminate_destroy) SYM(mpv_free)
    setenv("PUSHMPC_IINA_K", "2", 1);
    mpv_handle* h = p_mpv_create();
    p_mpv_set_option_string(h, "vo", "null");
    p_mpv_set_option_string(h, "ao", "null");
    p_mpv_set_option_string(h, "hwdec", "no");
    p_mpv_set_option_string(h, "untimed", "yes");
    p_mpv_set_option_string(h, "terminal", "yes");
    p_mpv_set_option_string(h, "msg-level", "all=warn,lavfi=v,ffmpeg=warn");
    p_mpv_set_option_string(h, "idle", "yes");
    if (p_mpv_initialize(h) < 0) { fprintf(stderr, "mpv_initialize failed\n"); return 2; }
    const char* load[] = { "load-script", argv[2], NULL };
    if (p_mpv_command(h, load) < 0) { fprintf(stderr, "load-script failed\n"); return 1; }
    usleep(300000);
    char* st = p_mpv_get_property_string(h, "user-data/pushmpc/bridge");
    printf("bridge: %s\n", st ? st : "(not reported)");
    const int active = st && !strcmp(st, "active");
    if (st) p_mpv_free(st);
    const char* vf[] = { "vf", "add", "@pushmpc:lavfi=[null@pushmpc]", NULL };
    p_mpv_command(h, vf);
    const char* play[] = { "loadfile", "av://lavfi:testsrc2=size=640x360:rate=24000/1001:duration=8,format=yuv420p", NULL };
    p_mpv_command(h, play);
    double vffps = 0, best = 0;
    const int secs = atoi(argv[3]);
    for (int i = 0; i < secs * 10; ++i) {
        mpv_event* e = p_mpv_wait_event(h, 0.1);
        if (e->event_id == MPV_EVENT_END_FILE) break;
        if (p_mpv_get_property(h, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &vffps) >= 0 && vffps > best) best = vffps;
    }
    char* fr = p_mpv_get_property_string(h, "estimated-frame-number");
    printf("output rate (estimated-vf-fps, peak): %.3f; frame number %s\n", best, fr ? fr : "?");
    if (fr) p_mpv_free(fr);
    p_mpv_terminate_destroy(h);
    const int ok = active && best > 1.8 * 23.976 && best < 2.2 * 23.976;
    printf("IINA_BRIDGE_RESULT=%s\n", ok ? "ok" : (active ? "active-but-rate-wrong" : "inactive"));
    return ok ? 0 : 1;
}
