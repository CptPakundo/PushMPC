/* pushmpc_content_test: PushMPC's content library through its C interface, loaded the way the VLC module loads it
 * (vlcplugin/src/pm_content.c, from this program's folder). CPU only, synthetic frames, no media files.
 *
 *   loader and ABI; bad arguments refused; the decision appears after the first window and not before; a seek keeps it;
 *   the image model's evidence (given by the caller here) drives drawn -> drawn + rendered CG -> not drawn, with the
 *   policy for each (no _DS upscaler for rendered CG); every input layout (8-bit planar, 10-bit planar, NV12, P010, full
 *   range) measures the same picture alike; the worker thread (queue, drop, destroy with frames queued); without the
 *   model: the hand-built path. With PMC_TEST_MODEL=1 and PMML_RUNTIME set to a runtime with the model and ONNX Runtime
 *   (ort/core, CPU only): the image model runs and its evidence reaches the decision.
 * Exit code: the number of failed checks. */
#include "pm_content.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
static void SleepMs(int ms) { Sleep((DWORD)ms); }
static void SetEnv(const char *k, const char *v) { _putenv_s(k, v); }
static double NowMs(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return 1000.0 * (double)c.QuadPart / (double)f.QuadPart; }
#else
#include <unistd.h>
static void SleepMs(int ms) { usleep((useconds_t)ms * 1000); }
static void SetEnv(const char *k, const char *v) { setenv(k, v, 1); }
static double NowMs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void Log(void *user, const char *line) { (void)user; if (getenv("PMC_TEST_VERBOSE")) printf("  [log] %s\n", line); }

/* ---- synthetic pictures: 8-bit I420, video range */
enum { W = 640, H = 360 };
typedef struct { uint8_t y[W * H], u[W * H / 4], v[W * H / 4]; } Frame;
static uint32_t rng = 12345;
static int Rnd(int n) { rng = rng * 1664525u + 1013904223u; return (int)((rng >> 8) % (uint32_t)n); }
/* drawn: flat colour fields, a character (a filled disc with a dark outline) that moves on twos over a background that
 * holds; live: a textured gradient panning every frame, sensor noise */
static void MakeDrawn(Frame *f, int n)
{
    const int step = n / 2, cx = 120 + (step * 7) % 400, cy = 180 + (int)(60 * sin(step * 0.3)), r = 50;
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++) {
            int yv = j < H / 3 ? 180 : (i < W / 2 ? 120 : 90);
            const int d2 = (i - cx) * (i - cx) + (j - cy) * (j - cy);
            if (d2 < r * r) yv = d2 > (r - 3) * (r - 3) ? 30 : 200;
            f->y[j * W + i] = (uint8_t)yv;
        }
    for (int j = 0; j < H / 2; j++)
        for (int i = 0; i < W / 2; i++) {
            const int d2 = (2 * i - cx) * (2 * i - cx) + (2 * j - cy) * (2 * j - cy);
            const int inside = d2 < (r - 3) * (r - 3);
            f->u[j * W / 2 + i] = (uint8_t)(inside ? 100 : j < H / 6 ? 150 : 110);
            f->v[j * W / 2 + i] = (uint8_t)(inside ? 170 : j < H / 6 ? 110 : 140);
        }
}
static void MakeLive(Frame *f, int n)
{
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++) {
            const int x = i + n * 3;
            int yv = 60 + (x * 120) / (W + 600) + (int)(25 * sin(x * 0.07) * cos(j * 0.05)) + Rnd(9) - 4;
            f->y[j * W + i] = (uint8_t)(yv < 16 ? 16 : yv > 235 ? 235 : yv);
        }
    for (int k = 0; k < W * H / 4; k++) { f->u[k] = (uint8_t)(120 + Rnd(5)); f->v[k] = (uint8_t)(132 + Rnd(5)); }
}

