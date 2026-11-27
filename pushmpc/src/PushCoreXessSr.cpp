// pushcore: Intel XeSS super resolution in the graph's temporal upscaler slot, the runtime file checks, the SR self-test
// (PushCoreXess.h).
//
// Video use: the graph gives the slot what it gives FSR: the source-size working colour, PushFPS's refined optical flow
// as the motion (current -> previous, source pixels, "previous = current + motion" = XeSS's own convention), constant
// depth, reset on discontinuities (seek, graph reset, no valid flow) and the frame time. That is the better choice for
// video than zero motion with a reset on every cut: XeSS SR is a temporal accumulator, and zero motion on moving video
// either smears the history over the motion (ghosting) or, with a reset each frame, turns it into a spatial-only upscaler
// that throws away the multi-frame detail XeSS is built for. The flow is there anyway (the graph computes it for FSR and
// the frame generation), and its reset rule already covers cuts where the flow is invalid. The self-test measures it
// against a bilinear upscale on a moving scene with exact motion.
#include "PushCoreXess.h"
#include "PushCoreSelfTest.h"
#include "veyra/gfx/FsrSrBackend.h"
#include "veyra/RuntimePaths.h"
#include "veyra/Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <xess/xess_d3d12.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")

using Microsoft::WRL::ComPtr;

namespace pushmpc::xess {
// shared with PushCoreXefg.cpp (the self-test device)
bool selfTestDevice(uint64_t luid, bool warp, ComPtr<ID3D12Device>& device, ComPtr<ID3D12CommandQueue>& queue, std::string& adapterName, uint32_t& vendor, uint32_t& dev,
                    std::wstring& driver, std::string& why);
std::string narrowPath(const std::filesystem::path& p);

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// The builds of Intel's XeSS SDK 3.0.2 release (github.com/intel/xess, XeSS_SDK_3.0.2.zip, bin\): what
// scripts\xess-fetch.ps1 installs. libxess_fg.dll 1.3.1.78 is also the build PushFPS's MFG unlock is audited for.
struct Pin { const wchar_t* name; uint64_t size; const char* sha256; const char* version; };
const Pin kPins[] = {
    {L"libxess.dll", 77795704ull, "251659DD84A3E84DE67C886A4186E01F3ECA49B00641906FE38BB6B807E5D5B7", "2.0.2.68"},
    {L"libxess_fg.dll", 22957432ull, "EC5E0C65E075570C6EDE72618BB666D0BE0C2E10B2EA9762C0FE8CB8E375AB27", "1.3.1.78"},
    {L"libxell.dll", 415368ull, "D2030DCD694FDA8F2EC7E044B13E6DB8F0B56D4BA9113A5EFAD334E3F3DED8C7", "1.3.2.10"},
};

std::string sha256File(const std::filesystem::path& path, uint64_t& size) {
    size = 0;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    std::vector<uint8_t> buf(1 << 20);
    DWORD got = 0;
    while (ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr) && got) { BCryptHashData(hash, buf.data(), got, 0); size += got; }
    uint8_t digest[32];
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); CloseHandle(f);
    std::string s; char hx[3];
    for (uint8_t b : digest) { snprintf(hx, 3, "%02X", b); s += hx; }
    return s;
}
std::string fileVersion(const std::filesystem::path& path) {
    DWORD h = 0;
    const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &h);
    if (!n) return "?";
    std::vector<uint8_t> b(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, b.data())) return "?";
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueW(b.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi) return "?";
    return std::format("{}.{}.{}.{}", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
}
// Authenticode: a valid signature (no network: revocation off, cached URLs only) whose signer is Intel Corporation
bool signedByIntel(const std::filesystem::path& path, std::string& signer) {
    WINTRUST_FILE_INFO fi{}; fi.cbStruct = sizeof(fi); fi.pcwszFilePath = path.c_str();
    WINTRUST_DATA wd{}; wd.cbStruct = sizeof(wd); wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE; wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi; wd.dwStateAction = WTD_STATEACTION_VERIFY; wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG trust = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    if (trust != ERROR_SUCCESS) { signer = std::format("signature not valid (0x{:08X})", unsigned(trust)); return false; }
    HCERTSTORE store = nullptr; HCRYPTMSG msg = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr,
                          &store, &msg, nullptr)) { signer = "no signer"; return false; }
    DWORD n = 0;
    CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &n);
    std::vector<uint8_t> buf(n);
    bool ok = false;
    if (n && CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, buf.data(), &n)) {
        auto* si = reinterpret_cast<CMSG_SIGNER_INFO*>(buf.data());
        CERT_INFO ci{}; ci.Issuer = si->Issuer; ci.SerialNumber = si->SerialNumber;
        if (PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci, nullptr)) {
            wchar_t name[256]{};
            CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
            const int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
            signer.assign(size_t(std::max(0, len - 1)), '\0');
            if (len > 1) WideCharToMultiByte(CP_UTF8, 0, name, -1, signer.data(), len, nullptr, nullptr);
            ok = signer == "Intel Corporation";
            CertFreeCertificateContext(cert);
        }
    }
    CryptMsgClose(msg); CertCloseStore(store, 0);
    return ok;
}
std::mutex gCheckMx;
std::map<std::wstring, std::pair<uint64_t, FileCheck>> gChecks;   // path -> (last write time, result)
}  // namespace

