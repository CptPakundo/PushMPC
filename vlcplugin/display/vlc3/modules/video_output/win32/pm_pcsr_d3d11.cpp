// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC: pushcore's upscaler as the output upscaler of the Direct3D 11 video output fork (see pm_pcsr_d3d11.h).
#include <vlc_common.h>
#include <vlc_configuration.h>
#include <vlc_modules.h>
#include <vlc_variables.h>
#include "pm_pcsr_d3d11.h"
#include "pm_nis_d3d11.h"
#undef min                      // windows.h's (the build force-includes winsock2.h): std::min / std::max below
#undef max
#include <new>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pm_pushcore.h"        // vlcplugin/pushcore_bridge: the bridge's C interface (no link dependency)
#include "PushMpcAnimeSr.h"     // filter/src: the anime models and Auto's pick (PickAnimeSr)
#include "win_anime_route.h"    // kodiaddon/src/engine: the switch that follows the measured class, as in Kodi

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------------------------------- the bridge
struct Bridge {
    bool tried = false;
    HMODULE dll = nullptr;
    uint32_t abi = 2;
    pm_pc_open_d3d11_fn open = nullptr;
    pm_pc_push_d3d11_fn push = nullptr;
    pm_pc_poll_d3d11_fn pollTex = nullptr;   // not "poll": VLC's vlc_threads.h defines poll() as a macro
    pm_pc_close_fn close = nullptr;
    pm_pc_describe_fn describe = nullptr;
    pm_pc_update_fn update = nullptr;
    pm_pc_output_fn output = nullptr;
};
Bridge B;                         // never unloaded: pushcore stays warm for the next file
SRWLOCK lockB = SRWLOCK_INIT;

// pm_pushcore.dll next to this plugin (as the PushMPC filter loads it); false with the reason
bool LoadBridge(std::string &why)
{
    AcquireSRWLockExclusive(&lockB);
    if (!B.tried) {
        B.tried = true;
        HMODULE self = nullptr;
        wchar_t path[MAX_PATH * 2];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(reinterpret_cast<void *>(&LoadBridge)), &self) &&
            GetModuleFileNameW(self, path, MAX_PATH * 2)) {
            wchar_t *slash = wcsrchr(path, L'\\');
            if (slash && size_t(slash + 1 - path) + 16 < MAX_PATH * 2) {
                wcscpy(slash + 1, L"pm_pushcore.dll");
                B.dll = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            }
        }
        if (B.dll) {
            auto sym = [](const char *n) { return reinterpret_cast<void *>(GetProcAddress(B.dll, n)); };
            B.open = reinterpret_cast<pm_pc_open_d3d11_fn>(sym("pm_pc_open_d3d11"));
            B.push = reinterpret_cast<pm_pc_push_d3d11_fn>(sym("pm_pc_push_d3d11"));
            B.pollTex = reinterpret_cast<pm_pc_poll_d3d11_fn>(sym("pm_pc_poll_d3d11"));
            B.close = reinterpret_cast<pm_pc_close_fn>(sym("pm_pc_close"));
            B.describe = reinterpret_cast<pm_pc_describe_fn>(sym("pm_pc_describe"));
            B.update = reinterpret_cast<pm_pc_update_fn>(sym("pm_pc_update"));
            B.output = reinterpret_cast<pm_pc_output_fn>(sym("pm_pc_output"));
            const auto abiFn = reinterpret_cast<pm_pc_abi_fn>(sym("pm_pc_abi"));
            B.abi = abiFn && B.update && B.output ? std::min<uint32_t>(abiFn(), PM_PC_ABI) : 2u;
        }
    }
    bool ok = false;
    if (!B.dll) why = "pm_pushcore.dll is not next to the plugin";
    else if (!B.open || !B.push || !B.pollTex || !B.close || !B.describe) why = "pm_pushcore.dll has no zero-copy path";
    else if (B.abi < 3) why = "pm_pushcore.dll is older than ABI 3 (no upscaler, no settings update)";
    else ok = true;
    ReleaseSRWLockExclusive(&lockB);
    return ok;
}

