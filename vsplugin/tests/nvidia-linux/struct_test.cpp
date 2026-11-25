// nvl_struct_test <runtime-dir>: the pinned NVIDIA Video Effects runtime (scripts/nvidia-vfx-pin.json, extracted by
// scripts/nvidia-vfx-fetch.sh) against the backend's loader, on a hosted runner with no NVIDIA GPU. libcuda.so.1 is a
// stub (make-cuda-stub.sh: every driver entry point the runtime imports, all answering "no device"), put first on
// LD_LIBRARY_PATH. Checks:
//   1. the loader: the stub binds, cuInit says no device, ProbeGpu reports it (Auto then picks another backend);
//   2. every library of the runtime dlopens with RTLD_NOW (all symbols resolve) through the wheel's RPATH layout;
//   3. the API the engine binds is all there; NvVFX_GetVersion answers 1.3;
//   4. NvCVImage_Create agrees with the enum values the engine uses (RGBA U8, RGB10A2 P32: 4 bytes per pixel);
//   5. the selectors: each effect is created (or refused with a GPU / CUDA error, never "selector") and each parameter
//      name the engine sets is accepted by its effect;
//   6. the RTX 30 compatibility on the real VFG library: identity (SHA-256 + size = the pin), its two GOT slots, the
//      hooks installed and restored; every fatbin in the library retargets (the census must equal the pin's 62);
//   7. the self-test refuses to run without a GPU.
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "nvl_ampere.h"
#include "nvl_engine.h"
#include "nvl_fatbin.h"
#include "nvl_selftest.h"
#include "nvl_sha256.h"
#include "nvl_vfx.h"

