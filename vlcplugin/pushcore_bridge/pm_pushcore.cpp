// pm_pushcore.dll: pushcore for the VLC module (see pm_pushcore.h). MSVC; reuses vsplugin's CoreHost and the filter's
// Auto rules.
#include "pm_pushcore.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include "corehost.h"
#include "autopick.h"
#include "vslog.h"

using namespace pmvs;

struct pm_pc {
    uint64_t token = 0;
    uint32_t w = 0, h = 0;
    int bits = 8;
    int k = 1;
    // the source frame handed to CoreHost::Submit through its source callback
    PlaneView cur;
    // generated frames land in a buffer of ours first (CoreHost allocates before it knows the frame), then go out
    std::vector<uint8_t> buf[4];
    size_t bufNext = 0;
    pm_pc_dest_fn dest = nullptr;
    pm_pc_done_fn done = nullptr;
    void* user = nullptr;
    int handed = 0;
    std::string describe;
};

static void SetErr(char* err, size_t n, const std::string& s) { if (err && n) { strncpy_s(err, n, s.c_str(), _TRUNCATE); } }

static std::wstring DefaultCoreDir()
{
    std::wstring d = LocalAppDataDir() + L"\\PushMPC\\core";
    return d;
}

extern "C" pm_pc* pm_pc_open(const pm_pc_cfg* cfg, int* kOut, char* err, size_t errsize)
{
    if (kOut) *kOut = 1;
    if (!cfg || cfg->abi != PM_PC_ABI) { SetErr(err, errsize, "pm_pushcore: ABI mismatch"); return nullptr; }
    if (!(cfg->matrix == 1 || cfg->matrix == 9 || cfg->matrix == 10)) {
        SetErr(err, errsize, std::format("pm_pushcore: matrix {} (SD / BT.601 sources stay on the portable engine)", cfg->matrix));
        return nullptr;
    }
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    HostPaths paths;
    paths.coreDir = cfg->core_dir && *cfg->core_dir ? cfg->core_dir : DefaultCoreDir();
    const std::wstring rt = LocalAppDataDir() + L"\\PushMPC\\runtime";
    paths.runtimeDir = GetFileAttributesW(rt.c_str()) != INVALID_FILE_ATTRIBUTES ? rt : paths.coreDir;
    const std::wstring sh = paths.coreDir + L"\\shaders";
    if (GetFileAttributesW(sh.c_str()) != INVALID_FILE_ATTRIBUTES) paths.shaderDir = sh;
    std::string e;
    if (!host.Load(paths, e) || !host.EnsureDevice(0, cfg->warp ? AdapterPick::Warp : AdapterPick::Auto, e)) { SetErr(err, errsize, e); return nullptr; }

    // Auto with the user's PushMPC settings; VLC keeps the source size and shows its own real frames
    pm::Settings s = pm::LoadSettings();
    if (cfg->smoother == 1) s.smoother = pm::kOff;
    else if (cfg->smoother >= 2) s.smoother = std::clamp(cfg->smoother, 2, 8);
    s.sharper = pm::kOff; s.cleanUp = pm::kOff; s.deband = pm::kOff; s.sdrToHdr = pm::kOff;
    pm::Gpu g; g.available = 0; g.hardware = 0; g.family = pm::kFamUnknown;
    PushCoreCaps caps;
    if (host.Caps(caps)) { g.family = caps.family; g.available = caps.available; g.hardware = caps.hardware; g.name = Widen(caps.adapter); }
    pm::Costs costs;
    costs.card = pm::CardFactor(g.name, g.family);
    LoadLearned(costs, g.name);
    pm::Source src;
    src.w = cfg->width; src.h = cfg->height;
    src.fps = cfg->fps_num > 0 && cfg->fps_den > 0 ? double(cfg->fps_num) / cfg->fps_den : 23.976;
    src.hdr = IsHdrTransfer(cfg->transfer); src.tenBit = cfg->bits > 8;
    pm::Display disp = pm::QueryDisplay(nullptr, s.vrr, g.nvidia());
    if (cfg->display_hz > 1) disp.refreshHz = cfg->display_hz;
    disp.hdr = src.hdr;   // HDR sources stay HDR10 (VLC presents them); SDR stays SDR
    pm::Choice c = pm::Decide(s, src, disp, costs, g);
    c.sr = false; c.nr = false; c.deband = false; c.hdr = false;
    PushCoreSettings ps = ToCore(s, c, disp, src.w, src.h, src.hdr, disp.hdr, nullptr);
    ps.sr = 0; ps.nr = 0; ps.deband = 0; ps.videoHdr = 0;

    PushCoreStreamInfo si = {};
    si.structSize = sizeof(si);
    si.width = cfg->width; si.height = cfg->height;
    si.format = cfg->bits > 8 ? PUSHCORE_INPUT_P010 : PUSHCORE_INPUT_NV12;
    si.fpsNum = cfg->fps_num; si.fpsDen = cfg->fps_den;
    si.range = cfg->range; si.matrix = cfg->matrix; si.primaries = cfg->primaries; si.transfer = cfg->transfer;
    si.displayHdr = disp.hdr ? 1u : 0u;
    CoreKey key;
    key.width = si.width; key.height = si.height; key.format = si.format; key.fpsNum = si.fpsNum; key.fpsDen = si.fpsDen;
    key.range = si.range; key.matrix = si.matrix; key.primaries = si.primaries; key.transfer = si.transfer; key.displayHdr = si.displayHdr;
    key.adapterLuid = host.DeviceLuid();
    key.settings.assign(reinterpret_cast<const uint8_t*>(&ps), reinterpret_cast<const uint8_t*>(&ps) + sizeof(ps));
    Reuse how = Reuse::Open; double ms = 0;
    auto* pc = new pm_pc();
    pc->token = host.Acquire(key, si, ps, how, ms, e);
    if (!pc->token) { SetErr(err, errsize, "pushcore_open: " + e); delete pc; return nullptr; }
    const PushCoreOutputInfo& oi = host.OutInfo();
    if (oi.width != cfg->width || oi.height != cfg->height) {
        host.Release(pc->token);
        SetErr(err, errsize, std::format("pushcore output {}x{} differs from the source {}x{}", oi.width, oi.height, cfg->width, cfg->height));
        delete pc; return nullptr;
    }
    if (!host.ConfigureEgress(cfg->bits > 8 || oi.hdrOutput ? OutFmt::Yuv420P10 : OutFmt::Yuv420P8, oi.hdrOutput != 0, e)) {
        host.Release(pc->token); SetErr(err, errsize, "egress: " + e); delete pc; return nullptr;
    }
    pc->w = cfg->width; pc->h = cfg->height; pc->bits = cfg->bits;
    pc->k = int(std::clamp<uint32_t>(oi.multiplier ? oi.multiplier : ps.multiplier, 1, 8));
    pc->describe = std::format("{} ({} in {:.0f} ms on {}; Auto: x{} {})", oi.description, ReuseName(how), ms, host.DeviceName(),
                               c.multiplier, Narrow(pm::FgMethodLabel(c.fgMethod)));
    Log("VLC: " + pc->describe);

    // the callbacks stay set for this owner
    host.source = [pc](int64_t, PlaneView& v) { v = pc->cur; return v.data[0] != nullptr; };
    host.alloc = [pc](uint32_t w, uint32_t h, OutImage& img) -> bool {
        const size_t bps = pc->bits > 8 ? 2 : 1, ysz = size_t(w) * h * bps, csz = size_t(w / 2) * (h / 2) * bps;
        auto& b = pc->buf[pc->bufNext]; pc->bufNext = (pc->bufNext + 1) % 4;
        b.resize(ysz + 2 * csz);
        img.data[0] = b.data(); img.data[1] = b.data() + ysz; img.data[2] = b.data() + ysz + csz;
        img.stride[0] = ptrdiff_t(w * bps); img.stride[1] = img.stride[2] = ptrdiff_t((w / 2) * bps);
        img.handle = &b;
        return true;
    };
    host.release = [](void*) {};
    host.tag = [pc](int64_t pts, bool generated, void* handle) {
        if (!generated || !pc->dest) return;
        const pm_pc_out* d = pc->dest(pc->user, pts);
        if (!d) return;
        auto* b = static_cast<std::vector<uint8_t>*>(handle);
        const size_t bps = pc->bits > 8 ? 2 : 1, ysz = size_t(pc->w) * pc->h * bps, csz = size_t(pc->w / 2) * (pc->h / 2) * bps;
        const uint8_t* srcp[3] = { b->data(), b->data() + ysz, b->data() + ysz + csz };
        for (int i = 0; i < 3; ++i) {
            const uint32_t pw = (i ? pc->w / 2 : pc->w), ph = (i ? pc->h / 2 : pc->h);
            for (uint32_t r = 0; r < ph; ++r) memcpy(d->data[i] + ptrdiff_t(r) * d->stride[i], srcp[i] + size_t(r) * pw * bps, pw * bps);
        }
        pc->handed++;
        if (pc->done) pc->done(pc->user, pts);
    };
    if (kOut) *kOut = pc->k;
    return pc;
}