// ---------------------------------------------------------------------------------------------------- the checks
// One pushcore per process (the bridge's host hands the core to the last instance that opens it): PushMPC's frame
// generation filter in the chain may hold it for the whole stream. VLC opens the output before it builds the filter
// chain, and the filter's copy path opens pushcore even with its Smoother off (it closes it again at x1, but the open has
// taken the core from the output by then): only its portable engines (CPU, Vulkan) leave pushcore to the output.
bool FilterMayHoldPushcore(vlc_object_t *o, std::string &why)
{
    char *chain = var_InheritString(o, "video-filter");
    bool listed = false;
    if (chain) {
        const std::string s = chain;
        free(chain);
        for (size_t at = 0; at <= s.size();) {
            const size_t end = std::min(s.find(':', at), s.size());
            std::string t = s.substr(at, end - at);
            t = t.substr(0, t.find('{'));   // "name{options}"
            if (t == "pushmpc" || t == "pushmpc_out" || (t == "postproc" && module_exists("pushmpc")))
                listed = true;
            at = end + 1;
        }
    }
    if (!listed) return false;
    const int64_t engine = config_GetType("pushmpc-engine") ? var_InheritInteger(o, "pushmpc-engine") : 0;
    if (engine == 1 || engine == 3) return false;
    why = "PushMPC's frame generation filter is in VLC's chain and opens pushcore (one pushcore per player; with the "
          "filter's portable engine pushcore is the output's)";
    return true;
}

