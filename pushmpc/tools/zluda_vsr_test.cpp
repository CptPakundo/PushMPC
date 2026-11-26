// zluda_vsr_test: the parts of the ZLUDA VSR route that run without an AMD GPU (pushmpc/include/pushmpc/zluda_vsr.h,
// pushmpc/src/ZludaVsrClient.h): NGX CUDA parameter packing, the worker protocol, LZ4 + the cubin -> PTX index, the
// picture check, the refusals ("no AMD GPU", "no ZLUDA", "no nvngx_vsr"), the self-test record, and the whole worker
// round trip with the STUB worker on WARP (frames, a worker that exits, one that hangs, one that refuses). Never on a
// hardware GPU: every D3D12 device here is WARP.
//
//   zluda_vsr_test [--stub <zluda_vsr_stub_worker.exe>] [--real <pushcore_zluda_vsr_worker.exe>] [--vsr-dll <nvngx_vsr.dll>]
// Exit code = number of failed checks.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "pushmpc/unlocks.h"
#include "pushmpc/zluda_vsr.h"
#include "../src/ZludaVsrClient.h"

using Microsoft::WRL::ComPtr;

namespace {
int g_fail = 0, g_pass = 0;
void Check(bool ok, const std::string& what)
{
    if (ok) ++g_pass; else ++g_fail;
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    fflush(stdout);
}

// ---- packing
struct Recorder {
    std::map<std::string, unsigned> u; std::map<std::string, void*> p;
    void Set(const char* n, unsigned v) { u[n] = v; }
    void Set(const char* n, void* v) { p[n] = v; }
};
void TestPacking()
{
    printf("[packing]\n");
    void* ctx = reinterpret_cast<void*>(uintptr_t(0x1111)); void* stream = reinterpret_cast<void*>(uintptr_t(0x2222));
    for (uint32_t layout : pmz::kLayoutOrder) {
        Recorder c; pmz::PackCreate(c, 960, 540, 1920, 1080, 3, layout, ctx, stream);
        const bool withCtx = !(layout & pmz::kLayoutNoContext);
        Check(c.u["Width"] == 960 && c.u["Height"] == 540 && c.u["OutWidth"] == 1920 && c.u["OutHeight"] == 1080 && c.u["VSR.QualityLevel"] == 3 &&
              c.u["CreationNodeMask"] == 1 && c.u["VisibilityNodeMask"] == 1, "create: sizes, quality, node masks (layout " + std::to_string(layout) + ")");
        Check(withCtx ? (c.p["Input1"] == ctx && c.p["Input2"] == stream) : (c.p.count("Input1") == 0 && c.p.count("Input2") == 0),
              withCtx ? "create: Input1 = CUcontext, Input2 = CUstream (NVIDIA's CUDA feature convention)" : "create: no context / stream (layout bit 2)");
        pmz::CudaImage in{ 0xABCDull, 960, 540, 3840, 28 }, out{ 0xBEEFull, 1920, 1080, 7680, 28 };
        Recorder e; pmz::PackEvaluate(e, in, out, 3, layout);
        const bool value = layout & pmz::kLayoutHandleValue;
        Check(value ? (e.p["Input1"] == reinterpret_cast<void*>(uintptr_t(0xABCD)) && e.p["Output"] == reinterpret_cast<void*>(uintptr_t(0xBEEF)))
                    : (e.p["Input1"] == &in && e.p["Output"] == &out),
              value ? "evaluate: images as the texture / surface object value" : "evaluate: images as a pointer to { object, width, height, pitch, format }");
        Check(e.u["Rect.X"] == 0 && e.u["Rect.Y"] == 0 && e.u["Rect.W"] == 960 && e.u["Rect.H"] == 540 && e.u["OutRect.W"] == 1920 && e.u["OutRect.H"] == 1080 && e.u["VSR.QualityLevel"] == 3,
              "evaluate: Rect / OutRect / quality");
    }
    pmz::CudaImage img{ 0x123456789ull, 1, 2, 3, 4 };
    uint64_t first = 0; memcpy(&first, &img, 8);
    Check(first == 0x123456789ull && sizeof(img) == 24, "descriptor: the object is the first 8 bytes (what NGX reads)");
}

// ---- protocol
void TestProtocol()
{
    printf("[protocol]\n");
    pmz::Init i = pmz::MakeInit();
    i.inW = 1920; i.inH = 1080; i.outW = 3840; i.outH = 2160; i.quality = 2; i.handles = { 1, 2, 3, 4 };
    pmz::PutPath(i.zluda, L"C:\\z"); pmz::PutPath(i.vsr, L"C:\\v.dll"); pmz::PutPath(i.cache, L"C:\\c"); pmz::PutPath(i.log, L"C:\\l.log");
    Check(pmz::ValidateInit(i).empty(), "a valid initialisation passes");
    auto bad = [&](auto edit, const char* what) { pmz::Init j = i; edit(j); Check(!pmz::ValidateInit(j).empty(), std::string("refused: ") + what); };
    bad([](pmz::Init& j) { j.h.magic = 0; }, "wrong magic");
    bad([](pmz::Init& j) { j.h.bytes = 12; }, "wrong size");
    bad([](pmz::Init& j) { j.h.sequence = 7; }, "non-zero sequence");
    bad([](pmz::Init& j) { j.outW = 1000; }, "output smaller than input");
    bad([](pmz::Init& j) { j.inW = 32; }, "input below 64");
    bad([](pmz::Init& j) { j.outW = 9000; j.outH = 4320; }, "output above 8K");
    bad([](pmz::Init& j) { j.inW = 640; j.outW = 3840; }, "more than 4x");
    bad([](pmz::Init& j) { j.quality = 0; }, "quality 0");
    bad([](pmz::Init& j) { j.quality = 5; }, "quality 5");
    bad([](pmz::Init& j) { j.layout = 4; }, "layout 4");
    bad([](pmz::Init& j) { j.handles[2] = 0; }, "a missing handle");
    bad([](pmz::Init& j) { j.vsr.fill(L'x'); }, "an unterminated path");
    bad([](pmz::Init& j) { j.cache[0] = 0; }, "an empty path");
    Check(pmz::ValidExtent(64, 64, 64, 64) && pmz::ValidExtent(4096, 2304, 7680, 4320) && !pmz::ValidExtent(4097, 2160, 7680, 4320), "extent limits");
    const auto e = pmz::MakeEvaluate(5);
    Check(pmz::ValidHeader(e.h, pmz::Op::Evaluate, pmz::Bytes<pmz::Evaluate>()) && e.h.sequence == 5, "evaluate header");
    const auto s = pmz::MakeStop(9);
    Check(pmz::ValidHeader(s, pmz::Op::Stop, sizeof(pmz::Header)), "stop header");
    const auto r = pmz::MakeReply(3, 1, std::string(2000, 'y'), 1.5f);
    Check(pmz::ValidHeader(r.h, pmz::Op::Reply, pmz::Bytes<pmz::Reply>()) && r.result == 1 && r.message.back() == 0 && strlen(r.message.data()) == r.message.size() - 1, "reply: long messages are cut and terminated");
}

// ---- LZ4, ELF, PTX index
std::vector<uint8_t> Lz4Literal(const std::string& s)
{
    // one literal-only sequence
    std::vector<uint8_t> b;
    size_t n = s.size();
    if (n < 15) b.push_back(uint8_t(n << 4));
    else { b.push_back(0xF0); n -= 15; while (n >= 255) { b.push_back(255); n -= 255; } b.push_back(uint8_t(n)); }
    b.insert(b.end(), s.begin(), s.end());
    return b;
}
std::vector<uint8_t> MakeElf(const std::vector<std::string>& kernels, uint32_t sm)
{
    // header, then the string table, then the section headers: null, .shstrtab, .text.<k>...
    std::string str(1, '\0');
    std::vector<uint32_t> names;
    names.push_back(uint32_t(str.size())); str += ".shstrtab"; str += '\0';
    for (const auto& k : kernels) { names.push_back(uint32_t(str.size())); str += ".text." + k; str += '\0'; }
    const uint64_t stroff = 64, shoff = (64 + str.size() + 7) & ~7ull;
    const uint16_t shnum = uint16_t(2 + kernels.size());
    std::vector<uint8_t> b(size_t(shoff + shnum * 64), 0);
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 2; b[5] = 1; b[6] = 1;
    const uint16_t type = 2, em = pmz::kEmCuda, shent = 64, strndx = 1; const uint32_t flags = sm | (sm << 16);
    memcpy(&b[16], &type, 2); memcpy(&b[18], &em, 2); memcpy(&b[40], &shoff, 8); memcpy(&b[48], &flags, 4);
    memcpy(&b[58], &shent, 2); memcpy(&b[60], &shnum, 2); memcpy(&b[62], &strndx, 2);
    memcpy(&b[stroff], str.data(), str.size());
    for (uint16_t i = 1; i < shnum; ++i) {
        uint8_t* h = &b[size_t(shoff + i * 64)];
        const uint32_t nm = names[i - 1], t = i == 1 ? 3u : 1u;
        memcpy(h, &nm, 4); memcpy(h + 4, &t, 4);
        const uint64_t off = i == 1 ? stroff : 0, sz = i == 1 ? str.size() : 0;
        memcpy(h + 24, &off, 8); memcpy(h + 32, &sz, 8);
    }
    return b;
}
void AppendEntry(std::vector<uint8_t>& fat, uint16_t kind, const std::vector<uint8_t>& payload, bool compressed, size_t rawSize)
{
    std::vector<uint8_t> h(64, 0);
    const uint32_t ehs = 64; const uint64_t size = payload.size(); const uint32_t csz = uint32_t(payload.size());
    const uint64_t flags = compressed ? 0x2041 : 0x41, dsz = compressed ? rawSize : 0;
    memcpy(&h[0], &kind, 2); memcpy(&h[4], &ehs, 4); memcpy(&h[8], &size, 8); memcpy(&h[16], &csz, 4); memcpy(&h[40], &flags, 8); memcpy(&h[56], &dsz, 8);
    fat.insert(fat.end(), h.begin(), h.end()); fat.insert(fat.end(), payload.begin(), payload.end());
}
std::vector<uint8_t> Fatbin(const std::vector<std::vector<uint8_t>>& entries)
{
    std::vector<uint8_t> body;
    for (const auto& e : entries) body.insert(body.end(), e.begin(), e.end());
    std::vector<uint8_t> f(16, 0);
    const uint32_t magic = 0xBA55ED50; const uint16_t ver = 1, hsz = 16; const uint64_t fsz = body.size();
    memcpy(&f[0], &magic, 4); memcpy(&f[4], &ver, 2); memcpy(&f[6], &hsz, 2); memcpy(&f[8], &fsz, 8);
    f.insert(f.end(), body.begin(), body.end());
    return f;
}
void TestLz4AndIndex()
{
    printf("[lz4 / cubin -> PTX]\n");
    std::vector<uint8_t> out;
    const auto lit = Lz4Literal("hello");
    Check(pmz::Lz4Block(lit.data(), lit.size(), out, 5) && std::string(out.begin(), out.end()) == "hello", "LZ4: a literal run");
    const uint8_t match[] = { 0x35, 'a', 'b', 'c', 0x03, 0x00 };   // "abc" + a 9-byte match at offset 3
    Check(pmz::Lz4Block(match, sizeof(match), out, 12) && std::string(out.begin(), out.end()) == "abcabcabcabc", "LZ4: an overlapping match");
    const uint8_t badOffset[] = { 0x35, 'a', 'b', 'c', 0x00, 0x00 };
    Check(!pmz::Lz4Block(badOffset, sizeof(badOffset), out, 12), "LZ4: offset 0 refused");
    const uint8_t overrun[] = { 0x35, 'a', 'b', 'c', 0x09, 0x00 };
    Check(!pmz::Lz4Block(overrun, sizeof(overrun), out, 12), "LZ4: an offset before the start refused");
    const auto longLit = Lz4Literal(std::string(300, 'q'));
    Check(pmz::Lz4Block(longLit.data(), longLit.size(), out, 300) && out.size() == 300, "LZ4: a long literal length");
    Check(!pmz::Lz4Block(lit.data(), lit.size(), out, 4), "LZ4: more output than declared refused");

    const std::string ptxA = ".version 8.6\n.target sm_80\n.address_size 64\n.visible .entry conv_a(\n.param .u64 p)\n{ ret; }\n.visible .entry conv_b(\n.param .u64 p)\n{ ret; }\n";
    const std::string ptxB = ".version 8.6\n.target sm_80\n.visible .entry upsample_c()\n{ ret; }\n";
    const auto elfAB = MakeElf({ "conv_a", "conv_b" }, 80), elfC = MakeElf({ "upsample_c" }, 80), elfX = MakeElf({ "turing_only" }, 75), elfMixed = MakeElf({ "conv_a", "upsample_c" }, 80);
    Check(pmz::IsCudaElf(elfAB.data()) && pmz::ElfSize(elfAB.data()) == elfAB.size(), "ELF: CUDA image and its size from the header");
    const auto k = pmz::ElfKernels(elfAB.data(), elfAB.size());
    Check(k.size() == 2 && k[0] == "conv_a" && k[1] == "conv_b", "ELF: kernel names from .text.<name>");
    std::vector<uint8_t> e1, e2;
    AppendEntry(e1, 1, std::vector<uint8_t>(ptxA.begin(), ptxA.end()), false, 0);
    const auto comp = Lz4Literal(ptxB);
    AppendEntry(e2, 1, comp, true, ptxB.size());
    std::vector<uint8_t> e3; AppendEntry(e3, 2, elfC, false, 0);   // an ELF inside a fatbin is not a "standalone" cubin
    std::vector<uint8_t> image(100, 0xCC);
    for (const auto& f : { Fatbin({ e1 }), Fatbin({ e2, e3 }) }) { image.insert(image.end(), f.begin(), f.end()); image.insert(image.end(), 37, 0); }
    const size_t elfAt = image.size(); image.insert(image.end(), elfAB.begin(), elfAB.end()); image.insert(image.end(), 16, 0);
    image.insert(image.end(), elfX.begin(), elfX.end());
    pmz::PtxIndex idx;
    Check(idx.Build(image.data(), image.size()) && idx.Modules() == 2 && idx.stats().compressed == 1 && idx.stats().bad == 0 && idx.stats().elfInFatbin == 1, "index: 2 PTX modules (1 LZ4), the fatbin's ELF counted apart");
    const std::string* m = idx.ForCubin(image.data() + elfAt);
    Check(m && m->find("conv_b") != std::string::npos, "index: a cubin maps to the PTX module with all its kernels");
    Check(idx.ForCubin(elfC.data()) && idx.ForCubin(elfC.data())->find("upsample_c") != std::string::npos, "index: the LZ4 module is found too");
    Check(!idx.ForCubin(elfX.data()), "index: a kernel without PTX maps to nothing (passed on as it is)");
    Check(!idx.ForCubin(elfMixed.data()), "index: kernels spread over two modules map to nothing");
    Check(!idx.ForCubin(ptxA.c_str()), "index: PTX text is not a cubin");
    auto cov = pmz::Coverage(image.data(), image.size(), idx);
    Check(cov.cubins[80] >= 1 && cov.mapped[80] >= 1 && cov.cubins[75] == 1 && cov.mapped[75] == 0 && cov.unmapped.size() == 1, "coverage: sm_80 mapped, the sm_75-only kernel listed");
}

void TestRealDll(const std::wstring& dll)
{
    printf("[nvngx_vsr.dll coverage: %ls]\n", dll.c_str());
    HANDLE f = CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Check(false, "the DLL can be read"); return; }
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    std::vector<uint8_t> img(size_t(sz.QuadPart)); DWORD got = 0;
    ReadFile(f, img.data(), DWORD(img.size()), &got, nullptr); CloseHandle(f);
    pmz::PtxIndex idx;
    const bool built = idx.Build(img.data(), img.size());
    Check(built && idx.stats().bad == 0, std::format("{} PTX modules from {} fatbins, {} LZ4, {} unreadable", idx.Modules(), idx.stats().fatbins, idx.stats().compressed, idx.stats().bad));
    const auto c = pmz::Coverage(img.data(), img.size(), idx);
    for (const auto& [smv, n] : c.cubins) printf("        sm_%d: %zu standalone cubins, %zu with a PTX twin\n", smv, n, c.mapped.count(smv) ? c.mapped.at(smv) : 0);
    const size_t c80 = c.cubins.count(80) ? c.cubins.at(80) : 0, m80 = c.mapped.count(80) ? c.mapped.at(80) : 0;
    Check(c80 == 0 || m80 == c80, std::format("every standalone sm_80 cubin has a PTX twin ({} of {})", m80, c80));
}