extern "C" int pm_pc_push(pm_pc* pc, const pm_pc_planes* src, int64_t pts100ns, int discontinuity, char* err, size_t errsize)
{
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    if (!host.Owns(pc->token)) { SetErr(err, errsize, "pushcore was taken over by another instance"); return -1; }
    pc->cur = PlaneView{};
    for (int i = 0; i < 3; ++i) { pc->cur.data[i] = src->data[i]; pc->cur.stride[i] = src->stride[i]; }
    pc->cur.width = pc->w; pc->cur.height = pc->h; pc->cur.bits = pc->bits;
    std::string e;
    const bool ok = host.Submit(0, pts100ns, discontinuity != 0, e);
    pc->cur = PlaneView{};
    if (!ok) { SetErr(err, errsize, e); return -1; }
    return 0;
}

extern "C" int pm_pc_poll(pm_pc* pc, uint32_t timeout_ms, pm_pc_dest_fn dest, pm_pc_done_fn done, void* user, char* err, size_t errsize)
{
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    if (!host.Owns(pc->token)) { SetErr(err, errsize, "pushcore was taken over by another instance"); return -1; }
    pc->dest = dest; pc->done = done; pc->user = user; pc->handed = 0;
    std::vector<Out<void*>> out;
    std::string e;
    const bool ok = host.Poll(timeout_ms, out, e);
    pc->dest = nullptr; pc->done = nullptr; pc->user = nullptr;
    if (!ok) { SetErr(err, errsize, e); return -1; }
    return int(out.size());   // every frame polled (real ones are returned to pushcore at once)
}

extern "C" int pm_pc_flush(pm_pc* pc)
{
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    if (!host.Owns(pc->token)) return -1;
    std::string e;
    return host.Flush(e) ? 0 : -1;
}

extern "C" void pm_pc_close(pm_pc* pc)
{
    if (!pc) return;
    {
        CoreHost& host = CoreHost::Get();
        std::lock_guard<std::mutex> lk(host.Lock());
        host.Release(pc->token);   // the core stays warm for the next file
    }
    delete pc;
}

extern "C" const char* pm_pc_describe(pm_pc* pc) { return pc ? pc->describe.c_str() : ""; }
