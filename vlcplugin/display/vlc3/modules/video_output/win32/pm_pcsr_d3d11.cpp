// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC: pushcore's upscaler as the output upscaler of the Direct3D 11 video output fork (see pm_pcsr_d3d11.h). The
// decisions are in pm_pcsr_core.h (VLC-free); this is VLC's side: the checks, the options, the log.
#include <vlc_common.h>
#include <vlc_configuration.h>
#include <vlc_modules.h>
#include <vlc_variables.h>
#include "pm_pcsr_d3d11.h"
#undef min                      // windows.h's (the build force-includes winsock2.h): std::min / std::max below
#undef max
#include <new>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pm_pushcore.h"        // vlcplugin/pushcore_bridge: the bridge's C interface (no link dependency)
#include "pm_pcsr_core.h"       // vlcplugin/pushcore_bridge: the output upscaler's logic (sharing, Auto's switch, draw)
#include "PushMpcAnimeSr.h"     // filter/src: the anime models and Auto's pick (PickAnimeSr)

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------------------------------- the bridge
struct Bridge {
    bool tried = false;
    HMODULE dll = nullptr;
    uint32_t abi = 2;
    pmpc::BridgeFns fns;
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
            B.fns = pmpc::BridgeFns::From(B.dll);
            const auto abiFn = reinterpret_cast<pm_pc_abi_fn>(reinterpret_cast<void *>(GetProcAddress(B.dll, "pm_pc_abi")));
            B.abi = abiFn && B.fns.update && B.fns.output ? std::min<uint32_t>(abiFn(), PM_PC_ABI) : 2u;
        }
    }
    bool ok = false;
    if (!B.dll) why = "pm_pushcore.dll is not next to the plugin";
    else if (!B.fns.open || !B.fns.push || !B.fns.pollTex || !B.fns.close || !B.fns.describe) why = "pm_pushcore.dll has no zero-copy path";
    else if (B.abi < 3) why = "pm_pushcore.dll is older than ABI 3 (no upscaler, no settings update)";
    else ok = true;
    ReleaseSRWLockExclusive(&lockB);
    return ok;
}

// ---------------------------------------------------------------------------------------------------- the checks
// An OLDER bridge only (no pm_pc_busy / pm_pc_decide): one pushcore per process, the last open takes it, and that filter
// opened pushcore even at x1; so the output runs pushcore only when PushMPC's frame generation filter cannot hold it
// (absent, or on a portable engine). With the current bridge the output and the filter share it (pm_pcsr_core.h).
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

} // namespace

struct pm_pcsr {
    pmpc::PcsrCore core;
    bool haveLast = false;
    mtime_t lastDate = 0;
};