// pmml's runtime with the anime models (as the Kodi add-on looks for it): PMML_RUNTIME, else PushMPC's per-user folder
bool PmmlRuntime()
{
    std::string dirs[2];
    if (const char *e = getenv("PMML_RUNTIME")) dirs[0] = e;
    if (const char *la = getenv("LOCALAPPDATA")) dirs[1] = std::string(la) + "\\PushMPC\\pmml";
    for (const std::string &d : dirs) {
        if (d.empty()) continue;
        const std::string f = d + "\\graphs\\anime-sr\\ArtCNN_C4F16.onnx";
        if (GetFileAttributesA(f.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

// the screen the video window is on (its mode, in pixels), at least the window
void ScreenOf(HWND wnd, unsigned &w, unsigned &h)
{
    HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (mon && GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        w = std::max<unsigned>(w, dm.dmPelsWidth); h = std::max<unsigned>(h, dm.dmPelsHeight);
    } else if (mon && GetMonitorInfoW(mon, &mi)) {
        w = std::max<unsigned>(w, unsigned(mi.rcMonitor.right - mi.rcMonitor.left));
        h = std::max<unsigned>(h, unsigned(mi.rcMonitor.bottom - mi.rcMonitor.top));
    }
}

std::string Narrow(const wchar_t *s) { std::string r; for (; s && *s; ++s) r += char(*s); return r; }
int ModelIndex(const std::string &id)
{
    return id.empty() ? -1 : pm::AnimeSrModelIndex(std::wstring(id.begin(), id.end()));
}

} // namespace

struct pm_pcsr {
    pm_pc *pc = nullptr;
    pm_pc_cfg cfg{};                  // the open's (pm_pc_update takes the same source and output size)
    HANDLE mutex = nullptr;           // VLC's context lock, as the bridge takes it (NULL: none)
    unsigned outW = 0, outH = 0;      // pushcore's output (its target box that holds the screen, the shape kept)
    bool hdr = false;                 // pushcore's output matrix: BT.2020 for HDR, else BT.709 (limited range)
    ComPtr<ID3D11Texture2D> dst;      // NV12 / P010 at the output size: where the bridge puts each picture
    ComPtr<ID3D11ShaderResourceView> srvY, srvUV;
    pm_pc_tex tex{};
    double frameMs = 1000.0 / 23.976;
    // the picture waited for, and whether pushcore's picture of it is in dst
    int64_t want = INT64_MIN;
    bool pending = false, got = false;
    bool haveLast = false;
    mtime_t lastDate = 0;
    // pictures pushcore delivered late (NIS drew them), per window; grace: pictures not counted (start, reload)
    int seen = 0, late = 0, grace = 12;
    // the anime model follows the measured class (Kodi's switch and gate)
    pmk::AnimeAutoSwitch sw;
    pmk::PushcoreContentGate gate;
    std::string autoModel;
};

static const pm_pc_tex *Dest(void *user, int64_t pts100)
{
    auto *p = static_cast<pm_pcsr *>(user);
    if (pts100 != p->want || p->got) return nullptr;   // an older picture (late, or before a seek): not wanted
    p->pending = true;
    return &p->tex;
}

static void Done(void *user, int64_t pts100)
{
    auto *p = static_cast<pm_pcsr *>(user);
    if (p->pending && pts100 == p->want) p->got = true;
    p->pending = false;
}

pm_pcsr *PM_PcsrCreate(vlc_object_t *o, d3d11_device_t *d, const video_format_t *src, vlc_fourcc_t chroma,
                       unsigned dispW, unsigned dispH, HWND window, bool forced)
{
    std::string why;
    const unsigned w = src->i_visible_width, h = src->i_visible_height;
    const bool tenBit = chroma == VLC_CODEC_D3D11_OPAQUE_10B;
    // the source: the decoder's own textures, HD / UHD matrices (the bridge's YUV output uses BT.709 / BT.2020)
    const bool uhd = w >= 3200 || h >= 1800;
    const int matrix = src->space == COLOR_SPACE_BT2020 ? 9 : src->space == COLOR_SPACE_BT601 ? 6 :
                       src->space == COLOR_SPACE_BT709 ? 1 : (w >= 1024 || h >= 600 ? (uhd && tenBit ? 9 : 1) : 6);
    unsigned sw = dispW, sh = dispH;
    ScreenOf(window, sw, sh);
    double dar = h ? double(w) / h : 1.0;
    if (src->i_sar_num && src->i_sar_den) dar *= double(src->i_sar_num) / src->i_sar_den;
    // the output: the screen, the source's shape kept (the Kodi add-on's PlanWindowsAnime)
    unsigned ow = sw, oh = unsigned(std::lround(sw / dar));
    if (oh > sh) { oh = sh; ow = unsigned(std::lround(sh * dar)); }
    ow &= ~1u; oh &= ~1u;
    const double fps = src->i_frame_rate && src->i_frame_rate_base ? double(src->i_frame_rate) / src->i_frame_rate_base : 23.976;
    pm::AnimeSrPick pick;
    if (!is_d3d11_opaque(chroma)) why = "software-decoded pictures (the upscaler reads the decoder's textures)";
    else if (w < 16 || h < 16 || (w & 1) || (h & 1) || src->i_x_offset || src->i_y_offset) why = "a cropped or odd-sized picture";
    else if (matrix != 1 && matrix != 9) why = "an SD (BT.601) source: pushcore's bridge takes HD and UHD sources";
    else if (h > 1080) why = "the source is taller than 1080 lines";
    else if (ow <= w * 11 / 10 && oh <= h * 11 / 10) why = "the screen is not larger than the video";
    else if (!PmmlRuntime()) why = "the anime models are not installed (pmml)";
    else {
        pick = pm::PickAnimeSr(w, h, std::max(sw, w), std::max(sh, h), fps, false, 1.0, nullptr, -1);
        if (pick.model < 0) why = "no anime model fits this source in time";
    }
    if (why.empty()) FilterMayHoldPushcore(o, why);
    if (why.empty()) LoadBridge(why);
    if (!why.empty()) {
        if (forced) msg_Warn(o, "PushMPC anime upscaler not used (%s): NIS", why.c_str());
        else msg_Info(o, "PushMPC anime upscaler not used (%s)", why.c_str());
        return nullptr;
    }

    auto *p = new (std::nothrow) pm_pcsr();
    if (!p) return nullptr;
    p->mutex = d->context_mutex != INVALID_HANDLE_VALUE ? d->context_mutex : nullptr;
    p->frameMs = 1000.0 / std::max(1.0, fps);
    pm_pc_cfg &c = p->cfg;
    c.abi = PM_PC_ABI; c.struct_size = sizeof(c);
    c.width = w; c.height = h; c.bits = tenBit ? 10 : 8;
    c.fps_num = int(src->i_frame_rate); c.fps_den = int(src->i_frame_rate_base);
    c.matrix = matrix;
    c.transfer = src->transfer == TRANSFER_FUNC_SMPTE_ST2084 ? 16 : src->transfer == TRANSFER_FUNC_HLG ? 18 : 1;
    c.primaries = src->primaries == COLOR_PRIMARIES_BT2020 || matrix == 9 ? 9 : 1;
    c.range = src->b_color_range_full ? 2 : 1;
    c.smoother = 1;                   // x1: the output gets one picture per picture
    c.fullscreen = var_InheritBool(o, "fullscreen") ? 1 : 0;
    c.sr = 1; c.out_width = ow; c.out_height = oh;
    c.anime_model = PM_PC_ANIME_NONE; // Video Super Resolution until the picture is measured as drawn
    c.held_frames = 0; c.deband = 0; c.held_wait = 0;   // no drawn treatment: the output cannot take pictures late
    c.real_out = 1;
    c.measure = 1;                    // ContentDetect on the pushed textures (the cells only are read back)
    char err[300] = "";
    int k = 1;
    p->pc = B.open(&c, d->d3ddevice, p->mutex, &k, err, sizeof(err));
    pm_pc_info info{};
    info.struct_size = sizeof(info);
    if (!p->pc) why = err[0] ? err : "pushcore refused the stream";
    else if (B.output(p->pc, &info) != 0) why = "pushcore does not say what runs";
    else if (k != 1 || !info.sr || (info.width & 1) || (info.height & 1) || info.width < 16 || info.height < 16)
        why = "pushcore does not run its upscaler at x1 here";
    if (why.empty()) {
        p->outW = info.width; p->outH = info.height;
        p->hdr = c.transfer != 1;
        D3D11_TEXTURE2D_DESC t = {};
        t.Width = p->outW; t.Height = p->outH; t.MipLevels = 1; t.ArraySize = 1;
        t.Format = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
        HRESULT hr = d->d3ddevice->CreateTexture2D(&t, nullptr, p->dst.GetAddressOf());
        v.Format = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        if (SUCCEEDED(hr)) hr = d->d3ddevice->CreateShaderResourceView(p->dst.Get(), &v, p->srvY.GetAddressOf());
        v.Format = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        if (SUCCEEDED(hr)) hr = d->d3ddevice->CreateShaderResourceView(p->dst.Get(), &v, p->srvUV.GetAddressOf());
        if (FAILED(hr)) why = "cannot create the " + std::string(tenBit ? "P010" : "NV12") + " picture for pushcore's output";
    }
    if (!why.empty()) {
        if (forced) msg_Warn(o, "PushMPC anime upscaler not used (%s): NIS", why.c_str());
        else msg_Info(o, "PushMPC anime upscaler not used (%s)", why.c_str());
        PM_PcsrDestroy(p);
        return nullptr;
    }
    p->tex.texture = p->dst.Get(); p->tex.slice = 0;
    p->autoModel = pmk::AnimeModelPlain(Narrow(pm::AnimeSrModel(pick.model).id));
    p->sw.Arm(p->autoModel);
    p->gate.SetDrawn(false, 0);       // held frames: never here
    msg_Info(o, "PushMPC anime upscaler: pushcore to %ux%u (screen %ux%u): Video Super Resolution, the anime upscaler %s "
             "once the picture is measured as drawn animation; %s", p->outW, p->outH, sw, sh, p->autoModel.c_str(), B.describe(p->pc));
    return p;
}

void PM_PcsrDestroy(pm_pcsr *p)
{
    if (!p) return;
    if (p->pc) B.close(p->pc);
    delete p;
}

// the measured class: the anime model on drawn pictures, Video Super Resolution otherwise (Kodi's gate and switch)
static int Follow(vlc_object_t *o, pm_pcsr *p)
{
    pm_pc_info info{};
    info.struct_size = sizeof(info);
    if (B.output(p->pc, &info) != 0) return VLC_SUCCESS;
    const double now = double(mdate()) / CLOCK_FREQ;
    const pmk::PushcoreContentGate::Ask a = p->gate.Update(info.measured_class, info.measured_cg != 0, now, p->sw, true, p->sw.Running());
    if (!a.update) return VLC_SUCCESS;
    pm_pc_cfg c = p->cfg;
    const int model = ModelIndex(a.model);
    c.anime_model = model >= 0 ? model : PM_PC_ANIME_NONE;
    c.content_class = a.cls; c.content_cg = a.cg ? 1 : 0;
    char err[300] = "";
    const int r = B.update(p->pc, &c, err, sizeof(err));
    bool ok = r >= 0;
    std::string why = ok ? "" : (err[0] ? err : "refused");
    if (ok && B.output(p->pc, &info) == 0 && model >= 0 && info.anime_model != model) {
        ok = false; why = "pushcore cannot run that anime upscaler on this GPU";
    }
    p->gate.Done(a, ok, now, p->sw);
    if (r == 0) p->grace = 12;        // reloaded: the pictures in flight were dropped
    if (a.switched)
        msg_Info(o, "PushMPC anime upscaler: %s (measured class %d%s)%s%s", a.model.empty() ? "Video Super Resolution" : a.model.c_str(),
                 a.cls, a.cg ? ", CG" : "", ok ? "" : ": not switched, ", why.c_str());
    if (r <= -2) {
        msg_Warn(o, "PushMPC anime upscaler: %s: NIS from here on", why.c_str());
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

int PM_PcsrFeed(vlc_object_t *o, pm_pcsr *p, d3d11_device_t *d, picture_sys_t *sys, mtime_t date)
{
    if (!p->pc) return VLC_EGENERIC;
    // pictures of another device (VLC's shared-texture path) or without a texture: NIS
    if (!sys || !sys->texture[KNOWN_DXGI_INDEX] || sys->context != d->d3dcontext) { p->got = false; p->want = INT64_MIN; return VLC_SUCCESS; }
    if (p->haveLast && date == p->lastDate) return VLC_SUCCESS;   // the same picture drawn again: pushcore's is kept
    const bool disc = !p->haveLast || date < p->lastDate || date - p->lastDate > CLOCK_FREQ / 2;
    p->want = int64_t(date) * 10;     // 100 ns
    p->got = false; p->pending = false;
    char err[300] = "";
    if (B.push(p->pc, sys->texture[KNOWN_DXGI_INDEX], sys->slice_index, p->want, disc ? 1 : 0, err, sizeof(err)) != 0) {
        msg_Warn(o, "PushMPC anime upscaler: pushcore refused a picture (%s): NIS from here on", err);
        return VLC_EGENERIC;
    }
    p->haveLast = true; p->lastDate = date;
    // this picture's, waited for at most about a frame (the bridge sleeps outside VLC's lock); then what is finished
    const ULONGLONG deadline = GetTickCount64() + ULONGLONG(std::clamp(1.5 * p->frameMs, 12.0, 50.0));
    for (;;) {
        if (B.pollTex(p->pc, 2, Dest, Done, p, err, sizeof(err)) < 0) {
            msg_Warn(o, "PushMPC anime upscaler: pushcore failed (%s): NIS from here on", err);
            return VLC_EGENERIC;
        }
        if (p->got || GetTickCount64() >= deadline) break;
    }
    for (int guard = 0; guard < 64; ++guard)
        if (B.pollTex(p->pc, 0, Dest, Done, p, err, sizeof(err)) <= 0) break;
    p->pending = false;
    // late pictures: NIS draws them; when most are late, pushcore does not keep up here
    if (p->grace > 0) p->grace--;
    else {
        p->seen++;
        if (!p->got) p->late++;
        if (p->seen >= 48) {
            if (p->late * 2 > p->seen) {
                msg_Warn(o, "PushMPC anime upscaler: pushcore delivered %d of the last %d pictures too late: NIS from here on",
                         p->late, p->seen);
                return VLC_EGENERIC;
            }
            p->seen = p->late = 0;
        }
    }
    return Follow(o, p);
}

int PM_PcsrDraw(vlc_object_t *o, pm_pcsr *p, pm_nis *nis, d3d11_device_t *d, const vout_display_place_t *place)
{
    if (!p->pc || !p->got || !nis) return VLC_EGENERIC;
    video_format_t f;
    memset(&f, 0, sizeof(f));
    f.i_width = f.i_visible_width = p->outW;
    f.i_height = f.i_visible_height = p->outH;
    f.space = p->hdr ? COLOR_SPACE_BT2020 : COLOR_SPACE_BT709;
    f.b_color_range_full = false;
    ID3D11ShaderResourceView *planes[2] = { p->srvY.Get(), p->srvUV.Get() };
    return PM_NisConvert(o, nis, d, planes, 2, &f, place);
}
