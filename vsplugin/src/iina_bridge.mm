// pushmpc-iina: PushMPC frame generation and super resolution INSIDE IINA, without modifying IINA.app
// An mpv C plugin (IINA 1.5.1's libmpv 0.42 has C plugins on, and IINA's
// entitlements disable library validation), loaded by the PushMPC IINA plugin with `load-script`. It patches libmpv's
// imports (GOT slots) of a handful of PUBLIC libavfilter functions, so mpv's own lavfi bridge (filters/f_lavfi.c)
// talks to us for one marked graph:
//   vf=@pushmpc:lavfi=[null@pushmpc]      (a no-op graph; "@pushmpc" in the graph string marks it)
//   avfilter_graph_parse2         remembers marked graphs; avfilter_graph_alloc_filter / _create_filter map filter
//                                 contexts (mpv's buffersrc / buffersink) to their graph; avfilter_graph_free forgets
//   av_buffersrc_add_frame        marked: the frame goes to our engine instead (NULL = EOF)
//   av_buffersrc_get_nb_failed_requests   marked: "wants input" while our output queue is empty
//   av_buffersink_get_frame_flags marked: our real and generated frames, in order; EAGAIN / EOF
//   av_buffersink_get_frame_rate  marked: the source rate x k
// Every other graph, and a marked one whose engine cannot run, passes straight through to libavfilter.
// Engines, best first (the same order as Plex for Mac): the apple backend on CVPixelBuffers (VideoToolbox-decoded
// frames, zero copy: AV_PIX_FMT_VIDEOTOOLBOX in and out, our own AVHWFramesContext when super resolution changes the
// size); else PushMPC's engine module (libpushmpc_engine.dylib: apple on software frames, then portable-vulkan on
// MoltenVK, then portable-cpu; VideoToolbox frames are downloaded for it) from the Mac engine bundle pushmpc-setup
// unpacks (~/.local/share/pushmpc/plex-mac, or PUSHMPC_ENGINE); else the built-in portable engine (CPU) for software
// yuv420p / yuv420p10. The log names the engine that ran ("engine: <name>").
// ABI exposure: only public FFmpeg API (function names, AVFrame / AVHWFramesContext layout per major version); the
// majors are checked against the headers this was built with before anything is patched. arm64e (pointer
// authentication on GOT slots) is detected and refused.
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <algorithm>
#include <atomic>
#include "pushmpc/content_detect.h"
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
#include "pushmpc/content_model.h"   // the image model (pmml: CoreML, else CPU)
#endif
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <mpv/client.h>

// AVFrame.duration is FFmpeg 6 and later (libavutil 58); older majors (Homebrew's ffmpeg@4, libavutil 56) carry the frame's
// duration in pkt_duration. The bridge is built once per FFmpeg major (pushmpc-iina-av<libavutil major>.so)
#if LIBAVUTIL_VERSION_MAJOR >= 58
#define PM_DURATION(fr) ((fr)->duration)
#else
#define PM_DURATION(fr) ((fr)->pkt_duration)
#endif
// the decoder's interlace flag (a prior only): AVFrame.flags from libavutil 58.7, interlaced_frame before
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
#define PM_INTERLACED(fr) (((fr)->flags & AV_FRAME_FLAG_INTERLACED) != 0)
#else
#define PM_INTERLACED(fr) ((fr)->interlaced_frame != 0)
#endif
}
#include "apple_engine.h"
#include "backend.h"        // pmvs::kPortableMaxK: the built-in portable engine's ceiling
#include "pmeng.h"
#include "portable_engine.h"

using namespace pmvs;

namespace {

// ------------------------------------------------------------------------------------------------ log
FILE* g_log = nullptr;
void Log(const char* f, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* f, ...)
{
    static std::mutex lk;
    std::lock_guard<std::mutex> g(lk);
    if (!g_log) {
        const char* home = getenv("HOME");
        std::string dir = std::string(home ? home : "/tmp") + "/Library/Logs/PushMPC";
        [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:dir.c_str()] withIntermediateDirectories:YES attributes:nil error:nil];
        g_log = fopen((dir + "/iina-bridge.log").c_str(), "a");
        if (!g_log) g_log = stderr;
    }
    va_list ap; va_start(ap, f);
    fprintf(g_log, "pushmpc-iina: "); vfprintf(g_log, f, ap); fprintf(g_log, "\n"); fflush(g_log);
    va_end(ap);
}

// ------------------------------------------------------------------------------------------------ FFmpeg through dlsym
// (the plugin links no FFmpeg: it uses the copy IINA loaded)
struct Av {
    unsigned (*avutil_version)() = nullptr;
    unsigned (*avfilter_version)() = nullptr;
    AVFrame* (*frame_alloc)() = nullptr;
    void (*frame_free)(AVFrame**) = nullptr;
    int (*frame_copy_props)(AVFrame*, const AVFrame*) = nullptr;
    int (*frame_get_buffer)(AVFrame*, int) = nullptr;
    void (*frame_move_ref)(AVFrame*, AVFrame*) = nullptr;
    void (*frame_unref)(AVFrame*) = nullptr;
    AVBufferRef* (*buffer_create)(uint8_t*, size_t, void (*)(void*, uint8_t*), void*, int) = nullptr;
    AVBufferRef* (*buffer_ref)(const AVBufferRef*) = nullptr;
    void (*buffer_unref)(AVBufferRef**) = nullptr;
    AVBufferRef* (*hwframe_ctx_alloc)(AVBufferRef*) = nullptr;
    int (*hwframe_ctx_init)(AVBufferRef*) = nullptr;
    int (*hwdevice_ctx_create)(AVBufferRef**, enum AVHWDeviceType, const char*, AVDictionary*, int) = nullptr;
    int (*hwframe_transfer_data)(AVFrame*, const AVFrame*, int) = nullptr;
    bool Load()
    {
        auto sym = [](const char* n) { return dlsym(RTLD_DEFAULT, n); };
#define PM_SYM(field, name) field = reinterpret_cast<decltype(field)>(sym(name)); if (!field) { Log("FFmpeg symbol %s not found", name); return false; }
        PM_SYM(avutil_version, "avutil_version") PM_SYM(avfilter_version, "avfilter_version") PM_SYM(frame_alloc, "av_frame_alloc")
        PM_SYM(frame_free, "av_frame_free") PM_SYM(frame_copy_props, "av_frame_copy_props") PM_SYM(frame_get_buffer, "av_frame_get_buffer")
        PM_SYM(frame_move_ref, "av_frame_move_ref") PM_SYM(frame_unref, "av_frame_unref") PM_SYM(buffer_create, "av_buffer_create")
        PM_SYM(buffer_ref, "av_buffer_ref") PM_SYM(buffer_unref, "av_buffer_unref") PM_SYM(hwframe_ctx_alloc, "av_hwframe_ctx_alloc")
        PM_SYM(hwframe_ctx_init, "av_hwframe_ctx_init") PM_SYM(hwdevice_ctx_create, "av_hwdevice_ctx_create")
        PM_SYM(hwframe_transfer_data, "av_hwframe_transfer_data")
#undef PM_SYM
        return true;
    }
} g_av;

