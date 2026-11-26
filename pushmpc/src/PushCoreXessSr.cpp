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
#ifdef VEYRA_FSRSR_PROVIDER_OVERRIDE   // the slot (patches/pushfps-4.5.4-xess.patch); without it XeSS SR reports "no slot"
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
namespace {
// the full-resolution ground truth at continuous coordinates: smooth shading, hard block edges and fine detail near the
// output's Nyquist limit (thin lines, a fine diagonal grating, small dots) that a 2x2-averaged input aliases and only a
// temporal upscaler with correct motion can bring back
double pattern(double x, double y, int k) {
    const double a = 0.5 + 0.2 * std::sin(x * 0.11 + k * 0.7) * std::cos(y * 0.07) + 0.1 * std::sin((x + y) * 0.031);
    const bool block = (int(std::floor(x / 23.0)) + int(std::floor(y / 17.0))) & 1;
    double v = a * (block ? 0.8 : 1.0) + (block ? 0.08 : 0.0);
    const double lx = std::fmod(x, 9.0), ly = std::fmod(y, 13.0);
    if (lx >= 0 && lx < 1.5) v = 0.25;                                   // thin vertical lines
    if (ly >= 0 && ly < 1.5) v = 0.8;                                    // thin horizontal lines
    if (std::fmod(x + 2.0 * y, 64.0) < 32.0) v += 0.05 * std::sin((x - y) * 1.3);   // a fine diagonal grating in half the area
    const double dx = std::fmod(x, 6.0) - 3.0, dy = std::fmod(y, 6.0) - 3.0;
    if (std::fmod(y, 48.0) < 24.0 && dx * dx + dy * dy < 1.2) v = 0.05;  // small dots
    return std::clamp(v, 0.0, 1.0);
}
uint16_t half(float f) { uint32_t u; memcpy(&u, &f, 4); const uint32_t s = (u >> 16) & 0x8000; int e = int((u >> 23) & 0xFF) - 127 + 15; uint32_t m = u & 0x7FFFFF;
    if (e <= 0) return uint16_t(s); if (e >= 31) return uint16_t(s | 0x7C00); return uint16_t(s | (uint32_t(e) << 10) | (m >> 13)); }
float unhalf(uint16_t h) { const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF; uint32_t u;
    if (!e) u = s; else if (e == 31) u = s | 0x7F800000 | (m << 13); else u = s | ((e + 112) << 23) | (m << 13); float f; memcpy(&f, &u, 4); return f; }
double psnrF(const std::vector<float>& a, const std::vector<float>& b) {
    double se = 0; for (size_t i = 0; i < a.size(); ++i) { const double d = a[i] - b[i]; se += d * d; }
    const double mse = se / double(a.size());
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
}
}  // namespace