/* the same picture in other layouts */
enum Layout { I420, I0AL, NV12, P010, J420 };
static uint16_t y16[W * H], c16[W * H / 2];
static uint8_t c8[W * H / 2], yj[W * H], uj[W * H / 4], vj[W * H / 4];
static int Push(pm_content *c, const Frame *f, enum Layout l, double t, int disc)
{
    switch (l) {
    case I420: return pm_content_push(c, f->y, W, f->u, f->v, W / 2, W, H, 8, 1, t, disc);
    case I0AL:   /* 10 bits in the low bits of 16 */
        for (int k = 0; k < W * H; k++) y16[k] = (uint16_t)(f->y[k] << 2);
        for (int k = 0; k < W * H / 4; k++) { c16[k] = (uint16_t)(f->u[k] << 2); c16[W * H / 4 + k] = (uint16_t)(f->v[k] << 2); }
        return pm_content_push(c, y16, W * 2, c16, c16 + W * H / 4, W, W, H, 10, 1, t, disc);
    case NV12:
        for (int k = 0; k < W * H / 4; k++) { c8[2 * k] = f->u[k]; c8[2 * k + 1] = f->v[k]; }
        return pm_content_push(c, f->y, W, c8, NULL, W, W, H, 8, 1, t, disc);
    case P010:   /* 10 bits in the top bits of 16 */
        for (int k = 0; k < W * H; k++) y16[k] = (uint16_t)(f->y[k] << 8);
        for (int k = 0; k < W * H / 4; k++) { c16[2 * k] = (uint16_t)(f->u[k] << 8); c16[2 * k + 1] = (uint16_t)(f->v[k] << 8); }
        return pm_content_push(c, y16, W * 2, c16, NULL, W * 2, W, H, 16, 1, t, disc);
    case J420:   /* full range */
        for (int k = 0; k < W * H; k++) yj[k] = (uint8_t)((f->y[k] - 16) * 255 / 219 < 0 ? 0 : (f->y[k] - 16) * 255 / 219 > 255 ? 255 : (f->y[k] - 16) * 255 / 219);
        for (int k = 0; k < W * H / 4; k++) { uj[k] = (uint8_t)(128 + (f->u[k] - 128) * 255 / 224); vj[k] = (uint8_t)(128 + (f->v[k] - 128) * 255 / 224); }
        return pm_content_push(c, yj, W, uj, vj, W / 2, W, H, 8, 0, t, disc);
    }
    return -1;
}

static Frame fr;
static int paceMs;   /* the worker thread's runs: frames handed over at about playback pace, not as fast as made */
static pmc_state Run(pm_content *c, int drawn, enum Layout l, int frames, double fps, int start)
{
    pmc_state s;
    for (int n = start; n < start + frames; n++) {
        if (drawn) MakeDrawn(&fr, n); else MakeLive(&fr, n);
        Push(c, &fr, l, n / fps, 0);
        if (paceMs) SleepMs(paceMs);
    }
    pm_content_state(c, &s);
    return s;
}