// the originals of the hooked functions
int (*o_parse2)(AVFilterGraph*, const char*, AVFilterInOut**, AVFilterInOut**) = nullptr;
AVFilterContext* (*o_alloc_filter)(AVFilterGraph*, const AVFilter*, const char*) = nullptr;
int (*o_create_filter)(AVFilterContext**, const AVFilter*, const char*, const char*, void*, AVFilterGraph*) = nullptr;
void (*o_graph_free)(AVFilterGraph**) = nullptr;
int (*o_add_frame)(AVFilterContext*, AVFrame*) = nullptr;
unsigned (*o_failed_requests)(AVFilterContext*) = nullptr;
int (*o_get_frame_flags)(AVFilterContext*, AVFrame*, int) = nullptr;
AVRational (*o_get_frame_rate)(const AVFilterContext*) = nullptr;

// ------------------------------------------------------------------------------------------------ one marked graph
mpv_handle* g_mpv = nullptr;
// ContentDetect's class of the picture (measured in Feed on the frames; published to the plugin as
// user-data/pushmpc/content by the event thread: the filter thread never calls into mpv)
std::atomic<int> g_contentClass{0};
pushmpc::content::ContentDetector g_content;   // filter thread only
std::vector<float> g_vtThumb;                   // the image model's thumbnail of a VideoToolbox frame (filter thread)

void ReleasePixelBuffer(void* opaque, uint8_t*) { CVPixelBufferRelease(static_cast<CVPixelBufferRef>(opaque)); }

AVPixelFormat SwFormatOf(OSType f)
{
    switch (f) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange: case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange: return AV_PIX_FMT_NV12;
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange: case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange: return AV_PIX_FMT_P010;
    default: return AV_PIX_FMT_NONE;
    }
}

apple::Colour ColourOf(const AVFrame* f)
{
    apple::Colour c;
    c.matrix = f->colorspace != AVCOL_SPC_UNSPECIFIED ? int(f->colorspace) : 1;
    c.transfer = f->color_trc != AVCOL_TRC_UNSPECIFIED ? int(f->color_trc) : 1;
    c.primaries = f->color_primaries != AVCOL_PRI_UNSPECIFIED ? int(f->color_primaries) : 1;
    c.fullRange = f->color_range == AVCOL_RANGE_JPEG;
    return c;
}

struct Bridge {
    enum class Mode { Unset, Apple, Module, Portable, Pass } mode = Mode::Unset;
    std::unique_ptr<apple::Engine> apple;
    std::unique_ptr<portable::Engine> port;
    const pmeng_api* api = nullptr;         // the engine module (Mode::Module)
    void* eng = nullptr;
    uint32_t engFmt = PMENG_FMT_NONE;
    bool engDownload = false;               // VideoToolbox frames are downloaded for it
    std::deque<AVFrame*> out;
    bool eofIn = false;
    int k = 1;
    int wantK = 0;                          // what DecideK asked for at the engine's set-up (before the engine's own ceiling)
    double decidedFps = 0;                  // the source rate it was decided with
    bool redecided = false;                 // set up again once for the measured rate (never more per graph)
    int64_t lastPts = AV_NOPTS_VALUE, prevPts = AV_NOPTS_VALUE;
    AVFrame* prevFrame = nullptr;           // props template of the previous source frame
    AVBufferRef* outFrames = nullptr;       // our AVHWFramesContext (super resolution changes the size)
    AVBufferRef* device = nullptr;
    uint64_t in = 0, produced = 0, emitted = 0;
    std::string why;
    ~Bridge()
    {
        for (AVFrame* f : out) g_av.frame_free(&f);
        if (prevFrame) g_av.frame_free(&prevFrame);
        if (outFrames) g_av.buffer_unref(&outFrames);
        if (device) g_av.buffer_unref(&device);
        if (api && eng) { char st[512] = {}; api->stats(eng, st, sizeof(st)); Log("engine stats: %s", st); api->close(eng); }
    }
};
std::mutex g_lock;
std::map<AVFilterGraph*, std::shared_ptr<Bridge>> g_graphs;    // marked graphs
std::map<AVFilterContext*, AVFilterGraph*> g_ctx;              // their filter contexts

std::shared_ptr<Bridge> BridgeOf(const AVFilterContext* ctx)
{
    auto c = g_ctx.find(const_cast<AVFilterContext*>(ctx));
    if (c == g_ctx.end()) return nullptr;
    auto g = g_graphs.find(c->second);
    return g == g_graphs.end() ? nullptr : g->second;
}

// display-fps and container-fps, observed by the plugin's own thread (calling into mpv from the filter, which runs on
// mpv's core thread, could deadlock)
std::atomic<double> g_displayFps{ 0 }, g_containerFps{ 0 };
double MpvDouble(const char* name) { return std::string(name) == "display-fps" ? g_displayFps.load() : g_containerFps.load(); }

// The source rate, measured on the bridge's own input: the median frame step of the decoded timestamps
// (pushmpc::content::IntervalMeter, as Kodi and the VapourSynth filter measure it), not the container's tag ("29.97" on
// a 24p body). mpv's estimated-vf-fps, which pushmpc.lua reads, cannot serve here: it is the rate at the filter chain's
// OUTPUT (the bridge's own frames, source x k: the vsplugin CI and iina_bridge_host.c read it as such), so taking it
// as the source rate would divide k back into itself after a seek. The meter runs on mpv's core thread (Feed) and lives
// across graphs (mpv rebuilds the graph on a seek); the plugin thread asks for a reset at each new file (g_rateReset,
// MPV_EVENT_START_FILE). container-fps stays the prior until 8 steps are measured.
pushmpc::content::IntervalMeter g_rate;            // mpv's core thread only
std::atomic<double> g_measuredFps{ 0 };
std::atomic<bool> g_rateReset{ false };            // a new file: the rate and the interlace verdict start over
// Interlaced input, measured in the picture as the DirectShow filter, Kodi and the VapourSynth routes do
// (content_detect.h CombMeter on the native luma: 24 moving frames, half or more combed; 240 frames without enough motion
// count as progressive); the decoder's interlace flag is only logged. Interlaced: the graph passes through from that
// frame on (the frames already generated go out first), and the file's later graphs (a seek) start passing through.
// Per file, on mpv's core thread (the verdict is read there too).
pushmpc::content::CombMeter g_comb;
int g_ilSeen = 0, g_ilFlagged = 0;
bool g_ilDone = false;
std::atomic<int> g_ilVerdict{ 0 };                 // 0 measuring, 1 progressive, 2 interlaced
std::string g_ilWhy;
double SourceFps()
{
    const double m = g_measuredFps.load();
    return m > 1.0 ? m : MpvDouble("container-fps");
}

