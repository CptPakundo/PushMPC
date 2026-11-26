// pushcore.dll (stub): the pushcore ABI 3 exports with no NVIDIA stack, for the VapourSynth plugin's tests and CI (no GPU
// needed: it runs on WARP). It converts each NV12/P010 source frame to RGB on the CPU and hands it out as a real frame;
// with multiplier k > 1 it first hands out k - 1 generated frames that blend the previous and the current source frame
// (pts on the k-th grid), in pushcore's presentation order. The output size equals the source size.
//   PUSHCORE_STUB_SKIP_EVERY=N  : no generated frames for every N-th pair (as pushcore's skippedGenerated)
//   PUSHCORE_STUB_OPEN_MS=N     : pushcore_open sleeps N ms (a cold open)
//   PUSHCORE_STUB_NO_FENCE=1    : hand out frames without a fence
//   PUSHCORE_STUB_FAST_START=1  : pushcore_features() also reports PUSHCORE_FEATURE_FAST_START (read at each call), so the
//                                 hosts run their quick-start + handover path (tests; with PUSHMPC_TEST_QUICK_ANY_GPU=1)
//   PUSHCORE_STUB_FULL_OPEN_MS=N: an open with PUSHCORE_FLAG_WARMUP_GPU_ONLY (the hosts' full graph) sleeps N ms
// Every call is logged to PUSHCORE_STUB_LOG when set (the tests count opens, flushes and submits there).
#define PUSHCORE_BUILD
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include "pushmpc/pushcore_abi.h"

using Microsoft::WRL::ComPtr;