std::filesystem::path runtimeFolder() { return veyra::runtime::localDataDirectory() / L"intel" / L"experimental"; }

FileCheck checkFile(const std::filesystem::path& path) {
    FileCheck c;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) { c.why = narrowPath(path) + " is not installed"; return c; }
    const uint64_t stamp = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32 | a.ftLastWriteTime.dwLowDateTime) ^ (uint64_t(a.nFileSizeHigh) << 32 | a.nFileSizeLow);
    {
        std::lock_guard l(gCheckMx);
        if (auto it = gChecks.find(path.wstring()); it != gChecks.end() && it->second.first == stamp) return it->second.second;
    }
    c.present = true;
    c.sha256 = sha256File(path, c.size);
    c.version = fileVersion(path);
    std::string signer;
    c.signedByIntel = signedByIntel(path, signer);
    const std::wstring name = path.filename().wstring();
    for (const auto& p : kPins)
        if (_wcsicmp(p.name, name.c_str()) == 0) c.pinned = c.size == p.size && c.sha256 == p.sha256;
    if (!c.signedByIntel) c.why = std::format("{} is not signed by Intel ({})", narrowPath(path.filename()), signer);
    std::lock_guard l(gCheckMx);
    gChecks[path.wstring()] = {stamp, c};
    return c;
}
std::string narrowPath(const std::filesystem::path& p) {
    const std::wstring s = p.wstring();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
bool fgFilesUsable(std::string* why) {
    for (const wchar_t* n : {L"libxess_fg.dll", L"libxell.dll"}) {
        const FileCheck c = checkFile(runtimeFolder() / n);
        if (!c.present || !c.signedByIntel) {
            if (why) *why = c.present ? c.why : "the XeSS frame generation files are not installed (runtime_local\\intel\\experimental: libxess_fg.dll, libxell.dll)";
            return false;
        }
    }
    return true;
}
bool srFileUsable(std::string* why) {
    const FileCheck c = checkFile(runtimeFolder() / L"libxess.dll");
    if (!c.present || !c.signedByIntel) {
        if (why) *why = c.present ? c.why : "Intel's XeSS SR runtime is not installed (runtime_local\\intel\\experimental\\libxess.dll)";
        return false;
    }
    return true;
}

// ============================================================================================ the SR provider
namespace {
struct SrApi {
    HMODULE dll = nullptr;
    decltype(&xessGetVersion) getVersion = nullptr;
    decltype(&xessD3D12CreateContext) create = nullptr;
    decltype(&xessD3D12Init) init = nullptr;
    decltype(&xessD3D12Execute) execute = nullptr;
    decltype(&xessDestroyContext) destroy = nullptr;
    decltype(&xessGetOptimalInputResolution) optimal = nullptr;
    decltype(&xessIsOptimalDriver) optimalDriver = nullptr;
    std::string version;
};
std::mutex gSrMx;
SrApi gSr;
std::string gSrError;
bool loadSr(std::string& why) {
    std::lock_guard l(gSrMx);
    if (gSr.dll) return true;
    if (!gSrError.empty()) { why = gSrError; return false; }
    if (!srFileUsable(&why)) { gSrError = why; return false; }
    const auto path = runtimeFolder() / L"libxess.dll";
    SrApi a;
    a.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!a.dll) { why = gSrError = std::format("libxess.dll did not load (error {})", GetLastError()); return false; }
#define L_(f, n) a.f = reinterpret_cast<decltype(a.f)>(GetProcAddress(a.dll, #n)); if (!a.f) { why = gSrError = "libxess.dll lacks " #n; FreeLibrary(a.dll); return false; }
    L_(getVersion, xessGetVersion) L_(create, xessD3D12CreateContext) L_(init, xessD3D12Init) L_(execute, xessD3D12Execute) L_(destroy, xessDestroyContext)
    L_(optimal, xessGetOptimalInputResolution)
#undef L_
    a.optimalDriver = reinterpret_cast<decltype(a.optimalDriver)>(GetProcAddress(a.dll, "xessIsOptimalDriver"));
    xess_version_t v{}; a.getVersion(&v);
    const FileCheck fc = checkFile(path);
    a.version = std::format("XeSS SR {}.{}.{} ({}{})", v.major, v.minor, v.patch, fc.version, fc.pinned ? ", pinned build" : ", Intel-signed, not the pinned build");
    gSr = a;
    veyra::log::info("xess-sr", "runtime " + gSr.version);
    return true;
}
const char* presetName(xess_quality_settings_t q) {
    switch (q) {
    case XESS_QUALITY_SETTING_ULTRA_PERFORMANCE: return "Ultra Performance";
    case XESS_QUALITY_SETTING_PERFORMANCE: return "Performance";
    case XESS_QUALITY_SETTING_BALANCED: return "Balanced";
    case XESS_QUALITY_SETTING_QUALITY: return "Quality";
    case XESS_QUALITY_SETTING_ULTRA_QUALITY: return "Ultra Quality";
    case XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS: return "Ultra Quality Plus";
    case XESS_QUALITY_SETTING_AA: return "Native AA";
    default: return "?";
    }
}
struct SrContext {
    xess_context_handle_t h = nullptr;
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    std::string version;
    uint64_t executes = 0, failures = 0;
};
uint32_t gSrQuality = 3;
// The preset for this scale: PushMPC's level (1 Performance .. 4 Ultra Quality) when its input range holds the source
// size, else the nearest one that does (video has one fixed scale); Native AA at 1x.
bool pickPreset(xess_context_handle_t h, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, uint32_t quality, xess_quality_settings_t& out, std::string& note) {
    const xess_quality_settings_t order[] = {XESS_QUALITY_SETTING_ULTRA_PERFORMANCE, XESS_QUALITY_SETTING_PERFORMANCE, XESS_QUALITY_SETTING_BALANCED,
                                             XESS_QUALITY_SETTING_QUALITY, XESS_QUALITY_SETTING_ULTRA_QUALITY, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS, XESS_QUALITY_SETTING_AA};
    const int want = rw >= ow && rh >= oh ? 6 : std::clamp(int(quality), 1, 4);   // 1 -> Performance (index 1) .. 4 -> Ultra Quality (index 4)
    const xess_2d_t o{ow, oh};
    std::vector<int> fits;
    for (int i = 0; i < 7; ++i) {
        xess_2d_t opt{}, mn{}, mx{};
        if (gSr.optimal(h, &o, order[i], &opt, &mn, &mx) != XESS_RESULT_SUCCESS) continue;
        if (rw >= mn.x && rw <= mx.x && rh >= mn.y && rh <= mx.y) fits.push_back(i);
    }
    if (fits.empty()) { note = std::format("no XeSS preset takes {}x{} -> {}x{}", rw, rh, ow, oh); return false; }
    int best = fits[0];
    for (int i : fits) if (std::abs(i - want) < std::abs(best - want)) best = i;
    out = order[best];
    note = std::format("preset {}{}", presetName(out), best == want ? "" : std::format(" (asked {}, its range does not hold {}x{})", presetName(order[want]), rw, rh));
    return true;
}
void* srCreate(ID3D12Device* device, uint32_t maxRW, uint32_t maxRH, uint32_t maxUW, uint32_t maxUH, bool hdr, void* user) {
    std::string why;
    if (!loadSr(why)) { veyra::log::error("xess-sr", why); return nullptr; }
    auto* c = new SrContext();
    c->renderW = maxRW; c->renderH = maxRH; c->outW = maxUW; c->outH = maxUH; c->version = gSr.version;
    const uint32_t quality = user ? *static_cast<const uint32_t*>(user) : 3u;
    xess_result_t r = gSr.create(device, &c->h);
    if (r != XESS_RESULT_SUCCESS) { veyra::log::error("xess-sr", std::format("xessD3D12CreateContext failed ({})", int(r))); delete c; return nullptr; }
    if (gSr.optimalDriver) {
        const auto d = gSr.optimalDriver(c->h);
        if (d != XESS_RESULT_SUCCESS) veyra::log::warn("xess-sr", std::format("xessIsOptimalDriver: {} (a newer GPU driver is recommended)", int(d)));
    }
    xess_d3d12_init_params_t ip{};
    ip.outputResolution = {maxUW, maxUH};
    std::string note;
    if (!pickPreset(c->h, maxRW, maxRH, maxUW, maxUH, quality, ip.qualitySetting, note)) { veyra::log::error("xess-sr", note); gSr.destroy(c->h); delete c; return nullptr; }
    ip.initFlags = hdr ? XESS_INIT_FLAG_NONE : XESS_INIT_FLAG_LDR_INPUT_COLOR;   // low-res motion + depth (render extent)
    const auto t = Clock::now();
    r = gSr.init(c->h, &ip);
    if (r != XESS_RESULT_SUCCESS) { veyra::log::error("xess-sr", std::format("xessD3D12Init failed ({}), {}", int(r), note)); gSr.destroy(c->h); delete c; return nullptr; }
    veyra::log::info("xess-sr", std::format("ready {}x{} -> {}x{}, {}, level {}, {} input, init {:.0f} ms; {}", maxRW, maxRH, maxUW, maxUH, note, quality, hdr ? "HDR (linear)" : "LDR",
                                            msSince(t), c->version));
    return c;
}
bool srEvaluate(void* ctx, ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* output,
                uint32_t rw, uint32_t rh, uint32_t uw, uint32_t uh, bool reset, float) {
    auto* c = static_cast<SrContext*>(ctx);
    if (!c || !list || !color || !motion || !output || uw > c->outW || uh > c->outH) return false;
    xess_d3d12_execute_params_t e{};
    e.pColorTexture = color; e.pVelocityTexture = motion; e.pDepthTexture = depth; e.pOutputTexture = output;
    e.jitterOffsetX = e.jitterOffsetY = 0.0f; e.exposureScale = 1.0f; e.resetHistory = reset ? 1 : 0;
    e.inputWidth = rw; e.inputHeight = rh;
    const xess_result_t r = gSr.execute(c->h, list, &e);
    if (r != XESS_RESULT_SUCCESS) {
        if (++c->failures <= 5) veyra::log::error("xess-sr", std::format("xessD3D12Execute failed ({}) {}x{} -> {}x{}", int(r), rw, rh, uw, uh));
        return false;
    }
    ++c->executes;
    return true;
}
void srDestroy(void* ctx) {
    auto* c = static_cast<SrContext*>(ctx);
    if (!c) return;
    veyra::log::info("xess-sr", std::format("context destroyed after {} executes, {} failures", c->executes, c->failures));
    if (c->h) gSr.destroy(c->h);
    delete c;
}
const char* srVersion(void* ctx) { return ctx ? static_cast<SrContext*>(ctx)->version.c_str() : "XeSS SR"; }
#ifdef VEYRA_FSRSR_PROVIDER_OVERRIDE   // the slot (the XeSS hunks of patches/pushfps-4.5.5-pushmpc.patch); without it XeSS SR reports "no slot"
veyra::gfx::FsrSrBackend::ProviderOverride gProvider{&srCreate, &srEvaluate, &srDestroy, &srVersion, &gSrQuality};
#endif
}  // namespace