// multiplier ceilings per engine: the bridge asks for up to the highest, then each engine takes what it can do (apple:
// apple::kAppleMaxK, 16: VideoToolbox's low-latency x = 4 grid or frame-rate conversion, its self-test decides; the
// engine module reports its own; the built-in portable engine: pmvs::kPortableMaxK in backend.h, 16)
constexpr int kBridgeAppleMaxK = apple::kAppleMaxK, kBridgePortableMaxK = pmvs::kPortableMaxK, kBridgeMaxK = 16;
static_assert(kBridgeMaxK >= kBridgePortableMaxK && kBridgeMaxK >= kBridgeAppleMaxK, "the bridge asks for the highest engine ceiling");

int DecideK(double srcFps)
{
    if (const char* e = getenv("PUSHMPC_IINA_K"); e && *e) return std::clamp(atoi(e), 1, kBridgeMaxK);
    const double disp = MpvDouble("display-fps");
    if (srcFps <= 0) srcFps = SourceFps();
    if (srcFps <= 0) srcFps = 23.976;
    if (disp <= 0) return 2;
    return std::clamp(int(std::floor(disp / srcFps + 0.02)), 1, kBridgeMaxK);
}

// an output AVFrame from a CVPixelBuffer (retained by the frame)
AVFrame* WrapPixelBuffer(Bridge& b, const AVFrame* tmpl, CVPixelBufferRef pb, int64_t pts, int64_t duration)
{
    AVFrame* f = g_av.frame_alloc();
    if (!f) { CVPixelBufferRelease(pb); return nullptr; }
    g_av.frame_copy_props(f, tmpl);
    f->format = AV_PIX_FMT_VIDEOTOOLBOX;
    f->width = int(CVPixelBufferGetWidth(pb)); f->height = int(CVPixelBufferGetHeight(pb));
    f->data[3] = reinterpret_cast<uint8_t*>(pb);
    f->buf[0] = g_av.buffer_create(reinterpret_cast<uint8_t*>(pb), 0, ReleasePixelBuffer, pb, 0);
    f->pts = pts;
    PM_DURATION(f) = duration;
    const bool sameSize = tmpl->hw_frames_ctx && f->width == tmpl->width && f->height == tmpl->height;
    if (sameSize) f->hw_frames_ctx = g_av.buffer_ref(tmpl->hw_frames_ctx);
    else {
        if (!b.outFrames) {
            AVBufferRef* dev = tmpl->hw_frames_ctx ? reinterpret_cast<AVHWFramesContext*>(tmpl->hw_frames_ctx->data)->device_ref : nullptr;
            if (!dev && !b.device && g_av.hwdevice_ctx_create(&b.device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) < 0) { Log("no VideoToolbox device"); }
            if (!dev) dev = b.device;
            if (dev) {
                AVBufferRef* fr = g_av.hwframe_ctx_alloc(dev);
                auto* hc = reinterpret_cast<AVHWFramesContext*>(fr->data);
                hc->format = AV_PIX_FMT_VIDEOTOOLBOX;
                hc->sw_format = SwFormatOf(CVPixelBufferGetPixelFormatType(pb));
                hc->width = f->width; hc->height = f->height;
                if (g_av.hwframe_ctx_init(fr) < 0) { Log("av_hwframe_ctx_init failed"); g_av.buffer_unref(&fr); }
                b.outFrames = fr;
            }
        }
        if (b.outFrames) f->hw_frames_ctx = g_av.buffer_ref(b.outFrames);
    }
    return f;
}

bool ConfigureApple(Bridge& b, const AVFrame* f, CVPixelBufferRef pb)
{
    std::string err, info;
    apple::Config c;
    c.width = f->width; c.height = f->height;
    const OSType fmt = CVPixelBufferGetPixelFormatType(pb);
    c.bits = SwFormatOf(fmt) == AV_PIX_FMT_P010 ? 10 : 8;
    c.srcFps = SourceFps();
    c.k = std::min(b.k, kBridgeAppleMaxK);
    c.colour = ColourOf(f);
    if (const char* sr = getenv("PUSHMPC_IINA_SR")) c.sr = sr;
    if (const char* ct = getenv("PUSHMPC_IINA_CONTENT")) c.anime = std::string(ct) == "anime";
    b.apple = std::make_unique<apple::Engine>();
    b.apple->log = [](const std::string& m) { Log("%s", m.c_str()); };
    if (!b.apple->Configure(c, err, info)) { b.why = err; b.apple.reset(); return false; }
    b.k = std::max(1, b.apple->K());   // what VideoToolbox runs here
    Log("apple engine: %s", info.c_str());
    return true;
}

// software yuv420p / yuv420p10 through the portable engine (CPU)
bool ConfigurePortable(Bridge& b, const AVFrame* f)
{
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUV420P10) { b.why += "; portable: format " + std::to_string(f->format) + " not supported"; return false; }
    b.k = std::min(b.k, kBridgePortableMaxK);
    if (b.k < 2) return false;
    portable::Config c; c.width = f->width; c.height = f->height; c.bits = f->format == AV_PIX_FMT_YUV420P10 ? 10 : 8;
    b.port = std::make_unique<portable::Engine>();
    std::string err;
    if (!b.port->Configure(c, err)) { b.why += "; portable: " + err; b.port.reset(); return false; }
    Log("portable engine (CPU) %dx%d %d-bit, x%d", c.width, c.height, c.bits, b.k);
    return true;
}

// ------------------------------------------------------------------------------------------------ the engine module
// libpushmpc_engine.dylib (pmeng.h): PUSHMPC_ENGINE, next to this plugin, or the Mac engine bundle pushmpc-setup unpacked
const pmeng_api* EngineModule()
{
    static const pmeng_api* api = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        std::vector<std::string> paths;
        if (const char* e = getenv("PUSHMPC_ENGINE"); e && *e) paths.push_back(e);
        Dl_info di;
        if (dladdr(reinterpret_cast<void*>(&EngineModule), &di) && di.dli_fname) {
            std::string d = di.dli_fname; d = d.substr(0, d.rfind('/') + 1);
            paths.push_back(d + "libpushmpc_engine.dylib");
        }
        const char* xdg = getenv("XDG_DATA_HOME"); const char* home = getenv("HOME");
        const std::string data = xdg && *xdg ? std::string(xdg) : std::string(home ? home : "") + "/.local/share";
        paths.push_back(data + "/pushmpc/plex-mac/libpushmpc_engine.dylib");
        for (const std::string& p : paths) {
            void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h) continue;
            auto get = reinterpret_cast<pmeng_get_api_fn>(dlsym(h, "pmeng_get_api"));
            if (get && (api = get(PMENG_ABI_VERSION))) { Log("engine module: %s (%s)", p.c_str(), api->version()); return; }
        }
        Log("engine module: none found (pushmpc-setup install unpacks it); the built-in engines only");
    });
    return api;
}