namespace {
int EnvInt(const char* n, int def) { char b[32] = {}; return GetEnvironmentVariableA(n, b, 32) ? atoi(b) : def; }
void StubLog(const char* fmt, ...)
{
    char path[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("PUSHCORE_STUB_LOG", path, MAX_PATH)) return;
    static std::mutex m; std::lock_guard<std::mutex> lk(m);
    FILE* f = nullptr; if (fopen_s(&f, path, "ab") || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputs("\r\n", f); fclose(f);
}
std::atomic<uint32_t> g_opens{0};
} // namespace

struct PushCore {
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<ID3D11Fence> fence; uint64_t fenceValue = 0;
    PushCoreStreamInfo si{}; PushCoreSettings ps{}; PushCoreOutputInfo oi{}; PushCoreStats st{};
    bool hdrOut = false; uint32_t k = 1;
    ComPtr<ID3D11Texture2D> inStage; DXGI_FORMAT inFmt = DXGI_FORMAT_NV12;
    std::vector<uint32_t> prev, cur; bool havePrev = false; uint64_t pairs = 0; int64_t prevPts = 0;
    struct Item { ComPtr<ID3D11Texture2D> tex; PushCoreFrame fr{}; };
    std::deque<Item> queue; std::vector<Item> leased; std::vector<ComPtr<ID3D11Texture2D>> pool;
    uint64_t nextLease = 1, sourceIndex = 0;
    std::string err;
};

static bool ToRgb(PushCore* c, ID3D11Texture2D* src, uint32_t slice)
{
    const uint32_t w = c->si.width, h = c->si.height;
    if (!c->inStage) {
        D3D11_TEXTURE2D_DESC d = {}; src->GetDesc(&d);
        D3D11_TEXTURE2D_DESC s = d; s.ArraySize = 1; s.MipLevels = 1; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
        if (FAILED(c->dev->CreateTexture2D(&s, nullptr, &c->inStage))) { c->err = "stub: staging texture"; return false; }
        c->inFmt = d.Format;
    }
    c->ctx->CopySubresourceRegion(c->inStage.Get(), 0, 0, 0, 0, src, slice, nullptr);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(c->ctx->Map(c->inStage.Get(), 0, D3D11_MAP_READ, 0, &m))) { c->err = "stub: Map"; return false; }
    D3D11_TEXTURE2D_DESC d = {}; c->inStage->GetDesc(&d);
    const uint8_t* base = static_cast<const uint8_t*>(m.pData);
    const uint8_t* uvBase = base + size_t(m.RowPitch) * d.Height;
    const bool p010 = c->inFmt == DXGI_FORMAT_P010;
    const bool bt2020 = c->si.matrix == 9;
    const float kr = bt2020 ? 0.2627f : 0.2126f, kb = bt2020 ? 0.0593f : 0.0722f, kg = 1 - kr - kb;
    c->cur.resize(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        float Y, U, V;
        if (p010) {
            Y = (reinterpret_cast<const uint16_t*>(base + size_t(y) * m.RowPitch)[x] >> 6) / 1023.0f;
            const uint16_t* uv = reinterpret_cast<const uint16_t*>(uvBase + size_t(y / 2) * m.RowPitch);
            U = (uv[(x / 2) * 2] >> 6) / 1023.0f; V = (uv[(x / 2) * 2 + 1] >> 6) / 1023.0f;
        } else {
            Y = base[size_t(y) * m.RowPitch + x] / 255.0f;
            const uint8_t* uv = uvBase + size_t(y / 2) * m.RowPitch;
            U = uv[(x / 2) * 2] / 255.0f; V = uv[(x / 2) * 2 + 1] / 255.0f;
        }
        const float yl = (Y - 16.0f / 255) * 255 / 219, cb = (U - 0.5f) * 255 / 224, cr = (V - 0.5f) * 255 / 224;
        const float r = yl + 2 * (1 - kr) * cr, b = yl + 2 * (1 - kb) * cb, g = (yl - kr * r - kb * b) / kg;
        auto q = [&](float v, float mx) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * mx + 0.5f); };
        c->cur[size_t(y) * w + x] = c->hdrOut ? (q(r, 1023) | (q(g, 1023) << 10) | (q(b, 1023) << 20) | (3u << 30)) : (q(r, 255) | (q(g, 255) << 8) | (q(b, 255) << 16) | (255u << 24));
    }
    c->ctx->Unmap(c->inStage.Get(), 0);
    return true;
}
static uint32_t Blend(uint32_t a, uint32_t b, float t, bool ten)
{
    uint32_t o = 0;
    const int bits = ten ? 10 : 8, n = ten ? 3 : 4; const uint32_t mask = (1u << bits) - 1;
    for (int i = 0; i < n; ++i) { const float x = float((a >> (i * bits)) & mask), y = float((b >> (i * bits)) & mask); o |= uint32_t(x + (y - x) * t + 0.5f) << (i * bits); }
    return ten ? (o | (3u << 30)) : o;
}
static bool Emit(PushCore* c, const std::vector<uint32_t>& px, int64_t pts, uint32_t kind, uint32_t sub, uint64_t srcIdx)
{
    ComPtr<ID3D11Texture2D> t;
    if (!c->pool.empty()) { t = c->pool.back(); c->pool.pop_back(); }
    else {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = c->si.width; d.Height = c->si.height; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
        d.Format = c->hdrOut ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(c->dev->CreateTexture2D(&d, nullptr, &t))) { c->err = "stub: output texture"; return false; }
    }
    c->ctx->UpdateSubresource(t.Get(), 0, nullptr, px.data(), c->si.width * 4, 0);
    PushCore::Item it; it.tex = t;
    it.fr.texture = t.Get(); it.fr.pts100ns = pts; it.fr.sourceIndex = srcIdx; it.fr.kind = kind; it.fr.subframe = sub;
    it.fr.width = c->si.width; it.fr.height = c->si.height;
    if (c->fence && c->ctx4) { c->ctx4->Signal(c->fence.Get(), ++c->fenceValue); it.fr.readyFence = c->fence.Get(); it.fr.readyValue = c->fenceValue; }
    c->queue.push_back(it);
    c->st.outputs++; (kind == PUSHCORE_FRAME_REAL ? c->st.real : c->st.generated)++;
    return true;
}