// ---- picture check
void TestJudge()
{
    printf("[picture check]\n");
    const uint32_t w = 160, h = 90, ow = 320, oh = 180;
    const auto in = pmz::MakeInput(w, h);
    const auto bic = pmz::Bicubic(in, w, h, ow, oh);
    Check(pmz::Judge(in, w, h, bic, ow, oh, true).pass, "a bicubic upscale passes");
    Check(!pmz::Judge(in, w, h, std::vector<uint8_t>(size_t(ow) * oh * 4, 0), ow, oh, true).pass, "a black output fails (blank)");
    Check(!pmz::Judge(in, w, h, bic, ow, oh, false).pass, "a removed device fails");
    Check(!pmz::Judge(in, w, h, std::vector<uint8_t>(10), ow, oh, true).pass, "a short read back fails");
    // a softer picture: 5x5 box blur of the bicubic result
    std::vector<uint8_t> soft = bic;
    for (uint32_t y = 2; y + 2 < oh; ++y) for (uint32_t x = 2; x + 2 < ow; ++x) for (int c = 0; c < 3; ++c) {
        int s = 0; for (int j = -2; j <= 2; ++j) for (int i = -2; i <= 2; ++i) s += bic[(size_t(y + j) * ow + x + i) * 4 + c];
        soft[(size_t(y) * ow + x) * 4 + c] = uint8_t(s / 25);
    }
    const auto v = pmz::Judge(in, w, h, soft, ow, oh, true);
    Check(!v.pass && v.sharp < 0.9, "a blurred output fails (softer than bicubic)");
    std::vector<uint8_t> wrong = bic; for (size_t i = 0; i < wrong.size(); i += 4) { wrong[i] = 255 - wrong[i]; wrong[i + 1] = 255 - wrong[i + 1]; }
    Check(!pmz::Judge(in, w, h, wrong, ow, oh, true).pass, "an output that lost the picture fails (PSNR)");
}