uint32_t PmFormat(int f)
{
    switch (f) {
    case AV_PIX_FMT_YUV420P: return PMENG_FMT_YUV420P;
    case AV_PIX_FMT_YUV420P10: return PMENG_FMT_YUV420P10;
    case AV_PIX_FMT_NV12: return PMENG_FMT_NV12;
    case AV_PIX_FMT_P010: return PMENG_FMT_P010;
    default: return PMENG_FMT_NONE;
    }
}

// software frames (or VideoToolbox frames, downloaded) through the engine module: Auto picks apple, then
// portable-vulkan (MoltenVK), then portable-cpu
bool ConfigureModule(Bridge& b, const AVFrame* f)
{
    const pmeng_api* api = EngineModule();
    if (!api) { b.why += "; no engine module"; return false; }
    int sw = f->format;
    const bool vt = f->format == AV_PIX_FMT_VIDEOTOOLBOX;
    if (vt) sw = f->data[3] ? SwFormatOf(CVPixelBufferGetPixelFormatType(reinterpret_cast<CVPixelBufferRef>(f->data[3]))) : AV_PIX_FMT_NONE;
    const uint32_t fmt = PmFormat(sw);
    if (fmt == PMENG_FMT_NONE) { b.why += "; engine module: format " + std::to_string(sw) + " not supported"; return false; }
    pmeng_device dev{}; dev.struct_size = sizeof(dev); dev.mem = PMENG_MEM_CPU;
    pmeng_stream st{}; st.struct_size = sizeof(st); st.width = uint32_t(f->width); st.height = uint32_t(f->height); st.fmt = fmt;
    const double fps = SourceFps();
    if (fps > 0) { st.fps_num = int32_t(std::lround(fps * 1000)); st.fps_den = 1000; }
    st.range = f->color_range; st.matrix = f->colorspace; st.primaries = f->color_primaries; st.transfer = f->color_trc;
    pmeng_output out{}; out.struct_size = sizeof(out);
    std::string opts = "smoother=" + std::to_string(b.k) + "\n";
    if (const char* be = getenv("PUSHMPC_IINA_BACKEND"); be && *be) opts += std::string("backend=") + be + "\n";
    char err[512] = {};
    void* h = api->open(&dev, &st, opts.c_str(), &out, err, sizeof(err));
    if (!h) { b.why += std::string("; engine module: ") + err; return false; }
    if (out.mem != PMENG_MEM_CPU || out.fmt != fmt || out.width != st.width || out.height != st.height) {
        api->close(h); b.why += "; engine module: unexpected output"; return false;
    }
    b.api = api; b.eng = h; b.engFmt = fmt; b.engDownload = vt; b.k = std::max(1, int(out.multiplier));
    Log("engine: %s: %s", out.engine, out.description);
    return true;
}

pmeng_image ImageOf(const AVFrame* f, uint32_t fmt)
{
    pmeng_image im{};
    im.mem = PMENG_MEM_CPU; im.fmt = fmt; im.width = uint32_t(f->width); im.height = uint32_t(f->height);
    im.range = f->color_range; im.matrix = f->colorspace; im.primaries = f->color_primaries; im.transfer = f->color_trc;
    for (int i = 0; i < 4; ++i) { im.data[i] = f->data[i]; im.linesize[i] = f->linesize[i]; }
    return im;
}

AVFrame* NewSoftFrame(const AVFrame* tmpl, int64_t pts, int64_t duration)
{
    AVFrame* o = g_av.frame_alloc();
    if (!o) return nullptr;
    g_av.frame_copy_props(o, tmpl);
    o->format = tmpl->format; o->width = tmpl->width; o->height = tmpl->height;
    if (g_av.frame_get_buffer(o, 0) < 0) { g_av.frame_free(&o); return nullptr; }
    o->pts = pts; PM_DURATION(o) = duration;
    return o;
}