const void* srProvider(uint32_t quality) {
    gSrQuality = std::clamp(quality, 1u, 4u);
#ifdef VEYRA_FSRSR_PROVIDER_OVERRIDE
    return &gProvider;
#else
    return nullptr;
#endif
}
bool srProviderBuilt() {
#ifdef VEYRA_FSRSR_PROVIDER_OVERRIDE
    return true;
#else
    return false;
#endif
}

// ============================================================================================ SR self-test
// The scene, the measurement and the frame loop are the shared upscaler probe (PushCoreSelfTest.cpp
// selftest::upscaleProbe); XeSS runs in it through its provider functions, as the graph's FSR slot runs it.
XessSrSelfTestResult xessSrSelfTest(uint64_t luid, bool warp, uint32_t W, uint32_t H, uint32_t frames, int scene) {
    XessSrSelfTestResult r;
    W = std::max(64u, W) & ~1u; H = std::max(64u, H) & ~1u; frames = std::clamp(frames, 4u, 240u);
    const uint32_t w = W / 2, h = H / 2;
    std::string why;
    if (!loadSr(why)) { r.note = why; return r; }
    r.version = gSr.version;
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
    if (!selfTestDevice(luid, warp, device, queue, r.adapter, r.vendor, r.device, r.driver, why)) { r.note = why; return r; }
    r.ran = true;
    const uint32_t q = 3;
    SrContext* c = static_cast<SrContext*>(srCreate(device.Get(), w, h, W, H, false, const_cast<uint32_t*>(&q)));
    if (!c) { r.note = "XeSS SR did not start on this adapter (see the log: xessD3D12CreateContext / xessD3D12Init)"; return r; }
    // Diagnostics only (PUSHMPC_XESS_SELFTEST_MOTION): zero = the static scene with history, reset = the static scene with a
    // reset every frame, for the verdict run too
    wchar_t mode[16]{};
    GetEnvironmentVariableW(L"PUSHMPC_XESS_SELFTEST_MOTION", mode, 16);
    const selftest::UpscaleScene sc = scene == 1 || _wcsicmp(mode, L"zero") == 0 ? selftest::UpscaleScene::Static
                                    : scene == 2 || _wcsicmp(mode, L"reset") == 0 ? selftest::UpscaleScene::StaticReset : selftest::UpscaleScene::Pan;
    const selftest::UpscaleRun run = selftest::upscaleProbe(device.Get(), queue.Get(), W, H, frames, sc,
        [&](ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* out,
            uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh, bool reset) {
            return srEvaluate(c, list, color, motion, depth, out, iw, ih, ow, oh, reset, 33.3f);
        });
    srDestroy(c);
    r.summary = selftest::upscaleSummary(run, warp);
    if (!run.ok) { r.note = "XeSS SR: " + run.note; return r; }
    // the verdict as before: the median of the last 4 frames against bilinear
    std::vector<double> px, pb;
    for (size_t i = run.frames.size() >= 4 ? run.frames.size() - 4 : 0; i < run.frames.size(); ++i) { px.push_back(run.frames[i].psnr); pb.push_back(run.frames[i].bilinear); }
    std::sort(px.begin(), px.end()); std::sort(pb.begin(), pb.end());
    r.psnrXess = px[px.size() / 2]; r.psnrBilinear = pb[pb.size() / 2];
    r.msPerFrame = run.msPerFrame;
    r.passed = r.psnrXess >= r.psnrBilinear - 0.5;
    r.note = std::format("{}x{} -> {}x{}, {} frames ({}): PSNR XeSS {:.2f} dB, bilinear {:.2f} dB, {:.1f} ms per frame (submit + wait); {}; {}", w, h, W, H, frames,
                         selftest::sceneName(sc), r.psnrXess, r.psnrBilinear, r.msPerFrame, r.version, r.passed ? "PASS" : "FAIL (below bilinear)");
    return r;
}

}  // namespace pushmpc::xess