XessSrSelfTestResult xessSrSelfTest(uint64_t luid, bool warp, uint32_t W, uint32_t H, uint32_t frames) {
    XessSrSelfTestResult r;
    W = std::max(64u, W) & ~1u; H = std::max(64u, H) & ~1u; frames = std::clamp(frames, 4u, 120u);
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
    auto tex = [&](uint32_t tw, uint32_t th, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl, ComPtr<ID3D12Resource>& t) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = tw; rd.Height = th; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = f; rd.SampleDesc = {1, 0}; rd.Flags = fl;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t)));
    };
    auto buf = [&](uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& b) {
        D3D12_HEAP_PROPERTIES hp{type};
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc = {1, 0};
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&b)));
    };
    ComPtr<ID3D12Resource> color, motion, depth, out, upC, upM, upD, rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpC{}, fpM{}, fpD{}, fpO{}; UINT64 sC = 0, sM = 0, sD = 0, sO = 0;
    auto fp = [&](uint32_t tw, uint32_t th, DXGI_FORMAT f, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& o, UINT64& s) {
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = tw; rd.Height = th; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = f; rd.SampleDesc = {1, 0};
        device->GetCopyableFootprints(&rd, 0, 1, 0, &o, nullptr, nullptr, &s);
    };
    fp(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, fpC, sC); fp(w, h, DXGI_FORMAT_R16G16_FLOAT, fpM, sM); fp(w, h, DXGI_FORMAT_R32_FLOAT, fpD, sD); fp(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, fpO, sO);
    ComPtr<ID3D12CommandAllocator> alloc; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence; uint64_t fv = 0;
    if (!tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, color) || !tex(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, motion) ||
        !tex(w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, depth) || !tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, out) ||
        !buf(sC, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upC) || !buf(sM, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upM) ||
        !buf(sD, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upD) || !buf(sO, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, rb) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { srDestroy(c); r.note = "test resources"; return r; }
    list->Close();
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto idle = [&] { queue->Signal(fence.Get(), ++fv); if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 60000); } };
    auto bar = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b}; list->ResourceBarrier(1, &br);
    };
    auto up = [&](ID3D12Resource* dst, ID3D12Resource* src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f) {
        bar(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION s{src, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; s.PlacedFootprint = f;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        bar(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    };
    // constant depth, uniform motion: the picture pans left/up by (vx, vy) output px per frame (non-integer: new subpixel
    // phases every frame), so a point's previous position is current + (vx, vy): the motion is +(vx, vy) / 2 input px.
    // Diagnostics only (PUSHMPC_XESS_SELFTEST_MOTION): zero = zero motion with history, reset = zero motion and a reset
    // every frame (the alternative to the flow for video: spatial only)
    const double vx = 2.6, vy = 1.3;
    wchar_t mode[16]{};
    GetEnvironmentVariableW(L"PUSHMPC_XESS_SELFTEST_MOTION", mode, 16);
    const bool zeroMotion = _wcsicmp(mode, L"zero") == 0 || _wcsicmp(mode, L"reset") == 0, resetAlways = _wcsicmp(mode, L"reset") == 0;
    { uint8_t* m = nullptr; upD->Map(0, nullptr, reinterpret_cast<void**>(&m)); for (uint32_t y = 0; y < h; ++y) { auto* row = reinterpret_cast<float*>(m + fpD.Offset + size_t(y) * fpD.Footprint.RowPitch); for (uint32_t x = 0; x < w; ++x) row[x] = 0.5f; } upD->Unmap(0, nullptr); }
    { uint8_t* m = nullptr; upM->Map(0, nullptr, reinterpret_cast<void**>(&m)); const uint16_t mx = half(zeroMotion ? 0.0f : float(vx / 2)), my = half(zeroMotion ? 0.0f : float(vy / 2));
      for (uint32_t y = 0; y < h; ++y) { auto* row = reinterpret_cast<uint16_t*>(m + fpM.Offset + size_t(y) * fpM.Footprint.RowPitch); for (uint32_t x = 0; x < w; ++x) { row[2 * x] = mx; row[2 * x + 1] = my; } } upM->Unmap(0, nullptr); }
    std::vector<double> px, pb;
    double execMs = 0;
    std::vector<float> truth(size_t(W) * H), got(size_t(W) * H), bil(size_t(W) * H), lowres(size_t(w) * h);
    for (uint32_t k = 0; k < frames; ++k) {
        const double ox = vx * k, oy = vy * k;
        // input: 2x2 box average of the full-resolution pattern (what a downscaled source holds)
        uint8_t* m = nullptr; upC->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (uint32_t y = 0; y < h; ++y) {
            auto* row = reinterpret_cast<uint16_t*>(m + fpC.Offset + size_t(y) * fpC.Footprint.RowPitch);
            for (uint32_t x = 0; x < w; ++x) {
                double s = 0;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) s += pattern(2.0 * x + dx + 0.5 + ox, 2.0 * y + dy + 0.5 + oy, 0);
                const float v = float(s / 4); lowres[size_t(y) * w + x] = v;
                row[4 * x] = row[4 * x + 1] = row[4 * x + 2] = half(v); row[4 * x + 3] = half(1.0f);
            }
        }
        upC->Unmap(0, nullptr);
        alloc->Reset(); list->Reset(alloc.Get(), nullptr);
        up(color.Get(), upC.Get(), fpC);
        if (k == 0) { up(motion.Get(), upM.Get(), fpM); up(depth.Get(), upD.Get(), fpD); bar(out.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
        const auto t = Clock::now();
        if (!srEvaluate(c, list.Get(), color.Get(), motion.Get(), depth.Get(), out.Get(), w, h, W, H, k == 0 || resetAlways, 33.3f)) { list->Close(); CloseHandle(ev); srDestroy(c); r.note = "xessD3D12Execute failed"; return r; }
        bar(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        const bool measure = k + 4 >= frames;
        if (measure) {
            bar(out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION d{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; d.PlacedFootprint = fpO;
            D3D12_TEXTURE_COPY_LOCATION s{out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; s.SubresourceIndex = 0;
            list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            bar(out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        idle();
        execMs += msSince(t);
        if (FAILED(device->GetDeviceRemovedReason())) { CloseHandle(ev); srDestroy(c); r.note = "device removed during XeSS SR"; return r; }
        if (!measure) continue;
        uint8_t* o = nullptr; rb->Map(0, nullptr, reinterpret_cast<void**>(&o));
        double mean = 0;
        for (uint32_t y = 0; y < H; ++y) {
            auto* row = reinterpret_cast<const uint16_t*>(o + fpO.Offset + size_t(y) * fpO.Footprint.RowPitch);
            for (uint32_t x = 0; x < W; ++x) { got[size_t(y) * W + x] = unhalf(row[4 * x]); mean += got[size_t(y) * W + x]; truth[size_t(y) * W + x] = float(pattern(x + 0.5 + ox, y + 0.5 + oy, 0)); }
        }
        D3D12_RANGE none{0, 0}; rb->Unmap(0, &none);
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {   // bilinear upscale of the input
            const double sx = std::clamp((x + 0.5) / 2 - 0.5, 0.0, double(w - 1)), sy = std::clamp((y + 0.5) / 2 - 0.5, 0.0, double(h - 1));
            const uint32_t x0 = uint32_t(sx), y0 = uint32_t(sy), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
            const double fx = sx - x0, fy = sy - y0;
            bil[size_t(y) * W + x] = float((lowres[y0 * w + x0] * (1 - fx) + lowres[y0 * w + x1] * fx) * (1 - fy) + (lowres[y1 * w + x0] * (1 - fx) + lowres[y1 * w + x1] * fx) * fy);
        }
        mean /= double(W) * H;
        if (!(mean > 0.05 && mean < 0.95)) { CloseHandle(ev); srDestroy(c); r.note = std::format("XeSS SR output is blank or saturated (mean {:.3f})", mean); return r; }
        px.push_back(psnrF(got, truth)); pb.push_back(psnrF(bil, truth));
    }
    CloseHandle(ev);
    srDestroy(c);
    std::sort(px.begin(), px.end()); std::sort(pb.begin(), pb.end());
    r.psnrXess = px[px.size() / 2]; r.psnrBilinear = pb[pb.size() / 2];
    r.msPerFrame = execMs / frames;
    r.passed = r.psnrXess >= r.psnrBilinear - 0.5;
    r.note = std::format("{}x{} -> {}x{}, {} frames{}: PSNR XeSS {:.2f} dB, bilinear {:.2f} dB, {:.1f} ms per frame (submit + wait); {}; {}", w, h, W, H, frames, resetAlways ? " (zero motion, reset every frame)" : zeroMotion ? " (zero motion)" : "", r.psnrXess, r.psnrBilinear,
                         r.msPerFrame, r.version, r.passed ? "PASS" : "FAIL (below bilinear)");
    return r;
}

}  // namespace pushmpc::xess