// one source frame through the bridge's engine; false: the engine cannot take it (the graph then passes through)
bool Feed(Bridge& b, AVFrame* f)
{
    ++b.in;
    {   // the source rate from the input timestamps (microseconds: mpv's lavfi time base)
        if (g_rateReset.exchange(false)) {
            g_rate.Reset(); g_measuredFps.store(0);
            g_comb.Reset(); g_ilSeen = g_ilFlagged = 0; g_ilDone = false; g_ilVerdict.store(0); g_ilWhy.clear();
        }
        const bool disc = f->pts == AV_NOPTS_VALUE || b.lastPts == AV_NOPTS_VALUE || f->pts <= b.lastPts;
        if (f->pts != AV_NOPTS_VALUE && g_rate.Push(double(f->pts) / 1e6, disc)) {
            g_measuredFps.store(g_rate.fps());
            Log("source rate measured from the frame timestamps: %.3f fps (container-fps says %.3f)", g_rate.fps(), MpvDouble("container-fps"));
        }
        // set up before the measurement with another rate (the container's tag) whose multiplier differs: once more, with
        // the measured one (a new pair starts; the frames already out stay)
        const double m = g_measuredFps.load();
        if (!b.redecided && b.mode != Bridge::Mode::Unset && b.mode != Bridge::Mode::Pass && m > 1.0 && b.decidedFps > 0 &&
            std::fabs(m - b.decidedFps) / m > 0.05 && DecideK(m) != b.wantK) {
            b.redecided = true;
            Log("the measured %.3f fps asks for x%d (set up with %.3f fps: x%d): the engine is set up again", m, DecideK(m), b.decidedFps, b.wantK);
            b.apple.reset(); b.port.reset();
            if (b.api && b.eng) { b.api->close(b.eng); b.eng = nullptr; b.api = nullptr; }
            b.mode = Bridge::Mode::Unset; b.why.clear();
            b.lastPts = b.prevPts = AV_NOPTS_VALUE;   // no pair across the new set-up
        }
    }
    if (g_ilVerdict.load() == 2 && b.mode != Bridge::Mode::Pass) {   // measured interlaced earlier in this file (an earlier graph)
        Log("interlaced: %s, earlier in this file: pass-through at the source rate", g_ilWhy.c_str());
        b.mode = Bridge::Mode::Pass;
        return false;
    }
    if (b.mode == Bridge::Mode::Unset) {
        b.decidedFps = SourceFps();
        b.k = b.wantK = DecideK(b.decidedFps);
        if (f->format == AV_PIX_FMT_VIDEOTOOLBOX && f->data[3] && ConfigureApple(b, f, reinterpret_cast<CVPixelBufferRef>(f->data[3]))) { b.mode = Bridge::Mode::Apple; Log("engine: apple (VideoToolbox, zero copy)"); }
        else if (b.k >= 2 && ConfigureModule(b, f)) b.mode = Bridge::Mode::Module;
        else if (ConfigurePortable(b, f)) { b.mode = Bridge::Mode::Portable; Log("engine: portable-cpu (built in)"); }
        else { b.mode = Bridge::Mode::Pass; Log("pass-through: %s", b.why.c_str()); return false; }
    }
    if (b.mode == Bridge::Mode::Pass) return false;
    const int64_t inPts = f->pts;
    {   // what the picture is, measured (pts in mpv's lavfi time base, microseconds)
        using namespace pushmpc::content;
        Cells c;
        if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUV420P10) {
            c = f->format == AV_PIX_FMT_YUV420P
                ? CellsFromYuv(static_cast<const uint8_t*>(f->data[0]), f->linesize[0], static_cast<const uint8_t*>(f->data[1]), static_cast<const uint8_t*>(f->data[2]), f->linesize[1], f->width, f->height, 8, true)
                : CellsFromYuv(reinterpret_cast<const uint16_t*>(f->data[0]), f->linesize[0] / 2, reinterpret_cast<const uint16_t*>(f->data[1]), reinterpret_cast<const uint16_t*>(f->data[2]), f->linesize[1] / 2, f->width, f->height, 10, true);
            if (!g_ilDone) {
                if (f->format == AV_PIX_FMT_YUV420P) g_comb.Push(static_cast<const uint8_t*>(f->data[0]), ptrdiff_t(f->linesize[0]), f->width, f->height, 0);
                else g_comb.Push(reinterpret_cast<const uint16_t*>(f->data[0]), ptrdiff_t(f->linesize[0] / 2), f->width, f->height, 2);
            }
        } else if (f->format == AV_PIX_FMT_VIDEOTOOLBOX && f->data[3]) {
            CVPixelBufferRef pb = reinterpret_cast<CVPixelBufferRef>(f->data[3]);
            if (CVPixelBufferGetPlaneCount(pb) == 2 && CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess) {
                const OSType t = CVPixelBufferGetPixelFormatType(pb);
                const bool p10 = t == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || t == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
                const int w = int(CVPixelBufferGetWidth(pb)), h = int(CVPixelBufferGetHeight(pb));
                const void* y = CVPixelBufferGetBaseAddressOfPlane(pb, 0); const void* uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
                const ptrdiff_t ys = ptrdiff_t(CVPixelBufferGetBytesPerRowOfPlane(pb, 0)), cs = ptrdiff_t(CVPixelBufferGetBytesPerRowOfPlane(pb, 1));
                c = p10 ? CellsFromYuv(static_cast<const uint16_t*>(y), ys / 2, static_cast<const uint16_t*>(uv), static_cast<const uint16_t*>(nullptr), cs / 2, w, h, 16, true)
                        : CellsFromYuv(static_cast<const uint8_t*>(y), ys, static_cast<const uint8_t*>(uv), static_cast<const uint8_t*>(nullptr), cs, w, h, 8, true);
                if (!g_ilDone) {   // P010-style words: the sample in the high bits
                    if (p10) g_comb.Push(static_cast<const uint16_t*>(y), ys / 2, w, h, 8);
                    else g_comb.Push(static_cast<const uint8_t*>(y), ys, w, h, 0);
                }
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
                static double vtNext = -1; const double tt = double(f->pts) / 1e6;
                if (f->pts != AV_NOPTS_VALUE && (vtNext < 0 || tt >= vtNext || tt < vtNext - 5.0)) {
                    vtNext = tt + 1.0;
                    g_vtThumb = p10 ? ClipInput(static_cast<const uint16_t*>(y), ys / 2, static_cast<const uint16_t*>(uv), static_cast<const uint16_t*>(nullptr), cs / 2, w, h, 16, true)
                                    : ClipInput(static_cast<const uint8_t*>(y), ys, static_cast<const uint8_t*>(uv), static_cast<const uint8_t*>(nullptr), cs, w, h, 8, true);
                }
#endif
                CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
            }
        }
        if (b.lastPts == AV_NOPTS_VALUE || f->pts == AV_NOPTS_VALUE || f->pts <= b.lastPts) g_content.Discontinuity();
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
        if (c.ok() && f->pts != AV_NOPTS_VALUE) {   // the thumbnail: software frames here, VideoToolbox frames above (g_vtThumb)
            static std::unique_ptr<ContentModel> model; static double next = -1;
            const double t = double(f->pts) / 1e6;
            if (!model) model = std::make_unique<ContentModel>([](const std::string& m) { Log("%s", m.c_str()); });
            double p, pt;
            double pc; if (model->Take(p, pt, pc)) g_content.SetImageEvidence(p, pt, pc);
            if (!model->Failed() && !g_vtThumb.empty()) { model->Offer(std::move(g_vtThumb), t); g_vtThumb.clear(); }
            else if (!model->Failed() && (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUV420P10) && (next < 0 || t >= next || t < next - 5.0)) {
                next = t + 1.0;
                auto in = f->format == AV_PIX_FMT_YUV420P
                    ? ClipInput(static_cast<const uint8_t*>(f->data[0]), f->linesize[0], static_cast<const uint8_t*>(f->data[1]), static_cast<const uint8_t*>(f->data[2]), f->linesize[1], f->width, f->height, 8, true)
                    : ClipInput(reinterpret_cast<const uint16_t*>(f->data[0]), f->linesize[0] / 2, reinterpret_cast<const uint16_t*>(f->data[1]), reinterpret_cast<const uint16_t*>(f->data[2]), f->linesize[1] / 2, f->width, f->height, 10, true);
                if (!in.empty()) model->Offer(std::move(in), t);
            }
        }
#endif
        if (c.ok() && f->pts != AV_NOPTS_VALUE && g_content.Push(c, double(f->pts) / 1e6)) g_contentClass.store(g_content.Current().cls);
    }
    if (!g_ilDone) {   // interlacing: decided once per file (the filter's rule: 24 moving frames, or 240 frames without motion)
        using pushmpc::content::CombMeter;
        if (++g_ilSeen <= 3) Log("frame %d: %s (the decoder's flag: a prior; the picture decides)", g_ilSeen - 1, PM_INTERLACED(f) ? "interlaced" : "progressive");
        if (PM_INTERLACED(f)) ++g_ilFlagged;
        if (g_comb.verdict() != CombMeter::kMeasuring || g_ilSeen >= CombMeter::kGiveUp) {
            g_ilDone = true;
            const bool il = g_comb.verdict() == CombMeter::kInterlaced;
            char why[200];
            if (il) snprintf(why, sizeof(why), "interlaced input (field combing measured on %d of %d moving frames)", g_comb.combed(), g_comb.measured());
            else snprintf(why, sizeof(why), "progressive (measured): %d of %d moving frames combed%s; flags said %d of %d frames interlaced", g_comb.combed(), g_comb.measured(),
                          g_comb.verdict() == CombMeter::kMeasuring ? " (too little motion to tell: treated as progressive)" : "", g_ilFlagged, g_ilSeen);
            g_ilWhy = why;
            g_ilVerdict.store(il ? 2 : 1);
            if (il) { Log("interlaced: %s: pass-through at the source rate from here", why); b.mode = Bridge::Mode::Pass; return false; }
            Log("%s", why);
        }
    }
    const bool contiguous = b.lastPts != AV_NOPTS_VALUE && f->pts != AV_NOPTS_VALUE && f->pts > b.lastPts &&
                            (b.prevPts == AV_NOPTS_VALUE || f->pts - b.lastPts <= 3 * std::max<int64_t>(1, b.lastPts - b.prevPts));
    const int64_t step = b.lastPts != AV_NOPTS_VALUE && f->pts != AV_NOPTS_VALUE ? f->pts - b.lastPts : 0;
    std::string err;
    if (b.mode == Bridge::Mode::Apple) {
        if (f->format != AV_PIX_FMT_VIDEOTOOLBOX) { Log("the stream left VideoToolbox decoding: pass-through"); b.mode = Bridge::Mode::Pass; return false; }
        std::vector<void*> gen;
        void* cur = nullptr;
        bool havePair = false;
        if (!b.apple->ProcessBuffer(f->data[3], ColourOf(f), f->pts == AV_NOPTS_VALUE ? int64_t(b.in) : f->pts, contiguous, gen, cur, havePair, err)) {
            Log("apple engine: %s: pass-through from here", err.c_str());
            b.mode = Bridge::Mode::Pass;
            return false;
        }
        const int64_t dur = step > 0 ? step / b.k : PM_DURATION(f) / std::max(1, b.k);
        for (size_t j = 0; j < gen.size(); ++j) {
            AVFrame* o = WrapPixelBuffer(b, f, static_cast<CVPixelBufferRef>(gen[j]), b.lastPts + step * int64_t(j + 1) / b.k, dur);
            if (o) b.out.push_back(o);
        }
        if (AVFrame* o = WrapPixelBuffer(b, f, static_cast<CVPixelBufferRef>(cur), f->pts, havePair ? dur : PM_DURATION(f))) b.out.push_back(o);
    } else if (b.mode == Bridge::Mode::Module) {
        AVFrame* src = f;
        AVFrame* dl = nullptr;
        if (b.engDownload) {
            if (f->format != AV_PIX_FMT_VIDEOTOOLBOX) { Log("the stream left VideoToolbox decoding: pass-through"); b.mode = Bridge::Mode::Pass; return false; }
            dl = g_av.frame_alloc();
            if (!dl || g_av.hwframe_transfer_data(dl, f, 0) < 0) { if (dl) g_av.frame_free(&dl); Log("download failed: pass-through"); b.mode = Bridge::Mode::Pass; return false; }
            if (PmFormat(dl->format) != b.engFmt) { g_av.frame_free(&dl); Log("the downloaded format differs: pass-through"); b.mode = Bridge::Mode::Pass; return false; }
            g_av.frame_copy_props(dl, f);
            src = dl;
        } else if (PmFormat(f->format) != b.engFmt) { Log("the format changed: pass-through"); b.mode = Bridge::Mode::Pass; return false; }
        const pmeng_image im = ImageOf(src, b.engFmt);
        const uint32_t flags = contiguous ? 0u : PMENG_SUBMIT_DISCONTINUITY;
        if (b.api->submit(b.eng, &im, f->pts == AV_NOPTS_VALUE ? int64_t(b.in) : f->pts, flags) != PMENG_OK) {
            Log("engine module: %s: pass-through from here", b.api->last_error(b.eng));
            if (dl) g_av.frame_free(&dl);
            b.mode = Bridge::Mode::Pass;
            return false;
        }
        const int64_t dur = step > 0 ? step / b.k : PM_DURATION(f) / std::max(1, b.k);
        pmeng_out_info oi{};
        bool generated = false;
        while (b.api->peek(b.eng, &oi, 0) == 1) {
            if (oi.subframe == 0 || step <= 0) { b.api->skip(b.eng); continue; }   // the real frame: the source itself, below
            AVFrame* o = NewSoftFrame(src, b.lastPts + step * int64_t(oi.subframe) / std::max(1, int(oi.multiplier)), dur);
            if (!o) { b.api->skip(b.eng); continue; }
            const pmeng_image di = ImageOf(o, b.engFmt);
            if (b.api->write(b.eng, &di) != PMENG_OK) { g_av.frame_free(&o); continue; }
            b.out.push_back(o);
            generated = true;
        }
        AVFrame* o = g_av.frame_alloc();
        if (o) { g_av.frame_copy_props(o, src); g_av.frame_move_ref(o, src); PM_DURATION(o) = generated ? dur : PM_DURATION(o); b.out.push_back(o); }
        if (dl) g_av.frame_free(&dl);
    } else {
        portable::YuvView v; v.width = f->width; v.height = f->height; v.bits = f->format == AV_PIX_FMT_YUV420P10 ? 10 : 8;
        for (int i = 0; i < 3; ++i) { v.data[i] = f->data[i]; v.stride[i] = f->linesize[i]; }
        if (!b.port->Push(v, contiguous, err)) { Log("portable engine: %s: pass-through", err.c_str()); b.mode = Bridge::Mode::Pass; return false; }
        const int64_t dur = step > 0 ? step / b.k : PM_DURATION(f) / std::max(1, b.k);
        if (b.port->HavePair() && step > 0)
            for (int j = 1; j < b.k; ++j) {
                AVFrame* o = NewSoftFrame(f, b.lastPts + step * j / b.k, dur);
                if (!o) break;
                portable::YuvOut yo; for (int i = 0; i < 3; ++i) { yo.data[i] = o->data[i]; yo.stride[i] = o->linesize[i]; }
                b.port->Generate(float(j) / float(b.k), yo);
                b.out.push_back(o);
            }
        AVFrame* o = g_av.frame_alloc();
        if (o) { g_av.frame_copy_props(o, f); g_av.frame_move_ref(o, f); PM_DURATION(o) = b.port->HavePair() ? dur : PM_DURATION(o); b.out.push_back(o); }
    }
    b.prevPts = b.lastPts;
    b.lastPts = inPts;   // (f may have been moved into the output queue)
    b.produced = b.in;
    return true;
}