// ---- refusals and the unlock
void TestRefusals()
{
    printf("[refusals]\n");
    auto e = pmz::Check(0x10DE, 1, true, L"", true);
    Check(!e.hardware && e.why.find("no AMD GPU") != std::string::npos, "NVIDIA: refused, no AMD GPU");
    e = pmz::Check(0x8086, 11, true, L"", true);
    Check(!e.hardware && e.why.find("no AMD GPU") != std::string::npos, "Intel: refused, no AMD GPU");
    e = pmz::Check(0x1002, 8, true, L"", true);
    Check(!e.hardware && e.why.find("RDNA3 / RDNA4") != std::string::npos, "RX 6000: refused (RX 7000 / 9000 only)");
    e = pmz::Check(0x1002, 7, false, L"", true);
    Check(e.hardware && !e.files && e.why.find("HIP") != std::string::npos, "RX 7000 without HIP: no files");
    e = pmz::Check(0x1002, 6, true, L"nvcuda.dll", true);
    Check(e.hardware && !e.files && e.why.find("ZLUDA is not installed") != std::string::npos && e.why.find("nvcuda.dll") != std::string::npos, "RX 9000 without ZLUDA: no ZLUDA");
    e = pmz::Check(0x1002, 7, true, L"", false);
    Check(e.hardware && !e.files && e.why.find("nvngx_vsr.dll") != std::string::npos, "RX 7000 without nvngx_vsr.dll: no nvngx_vsr");
    e = pmz::Check(0x1002, 7, true, L"", true);
    Check(e.hardware && e.files && e.why.empty(), "RX 7000 with everything: eligible");
    // the file layout on disk
    const auto root = std::filesystem::temp_directory_path() / std::format("pushmpc-zluda-test-{}", GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::create_directories(root / "runtime" / "experimental", ec);
    std::filesystem::create_directories(root / "runtime" / "amd-nr-rdna3" / "zluda", ec);
    std::filesystem::create_directories(root / "runtime" / "zluda-vsr", ec);
    std::filesystem::create_directories(root / "runtime" / "nvidia-vfg", ec);
    const std::wstring rt = (root / "runtime" / "experimental").wstring();
    auto touch = [](const std::filesystem::path& p) { FILE* f = nullptr; _wfopen_s(&f, p.c_str(), L"wb"); if (f) fclose(f); };
    Check(pmz::MissingZluda(rt) == L"nvapi64.dll", "files: an empty ZLUDA folder names nvapi64.dll first");
    for (const wchar_t* n : { L"nvapi64.dll", L"zluda_nvapi.dll", L"zluda_real.dll" }) touch(root / "runtime" / "amd-nr-rdna3" / "zluda" / n);
    Check(pmz::MissingZluda(rt) == L"nvcuda.dll", "files: the first missing ZLUDA file is named");
    for (const wchar_t* n : { L"nvcuda.dll", L"nvngx.dll" }) touch(root / "runtime" / "amd-nr-rdna3" / "zluda" / n);
    Check(pmz::MissingZluda(rt).empty(), "files: a complete ZLUDA folder");
    Check(pmz::FindVsr(rt).empty(), "files: no nvngx_vsr.dll anywhere");
    touch(root / "runtime" / "nvidia-vfg" / "nvngx_vsr.dll");
    Check(pmz::FindVsr(rt).find(L"nvidia-vfg") != std::wstring::npos, "files: the Video Effects copy when it is the only one");
    touch(root / "runtime" / "zluda-vsr" / "nvngx_vsr.dll");
    Check(pmz::FindVsr(rt).find(L"zluda-vsr") != std::wstring::npos, "files: PushMPC's own AMD copy first");
    std::filesystem::remove_all(root, ec);

    printf("[unlock ZludaVsr]\n");
    using namespace pmu;
    SelfTest none; SelfTest pass; pass.known = true; pass.passed = true; pass.reason = L"pass layout=2 ms=7.250 at 960x540 x2"; pass.when = L"2026-11-26T00:00:00Z";
    SelfTest fail; fail.known = true; fail.passed = false; fail.reason = L"layout 0: CreateFeature 0xBAD00001";
    auto d = DecideUnlock(Unlock::ZludaVsr, UnlockMode::Auto, false, L"needs an AMD RX 7000 or RX 9000 (no AMD GPU)", pass);
    Check(!d.on && !d.runSelfTest && d.why.find(L"not for this GPU") != std::wstring::npos, "ineligible: off, no self-test");
    d = DecideUnlock(Unlock::ZludaVsr, UnlockMode::Auto, true, L"", none);
    Check(!d.on && d.runSelfTest, "eligible, untested: off until the self-test ran (it is started)");
    d = DecideUnlock(Unlock::ZludaVsr, UnlockMode::Auto, true, L"", pass);
    Check(d.on && !d.runSelfTest, "self-test passed: on");
    d = DecideUnlock(Unlock::ZludaVsr, UnlockMode::Auto, true, L"", fail);
    Check(!d.on && !d.runSelfTest && d.why.find(L"0xBAD00001") != std::wstring::npos, "self-test failed: off with its reason");
    d = DecideUnlock(Unlock::ZludaVsr, UnlockMode::Off, true, L"", pass, false, L"PUSHMPC_DISABLE_ZLUDAVSR=1");
    Check(!d.on && d.why.find(L"off") != std::wstring::npos, "off switch wins over a pass");
    d = DecideUnlock(Unlock::ZludaVsr, UnlockMode::On, true, L"", none);
    Check(d.on, "forced on (testers) without a result");
    Check(EnvName(Unlock::ZludaVsr) == L"PUSHMPC_DISABLE_ZLUDAVSR", "env name PUSHMPC_DISABLE_ZLUDAVSR");
    Check(pmz::LayoutFromReason(pass.reason) == 2 && std::fabs(pmz::MsFromReason(pass.reason) - 7.25) < 1e-6, "the record: layout and ms read back");
    Check(pmz::LayoutFromReason(L"no layout") == 0 && pmz::MsFromReason(L"nothing") < 0, "the record: absent values");
    // the registry round trip on a scratch root (never the user's key)
    RegBase() = L"Software\\PushMPC-zluda_vsr_test";
    const std::wstring key = SelfTestKey(0x1002, 0x744C, L"32.0.21025.1000");
    WriteSelfTest(Unlock::ZludaVsr, key, true, L"pass layout=1 ms=5.000 at 960x540 x2");
    const auto back = ReadSelfTest(Unlock::ZludaVsr, key);
    Check(back.known && back.passed && pmz::LayoutFromReason(back.reason) == 1, "registry: the record round trip (scratch key)");
    Check(!ReadSelfTest(Unlock::ZludaVsr, SelfTestKey(0x1002, 0x744C, L"32.0.21025.2000")).known, "registry: another driver = no result (the test runs again)");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\PushMPC-zluda_vsr_test");
    RegBase() = L"Software\\PushMPC";
}

// ---- the worker round trip with the stub, on WARP
struct Warp {
    ComPtr<ID3D12Device> dev; ComPtr<ID3D12CommandQueue> q; ComPtr<ID3D12CommandAllocator> a; ComPtr<ID3D12GraphicsCommandList> l;
    ComPtr<ID3D12Fence> f; UINT64 v = 0; HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool Init()
    {
        ComPtr<IDXGIFactory4> fac; ComPtr<IDXGIAdapter> warp;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) || FAILED(fac->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) return false;
        if (FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return false;
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        return SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && SUCCEEDED(dev->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&a))) &&
               SUCCEEDED(dev->CreateCommandList(0, qd.Type, a.Get(), nullptr, IID_PPV_ARGS(&l))) && SUCCEEDED(l->Close()) &&
               SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)));
    }
    void Begin() { a->Reset(); l->Reset(a.Get(), nullptr); }
    void Exec() { l->Close(); ID3D12CommandList* x[] = { l.Get() }; q->ExecuteCommandLists(1, x); }
    bool Flush(DWORD ms = 20000) { q->Signal(f.Get(), ++v); if (f->GetCompletedValue() >= v) return true; f->SetEventOnCompletion(v, ev); return WaitForSingleObject(ev, ms) == WAIT_OBJECT_0; }
    void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES x, D3D12_RESOURCE_STATES y)
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r; b.Transition.StateBefore = x; b.Transition.StateAfter = y;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; l->ResourceBarrier(1, &b);
    }
    ComPtr<ID3D12Resource> Tex(UINT w, UINT h)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> r; dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)); return r;
    }
    ComPtr<ID3D12Resource> Buf(UINT64 n, D3D12_HEAP_TYPE t)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = t;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = n; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r; dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, t == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)); return r;
    }
    bool Upload(ID3D12Resource* t, const std::vector<uint8_t>& px, UINT w, UINT h)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0; const auto d = t->GetDesc(); dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto s = Buf(total, D3D12_HEAP_TYPE_UPLOAD); uint8_t* m = nullptr; s->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (UINT y = 0; y < h; ++y) memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, &px[size_t(y) * w * 4], size_t(w) * 4);
        s->Unmap(0, nullptr);
        Begin(); D3D12_TEXTURE_COPY_LOCATION dst{ t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, src{ s.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; src.PlacedFootprint = fp;
        Barrier(t, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST); l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); Barrier(t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Exec(); return Flush();
    }
    std::vector<uint8_t> Read(ID3D12Resource* t, UINT w, UINT h)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0; const auto d = t->GetDesc(); dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto rb = Buf(total, D3D12_HEAP_TYPE_READBACK);
        Begin(); D3D12_TEXTURE_COPY_LOCATION src{ t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, dst{ rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint = fp;
        Barrier(t, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE); l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); Barrier(t, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Exec(); std::vector<uint8_t> out; if (!Flush()) return out;
        out.resize(size_t(w) * h * 4); uint8_t* m = nullptr; rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (UINT y = 0; y < h; ++y) memcpy(&out[size_t(y) * w * 4], m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, size_t(w) * 4);
        rb->Unmap(0, nullptr); return out;
    }
};

void Env(const wchar_t* n, const wchar_t* v) { SetEnvironmentVariableW(n, v); }

void TestStub(const std::wstring& stub)
{
    printf("[worker round trip: stub on WARP]\n");
    Warp g;
    if (!g.Init()) { Check(false, "WARP device"); return; }
    const UINT w = 128, h = 96, ow = 256, oh = 192;
    const auto tmp = std::filesystem::temp_directory_path() / std::format("pushmpc-zluda-stub-{}", GetCurrentProcessId());
    std::error_code ec; std::filesystem::create_directories(tmp, ec);
    pmz::WorkerClient::Config cfg;
    cfg.worker = stub; cfg.zluda = tmp.wstring(); cfg.vsr = (tmp / "nvngx_vsr.dll").wstring(); cfg.cache = tmp.wstring(); cfg.log = (tmp / "worker.log").wstring();
    cfg.inW = w; cfg.inH = h; cfg.outW = ow; cfg.outH = oh; cfg.quality = 1; cfg.initTimeoutMs = 30000; cfg.frameTimeoutMs = 5000;
    auto src = g.Tex(w, h), dst = g.Tex(ow, oh);
    std::vector<uint8_t> pics[3];
    for (int i = 0; i < 3; ++i) { pics[i] = pmz::MakeInput(w, h); for (size_t p = 0; p < pics[i].size(); p += 4) pics[i][p] = uint8_t(pics[i][p] + 40 * i); }
    // one frame: copy in, run, copy out, then the CPU reads the result (the test only)
    auto frame = [&](pmz::WorkerClient& c, const std::vector<uint8_t>& px, std::vector<uint8_t>* out) -> bool {
        g.Upload(src.Get(), px, w, h);
        g.Begin(); g.Barrier(src.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        c.CopyIn(g.l.Get(), src.Get()); g.Barrier(src.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON); g.Exec();
        if (!c.Run(g.q.Get())) return false;
        g.Begin(); g.Barrier(dst.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        c.CopyOut(g.l.Get(), dst.Get()); g.Barrier(dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON); g.Exec();
        if (!g.Flush(15000)) return false;
        if (out) *out = g.Read(dst.Get(), ow, oh);
        return true;
    };
    {
        Env(L"PUSHMPC_ZLUDA_STUB_EXIT_AFTER", nullptr); Env(L"PUSHMPC_ZLUDA_STUB_HANG_AFTER", nullptr); Env(L"PUSHMPC_ZLUDA_STUB_REFUSE", nullptr);
        pmz::WorkerClient c;
        const bool opened = c.Open(g.dev.Get(), cfg);
        Check(opened, "the stub worker starts and opens the shared textures and fences" + (opened ? std::string() : ": " + c.Error()));
        if (!opened) return;
        bool all = true; double worst = 99;
        for (int i = 0; i < 6; ++i) {
            std::vector<uint8_t> out;
            if (!frame(c, pics[i % 3], &out)) { all = false; break; }
            worst = std::min(worst, pmz::Psnr(out, pmz::Bicubic(pics[i % 3], w, h, ow, oh)));
        }
        Check(all && worst > 60, std::format("6 frames through the worker, each the bicubic upscale of its own input (worst PSNR {:.1f} dB)", worst));
        Check(!c.Lost() && c.LastMs() >= 0, "no loss; the worker reports its time per frame");
        c.Close();
        Check(!c.IsOpen(), "a clean stop");
    }
    {
        Env(L"PUSHMPC_ZLUDA_STUB_EXIT_AFTER", L"2");
        pmz::WorkerClient c;
        bool ok = c.Open(g.dev.Get(), cfg);
        int done = 0;
        for (int i = 0; ok && i < 5; ++i) { if (!frame(c, pics[0], nullptr)) break; ++done; }
        // frame 3 was sent but never finished: the reader notices the exit, the done fence is released
        bool lost = c.Lost();
        if (ok && !lost) { frame(c, pics[0], nullptr); lost = c.Lost(); }
        Check(ok && done >= 2 && lost, std::format("a worker that exits mid-stream: lost after {} frames ({})", done, c.Error()));
        Check(g.Flush(5000), "the queue does not hang on the lost frame (done fence released)");
        c.Close();
        Env(L"PUSHMPC_ZLUDA_STUB_EXIT_AFTER", nullptr);
    }
    {
        Env(L"PUSHMPC_ZLUDA_STUB_HANG_AFTER", L"1");
        pmz::WorkerClient::Config hc = cfg; hc.frameTimeoutMs = 1500;
        pmz::WorkerClient c;
        bool ok = c.Open(g.dev.Get(), hc);
        const auto t0 = std::chrono::steady_clock::now();
        int done = 0;
        for (int i = 0; ok && i < 4; ++i) { if (!frame(c, pics[1], nullptr)) break; ++done; }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Check(ok && c.Lost() && s < 10, std::format("a worker that hangs: stopped by the watchdog after {:.1f} s ({})", s, c.Error()));
        Check(g.Flush(5000), "the queue does not hang on the hung frame");
        c.Close();
        Env(L"PUSHMPC_ZLUDA_STUB_HANG_AFTER", nullptr);
    }
    {
        Env(L"PUSHMPC_ZLUDA_STUB_REFUSE", L"test");
        pmz::WorkerClient c;
        const bool ok = c.Open(g.dev.Get(), cfg);
        Check(!ok && c.Error().find("refused") != std::string::npos, "a worker that refuses at open: the open fails with its reason (" + c.Error() + ")");
        Env(L"PUSHMPC_ZLUDA_STUB_REFUSE", nullptr);
    }
    {
        pmz::WorkerClient::Config bad = cfg; bad.worker = (tmp / "missing.exe").wstring();
        pmz::WorkerClient c;
        Check(!c.Open(g.dev.Get(), bad) && c.Error().find("did not start") != std::string::npos, "no worker exe: the open fails cleanly");
        pmz::WorkerClient::Config big = cfg; big.outW = 1024;
        Check(!c.Open(g.dev.Get(), big) && c.Error().find("range") != std::string::npos, "a size outside RTX VSR's range is refused before any worker starts");
    }
    // the self-test through the stub on WARP, recorded on a scratch key
    {
        const std::wstring regBase = L"Software\\PushMPC-zluda_vsr_test";
        auto run = [&](const std::wstring& extra) {
            std::wstring cmd = std::format(L"\"{}\" --self-test --warp 1 --any-vendor --width 128 --height 96 --scale 2 --zluda \"{}\" --vsr \"{}\" --cache \"{}\" --record 1002:744C:test --regbase \"{}\"{}",
                                           stub, tmp.wstring(), (tmp / "nvngx_vsr.dll").wstring(), tmp.wstring(), regBase, extra);
            STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
            if (!CreateProcessW(stub.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
            WaitForSingleObject(pi.hProcess, 60000); DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            return int(code);
        };
        pmu::RegBase() = regBase;
        Check(run(L"") == 0, "the self-test (stub) passes on WARP");
        auto t = pmu::ReadSelfTest(pmu::Unlock::ZludaVsr, L"1002:744C:test");
        Check(t.known && t.passed && t.reason.find(L"layout=0") != std::wstring::npos && pmz::MsFromReason(t.reason) > 0, "the self-test recorded pass, layout 0 and its time");
        Env(L"PUSHMPC_ZLUDA_STUB_LAYOUT", L"3");
        Check(run(L"") == 0, "the self-test tries the layouts in turn (the stub takes only layout 3)");
        t = pmu::ReadSelfTest(pmu::Unlock::ZludaVsr, L"1002:744C:test");
        Check(t.passed && pmz::LayoutFromReason(t.reason) == 3, "the layout that passed is recorded");
        Env(L"PUSHMPC_ZLUDA_STUB_LAYOUT", nullptr);
        Env(L"PUSHMPC_ZLUDA_STUB_BLANK", L"1");
        Check(run(L"") == 1, "a blank result fails the self-test");
        t = pmu::ReadSelfTest(pmu::Unlock::ZludaVsr, L"1002:744C:test");
        Check(t.known && !t.passed && t.reason.find(L"blank") != std::wstring::npos, "the failure is recorded with its reason");
        Env(L"PUSHMPC_ZLUDA_STUB_BLANK", nullptr);
        RegDeleteTreeW(HKEY_CURRENT_USER, regBase.c_str());
        pmu::RegBase() = L"Software\\PushMPC";
    }
    std::filesystem::remove_all(tmp, ec);
}

// the real worker never touches a GPU that is not AMD: on WARP it refuses before loading anything
void TestRealRefusal(const std::wstring& real)
{
    printf("[real worker refusals]\n");
    auto run = [&](const std::wstring& args, std::string& out) {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE }; HANDLE r = nullptr, w = nullptr; CreatePipe(&r, &w, &sa, 0); SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
        std::wstring cmd = L"\"" + real + L"\" " + args;
        STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = w; si.hStdError = w;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(real.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(r); CloseHandle(w); return -1; }
        CloseHandle(w);
        char buf[4096]; DWORD n = 0; while (ReadFile(r, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, 60000); DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(r);
        return int(code);
    };
    std::string out;
    int code = run(L"--self-test --warp 1 --zluda C:\\none --vsr C:\\none\\nvngx_vsr.dll --cache C:\\none", out);
    Check(code == 1 && out.find("RESULT pass=0") != std::string::npos && out.find("software adapter") != std::string::npos, "on WARP: refused (a software adapter), nothing loaded");
    out.clear();
    code = run(L"--self-test --warp 1", out);
    Check(code == 2 && out.find("usage") != std::string::npos, "without its paths: usage");
    out.clear();
    code = run(L"1 2 3", out);
    Check(code == 2, "a wrong command line: usage");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring stub, real, dll;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i];
        if (k == L"--stub") stub = argv[i + 1]; else if (k == L"--real") real = argv[i + 1]; else if (k == L"--vsr-dll") dll = argv[i + 1];
    }
    TestPacking();
    TestProtocol();
    TestLz4AndIndex();
    TestJudge();
    TestRefusals();
    if (!dll.empty()) TestRealDll(dll);
    if (!stub.empty()) TestStub(stub);
    if (!real.empty()) TestRealRefusal(real);
    printf("zluda_vsr_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
