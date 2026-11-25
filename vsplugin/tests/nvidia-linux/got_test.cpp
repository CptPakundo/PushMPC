// nvl_got_test <libnvl_gotdemo.so> <libnvl_cudademo.so>: the RTX 30 compatibility's GOT hooks (nvl_ampere.cpp) on a
// demo pair, on any Linux machine (no GPU): the slots are found, the attribute reads 9 instead of 6 and a fatbin
// arrives retargeted SM89 -> SM86 while the hooks are in, other callers of the same functions are unchanged, and
// everything is as before after Restore.
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "nvl_ampere.h"
#include "nvl_fatbin.h"

using namespace pmvs::nvl;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::vector<uint8_t> Sm89Fatbin()
{
    std::vector<uint8_t> b(16 + 64 + 128, 0);
    auto p32 = [&](size_t p, uint32_t x) { std::memcpy(b.data() + p, &x, 4); };
    auto p16 = [&](size_t p, uint16_t x) { std::memcpy(b.data() + p, &x, 2); };
    p32(0, kFatbinMagic); p16(4, 1); p16(6, 16); const uint64_t payload = b.size() - 16; std::memcpy(b.data() + 8, &payload, 8);
    p16(16, 2); p16(18, 0x101); p32(20, 64); const uint64_t bytes = 128; std::memcpy(b.data() + 24, &bytes, 8); p32(16 + 28, 89);
    p32(80, 0x464c457f); b[84] = 2; b[85] = 1; p16(80 + 18, 190); p32(80 + 48, 0x00590559);
    return b;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: nvl_got_test <libnvl_gotdemo.so> <libnvl_cudademo.so>\n"); return 2; }
    void* cuda = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    void* demo = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!cuda || !demo) { std::printf("dlopen: %s\n", dlerror()); return 2; }
    auto minor = reinterpret_cast<int (*)(int)>(dlsym(demo, "demo_minor"));
    auto load = reinterpret_cast<int (*)(const void*)>(dlsym(demo, "demo_load"));
    auto last = reinterpret_cast<const uint8_t* (*)(size_t*)>(dlsym(cuda, "demo_last_image"));
    auto direct = reinterpret_cast<int (*)(int*, int, int)>(dlsym(cuda, "cuDeviceGetAttribute"));
    CHECK(minor && load && last && direct, "demo symbols");
    CHECK(minor(0) == 6, "before: SM x.6");

    std::vector<GotSlot> slots; std::string why;
    CHECK(FindGotSlots(demo, { "cuDeviceGetAttribute", "cuModuleLoadData" }, slots, why), "slots: %s", why.c_str());
    for (const auto& s : slots) std::printf("  GOT slot %s at %p%s\n", s.symbol.c_str(), reinterpret_cast<void*>(s.address), s.relro ? " (RELRO)" : "");
    std::vector<GotSlot> none;
    CHECK(!FindGotSlots(demo, { "cuNotImported" }, none, why), "a symbol the library does not import has no slot");

    AmpereCompat compat;
    CHECK(compat.Install(demo, argv[1], cuda, 0, why, false), "install: %s", why.c_str());
    CHECK(minor(0) == 9, "hooked: SM x.9 on the selected device (got %d)", minor(0));
    CHECK(minor(1) == 6, "hooked: another device unchanged");
    int v = 0; direct(&v, 76, 0);
    CHECK(v == 6, "other callers of libcuda unchanged");
    const auto fb = Sm89Fatbin();
    CHECK(load(fb.data()) == 0, "load through the hook");
    size_t n = 0; const uint8_t* got = last(&n);
    uint32_t sm = 0, flags = 0;
    if (n == fb.size()) { std::memcpy(&sm, got + 16 + 28, 4); std::memcpy(&flags, got + 80 + 48, 4); }
    CHECK(n == fb.size() && sm == 86 && flags == 0x00560556, "the driver received the SM86 copy (sm %u flags 0x%08X)", sm, flags);
    std::memcpy(&sm, fb.data() + 16 + 28, 4);
    CHECK(sm == 89, "the caller's image untouched");
    CHECK(AmpereCompat::PatchedLoads() == 1 && AmpereCompat::AttributeSpoofs() >= 1 && AmpereCompat::RejectedLoads() == 0, "counters");
    AmpereCompat second;
    CHECK(!second.Install(demo, argv[1], cuda, 0, why, false), "a second owner is refused");

    CHECK(compat.Restore(), "restore");
    CHECK(minor(0) == 6, "restored: SM x.6");
    load(fb.data());
    got = last(&n); std::memcpy(&sm, got + 16 + 28, 4);
    CHECK(sm == 89, "restored: the original image reaches the driver");
    // the identity check refuses an unknown library
    AmpereCompat id;
    CHECK(!id.Install(demo, argv[1], cuda, 0, why, true) && why.find("does not recognise") != std::string::npos, "identity check: %s", why.c_str());
    std::printf("nvl_got_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