extern "C" {
PUSHCORE_API uint32_t pushcore_abiVersion(void) { return PUSHCORE_ABI_VERSION; }
PUSHCORE_API uint32_t pushcore_features(void) { return PUSHCORE_FEATURE_FG_METHODS | PUSHCORE_FEATURE_CUT_COUNT | (EnvInt("PUSHCORE_STUB_FAST_START", 0) ? PUSHCORE_FEATURE_FAST_START : 0u); }
PUSHCORE_API int pushcore_capabilities(uint64_t, const wchar_t*, PushCoreCaps* out)
{
    if (!out || out->structSize < sizeof(PushCoreCaps)) return PUSHCORE_ERROR;
    if (EnvInt("PUSHCORE_STUB_CAPS_FAIL", 0)) return PUSHCORE_ERROR;   // tests: a core that cannot answer
    const uint32_t size = out->structSize;
    memset(out, 0, sizeof(*out)); out->structSize = size;
    out->family = PUSHCORE_FAMILY_RTX50; out->vendorId = 0x10DE;
    out->available = out->hardware = (1u << PUSHCORE_CAP_COUNT) - 1;
    strcpy_s(out->adapter, "PushCore stub (RTX 5090 class)");
    return PUSHCORE_OK;
}
PUSHCORE_API int pushcore_pushFlowBench(uint64_t, const wchar_t*, PushCoreFlowBench*) { return PUSHCORE_ERROR; }
PUSHCORE_API PushCore* pushcore_open(const PushCoreConfig* cfg, const PushCoreStreamInfo* si, ID3D11Device* dev, char* eb, uint32_t ebn)
{
    if (!cfg || cfg->abiVersion != PUSHCORE_ABI_VERSION || !si || !dev) { if (eb && ebn) strcpy_s(eb, ebn, "stub: bad arguments"); return nullptr; }
    if (const int ms = EnvInt("PUSHCORE_STUB_OPEN_MS", 0)) Sleep(DWORD(ms));
    if (cfg->flags & PUSHCORE_FLAG_WARMUP_GPU_ONLY) if (const int ms = EnvInt("PUSHCORE_STUB_FULL_OPEN_MS", 0)) Sleep(DWORD(ms));
    auto* c = new PushCore();
    c->dev = dev; dev->GetImmediateContext(&c->ctx); c->ctx.As(&c->ctx4);
    c->si = *si;
    if (cfg->settings) memcpy(&c->ps, cfg->settings, std::min<size_t>(sizeof(c->ps), cfg->settings->structSize));
    else c->ps.multiplier = cfg->multiplier > 0 ? uint32_t(cfg->multiplier) : 1u;
    c->k = std::clamp<uint32_t>(c->ps.multiplier, 1, 8);
    c->hdrOut = (si->transfer == 16 || si->transfer == 18 || c->ps.videoHdr) && si->displayHdr;
    ComPtr<ID3D11Device5> d5;
    if (!EnvInt("PUSHCORE_STUB_NO_FENCE", 0) && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d5)))) d5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&c->fence));
    c->oi.width = si->width; c->oi.height = si->height; c->oi.multiplier = c->k;
    c->oi.dxgiFormat = c->hdrOut ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    c->oi.hdrInput = (si->transfer == 16 || si->transfer == 18) ? 1 : 0; c->oi.hdrOutput = c->hdrOut ? 1 : 0;
    snprintf(c->oi.description, sizeof(c->oi.description), "stub: source %ux%u %s -> output %ux%u; FG blend %uX%s", si->width, si->height, si->format ? "P010" : "NV12",
             si->width, si->height, c->k, c->hdrOut ? "; HDR10" : "");
    c->st.structSize = sizeof(c->st);
    const uint32_t n = ++g_opens;
    StubLog("open %u %ux%u fmt %u k %u transfer %d displayHdr %u fence %d flags %u", n, si->width, si->height, si->format, c->k, si->transfer, si->displayHdr, c->fence ? 1 : 0, cfg->flags);
    return c;
}
PUSHCORE_API int pushcore_canSubmit(PushCore* c) { return c && c->leased.size() + c->queue.size() < 4u * c->k + 4u; }
PUSHCORE_API int pushcore_submit2(PushCore* c, ID3D11Texture2D* tex, uint32_t slice, int64_t pts, uint32_t flags, const PushCoreSubmitTiming*)
{
    if (!c || !tex) return PUSHCORE_ERROR;
    if (!pushcore_canSubmit(c)) { c->st.busy++; return PUSHCORE_BUSY; }
    if (!ToRgb(c, tex, slice)) return PUSHCORE_ERROR;
    const bool disc = (flags & PUSHCORE_SUBMIT_DISCONTINUITY) != 0;
    StubLog("submit pts %lld disc %d", (long long)pts, disc ? 1 : 0);
    if (disc) c->havePrev = false;
    if (c->havePrev && c->k > 1) {
        const int skipEvery = EnvInt("PUSHCORE_STUB_SKIP_EVERY", 0);
        ++c->pairs;
        if (skipEvery > 0 && c->pairs % uint64_t(skipEvery) == 0) c->st.skippedGenerated += c->k - 1;
        else {
            std::vector<uint32_t> g(c->cur.size());
            for (uint32_t i = 1; i < c->k; ++i) {
                const float t = float(i) / float(c->k);
                for (size_t p = 0; p < g.size(); ++p) g[p] = Blend(c->prev[p], c->cur[p], t, c->hdrOut);
                const int64_t gp = c->prevPts + int64_t(std::llround(double(pts - c->prevPts) * i / c->k));
                if (!Emit(c, g, gp, PUSHCORE_FRAME_GENERATED, i, c->sourceIndex - 1)) return PUSHCORE_ERROR;
            }
        }
    }
    if (!Emit(c, c->cur, pts, PUSHCORE_FRAME_REAL, 0, c->sourceIndex)) return PUSHCORE_ERROR;
    c->prev.swap(c->cur); c->havePrev = true; c->prevPts = pts;
    c->sourceIndex++; c->st.submitted++;
    return PUSHCORE_OK;
}
PUSHCORE_API int pushcore_submit(PushCore* c, ID3D11Texture2D* t, uint32_t s, int64_t pts, uint32_t f) { return pushcore_submit2(c, t, s, pts, f, nullptr); }
PUSHCORE_API int pushcore_poll(PushCore* c, PushCoreFrame* frames, uint32_t cap, uint32_t* count, uint32_t)
{
    if (!c || !count) return PUSHCORE_ERROR;
    *count = 0;
    while (*count < cap && !c->queue.empty()) {
        PushCore::Item it = c->queue.front(); c->queue.pop_front();
        it.fr.lease = c->nextLease++;
        frames[(*count)++] = it.fr;
        c->leased.push_back(it);
    }
    return PUSHCORE_OK;
}
PUSHCORE_API int pushcore_presented(PushCore* c, uint64_t lease, ID3D11DeviceContext*)
{
    if (!c) return PUSHCORE_ERROR;
    for (size_t i = 0; i < c->leased.size(); ++i) if (c->leased[i].fr.lease == lease) { c->pool.push_back(c->leased[i].tex); c->leased.erase(c->leased.begin() + ptrdiff_t(i)); return PUSHCORE_OK; }
    return PUSHCORE_ERROR;
}
PUSHCORE_API int pushcore_flush(PushCore* c)
{
    if (!c) return PUSHCORE_ERROR;
    if (!c->leased.empty()) return PUSHCORE_BUSY;
    for (auto& it : c->queue) c->pool.push_back(it.tex);
    c->queue.clear(); c->havePrev = false;
    StubLog("flush");
    return PUSHCORE_OK;
}
PUSHCORE_API int pushcore_reload(PushCore* c)
{
    if (!c) return PUSHCORE_ERROR;
    if (!c->leased.empty()) return PUSHCORE_BUSY;
    c->k = std::clamp<uint32_t>(c->ps.multiplier, 1, 8); c->oi.multiplier = c->k;
    c->queue.clear(); c->havePrev = false;
    StubLog("reload k %u", c->k);
    return PUSHCORE_OK;
}
PUSHCORE_API int pushcore_applySettings(PushCore* c) { return c ? PUSHCORE_NEEDS_REBUILD : PUSHCORE_ERROR; }
PUSHCORE_API int pushcore_setSettings(PushCore* c, const PushCoreSettings* s)
{
    if (!c) return PUSHCORE_ERROR;
    if (s) memcpy(&c->ps, s, std::min<size_t>(sizeof(c->ps), s->structSize));
    StubLog("setSettings k %u", c->ps.multiplier);
    return PUSHCORE_OK;
}
PUSHCORE_API int pushcore_importChainSession(const wchar_t*, PushCoreSettings*, char* eb, uint32_t n) { if (eb && n) strcpy_s(eb, n, "stub: no chain session"); return PUSHCORE_ERROR; }
PUSHCORE_API int pushcore_outputInfo(PushCore* c, PushCoreOutputInfo* i) { if (!c || !i) return PUSHCORE_ERROR; *i = c->oi; return PUSHCORE_OK; }
PUSHCORE_API int pushcore_stats(PushCore* c, PushCoreStats* s)
{
    if (!c || !s) return PUSHCORE_ERROR;
    const uint32_t size = std::min<uint32_t>(s->structSize ? s->structSize : sizeof(*s), sizeof(*s));
    c->st.leasesOut = c->leased.size(); c->st.pendingFrames = c->queue.size();
    memcpy(s, &c->st, size); s->structSize = size;
    return PUSHCORE_OK;
}
PUSHCORE_API int pushcore_takeGpuTimings(PushCore*, PushCoreGpuTiming*, uint32_t, uint32_t* n) { if (n) *n = 0; return PUSHCORE_OK; }
PUSHCORE_API const char* pushcore_lastError(PushCore* c) { return c ? c->err.c_str() : "stub: no instance"; }
PUSHCORE_API void pushcore_close(PushCore* c) { StubLog("close"); delete c; }
PUSHCORE_API const PushCoreApi* pushcore_getApi(uint32_t abi)
{
    if (abi != PUSHCORE_ABI_VERSION) return nullptr;
    static const PushCoreApi api = { sizeof(PushCoreApi), PUSHCORE_ABI_VERSION, pushcore_abiVersion, pushcore_open, pushcore_canSubmit, pushcore_submit, pushcore_poll,
        pushcore_presented, pushcore_reload, pushcore_flush, pushcore_outputInfo, pushcore_stats, pushcore_takeGpuTimings, pushcore_lastError, pushcore_close,
        pushcore_submit2, pushcore_applySettings, pushcore_setSettings, pushcore_importChainSession };
    return &api;
}
PUSHCORE_API uint32_t pushcore_stub_opens(void) { return g_opens; }
}