// ------------------------------------------------------------------------------------------------ the hooks
int H_parse2(AVFilterGraph* g, const char* filters, AVFilterInOut** in, AVFilterInOut** out)
{
    const int r = o_parse2(g, filters, in, out);
    if (r >= 0 && filters && strstr(filters, "@pushmpc")) {
        std::lock_guard<std::mutex> lk(g_lock);
        g_graphs[g] = std::make_shared<Bridge>();
        Log("marked graph '%s'", filters);
    }
    return r;
}
AVFilterContext* H_alloc_filter(AVFilterGraph* g, const AVFilter* f, const char* name)
{
    AVFilterContext* c = o_alloc_filter(g, f, name);
    if (c) { std::lock_guard<std::mutex> lk(g_lock); g_ctx[c] = g; }
    return c;
}
int H_create_filter(AVFilterContext** c, const AVFilter* f, const char* name, const char* args, void* opaque, AVFilterGraph* g)
{
    const int r = o_create_filter(c, f, name, args, opaque, g);
    if (r >= 0 && c && *c) { std::lock_guard<std::mutex> lk(g_lock); g_ctx[*c] = g; }
    return r;
}
void H_graph_free(AVFilterGraph** g)
{
    if (g && *g) {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_graphs.find(*g);
        if (it != g_graphs.end()) {
            Log("graph closed: %llu frames in, %llu out (x%d)", (unsigned long long)it->second->in, (unsigned long long)it->second->emitted, it->second->k);
            g_graphs.erase(it);
        }
        for (auto c = g_ctx.begin(); c != g_ctx.end();) c = c->second == *g ? g_ctx.erase(c) : std::next(c);
    }
    o_graph_free(g);
}
int H_add_frame(AVFilterContext* ctx, AVFrame* f)
{
    std::shared_ptr<Bridge> b;
    { std::lock_guard<std::mutex> lk(g_lock); b = BridgeOf(ctx); }
    if (!b || b->mode == Bridge::Mode::Pass) return o_add_frame(ctx, f);
    if (!f) { b->eofIn = true; return 0; }
    if (Feed(*b, f)) { g_av.frame_unref(f); return 0; }
    // the engine cannot take it: queued output first, then this frame through the real graph
    return o_add_frame(ctx, f);
}
unsigned H_failed_requests(AVFilterContext* ctx)
{
    std::shared_ptr<Bridge> b;
    { std::lock_guard<std::mutex> lk(g_lock); b = BridgeOf(ctx); }
    if (!b || b->mode == Bridge::Mode::Pass) return o_failed_requests(ctx);
    return b->out.empty() && !b->eofIn ? 1u : 0u;
}
int H_get_frame_flags(AVFilterContext* ctx, AVFrame* f, int flags)
{
    std::shared_ptr<Bridge> b;
    { std::lock_guard<std::mutex> lk(g_lock); b = BridgeOf(ctx); }
    if (!b) return o_get_frame_flags(ctx, f, flags);
    if (!b->out.empty()) {
        AVFrame* o = b->out.front();
        b->out.pop_front();
        g_av.frame_move_ref(f, o);
        g_av.frame_free(&o);
        ++b->emitted;
        return 0;
    }
    if (b->mode == Bridge::Mode::Pass) return o_get_frame_flags(ctx, f, flags);
    return b->eofIn ? AVERROR_EOF : AVERROR(EAGAIN);
}
AVRational H_get_frame_rate(const AVFilterContext* ctx)
{
    AVRational r = o_get_frame_rate(ctx);
    std::shared_ptr<Bridge> b;
    { std::lock_guard<std::mutex> lk(g_lock); b = BridgeOf(ctx); }
    if (b && b->mode != Bridge::Mode::Pass && b->k > 1 && r.num > 0) r.num *= b->k;
    return r;
}