int main(void)
{
    char why[400], where[700], pol[200];
    /* never the user's own runtime: the model runs only from a runtime this test is pointed at */
    const int wantModel = getenv("PMC_TEST_MODEL") && atoi(getenv("PMC_TEST_MODEL")) == 1;
    SetEnv("LOCALAPPDATA", "pmc-test-none");
    SetEnv("HOME", "pmc-test-none");
    SetEnv("XDG_DATA_HOME", "pmc-test-none");
    if (!wantModel) SetEnv("PMML_RUNTIME", "pmc-test-none");

    /* 1. loader, ABI, arguments */
    pm_content *c = pm_content_open(Log, NULL, PMC_FLAG_NO_MODEL | PMC_FLAG_SYNC, why, sizeof(why), where, sizeof(where));
    CHECK(c != NULL, "the library did not load: %s", why);
    if (!c) { printf("%d of %d checks failed\n", fails, checks); return fails ? fails : 1; }
    printf("loaded %s\n", where);
    pmc_state s;
    CHECK(pm_content_state(c, &s) == 0 && s.cls == PMC_CLASS_MEASURING && s.model == PMC_MODEL_OFF && s.policy == 0,
          "initial state cls %d model %d policy %u", s.cls, s.model, s.policy);
    MakeDrawn(&fr, 0);
    CHECK(pm_content_push(c, NULL, W, fr.u, fr.v, W / 2, W, H, 8, 1, 0, 0) < 0, "NULL luma taken");
    CHECK(pm_content_push(c, fr.y, W, NULL, NULL, W / 2, W, H, 8, 1, 0, 0) < 0, "NULL chroma taken");
    CHECK(pm_content_push(c, fr.y, W, fr.u, fr.v, W / 2, 8, 8, 8, 1, 0, 0) < 0, "8x8 taken");
    CHECK(pm_content_push(c, fr.y, W, fr.u, fr.v, W / 2, W, H, 7, 1, 0, 0) < 0, "7 bits taken");
    CHECK(strcmp(pm_content_class_name(c, 1), "drawn animation") == 0, "class name %s", pm_content_class_name(c, 1));

    /* 2. measuring until the first window, then a decision; the cost per frame */
    s = Run(c, 1, I420, 20, 24.0, 0);   /* 0.8 s */
    CHECK(s.cls == PMC_CLASS_MEASURING && s.windows == 0, "decided before 1 s: cls %d windows %u", s.cls, s.windows);
    const double t0 = NowMs();
    s = Run(c, 1, I420, 220, 24.0, 20);   /* to 10 s */
    const double perFrame = (NowMs() - t0) / 220.0;
    CHECK(s.cls != PMC_CLASS_MEASURING && s.windows > 10 && s.first_decision_sec > 0, "no decision after 10 s: windows %u", s.windows);
    CHECK(s.processed == s.frames && s.frames == 240, "frames %llu processed %llu", (unsigned long long)s.frames, (unsigned long long)s.processed);
    printf("drawn clip, hand-built cues: %s (%.0f %%, drawn share %.2f) after %.1f s; caller %.2f ms + detector %.2f ms a frame "
           "(max %.2f) at %dx%d; %.2f ms a frame in all\n", pm_content_class_name(c, s.cls), 100.0 * s.confidence, s.drawn_share,
           s.first_decision_sec, s.caller_ms, s.detect_ms, s.detect_ms_max, W, H, perFrame);
    const int clsBefore = s.cls; const uint32_t changesBefore = s.changes;
    MakeDrawn(&fr, 240);
    Push(c, &fr, I420, 10.0, 1);   /* a seek: the decision stays */
    pm_content_state(c, &s);
    CHECK(s.cls == clsBefore && s.changes == changesBefore, "a seek changed the decision");
    pm_content_close(c);

    c = pm_content_open(Log, NULL, PMC_FLAG_NO_MODEL | PMC_FLAG_SYNC, why, sizeof(why), where, sizeof(where));
    s = Run(c, 0, I420, 240, 24.0, 0);
    printf("live clip, hand-built cues: %s (%.0f %%, drawn share %.2f)\n", pm_content_class_name(c, s.cls), 100.0 * s.confidence, s.drawn_share);
    CHECK(s.cls != PMC_CLASS_MEASURING, "live clip not decided");
    pm_content_close(c);

    /* 3. the image model's evidence (from the caller): drawn -> rendered CG -> not drawn, and the policy of each */
    c = pm_content_open(Log, NULL, PMC_FLAG_NO_MODEL | PMC_FLAG_SYNC, why, sizeof(why), where, sizeof(where));
    int n = 0; uint32_t serial = 0;
    for (; n < 24 * 6; n++) { if (n % 24 == 0) pm_content_evidence(c, 0.999, n / 24.0, 0.01); MakeLive(&fr, n); Push(c, &fr, I420, n / 24.0, 0); }
    pm_content_state(c, &s);
    pm_content_policy_text(s.policy, pol, sizeof(pol));
    CHECK(s.cls == PMC_CLASS_DRAWN && !s.cg && s.image_evidence, "evidence drawn: cls %d cg %d img %d", s.cls, s.cg, s.image_evidence);
    CHECK(s.policy == (PMC_POLICY_HOLD_REPAIR | PMC_POLICY_ANIME_UPSCALER | PMC_POLICY_SIGN_HOLD |
                       PMC_POLICY_LINE_ART | PMC_POLICY_ANIME_DEBAND), "hand-drawn policy %s", pol);
    printf("evidence drawn: %s, policy %s\n", pm_content_class_name(c, s.cls), pol);
    CHECK(s.serial > serial, "serial did not move"); serial = s.serial;
    for (; n < 24 * 10; n++) { if (n % 24 == 0) pm_content_evidence(c, 0.999, n / 24.0, 0.99); MakeLive(&fr, n); Push(c, &fr, I420, n / 24.0, 0); }
    pm_content_state(c, &s);
    pm_content_policy_text(s.policy, pol, sizeof(pol));
    CHECK(s.cls == PMC_CLASS_DRAWN && s.cg == 1 && s.cg_share > 0.9f, "evidence CG: cls %d cg %d share %.2f", s.cls, s.cg, s.cg_share);
    CHECK(s.policy == (PMC_POLICY_HOLD_REPAIR | PMC_POLICY_ANIME_UPSCALER), "rendered CG policy %s", pol);
    CHECK(!(s.policy & PMC_POLICY_ANIME_UPSCALER_DS), "rendered CG allows the _DS upscaler");
    printf("evidence rendered CG: policy %s\n", pol);
    CHECK(s.serial > serial, "serial did not move on CG"); serial = s.serial;
    for (; n < 24 * 30; n++) { if (n % 24 == 0) pm_content_evidence(c, 0.001, n / 24.0, -1); MakeDrawn(&fr, n); Push(c, &fr, I420, n / 24.0, 0); }
    pm_content_state(c, &s);
    CHECK(s.cls == PMC_CLASS_NOT_DRAWN && !s.cg && s.policy == 0 && s.changes >= 1, "evidence not drawn: cls %d cg %d policy %u changes %u",
          s.cls, s.cg, s.policy, s.changes);
    printf("evidence not drawn: %s, policy %u, class changes %u\n", pm_content_class_name(c, s.cls), s.policy, s.changes);
    pm_content_close(c);

    /* 4. every layout measures the same picture alike */
    {
        static const char *names[] = { "I420", "I0AL", "NV12", "P010", "J420" };
        pmc_state r[5];
        for (int l = 0; l < 5; l++) {
            c = pm_content_open(Log, NULL, PMC_FLAG_NO_MODEL | PMC_FLAG_SYNC, why, sizeof(why), where, sizeof(where));
            rng = 777; r[l] = Run(c, 1, (enum Layout)l, 120, 24.0, 0);
            pm_content_close(c);
            CHECK(r[l].frames == 120 && r[l].windows == r[0].windows && r[l].cls == r[0].cls, "%s: frames %llu windows %u cls %d (I420: %u, %d)",
                  names[l], (unsigned long long)r[l].frames, r[l].windows, r[l].cls, r[0].windows, r[0].cls);
            CHECK(fabsf(r[l].drawn_share - r[0].drawn_share) < 0.05f, "%s: drawn share %.3f, I420 %.3f", names[l], r[l].drawn_share, r[0].drawn_share);
        }
        printf("layouts: drawn share I420 %.3f I0AL %.3f NV12 %.3f P010 %.3f J420 %.3f\n", r[0].drawn_share, r[1].drawn_share,
               r[2].drawn_share, r[3].drawn_share, r[4].drawn_share);
    }

    /* 5. the worker thread: everything taken or dropped as a gap; destroy with frames queued */
    c = pm_content_open(Log, NULL, PMC_FLAG_NO_MODEL, why, sizeof(why), where, sizeof(where));
    for (n = 0; n < 200; n++) { MakeDrawn(&fr, n); Push(c, &fr, I420, n / 24.0, 0); }
    for (int i = 0; i < 200; i++) { pm_content_state(c, &s); if (s.processed + s.dropped >= s.frames) break; SleepMs(20); }
    CHECK(s.frames == 200 && s.processed + s.dropped == 200 && s.processed > 0, "worker: frames %llu processed %llu dropped %llu",
          (unsigned long long)s.frames, (unsigned long long)s.processed, (unsigned long long)s.dropped);
    printf("worker: %llu frames, %llu taken, %llu dropped (detector behind: frames handed over faster than played), caller %.2f ms "
           "and detector %.2f ms a frame\n", (unsigned long long)s.frames, (unsigned long long)s.processed, (unsigned long long)s.dropped,
           s.caller_ms, s.detect_ms);
    for (n = 0; n < 50; n++) { MakeDrawn(&fr, n); Push(c, &fr, I420, 20 + n / 24.0, 0); }
    pm_content_close(c);   /* frames still queued */
    CHECK(1, "destroy with frames queued");

    /* the cost on the caller's thread at 1080p (cell grid, the thumbnail once a second, the queue): 10-bit P010, paced */
    {
        enum { BW = 1920, BH = 1080 };
        uint16_t *by = malloc(sizeof(uint16_t) * BW * BH), *bc = malloc(sizeof(uint16_t) * BW * BH / 2);
        c = pm_content_open(Log, NULL, PMC_FLAG_NO_MODEL, why, sizeof(why), where, sizeof(where));
        if (by && bc && c) {
            for (n = 0; n < 96; n++) {
                for (int j = 0; j < BH; j++) for (int i = 0; i < BW; i++) by[j * BW + i] = (uint16_t)(((i + 4 * n) & 255) << 8);
                for (int k = 0; k < BW * BH / 2; k++) bc[k] = (uint16_t)(128 << 8);
                CHECK(pm_content_push(c, by, BW * 2, bc, NULL, BW * 2, BW, BH, 16, 1, n / 24.0, 0) == 0, "1080p P010 refused");
                SleepMs(30);
            }
            for (int i = 0; i < 200; i++) { pm_content_state(c, &s); if (s.processed + s.dropped >= s.frames) break; SleepMs(20); }
            printf("1080p P010: caller %.2f ms a frame; detector %.2f ms a frame (max %.2f) on its own thread; %llu of %llu dropped\n",
                   s.caller_ms, s.detect_ms, s.detect_ms_max, (unsigned long long)s.dropped, (unsigned long long)s.frames);
        }
        pm_content_close(c); free(by); free(bc);
    }

    /* 6. the model asked for: absent here (no runtime) -> the hand-built path; or, with PMC_TEST_MODEL=1, it runs */
    paceMs = 30;
    c = pm_content_open(Log, NULL, 0, why, sizeof(why), where, sizeof(where));
    CHECK(c != NULL, "open with the model: %s", why);
    if (c) {
        pm_content_state(c, &s);
        if (s.model == PMC_MODEL_NOT_BUILT) {
            s = Run(c, 1, I420, 240, 24.0, 0);
            for (int i = 0; i < 200; i++) { pm_content_state(c, &s); if (s.processed + s.dropped >= 240) break; SleepMs(20); }
            CHECK(s.model == PMC_MODEL_NOT_BUILT && s.cls != PMC_CLASS_MEASURING && !s.image_evidence, "no pmml: model %d cls %d", s.model, s.cls);
            printf("model: not in this build; the hand-built cues decided %s\n", pm_content_class_name(c, s.cls));
        } else if (!wantModel) {
            for (int i = 0; i < 300; i++) { pm_content_state(c, &s); if (s.model == PMC_MODEL_FAILED) break; SleepMs(10); }
            CHECK(s.model == PMC_MODEL_FAILED, "no runtime: model state %d (want failed)", s.model);
            s = Run(c, 1, I420, 240, 24.0, 0);
            for (int i = 0; i < 200; i++) { pm_content_state(c, &s); if (s.processed + s.dropped >= 240) break; SleepMs(20); }
            CHECK(s.cls != PMC_CLASS_MEASURING && !s.image_evidence, "no runtime: cls %d image evidence %d", s.cls, s.image_evidence);
            printf("model: absent (no runtime): the hand-built cues decided %s\n", pm_content_class_name(c, s.cls));
        } else {   /* real time: about one thumbnail a second until the model has answered a few times */
            const double start = NowMs();
            int img = 0;
            for (n = 0; NowMs() - start < 120000; n++) {
                MakeDrawn(&fr, n); Push(c, &fr, I420, n / 24.0, 0);
                pm_content_state(c, &s);
                if (s.model == PMC_MODEL_FAILED) break;
                if (s.image_evidence) img++;
                if (img > 48 && s.cls != PMC_CLASS_MEASURING) break;
                SleepMs(41);
            }
            pm_content_state(c, &s);
            CHECK(s.model == PMC_MODEL_RUNNING && s.image_evidence, "model: state %d image evidence %d", s.model, s.image_evidence);
            printf("model: %s; last run %.0f ms; %s (%.0f %%, drawn share %.2f, CG share %.2f) after %.1f s of frames\n",
                   s.model == PMC_MODEL_RUNNING ? "running" : "not running", s.model_ms, pm_content_class_name(c, s.cls),
                   100.0 * s.confidence, s.drawn_share, s.cg_share, n / 24.0);
        }
        pm_content_close(c);
    }

    printf("%d of %d checks failed\n", fails, checks);
    return fails;
}