using namespace pmvs::nvl;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// NvCVImage's leading fields (nvCVImage.h): read only here, to check the enum values (the engine never uses the layout)
struct NvCVImageHead { unsigned width, height; int pitch; int pixelFormat; int componentType; unsigned char pixelBytes, componentBytes, numComponents, planar, gpuMem; };

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: nvl_struct_test <runtime-dir>\n"); return 2; }
    const std::string root = argv[1], libs = VfxApi::LibDir(root);
    setenv("PUSHMPC_NVVFX_DIR", root.c_str(), 1);

    // 1. the loader against the stub driver
    const GpuInfo g = ProbeGpu(0, root);
    std::printf("probe: driver %d, runtime %d, why: %s\n", g.driver, g.runtime, g.why.c_str());
    CHECK(!g.driver && g.why.find("cuInit") != std::string::npos, "no GPU reported through cuInit: %s", g.why.c_str());
    CHECK(g.runtime, "runtime found in %s: %s", root.c_str(), g.runtimeWhy.c_str());

    // 2. every library resolves (after the loader preloaded the runtime in dependency order, as the engine does)
    VfxApi vfx; std::string why;
    CHECK(vfx.Load(root, why), "VfxApi::Load: %s", why.c_str());
    const char* all[] = { "libNVCVImage.so", "libVideoFX.so", "libVideoFXLocal.so", "libnvVFXVideoSuperRes.so", "libnvVFXTrueHDR.so", "libnvVFXVideoFrameGeneration.so",
                          "libnvidia-ngx-vsr.so.1.8.3", "libnvidia-ngx-truehdr.so.1.2.3", "libnvngxruntime.so" };
    std::vector<void*> handles;
    for (const char* f : all) {
        void* h = dlopen((libs + "/" + f).c_str(), RTLD_NOW | RTLD_GLOBAL);
        CHECK(h != nullptr, "dlopen %s: %s", f, h ? "" : dlerror());
        handles.push_back(h);
    }

    // 3. the API
    if (!vfx.CreateEffect) { std::printf("nvl_struct_test: %d passed, %d failed\n", g_pass, g_fail); return 1; }
    unsigned version = 0;
    const NvCV_Status vs = vfx.GetVersion(&version);
    std::printf("NvVFX_GetVersion: status %d version 0x%08X\n", vs, version);
    CHECK(vs == NVCV_SUCCESS && (version >> 24) == 1 && ((version >> 16) & 0xFF) == 3, "SDK 1.3 (0x%08X)", version);

    // 4. enum values through NvCVImage_Create (CPU memory: no GPU needed)
    struct Fmt { int pf, ct; const char* name; } fmts[] = { { NVCV_RGBA, NVCV_U8, "RGBA U8" }, { NVCV_RGB10A2, NVCV_P32, "RGB10A2 P32" } };
    for (const auto& f : fmts) {
        void* im = nullptr;
        const NvCV_Status s = vfx.ImageCreate(8, 4, f.pf, f.ct, NVCV_CHUNKY, 0 /* NVCV_CPU */, 1, &im);
        CHECK(s == NVCV_SUCCESS && im, "NvCVImage_Create %s: %s", f.name, vfx.Error(s).c_str());
        if (im) {
            const auto* h = static_cast<const NvCVImageHead*>(im);
            std::printf("  %s: %ux%u pitch %d pixelFormat %d componentType %d pixelBytes %u componentBytes %u components %u planar %u gpu %u\n", f.name, h->width, h->height, h->pitch,
                        h->pixelFormat, h->componentType, h->pixelBytes, h->componentBytes, h->numComponents, h->planar, h->gpuMem);
            CHECK(h->width == 8 && h->height == 4 && h->pixelFormat == f.pf && h->componentType == f.ct && h->pixelBytes == 4 && h->pitch >= 32 && h->planar == 0, "%s layout", f.name);
            vfx.ImageDestroy(im);
        }
    }

    // 5. selectors
    struct Eff { const char* sel; std::vector<std::pair<const char*, unsigned>> u32; };
    const Eff effs[] = {
        { sel::kVsr, { { sel::kQuality, 1 }, { sel::kEncoding, 1 } } },
        { sel::kTrueHdr, { { sel::kContrast, 100 }, { sel::kSaturation, 100 }, { sel::kMiddleGray, 50 }, { sel::kLuminance, 1000 }, { sel::kDebandOff, 0 } } },
        { sel::kVfg, { { sel::kInW, 1920 }, { sel::kInH, 1080 }, { sel::kMode, 1 }, { sel::kAutoShot, 0 }, { sel::kMultiplier, 2 }, { sel::kIndex, 1 }, { sel::kShot, 0 } } },
    };
    void* bogus = nullptr;
    const NvCV_Status bs = vfx.CreateEffect("NoSuchEffect", &bogus);
    std::printf("CreateEffect(NoSuchEffect): %s\n", vfx.Error(bs).c_str());
    CHECK(bs != NVCV_SUCCESS, "an unknown selector is refused");
    for (const auto& e : effs) {
        void* h = nullptr;
        const NvCV_Status s = vfx.CreateEffect(e.sel, &h);
        std::printf("CreateEffect(%s): %s\n", e.sel, vfx.Error(s).c_str());
        CHECK(s == NVCV_SUCCESS || s != bs, "%s is a known effect (status %d, unknown-selector status %d)", e.sel, s, bs);
        if (s != NVCV_SUCCESS || !h) continue;
        const NvCV_Status bad = vfx.SetU32(h, "NoSuchParameter", 1);
        std::printf("  SetU32(%s, NoSuchParameter): %s\n", e.sel, vfx.Error(bad).c_str());
        CHECK(bad != NVCV_SUCCESS, "%s refuses an unknown parameter (so the accepted ones are real)", e.sel);
        for (const auto& [name, val] : e.u32) {
            const NvCV_Status ps = vfx.SetU32(h, name, val);
            std::printf("  SetU32(%s, %s=%u): %s\n", e.sel, name, val, vfx.Error(ps).c_str());
            CHECK(ps == NVCV_SUCCESS || (bad != NVCV_SUCCESS && ps != bad), "%s.%s accepted (status %d; unknown parameter gives %d)", e.sel, name, ps, bad);
        }
        const NvCV_Status ss = vfx.SetImage(h, sel::kSrc0, nullptr);
        std::printf("  SetImage(%s, SrcImage0, null): %s\n", e.sel, vfx.Error(ss).c_str());
        vfx.DestroyEffect(h);
    }

    // 6. the RTX 30 compatibility on the real VFG library
    const std::string vfgPath = libs + "/libnvVFXVideoFrameGeneration.so";
    std::string hex; uint64_t size = 0;
    CHECK(Sha256::File(vfgPath, hex, size) && hex == AmpereCompat::kKnownSha256 && size == AmpereCompat::kKnownSize, "VFG identity %s %llu", hex.c_str(), (unsigned long long)size);
    void* vfg = dlopen(vfgPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    void* cuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    std::vector<GotSlot> slots;
    CHECK(vfg && FindGotSlots(vfg, { "cuDeviceGetAttribute", "cuModuleLoadData" }, slots, why), "VFG GOT slots: %s", why.c_str());
    for (const auto& s : slots) std::printf("  VFG GOT slot %s%s\n", s.symbol.c_str(), s.relro ? " (RELRO)" : "");
    {
        AmpereCompat c;
        CHECK(c.Install(vfg, vfgPath, cuda, 0, why, true), "install on the real VFG library: %s", why.c_str());
        CHECK(c.Restore(), "restore");
    }
    std::ifstream f(vfgPath, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    unsigned fatbins = 0, changed = 0, rejected = 0;
    for (size_t p = 0; p + 16 <= bytes.size(); ++p) {
        if (bytes[p] != 0x50 || bytes[p + 1] != 0xED || bytes[p + 2] != 0x55 || bytes[p + 3] != 0xBA) continue;
        const size_t n = FatbinSize(bytes.data() + p);
        if (!n || p + n > bytes.size()) continue;
        std::vector<uint8_t> out; FatbinChanges ch;
        ++fatbins;
        if (RetargetVfgFatbin(bytes.data() + p, n, out, ch, why)) changed += ch.containers; else { ++rejected; std::printf("  fatbin at %zu rejected: %s\n", p, why.c_str()); }
        p += n - 1;
    }
    std::printf("VFG fatbins: %u, SM89 containers retargeted: %u, rejected: %u\n", fatbins, changed, rejected);
    CHECK(fatbins == 62 && changed == 62 && rejected == 0, "census 62 / 62 / 0");

    // 7. the self-test refuses without a GPU
    std::string report;
    CHECK(!SelfTestRun(g, true, report), "self-test without a GPU fails: %s", report.c_str());

    std::printf("nvl_struct_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