// ------------------------------------------------------------------------------------------------ GOT patching
// libmpv's import slots (__got / __la_symbol_ptr in __DATA_CONST / __DATA) that hold target; returns the count
int PatchImage(const mach_header_64* mh, void* target, void* repl)
{
    int n = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(mh) + sizeof(mach_header_64);
    intptr_t slide = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) if (_dyld_get_image_header(i) == reinterpret_cast<const mach_header*>(mh)) slide = _dyld_get_image_vmaddr_slide(i);
    for (uint32_t c = 0; c < mh->ncmds; ++c) {
        const auto* lc = reinterpret_cast<const load_command*>(p);
        if (lc->cmd == LC_SEGMENT_64) {
            const auto* seg = reinterpret_cast<const segment_command_64*>(lc);
            if (!strcmp(seg->segname, "__DATA_CONST") || !strcmp(seg->segname, "__DATA") || !strcmp(seg->segname, "__AUTH_CONST")) {
                const auto* sec = reinterpret_cast<const section_64*>(seg + 1);
                for (uint32_t s = 0; s < seg->nsects; ++s, ++sec) {
                    if (strcmp(sec->sectname, "__got") && strcmp(sec->sectname, "__la_symbol_ptr")) continue;
                    void** slots = reinterpret_cast<void**>(sec->addr + uint64_t(slide));
                    const size_t count = size_t(sec->size / sizeof(void*));
                    for (size_t k = 0; k < count; ++k) {
                        if (slots[k] != target) continue;
                        vm_address_t page = vm_address_t(&slots[k]) & ~vm_address_t(vm_page_size - 1);
                        // the page's current protection, restored afterwards (it is not always read-only: a __DATA
                        // page, or one the Objective-C runtime still writes to, must stay writable)
                        vm_address_t ra = page; vm_size_t rs = 0;
                        vm_region_basic_info_data_64_t info{};
                        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
                        mach_port_t obj = MACH_PORT_NULL;
                        vm_prot_t orig = VM_PROT_READ;
                        if (vm_region_64(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &cnt, &obj) == KERN_SUCCESS) orig = info.protection;
                        if (!(orig & VM_PROT_WRITE)) {
                            if (vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) { Log("vm_protect failed"); continue; }
                        }
                        slots[k] = repl;
                        if (!(orig & VM_PROT_WRITE)) vm_protect(mach_task_self(), page, vm_page_size, false, orig);
                        ++n;
                    }
                }
            }
        }
        p += lc->cmdsize;
    }
    return n;
}