pm_pcsr *PM_PcsrCreate(vlc_object_t *o, d3d11_device_t *d, const video_format_t *src, vlc_fourcc_t chroma,
                       unsigned dispW, unsigned dispH, HWND window, int mode, int driverSr)
{
    std::string why;
    const bool anime = mode == PM_PCSR_ANIME;
    const unsigned w = src->i_visible_width, h = src->i_visible_height;
    const bool tenBit = chroma == VLC_CODEC_D3D11_OPAQUE_10B;
    const bool hdr = src->transfer == TRANSFER_FUNC_SMPTE_ST2084 || src->transfer == TRANSFER_FUNC_HLG;
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
    else if (!anime && !(why = pmpc::PcsrAutoRefusal(driverSr != 0, src->transfer == TRANSFER_FUNC_SMPTE_ST2084 ? 16 : hdr ? 18 : 1)).empty()) {}
    else if (ow <= w * 11 / 10 && oh <= h * 11 / 10) why = "the screen is not larger than the video";
    else if (!PmmlRuntime()) why = "the anime models are not installed (pmml)";
    else {
        pick = pm::PickAnimeSr(w, h, std::max(sw, w), std::max(sh, h), fps, false, 1.0, nullptr, -1);
        if (pick.model < 0 && !anime) why = "no anime model fits this source in time";
    }
    if (why.empty()) LoadBridge(why);
    // an older bridge cannot share pushcore with the filter: the older rule (the filter absent or on a portable engine)
    if (why.empty() && !B.fns.Sharing()) FilterMayHoldPushcore(o, why);
    if (!why.empty()) {
        if (anime) msg_Warn(o, "PushMPC anime upscaler not used (%s)", why.c_str());
        else msg_Info(o, "PushMPC anime upscaler not used (%s)", why.c_str());
        return nullptr;
    }

    auto *p = new (std::nothrow) pm_pcsr();
    if (!p) return nullptr;
    pmpc::PcsrCore::Setup s;
    s.dev = d->d3ddevice;
    s.mutex = d->context_mutex != INVALID_HANDLE_VALUE ? d->context_mutex : nullptr;
    s.mode = anime ? pmpc::PcsrMode::Anime : pmpc::PcsrMode::Auto;
    s.autoModel = pick.model >= 0 ? pmk::AnimeModelPlain(Narrow(pm::AnimeSrModel(pick.model).id)) : std::string();
    s.frameMs = 1000.0 / std::max(1.0, fps);
    s.tenBit = tenBit; s.hdr = hdr;
    pm_pc_cfg &c = s.cfg;
    c.abi = PM_PC_ABI; c.struct_size = sizeof(c);
    c.width = w; c.height = h; c.bits = tenBit ? 10 : 8;
    c.fps_num = int(src->i_frame_rate); c.fps_den = int(src->i_frame_rate_base);
    c.matrix = matrix;
    c.transfer = src->transfer == TRANSFER_FUNC_SMPTE_ST2084 ? 16 : src->transfer == TRANSFER_FUNC_HLG ? 18 : 1;
    c.primaries = src->primaries == COLOR_PRIMARIES_BT2020 || matrix == 9 ? 9 : 1;
    c.range = src->b_color_range_full ? 2 : 1;
    c.fullscreen = var_InheritBool(o, "fullscreen") ? 1 : 0;
    c.sr = 1; c.out_width = ow; c.out_height = oh;
    c.anime_model = PM_PC_ANIME_NONE;
    vlc_object_t *lo = o;   // the output outlives this (destroyed with its scaler)
    auto log = [lo](bool warn, const std::string &line) {
        if (warn) msg_Warn(lo, "PushMPC anime upscaler: %s", line.c_str());
        else msg_Info(lo, "PushMPC anime upscaler: %s", line.c_str());
    };
    if (!p->core.Init(B.fns, s, log, why)) {
        if (anime) msg_Warn(o, "PushMPC anime upscaler not used (%s)", why.c_str());
        else msg_Info(o, "PushMPC anime upscaler not used (%s)", why.c_str());
        delete p;
        return nullptr;
    }
    if (anime)
        msg_Info(o, "PushMPC anime upscaler: pushcore's upscaler to %ux%u (screen %ux%u) for every picture: Video Super "
                 "Resolution, the anime upscaler %s once the picture is measured as drawn; NIS while pushcore is busy",
                 ow, oh, sw, sh, s.autoModel.empty() ? "(none fits)" : s.autoModel.c_str());
    else
        msg_Info(o, "PushMPC anime upscaler: Auto: the scaler for every picture; pushcore's anime upscaler %s to %ux%u "
                 "(screen %ux%u) while the picture is measured as drawn animation", s.autoModel.c_str(), ow, oh, sw, sh);
    return p;
}

void PM_PcsrDestroy(pm_pcsr *p)
{
    delete p;
}

int PM_PcsrFeed(vlc_object_t *o, pm_pcsr *p, d3d11_device_t *d, picture_sys_t *sys, mtime_t date)
{
    // pictures of another device (VLC's shared-texture path) or without a texture: the scaler
    if (!sys || !sys->texture[KNOWN_DXGI_INDEX] || sys->context != d->d3dcontext) return VLC_SUCCESS;
    if (p->haveLast && date == p->lastDate) return VLC_SUCCESS;   // the same picture drawn again: pushcore's is kept
    const bool disc = !p->haveLast || date < p->lastDate || date - p->lastDate > CLOCK_FREQ / 2;
    p->haveLast = true; p->lastDate = date;
    if (!p->core.Feed(sys->texture[KNOWN_DXGI_INDEX], sys->slice_index, int64_t(date) * 10, disc, double(mdate()) / CLOCK_FREQ)) {
        const pmpc::PcsrCore::Stats &st = p->core.Stat();
        msg_Warn(o, "PushMPC anime upscaler: pushcore does not keep up here: NIS from here on (%d pictures, %.1f ms each on "
                 "average)", st.feeds, st.feeds ? st.feedMsSum / st.feeds : 0.0);
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

int PM_PcsrDraw(vlc_object_t *o, pm_pcsr *p, d3d11_device_t *d, ID3D11Texture2D *target, const vout_display_place_t *place)
{
    (void)o;
    if (!place || place->x < 0 || place->y < 0 || !place->width || !place->height) return VLC_EGENERIC;
    return p->core.Draw(d->d3dcontext, target, unsigned(place->x), unsigned(place->y), place->width, place->height)
           ? VLC_SUCCESS : VLC_EGENERIC;
}