bool InstallHooks(std::string& why)
{
#if defined(__arm64e__)
    why = "arm64e build: pointer-authenticated imports are not patched";
    return false;
#endif
    if (!g_av.Load()) { why = "FFmpeg symbols missing"; return false; }
    if ((g_av.avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR || (g_av.avfilter_version() >> 16) != LIBAVFILTER_VERSION_MAJOR) {
        why = "FFmpeg major versions differ from this build (libavutil " + std::to_string(g_av.avutil_version() >> 16) + " vs " + std::to_string(LIBAVUTIL_VERSION_MAJOR) +
              ", libavfilter " + std::to_string(g_av.avfilter_version() >> 16) + " vs " + std::to_string(LIBAVFILTER_VERSION_MAJOR) + ")";
        return false;
    }
    const mach_header_64* mpv = nullptr;
    {   // the image that defines the client API: a PushMPC libmpv stand-in (pushmpc-apps-mac.sh) re-exports the app's
        // original libmpv, and that original is the one whose imports go to libavfilter
        Dl_info di{};
        void* s = dlsym(RTLD_DEFAULT, "mpv_create");
        if (s && dladdr(s, &di) && di.dli_fbase && di.dli_fbase != _dyld_get_image_header(0)) {
            mpv = static_cast<const mach_header_64*>(di.dli_fbase);
            Log("libmpv: %s", di.dli_fname ? di.dli_fname : "?");
        }
    }
    for (uint32_t i = 0; !mpv && i < _dyld_image_count(); ++i) {
        const char* n = _dyld_get_image_name(i);
        if (n && strstr(n, "libmpv")) { mpv = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i)); Log("libmpv: %s", n); break; }
    }
    if (!mpv) {
        // mpv linked statically into the executable (the mpv CLI): patch the main image instead
        mpv = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
        Log("no libmpv image: patching the main executable %s", _dyld_get_image_name(0));
    }
    if (mpv->cputype == CPU_TYPE_ARM64 && (mpv->cpusubtype & ~CPU_SUBTYPE_MASK) == CPU_SUBTYPE_ARM64E) { why = "libmpv is arm64e"; return false; }
    struct H { const char* name; void** orig; void* repl; bool required; };
    H hooks[] = {
        { "avfilter_graph_parse2", reinterpret_cast<void**>(&o_parse2), reinterpret_cast<void*>(&H_parse2), true },
        { "avfilter_graph_alloc_filter", reinterpret_cast<void**>(&o_alloc_filter), reinterpret_cast<void*>(&H_alloc_filter), true },
        { "avfilter_graph_create_filter", reinterpret_cast<void**>(&o_create_filter), reinterpret_cast<void*>(&H_create_filter), true },
        { "avfilter_graph_free", reinterpret_cast<void**>(&o_graph_free), reinterpret_cast<void*>(&H_graph_free), true },
        { "av_buffersrc_add_frame", reinterpret_cast<void**>(&o_add_frame), reinterpret_cast<void*>(&H_add_frame), true },
        { "av_buffersrc_get_nb_failed_requests", reinterpret_cast<void**>(&o_failed_requests), reinterpret_cast<void*>(&H_failed_requests), true },
        { "av_buffersink_get_frame_flags", reinterpret_cast<void**>(&o_get_frame_flags), reinterpret_cast<void*>(&H_get_frame_flags), true },
        { "av_buffersink_get_frame_rate", reinterpret_cast<void**>(&o_get_frame_rate), reinterpret_cast<void*>(&H_get_frame_rate), false },
    };
    for (auto& h : hooks) {
        *h.orig = dlsym(RTLD_DEFAULT, h.name);
        if (!*h.orig) { why = std::string("symbol ") + h.name + " not found"; return false; }
    }
    // all or nothing: count first
    for (auto& h : hooks) {
        const int n = PatchImage(mpv, *h.orig, h.repl);
        Log("%s: %d import slot(s) patched", h.name, n);
        if (!n && h.required) { why = std::string("no import slot for ") + h.name + " in libmpv"; return false; }
    }
    return true;
}

} // namespace

// ------------------------------------------------------------------------------------------------ the mpv C plugin
extern "C" __attribute__((visibility("default"))) int mpv_open_cplugin(mpv_handle* handle)
{
    // This build serves one FFmpeg major (its structures). In a player with another libavutil major, the build made for
    // that major, pushmpc-iina-av<major>.so next to this file, runs instead (IINA's plugin and older Homebrew setups load
    // pushmpc-iina.so by name); without one, this build stays "not active" and says why.
    if (g_av.Load() && (g_av.avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR) {
        Dl_info self{};
        if (dladdr(reinterpret_cast<void*>(&mpv_open_cplugin), &self) && self.dli_fname) {
            std::string sib = self.dli_fname;
            sib = sib.substr(0, sib.rfind('/') + 1) + "pushmpc-iina-av" + std::to_string(g_av.avutil_version() >> 16) + ".so";
            if (sib != self.dli_fname && access(sib.c_str(), R_OK) == 0) {
                if (void* h = dlopen(sib.c_str(), RTLD_NOW | RTLD_LOCAL)) {
                    using OpenFn = int (*)(mpv_handle*);
                    if (auto open = reinterpret_cast<OpenFn>(dlsym(h, "mpv_open_cplugin"))) {
                        Log("libavutil %u here, this build is for %d: %s runs instead", g_av.avutil_version() >> 16, LIBAVUTIL_VERSION_MAJOR, sib.c_str());
                        return open(handle);
                    }
                } else {
                    Log("%s could not be loaded: %s", sib.c_str(), dlerror());
                }
            }
        }
    }
    g_mpv = handle;
    static std::once_flag once;
    static bool ok = false;
    static std::string why;
    std::call_once(once, [] { ok = InstallHooks(why); });
    Log("%s (client %s)", ok ? "hooks installed: vf=@pushmpc:lavfi=[null@pushmpc] now runs PushMPC" : ("not active: " + why).c_str(), mpv_client_name(handle));
    if (ok) mpv_set_property_string(handle, "user-data/pushmpc/bridge", "active");
    else mpv_set_property_string(handle, "user-data/pushmpc/bridge", ("off: " + why).c_str());
    mpv_observe_property(handle, 1, "display-fps", MPV_FORMAT_DOUBLE);
    mpv_observe_property(handle, 2, "container-fps", MPV_FORMAT_DOUBLE);
    while (true) {
        mpv_event* e = mpv_wait_event(handle, 0.5);
        {   // ContentDetect's class for the IINA plugin's Auto content mode (main.js reads user-data/pushmpc/content)
            static int published = -1;
            const int cls = g_contentClass.load();
            if (cls != published) { published = cls; mpv_set_property_string(handle, "user-data/pushmpc/content", cls == 1 ? "anime" : cls == 0 ? "measuring" : "live"); }
        }
        if (e->event_id == MPV_EVENT_NONE) continue;
        if (e->event_id == MPV_EVENT_SHUTDOWN) break;
        if (e->event_id == MPV_EVENT_START_FILE) g_rateReset.store(true);   // a new file: its rate is measured afresh
        if (e->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* pr = static_cast<mpv_event_property*>(e->data);
            const double v = pr->format == MPV_FORMAT_DOUBLE && pr->data ? *static_cast<double*>(pr->data) : 0.0;
            (e->reply_userdata == 1 ? g_displayFps : g_containerFps).store(v);
        }
    }
    g_mpv = nullptr;
    return 0;
}
