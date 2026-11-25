// inputstream.pushmpc: PushMPC frame generation inside Kodi's own VideoPlayer (Kodi 21 Omega, 22 Piers).
//
// How Kodi lets an add-on do this (Kodi source, xbmc/cores/VideoPlayer, 21.3-Omega and 22 Piers):
//   - CDVDFactoryInputStream asks every enabled inputstream add-on CInputStreamAddon::Supports(); with no ListItem
//     property it matches the file extension against addon.xml's @extension. So this add-on demuxes normal local files.
//   - A stream reported with INPUTSTREAM_FEATURE_DECODE gets externalInterfaces, and CDVDFactoryCodec::CreateVideoCodec
//     then creates CAddonVideoCodec on *this* add-on (IAddonInstance::CreateInstance on our inputstream instance).
//   - VideoPlayerVideo polls GetPicture until VC_BUFFER, so one packet may yield several pictures; each carries its own
//     pts, and RenderManager picks pictures by pts at the display rate.
// Audio and subtitle streams are reported without DECODE, so Kodi decodes, renders and passes them through as usual.
#include <kodi/AddonBase.h>
#include <kodi/Filesystem.h>
#include <kodi/General.h>
#include <kodi/addon-instance/Inputstream.h>
#include <kodi/addon-instance/VideoCodec.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "cadence.h"
#include "android/ahb_path.h"
#if defined(__ANDROID__) && defined(PM_AHB)
#include "gpu_engine.h"   // the engine type the hardware-buffer importer fills
#endif
#include "tonemap.h"
#include "kodi_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#if defined(__linux__)
#include <sys/stat.h>
#endif
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

extern "C" double pm_display_hz(void);

#ifndef PM_ADDON_VERSION
#define PM_ADDON_VERSION "0.0.0"
#endif
#ifndef PM_KODI22
#define PM_KODI22 0
#endif

namespace {

constexpr double kNoPts = double(STREAM_NOPTS_VALUE);
constexpr AVRational kUs{1, STREAM_TIME_BASE};

template<typename... Args>
void Log(ADDON_LOG level, const char* fmt, Args... args)
{
    kodi::Log(level, (std::string("PushMPC: ") + fmt).c_str(), args...);
}

std::string FfErr(int e)
{
    char b[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, b, sizeof(b));
    return b;
}

// ------------------------------------------------------------------------------------------- settings (add-on UI)
struct Settings {
    bool enabled = true;
    int smoother = 0;   // 0 Auto, 1 off, 2..10 fixed
    int quality = 1;
    bool hdr = true;
    int upscale = 0;    // 0 Kodi's scaler, 2 = x2 in the add-on (test)
    int displayHz = 0;
    int threads = 0;
    int engine = 0;     // 0 Auto (GPU, else CPU), 1 CPU, 2 GPU
    bool hdr21Sdr = false;   // Kodi 21: frame generation for HDR files, tone-mapped to SDR (off: Kodi plays them as HDR)
    static Settings Read()
    {
        Settings s;
        s.enabled = kodi::addon::GetSettingBoolean("enabled", true);
        s.smoother = kodi::addon::GetSettingInt("smoother", 0);
        s.quality = kodi::addon::GetSettingInt("quality", 1);
        s.hdr = kodi::addon::GetSettingBoolean("hdr", true);
        s.upscale = kodi::addon::GetSettingInt("upscale", 0);
        s.displayHz = kodi::addon::GetSettingInt("displayhz", 0);
        s.threads = kodi::addon::GetSettingInt("threads", 0);
        s.engine = kodi::addon::GetSettingInt("engine", 0);
        s.hdr21Sdr = kodi::addon::GetSettingBoolean("hdr21sdr", false);
        return s;
    }
};

// ------------------------------------------------------------------------------------- Kodi VFS as an AVIOContext
// (local paths, smb://, nfs://, special:// ... all go through Kodi's own file layer)
struct KodiIo {
    kodi::vfs::CFile file;
    static int Read(void* opaque, uint8_t* buf, int size)
    {
        auto* io = static_cast<KodiIo*>(opaque);
        const ssize_t n = io->file.Read(buf, size_t(size));
        if (n < 0) return AVERROR(EIO);
        return n == 0 ? AVERROR_EOF : int(n);
    }
    static int64_t Seek(void* opaque, int64_t pos, int whence)
    {
        auto* io = static_cast<KodiIo*>(opaque);
        if (whence & AVSEEK_SIZE) return io->file.GetLength();
        whence &= ~AVSEEK_FORCE;
        return io->file.Seek(pos, whence);
    }
};

// codecs Kodi's CAddonVideoCodec::CopyToInitData accepts (else it refuses to open the add-on decoder):
// Kodi 21: h264, vp8, vp9, av1. Kodi 22: + hevc, rawvideo.
bool KodiAcceptsCodecName(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: case AV_CODEC_ID_VP8: case AV_CODEC_ID_VP9: case AV_CODEC_ID_AV1: return true;
#if PM_KODI22
    case AV_CODEC_ID_HEVC: return true;
#endif
    default: return false;
    }
}

// the decoder name Kodi's avcodec_find_decoder_by_name() knows for a codec (descriptor names mostly match)
std::string KodiDecoderName(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_DTS: return "dca";
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE: return "pgssub";
    case AV_CODEC_ID_DVD_SUBTITLE: return "dvdsub";
    case AV_CODEC_ID_DVB_SUBTITLE: return "dvbsub";
    case AV_CODEC_ID_MP3: return "mp3float";
    default: return avcodec_get_name(id);
    }
}

STREAMCODEC_PROFILE AudioProfile(const AVCodecParameters* p)
{
    if (p->codec_id == AV_CODEC_ID_DTS) {
        switch (p->profile) {
        case AV_PROFILE_DTS: return DTSCodecProfile;
        case AV_PROFILE_DTS_ES: return DTSCodecProfileES;
        case AV_PROFILE_DTS_96_24: return DTSCodecProfile9624;
        case AV_PROFILE_DTS_HD_HRA: return DTSCodecProfileHDHRA;
        case AV_PROFILE_DTS_HD_MA: return DTSCodecProfileHDMA;
        case AV_PROFILE_DTS_EXPRESS: return DTSCodecProfileHDExpress;
        default: return CodecProfileUnknown;
        }
    }
    if (p->codec_id == AV_CODEC_ID_AAC) {
        switch (p->profile) {
        case AV_PROFILE_AAC_MAIN: return AACCodecProfileMAIN;
        case AV_PROFILE_AAC_LOW: return AACCodecProfileLOW;
        case AV_PROFILE_AAC_HE: return AACCodecProfileHE;
        case AV_PROFILE_AAC_HE_V2: return AACCodecProfileHEV2;
        default: return CodecProfileUnknown;
        }
    }
    return CodecProfileUnknown;
}

bool IsHdrTransfer(AVColorTransferCharacteristic t) { return t == AVCOL_TRC_SMPTE2084 || t == AVCOL_TRC_ARIB_STD_B67; }

bool HasDolbyVision(const AVStream* st)
{
    const AVCodecParameters* p = st->codecpar;
    if (p->codec_tag == MKTAG('d', 'v', 'h', '1') || p->codec_tag == MKTAG('d', 'v', 'h', 'e')) return true;
    return av_packet_side_data_get(p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF) != nullptr;
}

double StreamFps(const AVStream* st, int& rate, int& scale)
{
    AVRational r = st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0 ? st->avg_frame_rate : st->r_frame_rate;
    if (r.num <= 0 || r.den <= 0) { rate = scale = 0; return 0.0; }
    rate = r.num; scale = r.den;
    return av_q2d(r);
}

// The hardware decoder device, created on a worker thread with a time limit: Kodi also creates add-on decoders on its
// GUI thread (stream-detail probes of HDR files, DVDFileInfo GetDetailsFromFrame), and a D3D11/DXGI device created on
// the thread that owns the window can wait for window messages that thread is not pumping. On timeout: software.
bool CreateHwDevice(AVHWDeviceType type, AVBufferRef*& out)
{
    struct State { std::mutex mu; std::condition_variable cv; bool done = false; AVBufferRef* dev = nullptr; bool abandoned = false; };
    auto st = std::make_shared<State>();
    std::thread([st, type] {
        AVBufferRef* dev = nullptr;
        if (av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0) dev = nullptr;
        std::lock_guard<std::mutex> lk(st->mu);
        if (st->abandoned) { av_buffer_unref(&dev); return; }
        st->dev = dev; st->done = true; st->cv.notify_all();
    }).detach();
    std::unique_lock<std::mutex> lk(st->mu);
    if (!st->cv.wait_for(lk, std::chrono::milliseconds(1500), [&] { return st->done; })) {
        st->abandoned = true;
        Log(ADDON_LOG_WARNING, "hardware decoder device: no answer in 1.5 s, software decoding");
        return false;
    }
    out = st->dev;
    return out != nullptr;
}

class CPushVideoCodec;

// =================================================================================================== the inputstream
class CPushInputStream : public kodi::addon::CInstanceInputStream
{
public:
    explicit CPushInputStream(const kodi::addon::IInstanceInfo& instance) : CInstanceInputStream(instance) {}
    ~CPushInputStream() override { Close(); }

    ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance, KODI_ADDON_INSTANCE_HDL& hdl) override;

    void GetCapabilities(kodi::addon::InputstreamCapabilities& caps) override
    {
        caps.SetMask(INPUTSTREAM_SUPPORTS_IDEMUX | INPUTSTREAM_SUPPORTS_ITIME | INPUTSTREAM_SUPPORTS_SEEK |
                     INPUTSTREAM_SUPPORTS_PAUSE | INPUTSTREAM_SUPPORTS_ICHAPTER);
    }

    bool Open(const kodi::addon::InputstreamProperty& props) override
    {
        m_settings = Settings::Read();
        const std::string url = props.GetURL();
        Log(ADDON_LOG_INFO, "%s, Kodi API %s: opening %s", PM_ADDON_VERSION, PM_KODI22 ? "22" : "21", url.c_str());
        // as Kodi's own file input opens files: Kodi's read cache only for network sources. Kodi also opens add-on inputs on its
        // GUI thread (stream-detail probes), where a cached seek can wait on the cache thread for good.
        const bool local = url.find("://") == std::string::npos || url.rfind("file://", 0) == 0 || url.rfind("special://", 0) == 0;
        const unsigned flags = ADDON_READ_TRUNCATED | ADDON_READ_BITRATE | ADDON_READ_CHUNKED | (local ? ADDON_READ_NO_CACHE : ADDON_READ_AUDIO_VIDEO);
        if (!m_io.file.OpenFile(url, flags)) {
            Log(ADDON_LOG_ERROR, "cannot open %s", url.c_str());
            return false;
        }
        constexpr int kIoBuf = 1 << 16;
        auto* buf = static_cast<uint8_t*>(av_malloc(kIoBuf));
        m_avio = avio_alloc_context(buf, kIoBuf, 0, &m_io, &KodiIo::Read, nullptr, &KodiIo::Seek);
        m_fmt = avformat_alloc_context();
        m_fmt->pb = m_avio;
        m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        int r = avformat_open_input(&m_fmt, url.c_str(), nullptr, nullptr);
        if (r < 0) {
            Log(ADDON_LOG_ERROR, "avformat_open_input: %s", FfErr(r).c_str());
            m_fmt = nullptr;
            return false;
        }
        r = avformat_find_stream_info(m_fmt, nullptr);
        if (r < 0) Log(ADDON_LOG_WARNING, "avformat_find_stream_info: %s", FfErr(r).c_str());
        m_startUs = m_fmt->start_time != AV_NOPTS_VALUE ? m_fmt->start_time : 0;
        m_videoIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
            const AVMediaType t = m_fmt->streams[i]->codecpar->codec_type;
            if (t == AVMEDIA_TYPE_VIDEO || t == AVMEDIA_TYPE_AUDIO || t == AVMEDIA_TYPE_SUBTITLE) m_ids.push_back(i + 1);
            // reported streams start enabled, as with Kodi's own demuxer (stream-detail and thumbnail probes never enable any)
            const bool reported = t == AVMEDIA_TYPE_VIDEO || t == AVMEDIA_TYPE_AUDIO || t == AVMEDIA_TYPE_SUBTITLE;
            m_fmt->streams[i]->discard = reported ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        }
        ExtractFonts();
        DecideVideo();
        return true;
    }

    void Close() override
    {
        if (m_fmt) avformat_close_input(&m_fmt);
        if (m_avio) { av_freep(&m_avio->buffer); avio_context_free(&m_avio); }
        m_io.file.Close();
    }

    bool IsRealTimeStream() override { return false; }

    bool GetStreamIds(std::vector<unsigned int>& ids) override { ids = m_ids; return !ids.empty(); }

    bool GetStream(int streamid, kodi::addon::InputstreamInfo& info) override
    {
        if (!m_fmt || streamid < 1 || unsigned(streamid) > m_fmt->nb_streams) return false;
        const AVStream* st = m_fmt->streams[streamid - 1];
        const AVCodecParameters* p = st->codecpar;
        info.SetPhysicalIndex(unsigned(streamid));
        if (const AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0)) info.SetLanguage(lang->value);
        if (const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0)) info.SetName(title->value);
        uint32_t flags = 0;
        if (st->disposition & AV_DISPOSITION_DEFAULT) flags |= INPUTSTREAM_FLAG_DEFAULT;
        if (st->disposition & AV_DISPOSITION_FORCED) flags |= INPUTSTREAM_FLAG_FORCED;
        if (st->disposition & AV_DISPOSITION_HEARING_IMPAIRED) flags |= INPUTSTREAM_FLAG_HEARING_IMPAIRED;
        if (st->disposition & AV_DISPOSITION_VISUAL_IMPAIRED) flags |= INPUTSTREAM_FLAG_VISUAL_IMPAIRED;
        if (st->disposition & AV_DISPOSITION_ORIGINAL) flags |= INPUTSTREAM_FLAG_ORIGINAL;
        if (st->disposition & AV_DISPOSITION_COMMENT) flags |= INPUTSTREAM_FLAG_COMMENT;
        info.SetFlags(flags);
        info.SetCodecFourCC(p->codec_tag);
        info.SetBitRate(uint32_t(p->bit_rate));
        if (p->extradata && p->extradata_size > 0) info.SetExtraData(p->extradata, size_t(p->extradata_size));

        if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
            info.SetStreamType(INPUTSTREAM_TYPE_VIDEO);
            const bool ours = streamid - 1 == m_videoIndex && m_decode;
            int rate = 0, scale = 0;
            StreamFps(st, rate, scale);
            if (ours) {
                int r2 = 0, s2 = 0;
                pmk::ReportedRate(rate, scale, m_k, r2, s2);
                rate = r2; scale = s2;
                info.SetFeatures(INPUTSTREAM_FEATURE_DECODE);
#if !PM_KODI22
                // Kodi 21 honours INPUTSTREAM_FEATURE_DECODE only inside its crypto-session branch
                // (InputStreamAddon.cpp, 21.3-Omega l.538-551; Kodi 22 moved it out). A crypto session on the video
                // stream is read by nothing but the video decoder, which is ours, and no packet carries crypto info.
                kodi::addon::StreamCryptoSession crypto;
                crypto.SetKeySystem(STREAM_CRYPTO_KEY_SYSTEM_CLEARKEY);
                crypto.SetSessionId("pushmpc");
                crypto.SetFlags(0);
                info.SetCryptoSession(crypto);
#endif
                // Kodi refuses to open an add-on decoder for codecs outside its short list (CopyToInitData); our decoder
                // takes the real codec from the demuxer, so such streams are announced under a name Kodi accepts.
                info.SetCodecName(KodiAcceptsCodecName(p->codec_id) ? KodiDecoderName(p->codec_id) : std::string("vp8"));
                info.SetCodecInternalName(std::string(avcodec_get_name(p->codec_id)) + " (PushMPC)");
                info.SetWidth(uint32_t(m_outW));
                info.SetHeight(uint32_t(m_outH));
            } else {
                info.SetCodecName(KodiDecoderName(p->codec_id));
                info.SetCodecInternalName(avcodec_get_name(p->codec_id));
                info.SetWidth(uint32_t(p->width));
                info.SetHeight(uint32_t(p->height));
            }
            info.SetCodecProfile(CodecProfileUnknown);
            info.SetFpsRate(unsigned(rate));
            info.SetFpsScale(unsigned(scale));
            const AVRational sar = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : p->sample_aspect_ratio;
            if (p->height > 0)
                info.SetAspect(float(double(p->width) / p->height * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0)));
            const bool main = streamid - 1 == m_videoIndex;   // the main stream's colour may come from its first frame
            info.SetColorSpace(static_cast<INPUTSTREAM_COLORSPACE>(main ? m_space : p->color_space));
            info.SetColorRange(static_cast<INPUTSTREAM_COLORRANGE>(p->color_range));
            info.SetColorPrimaries(static_cast<INPUTSTREAM_COLORPRIMARIES>(main ? m_prim : p->color_primaries));
            info.SetColorTransferCharacteristic(static_cast<INPUTSTREAM_COLORTRC>(main ? m_trc : p->color_trc));
            if (main && m_hasMdm && m_mdm.has_primaries) {
                kodi::addon::InputstreamMasteringMetadata md;
                // FFmpeg orders the primaries R, G, B
                md.SetPrimaryR_ChromaticityX(av_q2d(m_mdm.display_primaries[0][0])); md.SetPrimaryR_ChromaticityY(av_q2d(m_mdm.display_primaries[0][1]));
                md.SetPrimaryG_ChromaticityX(av_q2d(m_mdm.display_primaries[1][0])); md.SetPrimaryG_ChromaticityY(av_q2d(m_mdm.display_primaries[1][1]));
                md.SetPrimaryB_ChromaticityX(av_q2d(m_mdm.display_primaries[2][0])); md.SetPrimaryB_ChromaticityY(av_q2d(m_mdm.display_primaries[2][1]));
                md.SetWhitePoint_ChromaticityX(av_q2d(m_mdm.white_point[0])); md.SetWhitePoint_ChromaticityY(av_q2d(m_mdm.white_point[1]));
                if (m_mdm.has_luminance) { md.SetLuminanceMax(av_q2d(m_mdm.max_luminance)); md.SetLuminanceMin(av_q2d(m_mdm.min_luminance)); }
                info.SetMasteringMetadata(md);
            }
            if (main && m_hasCll) {
                kodi::addon::InputstreamContentlightMetadata cl;
                cl.SetMaxCll(m_cll.MaxCLL);
                cl.SetMaxFall(m_cll.MaxFALL);
                info.SetContentLightMetadata(cl);
            }
        } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
            info.SetStreamType(INPUTSTREAM_TYPE_AUDIO);
            info.SetCodecName(KodiDecoderName(p->codec_id));
            info.SetCodecInternalName(avcodec_get_name(p->codec_id));
            info.SetCodecProfile(AudioProfile(p));
            info.SetChannels(uint32_t(p->ch_layout.nb_channels));
            info.SetSampleRate(uint32_t(p->sample_rate));
            info.SetBlockAlign(uint32_t(p->block_align));
            info.SetBitsPerSample(uint32_t(p->bits_per_coded_sample ? p->bits_per_coded_sample : p->bits_per_raw_sample));
        } else if (p->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            info.SetStreamType(INPUTSTREAM_TYPE_SUBTITLE);
            info.SetCodecName(KodiDecoderName(p->codec_id));
            info.SetCodecInternalName(avcodec_get_name(p->codec_id));
        } else {
            return false;
        }
        return true;
    }

    void EnableStream(int streamid, bool enable) override
    {
        if (!m_fmt || streamid < 1 || unsigned(streamid) > m_fmt->nb_streams) return;
        m_fmt->streams[streamid - 1]->discard = enable ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    bool OpenStream(int streamid) override
    {
        EnableStream(streamid, true);
        if (streamid - 1 == m_videoIndex) m_playback = true;
        return true;
    }
    // VideoPlayer opens the streams it plays; Kodi's thumbnail and stream-detail probes never do
    bool Playback() const { return m_playback; }

    void DemuxReset() override {}
    void DemuxAbort() override {}
    void DemuxFlush() override { if (m_fmt) avformat_flush(m_fmt); }
    void DemuxSetSpeed(int) override {}

    DEMUX_PACKET* DemuxRead() override
    {
        if (!m_fmt) return nullptr;
        AVPacket* pkt = av_packet_alloc();
        for (;;) {
            const int r = av_read_frame(m_fmt, pkt);
            if (r < 0) {
                av_packet_free(&pkt);
                if (r != AVERROR_EOF) Log(ADDON_LOG_WARNING, "av_read_frame: %s", FfErr(r).c_str());
                return nullptr;   // end of stream
            }
            const AVStream* st = m_fmt->streams[pkt->stream_index];
            if (st->discard == AVDISCARD_ALL) { av_packet_unref(pkt); continue; }
            if (m_reads++ == 0) Log(ADDON_LOG_DEBUG, "first packet read");
            DEMUX_PACKET* dp = AllocateDemuxPacket(pkt->size);
            if (!dp) { av_packet_free(&pkt); return nullptr; }
            if (pkt->size) std::memcpy(dp->pData, pkt->data, size_t(pkt->size));
            dp->iSize = pkt->size;
            dp->iStreamId = pkt->stream_index + 1;
            dp->pts = ToUs(pkt->pts, st->time_base);
            dp->dts = ToUs(pkt->dts, st->time_base);
            dp->duration = pkt->duration > 0 ? double(av_rescale_q(pkt->duration, st->time_base, kUs)) : 0.0;
            av_packet_free(&pkt);
            return dp;
        }
    }

    bool DemuxSeekTime(double timeMs, bool backwards, double& startpts) override
    {
        if (!m_fmt) return false;
        Log(ADDON_LOG_DEBUG, "seek to %.0f ms", timeMs);
        // Kodi calls the seek with no startpts (CDVDDemuxClient::SeekTime from DVDFileInfo's stream-detail and thumbnail
        // probes passes nullptr) and the dev kit binds *nullptr to this reference: never write through it then. The
        // check lives in another translation unit so the compiler cannot assume a reference is never null.
        double* const out = pmk::NonNull(&startpts) ? &startpts : nullptr;
        if (timeMs <= 0 && m_reads == 0) { if (out) *out = 0; return true; }   // nothing read yet: already at the start
        const int64_t target = int64_t(timeMs * 1000.0) + m_startUs;
        const int r = av_seek_frame(m_fmt, -1, target, backwards ? AVSEEK_FLAG_BACKWARD : 0);
        if (r < 0) {
            Log(ADDON_LOG_WARNING, "seek to %.0f ms: %s", timeMs, FfErr(r).c_str());
            return false;
        }
        Log(ADDON_LOG_DEBUG, "seek done");
        if (out) *out = timeMs * 1000.0;
        return true;
    }

    bool GetTimes(kodi::addon::InputstreamTimes& times) override
    {
        if (!m_fmt) return false;
        times.SetStartTime(0);
        times.SetPtsStart(0);
        times.SetPtsBegin(0);
        times.SetPtsEnd(m_fmt->duration > 0 ? double(m_fmt->duration) : 0.0);
        return true;
    }
    int GetTotalTime() override { return m_fmt && m_fmt->duration > 0 ? int(m_fmt->duration / 1000) : -1; }

    // chapters are 1-based in Kodi's API; positions in milliseconds
    int GetChapterCount() override { return m_fmt ? int(m_fmt->nb_chapters) : 0; }
    int GetChapter() override { return -1; }   // VideoPlayer computes the current chapter from the play position
    const char* GetChapterName(int ch) override
    {
        if (!m_fmt || ch < 1 || unsigned(ch) > m_fmt->nb_chapters) return nullptr;
        const AVDictionaryEntry* t = av_dict_get(m_fmt->chapters[ch - 1]->metadata, "title", nullptr, 0);
        return t ? t->value : nullptr;
    }
    int64_t GetChapterPos(int ch) override
    {
        if (!m_fmt || ch < 1 || unsigned(ch) > m_fmt->nb_chapters) return 0;
        const AVChapter* c = m_fmt->chapters[ch - 1];
        return (av_rescale_q(c->start, c->time_base, kUs) - m_startUs) / 1000;
    }
    bool SeekChapter(int ch) override
    {
        if (!m_fmt || ch < 1 || unsigned(ch) > m_fmt->nb_chapters) return false;
        double start = 0;
        return DemuxSeekTime(double(GetChapterPos(ch)), true, start);   // a real variable: no null-reference case here
    }

    // for the videocodec instance (same process, same add-on)
    const AVCodecParameters* VideoPar() const { return m_fmt && m_videoIndex >= 0 ? m_fmt->streams[m_videoIndex]->codecpar : nullptr; }
    double VideoFrameUs() const { return m_fps > 0 ? 1e6 / m_fps : 0.0; }
    int K() const { return m_k; }
    const Settings& Config() const { return m_settings; }
    const pmk::EngineCfg& EngineConfig() const { return m_engineCfg; }
    bool ToneMap() const { return m_toneMap; }
    bool ToneMapPq() const { return m_trc == AVCOL_TRC_SMPTE2084; }
    double PeakNits() const { return m_hasMdm && m_mdm.has_luminance ? av_q2d(m_mdm.max_luminance) : 0.0; }
    // the engine opened (and calibrated) for Auto, handed over to the decoder once
    std::unique_ptr<pmk::IEngine> TakeEngine() { return std::move(m_engine); }
    int OutWidth() const { return m_outW; }
    int OutHeight() const { return m_outH; }

    // Kodi tells the add-on the screen size before Open (CInputStreamAddon::DetectScreenResolution)
    void SetVideoResolution(unsigned int width, unsigned int height) override { m_screenW = int(width); m_screenH = int(height); }
    void SetVideoResolution(unsigned int width, unsigned int height, unsigned int, unsigned int) override
    {
        m_screenW = int(width); m_screenH = int(height);
    }

private:
    double ToUs(int64_t ts, AVRational tb) const
    {
        if (ts == AV_NOPTS_VALUE) return kNoPts;
        return double(av_rescale_q(ts, tb, kUs) - m_startUs);
    }

    // the upscaler and output size: Auto = NIS to the screen on the GPU engine, except on Windows, where Kodi's DXVA
    // renderer can run RTX / Intel Video Super Resolution on the add-on's pictures (better than any shader upscaler)
    void DecideUpscale(const AVStream* st, pmk::EngineCfg& c, std::string& why) const
    {
        const AVCodecParameters* p = st->codecpar;
        int mode = m_settings.upscale;   // 0 Auto, 1 Kodi's scaler, 2 x2 (test), 3 NIS, 4 FSR1
#if defined(_WIN32)
        if (mode == 0) { why = "upscaling: Kodi's video scaler (Windows: RTX / Intel Video Super Resolution when enabled in Kodi)"; return; }
#endif
        if (mode == 0) mode = 3;
        if (mode == 1 || mode == 2) { why = mode == 1 ? "upscaling: Kodi's video scaler (your setting)" : "upscaling: x2 in the add-on (test)"; return; }
        if (m_screenW <= 0 || m_screenH <= 0) { why = "upscaling: screen size unknown: Kodi's video scaler"; return; }
        const AVRational sar = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : p->sample_aspect_ratio;
        const double dar = double(p->width) / p->height * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0);
        int ow = m_screenW, oh = int(std::lround(m_screenW / dar));
        if (oh > m_screenH) { oh = m_screenH; ow = int(std::lround(m_screenH * dar)); }
        ow &= ~1; oh &= ~1;
        if (ow <= p->width * 11 / 10 && oh <= p->height * 11 / 10) { why = "upscaling: the screen is not larger than the video: none"; return; }
        c.upscaler = mode == 4 ? 2 : 1;
        // square pixels at the output: the upscaler also undoes anamorphic storage
        c.outWidth = ow; c.outHeight = oh;
        why = std::string("upscaling: ") + (mode == 4 ? "FSR1" : "NIS") + " to " + std::to_string(ow) + "x" + std::to_string(oh) +
              (m_settings.upscale == 0 ? " (Auto)" : " (your setting)");
    }

    // MKV font attachments, as Kodi's own demuxer handles them (DVDDemuxFFmpeg.cpp, AVMEDIA_TYPE_ATTACHMENT): written to
    // special://temp/fonts/, the folder libass loads when Kodi opens the subtitle stream, so styled subtitles keep their fonts
    void ExtractFonts()
    {
        static const char* kFontTypes[] = {"application/x-truetype-font", "application/vnd.ms-opentype", "application/x-font-ttf",
                                           "application/x-font", "application/font-sfnt", "font/collection", "font/otf", "font/sfnt", "font/ttf"};
        const std::string dir = "special://temp/fonts/";
        int written = 0;
        for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
            const AVStream* st = m_fmt->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT || !st->codecpar->extradata || st->codecpar->extradata_size <= 0) continue;
            const AVDictionaryEntry* mime = av_dict_get(st->metadata, "mimetype", nullptr, 0);
            bool font = st->codecpar->codec_id == AV_CODEC_ID_TTF || st->codecpar->codec_id == AV_CODEC_ID_OTF;
            for (const char* t : kFontTypes) if (mime && std::strcmp(mime->value, t) == 0) font = true;
            const AVDictionaryEntry* name = av_dict_get(st->metadata, "filename", nullptr, 0);
            if (!font || !name || !*name->value) continue;
            std::string fn = name->value;
            for (char& ch : fn) if (std::strchr("/\\:*?\"<>|", ch)) ch = '_';   // a legal file name, as Kodi makes it
            if (written == 0) kodi::vfs::CreateDirectory(dir);
            kodi::vfs::CFile out;
            if (out.OpenFileForWrite(dir + fn, true)) {
                if (out.Write(st->codecpar->extradata, size_t(st->codecpar->extradata_size)) == st->codecpar->extradata_size) ++written;
                out.Close();
            }
        }
        if (written) Log(ADDON_LOG_INFO, "fonts: %d attachment(s) written for the subtitles", written);
    }

    // the transfer of the first decoded frame, for files whose container does not say (the codec's own VUI may):
    // reads from the start and seeks back
    AVColorTransferCharacteristic ProbeTransfer()
    {
        AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED;
        AVStream* st = m_fmt->streams[m_videoIndex];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) return trc;
        AVCodecContext* ctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(ctx, st->codecpar);
        ctx->thread_count = 1;
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frm = av_frame_alloc();
        const AVDiscard saved = st->discard;
        st->discard = AVDISCARD_DEFAULT;
        if (avcodec_open2(ctx, dec, nullptr) >= 0) {
            for (int n = 0; n < 200 && trc == AVCOL_TRC_UNSPECIFIED; ++n) {
                if (av_read_frame(m_fmt, pkt) < 0) break;
                if (pkt->stream_index == m_videoIndex && avcodec_send_packet(ctx, pkt) >= 0)
                    while (avcodec_receive_frame(ctx, frm) >= 0) {
                        trc = frm->color_trc;
                        m_prim = frm->color_primaries; m_space = frm->colorspace;
                        if (const AVFrameSideData* sd = av_frame_get_side_data(frm, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
                            if (!m_hasMdm) { m_mdm = *reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data); m_hasMdm = true; m_mdmFromFrame = true; }
                        }
                        if (const AVFrameSideData* sd = av_frame_get_side_data(frm, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
                            if (!m_hasCll) { m_cll = *reinterpret_cast<const AVContentLightMetadata*>(sd->data); m_hasCll = true; }
                        }
                        if (trc == AVCOL_TRC_UNSPECIFIED) trc = AVCOL_TRC_RESERVED0;   // decoded, and really unspecified
                        av_frame_unref(frm);
                    }
                av_packet_unref(pkt);
            }
        }
        if (trc == AVCOL_TRC_RESERVED0) trc = AVCOL_TRC_UNSPECIFIED;
        st->discard = saved;
        av_seek_frame(m_fmt, -1, m_fmt->start_time != AV_NOPTS_VALUE ? m_fmt->start_time : 0, AVSEEK_FLAG_BACKWARD);
        avformat_flush(m_fmt);
        av_frame_free(&frm);
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        return trc;
    }

    // does PushMPC decode the main video stream, and at which multiplier
    void DecideVideo()
    {
        m_decode = false;
        m_k = 1;
        if (m_videoIndex < 0) return;
        const AVStream* st = m_fmt->streams[m_videoIndex];
        const AVCodecParameters* p = st->codecpar;
        int rate = 0, scale = 0;
        m_fps = StreamFps(st, rate, scale);
        std::string why;
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(p->format));
        const int bits = d ? d->comp[0].depth : 8;
        AVColorTransferCharacteristic trc = p->color_trc;
        if (const AVPacketSideData* sd = av_packet_side_data_get(p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA)) {
            m_mdm = *reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data); m_hasMdm = true;
        }
        if (const AVPacketSideData* sd = av_packet_side_data_get(p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL)) {
            m_cll = *reinterpret_cast<const AVContentLightMetadata*>(sd->data); m_hasCll = true;
        }
        // the first frame tells what the container does not: the transfer, and for HDR the mastering display / light level
        if (m_settings.enabled && bits > 8 && (trc == AVCOL_TRC_UNSPECIFIED || (IsHdrTransfer(trc) && (!m_hasMdm || !m_hasCll)))) {
            const AVColorTransferCharacteristic probed = ProbeTransfer();
            if (trc == AVCOL_TRC_UNSPECIFIED) trc = probed;
        }
        m_trc = trc;
        if (m_prim == AVCOL_PRI_UNSPECIFIED) m_prim = p->color_primaries;
        if (m_space == AVCOL_SPC_UNSPECIFIED) m_space = p->color_space;
        if (m_hasMdm || m_hasCll)
            Log(ADDON_LOG_INFO, "hdr metadata: mastering %s (max %.1f, min %.4f cd/m2)%s, MaxCLL %u, MaxFALL %u", m_hasMdm ? "yes" : "no",
                m_hasMdm && m_mdm.has_luminance ? av_q2d(m_mdm.max_luminance) : 0.0, m_hasMdm && m_mdm.has_luminance ? av_q2d(m_mdm.min_luminance) : 0.0,
                m_mdmFromFrame ? " from the first frame" : "", m_hasCll ? m_cll.MaxCLL : 0u, m_hasCll ? m_cll.MaxFALL : 0u);
        Log(ADDON_LOG_INFO, "colour: %s, %s, %s, %s range%s", av_color_transfer_name(trc), av_color_primaries_name(m_prim),
            av_color_space_name(m_space), av_color_range_name(p->color_range), trc != p->color_trc ? " (transfer from the first frame)" : "");
        if (!m_settings.enabled) why = "off in the add-on settings";
        else if (!avcodec_find_decoder(p->codec_id)) why = std::string("no decoder for ") + avcodec_get_name(p->codec_id);
        else if (HasDolbyVision(st)) why = "Dolby Vision: Kodi's own decoder keeps it";
        else if (IsHdrTransfer(trc) && PM_KODI22 && !m_settings.hdr) why = "HDR is off in the add-on settings";
        else if (IsHdrTransfer(trc) && !PM_KODI22 && !m_settings.hdr21Sdr)
            why = "HDR: Kodi 21 cannot take HDR pictures from an add-on (frame generation in SDR is an add-on setting)";
        else if (p->field_order != AV_FIELD_PROGRESSIVE && p->field_order != AV_FIELD_UNKNOWN) why = "interlaced: Kodi deinterlaces it";
        else if (p->width < 16 || p->height < 16) why = "picture too small";
        else if (bits > 10) why = "more than 10 bits per sample";
        if (!why.empty()) {
            Log(ADDON_LOG_INFO, "video stays with Kodi's decoder: %s", why.c_str());
            return;
        }
        m_decode = true;
        m_toneMap = !PM_KODI22 && IsHdrTransfer(trc);   // only reached with the setting on
        if (m_toneMap) Log(ADDON_LOG_INFO, "HDR on Kodi 21: tone-mapped to SDR BT.709 for frame generation (add-on setting)");
        m_engineCfg = pmk::EngineCfg{};
        m_engineCfg.width = p->width; m_engineCfg.height = p->height; m_engineCfg.bits = bits > 8 && !m_toneMap ? 10 : 8;
        m_engineCfg.threads = m_settings.threads; m_engineCfg.quality = m_settings.quality;
        m_engineCfg.backend = m_settings.engine;
#if defined(__ANDROID__) && defined(PM_AHB)
        m_engineCfg.external = true;   // decoder frames straight into the engine (AImageReader hardware buffers)
#endif
#if PM_KODI22 && defined(_WIN32)
        m_engineCfg.semiPlanar = true;   // NV12 / P010: the DXVA renderer uploads them with a plain copy
#endif
        std::string upWhy, engWhy;
        DecideUpscale(st, m_engineCfg, upWhy);
        m_engine = pmk::OpenEngine(m_engineCfg, engWhy);
        const double hz = m_settings.displayHz > 0 ? double(m_settings.displayHz) : pm_display_hz();
        if (!m_engine) {
            why = "no engine (" + engWhy + "): x1";
        } else if (m_settings.smoother >= 9) {
            m_k = std::min(m_settings.smoother, 16);
            why = "x" + std::to_string(m_k) + " (your setting)";
        } else {
            m_k = std::max(1, m_engine->DecideK(m_settings.smoother, m_fps, hz, why));
            // Auto upscaling only where the GPU keeps up: one upscaled frame must fit the same budget as Auto's
            // multiplier (60 % of a source frame); else Kodi's scaler, and the multiplier is decided again without it
            const double frameMs = 1000.0 / (m_fps > 0 ? m_fps : 23.976);
            const double upMs = m_engine->Stats().genMs;
            if (m_settings.upscale == 0 && m_engineCfg.upscaler && m_engine->IsGpu() && upMs > 0.6 * frameMs) {
                char b[160];
                std::snprintf(b, sizeof(b), "; Auto dropped it: an upscaled frame takes %.1f ms here (budget %.1f ms)", upMs, 0.6 * frameMs);
                upWhy += b;
                m_engine.reset();
                m_engineCfg.upscaler = 0; m_engineCfg.outWidth = 0; m_engineCfg.outHeight = 0;
                std::string why2;
                m_engine = pmk::OpenEngine(m_engineCfg, why2);
                m_k = m_engine ? std::max(1, m_engine->DecideK(m_settings.smoother, m_fps, hz, why)) : 1;
            }
        }
        const int up = m_settings.upscale == 2 ? 2 : 1;
        m_outW = m_engine ? m_engine->OutWidth() * up : p->width * up;
        m_outH = m_engine ? m_engine->OutHeight() * up : p->height * up;
        if (m_engine && !m_engine->IsGpu() && m_engineCfg.upscaler) upWhy += "; the CPU engine has no upscaler: Kodi's video scaler";
        Log(ADDON_LOG_INFO, "video: %s %dx%d %d-bit %.3f fps, display %.2f Hz, screen %dx%d", avcodec_get_name(p->codec_id), p->width,
            p->height, bits, m_fps, hz, m_screenW, m_screenH);
        Log(ADDON_LOG_INFO, "engine: %s%s", m_engine ? m_engine->Describe().c_str() : "none", engWhy.empty() ? "" : (" [" + engWhy + "]").c_str());
        Log(ADDON_LOG_INFO, "%s; output %dx%d", upWhy.c_str(), m_outW, m_outH);
        Log(ADDON_LOG_INFO, "multiplier: %s", why.c_str());
    }

    Settings m_settings;
    KodiIo m_io;
    AVIOContext* m_avio = nullptr;
    AVFormatContext* m_fmt = nullptr;
    int64_t m_startUs = 0;
    int m_videoIndex = -1;
    std::vector<unsigned int> m_ids;
    bool m_decode = false;
    int m_k = 1;
    double m_fps = 0;
    int m_screenW = 0, m_screenH = 0, m_outW = 0, m_outH = 0;
    uint64_t m_reads = 0;
    AVColorTransferCharacteristic m_trc = AVCOL_TRC_UNSPECIFIED;
    bool m_toneMap = false;
    bool m_playback = false;
    AVColorPrimaries m_prim = AVCOL_PRI_UNSPECIFIED;
    AVColorSpace m_space = AVCOL_SPC_UNSPECIFIED;
    AVMasteringDisplayMetadata m_mdm{};
    AVContentLightMetadata m_cll{};
    bool m_hasMdm = false, m_hasCll = false, m_mdmFromFrame = false;
    pmk::EngineCfg m_engineCfg;
    std::unique_ptr<pmk::IEngine> m_engine;
};

// ===================================================================================================== the decoder
class CPushVideoCodec : public kodi::addon::CInstanceVideoCodec
{
public:
    CPushVideoCodec(const kodi::addon::IInstanceInfo& instance, CPushInputStream* in)
        : CInstanceVideoCodec(instance), m_in(in) {}
    ~CPushVideoCodec() override { Log(ADDON_LOG_DEBUG, "decoder closing"); Teardown(); Log(ADDON_LOG_DEBUG, "decoder closed"); }

    bool Open(const kodi::addon::VideoCodecInitdata&) override
    {
        const AVCodecParameters* par = m_in ? m_in->VideoPar() : nullptr;
        if (!par) return false;
        m_cfg = m_in->EngineConfig();
        m_engine = m_in->TakeEngine();
        const bool playback = m_in->Playback();
        const AVCodec* dec = nullptr;
#if defined(__ANDROID__)
        // Android: MediaCodec for playback (Kodi's probes decode in software: hardware decoder instances are few)
        if (playback) {
#if defined(PM_AHB)
            if (m_engine && m_engine->IsGpu()) {
                std::string why;
                auto ahb = std::make_unique<pmk::AhbDecoder>();
                if (ahb->Open(par, why)) {
                    m_ahb = std::move(ahb);
                    Log(ADDON_LOG_INFO, "decoder: MediaCodec into hardware buffers, imported into the GPU engine (zero copy)");
                } else {
                    Log(ADDON_LOG_INFO, "decoder: no zero-copy MediaCodec path (%s)", why.c_str());
                }
            }
#endif
            if (!HaveAhb()) {
                const std::string name = std::string(avcodec_get_name(par->codec_id)) + "_mediacodec";
                if (const AVCodec* mc = avcodec_find_decoder_by_name(name.c_str())) {
                    AVCodecContext* c = avcodec_alloc_context3(mc);
                    avcodec_parameters_to_context(c, par);
                    c->pkt_timebase = kUs;
                    AVDictionary* o = nullptr;
                    av_dict_set(&o, "ndk_codec", "1", 0);   // NDK MediaCodec: no Java VM needed
                    if (avcodec_open2(c, mc, &o) >= 0) { m_ctx = c; dec = mc; m_hw = true; }
                    else avcodec_free_context(&c);
                    av_dict_free(&o);
                }
            }
        }
#endif
        if (!HaveAhb() && !m_ctx) {
            dec = avcodec_find_decoder(par->codec_id);
            if (!dec) return false;
            m_ctx = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(m_ctx, par);
            m_ctx->pkt_timebase = kUs;
            m_ctx->thread_count = playback ? 0 : 2;
            if (playback) TryHardware();
            const int r = avcodec_open2(m_ctx, dec, nullptr);
            if (r < 0) {
                Log(ADDON_LOG_ERROR, "avcodec_open2: %s", FfErr(r).c_str());
                return false;
            }
        }
        m_frame = av_frame_alloc();
        m_sw = av_frame_alloc();
        m_pkt = av_packet_alloc();
        m_k = std::max(1, m_in->K());
        m_frameUs = m_in->VideoFrameUs();
        m_test2x = m_in->Config().upscale == 2;
        m_toneMap = m_in->ToneMap();
        if (m_toneMap) m_tm.Configure(m_in->ToneMapPq(), m_in->PeakNits());
        Log(ADDON_LOG_INFO, "decoder %s%s, x%d, output %dx%d", HaveAhb() ? "MediaCodec (hardware buffers)" : dec->name, m_hw ? " (hardware)" : "",
            m_k, m_in->OutWidth(), m_in->OutHeight());
        return true;
    }

    bool Reconfigure(const kodi::addon::VideoCodecInitdata&) override { return true; }

    bool AddData(const DEMUX_PACKET& packet) override
    {
#if defined(__ANDROID__) && defined(PM_AHB)
        if (m_ahb) {
            if (!packet.pData || packet.iSize <= 0) return true;
            return m_ahb->Send(packet.pData, packet.iSize, packet.pts == kNoPts ? -1 : int64_t(std::llround(packet.pts)));
        }
#endif
        if (!m_ctx) return false;
        if (m_drained) { avcodec_flush_buffers(m_ctx); m_drained = false; }
        if (!packet.pData || packet.iSize <= 0) return true;
        if (m_packets++ < 3) Log(ADDON_LOG_DEBUG, "packet %d in (%d bytes)", m_packets, packet.iSize);
        av_packet_unref(m_pkt);
        m_pkt->data = packet.pData;
        m_pkt->size = packet.iSize;
        m_pkt->pts = packet.pts == kNoPts ? AV_NOPTS_VALUE : int64_t(std::llround(packet.pts));
        m_pkt->dts = packet.dts == kNoPts ? AV_NOPTS_VALUE : int64_t(std::llround(packet.dts));
        const int r = avcodec_send_packet(m_ctx, m_pkt);
        m_pkt->data = nullptr; m_pkt->size = 0;
        if (r == AVERROR(EAGAIN)) return false;   // Kodi keeps the packet and offers it again after GetPicture
        if (r < 0) Log(ADDON_LOG_DEBUG, "avcodec_send_packet: %s", FfErr(r).c_str());
        return true;
    }

    VIDEOCODEC_RETVAL GetPicture(VIDEOCODEC_PICTURE& picture) override
    {
        const bool drain = (picture.flags & VIDEOCODEC_PICTURE_FLAG_DRAIN) != 0;
#if defined(__ANDROID__) && defined(PM_AHB)
        if (m_ahb) return GetPictureAhb(picture, drain);
#endif
        if (!m_ctx) return VC_ERROR;
        for (;;) {
            if (!m_queue.empty()) return Emit(picture);
            const int r = avcodec_receive_frame(m_ctx, m_frame);
            if (r == 0) { OnFrame(); continue; }
            if (r == AVERROR(EAGAIN)) {
                if (drain && !m_drained) { avcodec_send_packet(m_ctx, nullptr); m_drained = true; continue; }
                if (!drain) return VC_BUFFER;
            }
            // end of the drained stream (or an error): the last held frame goes out alone, then the end
            if (m_havePrev) {
                m_queue.push_back(Out{pmk::Slot{m_prevPts, 0.f, true}, m_prevSlot, true});
                m_havePrev = false;
                continue;
            }
            if (r == AVERROR_EOF || drain) { m_drained = true; return VC_EOF; }
            Log(ADDON_LOG_DEBUG, "avcodec_receive_frame: %s", FfErr(r).c_str());
            return VC_ERROR;
        }
    }

#if defined(__ANDROID__) && defined(PM_AHB)
    VIDEOCODEC_RETVAL GetPictureAhb(VIDEOCODEC_PICTURE& picture, bool drain)
    {
        if (drain) m_ahb->SendEnd();
        for (int spin = 0;; ++spin) {
            if (!m_queue.empty()) return Emit(picture);
            AHardwareBuffer* buf = nullptr;
            int64_t pts = 0;
            bool eof = false;
            if (m_ahb->Receive(buf, pts, eof)) { OnGpuFrame(buf, pts); continue; }
            if (drain && !eof && spin < 200) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
            if (m_havePrev) { m_queue.push_back(Out{pmk::Slot{m_prevPts, 0.f, true}, m_prevSlot, true}); m_havePrev = false; continue; }
            return drain ? VC_EOF : VC_BUFFER;
        }
    }
    // a decoder frame on the GPU: imported into the engine's source slot, then the pictures of the previous frame
    void OnGpuFrame(AHardwareBuffer* buf, int64_t pts)
    {
        m_gpuFrames = true;
        if (m_pic[0].buf.empty()) Setup(m_ahb->Width(), m_ahb->Height(), m_ahb->Bits());
        const bool contiguous = m_havePrev && pmk::Contiguous(m_prevPts, pts, m_frameUs);
        std::string err;
        const bool pushed = m_engine && m_engine->PushExternal([&](void* ge, std::string& e2) {
            return m_importer.Fill(*static_cast<pmvs::gpu::Engine*>(ge), buf, e2);
        }, contiguous, err);
        m_ahb->Release();
        if (!pushed && !m_warned) { Log(ADDON_LOG_WARNING, "engine (hardware buffer): %s", err.c_str()); m_warned = true; }
        if (m_havePrev) {
            const bool pair = pushed && m_engine->HavePair();
            for (const pmk::Slot& s : pmk::SlotsForPair(m_prevPts, pts, m_k, contiguous, pair)) m_queue.push_back(Out{s, m_prevSlot, false});
        }
        m_prevSlot = m_havePrev ? 1 - m_prevSlot : 0;
        m_prevPts = pts;
        m_havePrev = true;
    }
#endif

    const char* GetName() override { return "pushmpc"; }

    void Reset() override
    {
        Log(ADDON_LOG_DEBUG, "decoder reset");
        ReleaseImports();
        if (m_ctx) avcodec_flush_buffers(m_ctx);
#if defined(__ANDROID__) && defined(PM_AHB)
        if (m_ahb) m_ahb->Flush();
#endif
        m_drained = false;
        m_queue.clear();
        m_havePrev = false;
        if (m_engine) m_engine->Forget();
    }

private:
    struct Planes {  // one picture in the engine's layout: planar 4:2:0, 8 bits or 16-bit containers
        std::vector<uint8_t> buf;
        uint8_t* data[3] = {};
        ptrdiff_t stride[3] = {};
        void Alloc(int w, int h, int bytes)
        {
            const int cw = (w + 1) / 2, ch = (h + 1) / 2;
            stride[0] = ptrdiff_t(w) * bytes; stride[1] = stride[2] = ptrdiff_t(cw) * bytes;
            buf.assign(size_t(stride[0] * h + stride[1] * ch * 2), 0);
            data[0] = buf.data(); data[1] = data[0] + stride[0] * h; data[2] = data[1] + stride[1] * ch;
        }
    };
    struct Out { pmk::Slot slot; int pic = 0; bool current = false; };   // current: the newest frame (end of the clip)

    void TryHardware()
    {
#if defined(_WIN32)
        const AVHWDeviceType type = AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__APPLE__)
        const AVHWDeviceType type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
        const AVHWDeviceType type = AV_HWDEVICE_TYPE_VAAPI;
#endif
        if (std::getenv("PUSHMPC_NO_HWDEC")) return;
        AVBufferRef* dev = nullptr;
        if (!CreateHwDevice(type, dev)) return;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* c = avcodec_get_hw_config(m_ctx->codec, i);
            if (!c) break;
            if ((c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && c->device_type == type) {
                m_hwFmt = c->pix_fmt;
                m_ctx->hw_device_ctx = av_buffer_ref(dev);
                m_ctx->opaque = this;
                m_ctx->get_format = [](AVCodecContext* ctx, const AVPixelFormat* fmts) {
                    auto* self = static_cast<CPushVideoCodec*>(ctx->opaque);
                    for (const AVPixelFormat* f = fmts; *f != AV_PIX_FMT_NONE; ++f)
                        if (*f == self->m_hwFmt) return *f;
                    return fmts[0];   // the hardware cannot take this stream: software
                };
                m_hw = true;
                break;
            }
        }
        av_buffer_unref(&dev);
    }

    // a decoded frame: convert it to the engine's layout, push it, and queue the pictures of the previous frame
    void OnFrame()
    {
        AVFrame* f = m_frame;
        if (f->hw_frames_ctx) {
            av_frame_unref(m_sw);
            if (av_hwframe_transfer_data(m_sw, m_frame, 0) < 0) { av_frame_unref(m_frame); return; }
            av_frame_copy_props(m_sw, m_frame);
            f = m_sw;
        }
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
        const int bits = d && d->comp[0].depth > 8 && !m_toneMap ? 10 : 8;
        if (m_pic[0].buf.empty() || f->width != m_w || f->height != m_h || bits != m_bits) Setup(f->width, f->height, bits);
        int64_t pts = f->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = m_havePrev ? m_prevPts + int64_t(m_frameUs > 0 ? m_frameUs : 41708) : 0;

        const int cur = m_havePrev ? 1 - m_prevSlot : 0;
        if (m_toneMap) ToneMapPlanes(f, m_pic[cur]);
        else ToPlanes(f, m_pic[cur]);
        av_frame_unref(m_frame);
        av_frame_unref(m_sw);

        const bool contiguous = m_havePrev && pmk::Contiguous(m_prevPts, pts, m_frameUs);
        bool pushed = false;
        if (m_engine && (m_k > 1 || m_upscaled)) {
            pm_yuv src{};
            for (int i = 0; i < 3; ++i) { src.data[i] = m_pic[cur].data[i]; src.stride[i] = m_pic[cur].stride[i]; }
            std::string err;
            pushed = m_engine->Push(src, contiguous, err);
            if (!pushed && !m_warned) { Log(ADDON_LOG_WARNING, "engine: %s", err.c_str()); m_warned = true; }
        }
        if (m_havePrev) {
            const bool pair = pushed && m_engine->HavePair();
            for (const pmk::Slot& s : pmk::SlotsForPair(m_prevPts, pts, m_k, contiguous, pair)) m_queue.push_back(Out{s, m_prevSlot, false});
        }
        m_prevSlot = cur;
        m_prevPts = pts;
        m_havePrev = true;
    }

    void Setup(int w, int h, int bits)
    {
        if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
        if (m_swsOut) { sws_freeContext(m_swsOut); m_swsOut = nullptr; }
        m_w = w; m_h = h; m_bits = bits;
        const int bytes = bits > 8 ? 2 : 1;
        for (auto& p : m_pic) p.Alloc(w, h, bytes);
        m_gen.Alloc(w, h, bytes);
        ReleaseImports();
        if (!m_engine || w != m_cfg.width || h != m_cfg.height || bits != m_cfg.bits) {
            // the stream changed size or depth mid-way (rare): a new engine for the new pictures
            m_cfg.width = w; m_cfg.height = h; m_cfg.bits = bits;
            m_engine.reset();
            std::string why;
            m_engine = pmk::OpenEngine(m_cfg, why);
            if (!m_engine) { Log(ADDON_LOG_ERROR, "engine: %s", why.c_str()); m_k = 1; }
        }
        m_upscaled = m_engine && (m_engine->OutWidth() != w || m_engine->OutHeight() != h);
        m_outW = (m_upscaled ? m_engine->OutWidth() : w) * (m_test2x ? 2 : 1);
        m_outH = (m_upscaled ? m_engine->OutHeight() : h) * (m_test2x ? 2 : 1);
        m_havePrev = false;
    }

    AVPixelFormat EngineFmt() const { return m_bits > 8 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P; }

    // Kodi 21, HDR with the setting on: the decoded frame (any 10/12-bit 4:2:0 layout, via swscale to planar 16-bit)
    // tone-mapped into the engine's 8-bit SDR planes
    void ToneMapPlanes(const AVFrame* f, Planes& out)
    {
        const AVPixelFormat want = AV_PIX_FMT_YUV420P10LE;
        const uint16_t* sp[3];
        ptrdiff_t ss[3];
        int bits = 10;
        if (AVPixelFormat(f->format) == want || AVPixelFormat(f->format) == AV_PIX_FMT_YUV420P12LE) {
            if (AVPixelFormat(f->format) == AV_PIX_FMT_YUV420P12LE) bits = 12;
            for (int i = 0; i < 3; ++i) { sp[i] = reinterpret_cast<const uint16_t*>(f->data[i]); ss[i] = f->linesize[i]; }
        } else {
            if (m_hdrTmp.buf.empty()) m_hdrTmp.Alloc(m_w, m_h, 2);
            m_swsHdr = sws_getCachedContext(m_swsHdr, m_w, m_h, AVPixelFormat(f->format), m_w, m_h, want, SWS_POINT, nullptr, nullptr, nullptr);
            if (!m_swsHdr) return;
            int ls[4] = {int(m_hdrTmp.stride[0]), int(m_hdrTmp.stride[1]), int(m_hdrTmp.stride[2]), 0};
            uint8_t* dst[4] = {m_hdrTmp.data[0], m_hdrTmp.data[1], m_hdrTmp.data[2], nullptr};
            sws_scale(m_swsHdr, f->data, f->linesize, 0, m_h, dst, ls);
            for (int i = 0; i < 3; ++i) { sp[i] = reinterpret_cast<const uint16_t*>(m_hdrTmp.data[i]); ss[i] = m_hdrTmp.stride[i]; }
        }
        uint8_t* const dp[3] = {out.data[0], out.data[1], out.data[2]};
        m_tm.Run(sp, ss, bits, m_w, m_h, dp, out.stride, m_cfg.threads);
    }

    void ToPlanes(const AVFrame* f, Planes& out)
    {
        if (AVPixelFormat(f->format) == EngineFmt()) {
            const int bytes = m_bits > 8 ? 2 : 1;
            for (int i = 0; i < 3; ++i) {
                const int ph = i ? (m_h + 1) / 2 : m_h, pw = (i ? (m_w + 1) / 2 : m_w) * bytes;
                av_image_copy_plane(out.data[i], int(out.stride[i]), f->data[i], f->linesize[i], pw, ph);
            }
            return;
        }
        m_sws = sws_getCachedContext(m_sws, m_w, m_h, AVPixelFormat(f->format), m_w, m_h, EngineFmt(), SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) return;
        int ls[4] = {int(out.stride[0]), int(out.stride[1]), int(out.stride[2]), 0};
        uint8_t* dst[4] = {out.data[0], out.data[1], out.data[2], nullptr};
        sws_scale(m_sws, f->data, f->linesize, 0, m_h, dst, ls);
    }

    VIDEOCODEC_RETVAL Emit(VIDEOCODEC_PICTURE& picture)
    {
        const Out q = m_queue.front();
        m_queue.pop_front();
        const pmk::Slot& s = q.slot;
        const int bytes = m_bits > 8 ? 2 : 1;
        const int ow = m_outW, oh = m_outH, ocw = (ow + 1) / 2, och = (oh + 1) / 2;
        // The picture's layout, one of three:
        //  - engine: the GPU engine's own output layout (planar, or NV12 / P010 on Windows with Kodi 22, the DXVA
        //    renderer's copy-only formats), so the engine can write into Kodi's buffer in place: a dma-buf (Linux
        //    GBM/Wayland with a GLES renderer) or Kodi's system memory imported as host memory; else its readback is
        //    copied straight into the buffer;
        //  - P010 packed on the CPU (Windows, Kodi 22, the CPU engine);
        //  - plain planar (Kodi 21, the CPU engine elsewhere, the x2 test path).
        const bool semi = EngineSemi();
        const bool engineLayout = EngineLayout();
#if PM_KODI22
        const bool p010Cpu = !engineLayout && Windows() && m_bits > 8;
        picture.videoFormat = semi ? (m_bits > 8 ? VIDEOCODEC_FORMAT_P010 : VIDEOCODEC_FORMAT_NV12)
                              : p010Cpu ? VIDEOCODEC_FORMAT_P010 : m_bits > 8 ? VIDEOCODEC_FORMAT_YUV420P10 : VIDEOCODEC_FORMAT_I420;
#else
        const bool p010Cpu = false;
        picture.videoFormat = m_bits > 8 ? VIDEOCODEC_FORMAT_YUV420P10 : VIDEOCODEC_FORMAT_I420;
#endif
        picture.width = uint32_t(ow);
        picture.height = uint32_t(oh);
        CheckBufferPool(picture);   // before the layout: dma-bufs only where Kodi's renderer takes them
        const uint64_t hostAlign = engineLayout && !m_hostOff && !DmaPath() ? m_engine->HostAlignment() : 0;
        picture.planeOffsets[0] = 0;
        picture.stride[0] = uint32_t(ow * bytes);
        if (engineLayout) {
            picture.stride[0] = uint32_t(m_layout.pitchY);
            picture.stride[1] = uint32_t(m_layout.pitchC);
            picture.stride[2] = semi ? 0 : uint32_t(m_layout.pitchC);
            picture.planeOffsets[1] = uint32_t(m_layout.offU);
            picture.planeOffsets[2] = semi ? 0 : uint32_t(m_layout.offV);
            // host import: room to start the picture on an aligned address inside Kodi's buffer
            picture.decodedDataSize = size_t(m_layout.size + (hostAlign ? 2 * hostAlign : 0));
        } else if (p010Cpu) {
            picture.stride[1] = uint32_t(ocw * 4);
            picture.stride[2] = 0;
            picture.planeOffsets[1] = picture.stride[0] * uint32_t(oh);
            picture.planeOffsets[2] = 0;
            picture.decodedDataSize = size_t(picture.planeOffsets[1]) + size_t(picture.stride[1]) * size_t(och);
        } else {
            picture.stride[1] = picture.stride[2] = uint32_t(ocw * bytes);
            picture.planeOffsets[1] = picture.stride[0] * uint32_t(oh);
            picture.planeOffsets[2] = picture.planeOffsets[1] + picture.stride[1] * uint32_t(och);
            picture.decodedDataSize = size_t(picture.planeOffsets[2]) + size_t(picture.stride[2]) * size_t(och);
        }
        picture.pts = s.pts;
        if (!GetFrameBuffer(picture)) { Log(ADDON_LOG_WARNING, "Kodi gave no frame buffer"); return VC_ERROR; }
        if (!KeepSysMemIfNeeded(picture)) return VC_ERROR;
        int zc = -1;   // the imported Kodi buffer the engine writes into, or -1
        if (engineLayout) {
            if (DmaPath()) zc = ZeroCopyTarget(picture);
            else if (hostAlign) zc = HostTarget(picture, hostAlign);   // may move planeOffsets to an aligned start
        }
        uint8_t* dst[4] = {};
        int ds[3] = {};
        if (p010Cpu) {
            if (m_packTmp.buf.empty() || m_packW != ow || m_packH != oh) { m_packTmp.Alloc(ow, oh, 2); m_packW = ow; m_packH = oh; }
            for (int i = 0; i < 3; ++i) { dst[i] = m_packTmp.data[i]; ds[i] = int(m_packTmp.stride[i]); }
        } else {
            for (int i = 0; i < 3; ++i) { dst[i] = picture.decodedData + picture.planeOffsets[i]; ds[i] = int(picture.stride[i]); }
            if (semi) { dst[2] = nullptr; ds[2] = 0; }
        }
        pm_yuv_out kodiOut{};
        for (int i = 0; i < 3; ++i) { kodiOut.data[i] = dst[i]; kodiOut.stride[i] = ds[i]; }
        std::string err;
        bool done = false;
        const Planes* srcp = &m_pic[q.pic];
        if (zc >= 0) {
            done = s.real ? m_engine->RealInto(q.current, zc, err) : m_engine->GenerateInto(s.t, zc, err);
            if (done) { ++m_zeroCopied; if (!s.real) ++m_generated; }
            else { Log(ADDON_LOG_WARNING, "in-place output failed (%s): copy path from now on", err.c_str()); m_zcOff = m_hostOff = true; err.clear(); }
        }
        if (done) {
        } else if (!s.real && m_engine) {
            if (!m_test2x) {
                done = m_engine->Generate(s.t, kodiOut, err);   // the engine's readback straight into Kodi's buffer
                if (done) ++m_generated;
            } else {
                pm_yuv_out g{};
                for (int i = 0; i < 3; ++i) { g.data[i] = m_gen.data[i]; g.stride[i] = m_gen.stride[i]; }
                if (m_engine->Generate(s.t, g, err)) { srcp = &m_gen; ++m_generated; }
            }
        } else if (s.real && ((m_upscaled && !m_test2x) || semi || m_gpuFrames)) {
            done = m_engine->Real(q.current, kodiOut, err);   // through the engine: upscaled, or in its NV12 / P010 layout
        }
        if (!err.empty() && !m_warned) { Log(ADDON_LOG_WARNING, "engine: %s", err.c_str()); m_warned = true; }
        if (!done) {
            if (semi) {
                // the engine could not give this picture: the source frame packed on the CPU (rare: an engine error)
                PackSemi(picture, srcp, ow, oh);
            } else if (!m_test2x && !m_upscaled) {
                for (int i = 0; i < 3; ++i) {
                    const int ph = i ? (m_h + 1) / 2 : m_h, pw = (i ? (m_w + 1) / 2 : m_w) * bytes;
                    av_image_copy_plane(dst[i], ds[i], srcp->data[i], int(srcp->stride[i]), pw, ph);
                }
            } else {
                m_swsOut = sws_getCachedContext(m_swsOut, m_w, m_h, EngineFmt(), ow, oh, EngineFmt(), SWS_LANCZOS, nullptr, nullptr, nullptr);
                const uint8_t* sp[4] = {srcp->data[0], srcp->data[1], srcp->data[2], nullptr};
                int sl[4] = {int(srcp->stride[0]), int(srcp->stride[1]), int(srcp->stride[2]), 0};
                int dl[4] = {ds[0], ds[1], ds[2], 0};
                if (m_swsOut) sws_scale(m_swsOut, sp, sl, 0, m_h, dst, dl);
            }
        }
        if (p010Cpu) PackP010(picture, ow, oh);
        ++m_emitted;
        Log(ADDON_LOG_DEBUG, "out pts=%lld real=%d t=%.3f", static_cast<long long>(s.pts), s.real ? 1 : 0, double(s.t));
        if ((m_emitted % 600) == 0 && m_engine) {
            const pmk::EngineStats st = m_engine->Stats();
            Log(ADDON_LOG_INFO, "%llu pictures out (%llu generated, %llu written in place), cuts %llu, motion %.1f ms, generated frame %.1f ms",
                static_cast<unsigned long long>(m_emitted), static_cast<unsigned long long>(m_generated),
                static_cast<unsigned long long>(m_zeroCopied), static_cast<unsigned long long>(st.cuts), st.meMs, st.genMs);
        }
        return VC_PICTURE;
    }

    static bool Windows()
    {
#if defined(_WIN32)
        return true;
#else
        return false;
#endif
    }
    // the GPU engine's layout for Kodi's picture (planar, or NV12 / P010 when the engine was set up for it)
    bool EngineLayout()
    {
        if (!m_engine || !m_engine->IsGpu() || m_test2x) return false;
        if (!m_layoutKnown) { m_layoutKnown = m_engine->Layout(m_layout); }
        return m_layoutKnown;
    }
    bool EngineSemi() { return EngineLayout() && m_cfg.semiPlanar && PM_KODI22; }
    bool DmaPath() const { return m_dmaOk && !m_zcOff; }

    // Kodi's system-memory frame buffer as the engine's output (VK_EXT_external_memory_host): the picture starts on the
    // next aligned address inside the buffer (planeOffsets move with it), and that aligned range is imported. Kodi's
    // pool recycles its buffers for the life of this decoder, so the imports are cached by address and all released
    // with the decoder, on Reset and on a size change (before Kodi can free a buffer).
    int HostTarget(VIDEOCODEC_PICTURE& picture, uint64_t align)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(picture.decodedData);
        const uintptr_t start = (base + align - 1) / align * align;
        const uint64_t size = (m_layout.size + align - 1) / align * align;
        if (start + size > base + picture.decodedDataSize) return -1;
        const uint32_t pad = uint32_t(start - base);
        picture.planeOffsets[0] = pad;
        picture.planeOffsets[1] = pad + uint32_t(m_layout.offU);
        if (picture.planeOffsets[2]) picture.planeOffsets[2] = pad + uint32_t(m_layout.offV);
        const auto key = std::make_pair(uint64_t(start), size);
        auto it = m_hostImports.find(key);
        if (it != m_hostImports.end()) return it->second;
        std::string err;
        const int id = m_engine->ImportHost(reinterpret_cast<void*>(start), size, err);
        if (id < 0) {
            Log(ADDON_LOG_INFO, "in-place output: Kodi's buffer cannot be imported here (%s): copy path", err.c_str());
            m_hostOff = true;
            return -1;
        }
        if (m_hostImports.empty()) Log(ADDON_LOG_INFO, "in-place output: the engine writes into Kodi's system-memory frame buffers");
        m_hostImports[key] = id;
        return id;
    }

    // a source frame (engine layout planar) into Kodi's NV12 / P010 picture on the CPU
    void PackSemi(const VIDEOCODEC_PICTURE& picture, const Planes* src, int ow, int oh)
    {
        const int ocw = (ow + 1) / 2, och = (oh + 1) / 2;
        const int sh = m_bits > 8 ? 16 - m_bits : 0;
        uint8_t* y = picture.decodedData + picture.planeOffsets[0];
        uint8_t* uv = picture.decodedData + picture.planeOffsets[1];
        for (int r = 0; r < oh && r < m_h; ++r) {
            if (m_bits > 8) {
                const uint16_t* s = reinterpret_cast<const uint16_t*>(src->data[0] + src->stride[0] * r);
                uint16_t* d = reinterpret_cast<uint16_t*>(y + size_t(picture.stride[0]) * size_t(r));
                for (int x = 0; x < ow && x < m_w; ++x) d[x] = uint16_t(s[x] << sh);
            } else {
                std::memcpy(y + size_t(picture.stride[0]) * size_t(r), src->data[0] + src->stride[0] * r, size_t(std::min(ow, m_w)));
            }
        }
        for (int r = 0; r < och && r < (m_h + 1) / 2; ++r)
            for (int x = 0; x < ocw && x < (m_w + 1) / 2; ++x) {
                if (m_bits > 8) {
                    const uint16_t* u = reinterpret_cast<const uint16_t*>(src->data[1] + src->stride[1] * r);
                    const uint16_t* v = reinterpret_cast<const uint16_t*>(src->data[2] + src->stride[2] * r);
                    uint16_t* d = reinterpret_cast<uint16_t*>(uv + size_t(picture.stride[1]) * size_t(r));
                    d[2 * x] = uint16_t(u[x] << sh); d[2 * x + 1] = uint16_t(v[x] << sh);
                } else {
                    uint8_t* d = uv + size_t(picture.stride[1]) * size_t(r);
                    d[2 * x] = src->data[1][src->stride[1] * r + x]; d[2 * x + 1] = src->data[2][src->stride[2] * r + x];
                }
            }
    }

    // zero copy (Kodi 22 on Linux GBM/Wayland, GPU engine with dma-buf import): Kodi's frame buffers are dma-bufs
    // (DRM-PRIME, get_frame_buffer_platform_handle); laid out like the engine's output, the engine writes into them
    // directly. Anything unexpected switches to the copy path for good.
    bool ZeroCopyLayout()
    {
#if PM_KODI22 && defined(__linux__) && !defined(__ANDROID__)
        if (!m_dmaOk || m_zcOff || !EngineLayout() || !m_engine->CanImport() || std::getenv("PUSHMPC_NO_ZERO_COPY")) return false;
        return true;
#else
        return false;
#endif
    }
    int ZeroCopyTarget(const VIDEOCODEC_PICTURE& picture)
    {
#if PM_KODI22 && defined(__linux__) && !defined(__ANDROID__)
        if (!ZeroCopyLayout()) return -1;
        VIDEOCODEC_PLATFORM_BUFFER pb{};
        if (!GetFrameBufferPlatformHandle(picture.videoBufferHandle, pb) || pb.type != VIDEOCODEC_PLATFORM_BUFFER_DRM_PRIME || !pb.handle) {
            if (!m_zcNoted) { Log(ADDON_LOG_INFO, "zero copy: Kodi's frame buffers are system memory here (X11 or no dma-buf allocator): copy path"); m_zcNoted = true; }
            m_zcOff = true;
            return -1;
        }
        const auto* d = static_cast<const KODI_DRM_FRAME_DESCRIPTOR*>(pb.handle);
        const bool layoutOk = d->nb_objects == 1 && d->objects[0].fd >= 0 && d->objects[0].size >= m_layout.size && d->nb_layers >= 1 &&
                              d->layers[0].nb_planes == 3 && d->layers[0].planes[0].offset == 0 &&
                              d->layers[0].planes[1].offset == m_layout.offU && d->layers[0].planes[2].offset == m_layout.offV &&
                              int(d->layers[0].planes[0].pitch) == m_layout.pitchY && int(d->layers[0].planes[1].pitch) == m_layout.pitchC;
        if (!layoutOk) {
            Log(ADDON_LOG_WARNING, "zero copy: Kodi's dma-buf layout differs (objects %u, layers %u): copy path", d->nb_objects, d->nb_layers);
            m_zcOff = true;
            return -1;
        }
        const int fd = d->objects[0].fd;
        struct stat st{};
        if (fstat(fd, &st) != 0) { m_zcOff = true; return -1; }
        const auto key = std::make_pair(uint64_t(st.st_dev), uint64_t(st.st_ino));
        auto it = m_imports.find(key);
        if (it != m_imports.end()) return it->second;
        std::string err;
        const int id = m_engine->Import(fd, d->objects[0].size, err);
        if (id < 0) {
            Log(ADDON_LOG_WARNING, "zero copy: import of Kodi's dma-buf failed (%s): copy path", err.c_str());
            m_zcOff = true;
            return -1;
        }
        if (m_imports.empty()) Log(ADDON_LOG_INFO, "zero copy: the engine writes into Kodi's dma-buf frame buffers");
        m_imports[key] = id;
        return id;
#else
        (void)picture;
        return -1;
#endif
    }
    // Kodi 22 on Wayland/GBM registers a dma-buf pool first in its buffer manager, so the add-on's frame buffers can be
    // DRM-PRIME even though it asked for a CPU format. Kodi's GLES renderer takes those; its desktop-GL renderer does not
    // (CLinuxRendererGL::UploadTexture crashes: Kodi issue 29344). So, unless Kodi renders with GLES, the dma-buf pool is
    // configured with a decoy size once (the manager reuses a pool only for the same format and size), and every real
    // request then comes from a system-memory pool. A dma-buf that still arrives is given back and asked for again.
    static bool KodiRendersWithGles()
    {
#if defined(__linux__) && !defined(__ANDROID__)
        if (const char* e = std::getenv("PUSHMPC_KODI_DMABUF")) return e[0] == '1';
        FILE* m = std::fopen("/proc/self/maps", "r");
        if (!m) return false;
        bool gles = false, gl = false;
        char line[512];
        while (std::fgets(line, sizeof(line), m)) {
            if (std::strstr(line, "libGLESv2.so")) gles = true;
            if (std::strstr(line, "libGL.so") || std::strstr(line, "libOpenGL.so") || std::strstr(line, "libGLX.so")) gl = true;
        }
        std::fclose(m);
        return gles && !gl;
#else
        return false;
#endif
    }
    void CheckBufferPool(const VIDEOCODEC_PICTURE& real)
    {
#if defined(__linux__) && !defined(__ANDROID__)   // Kodi 21 has the same dma-buf pool on Wayland/GBM
        if (m_poolChecked) return;
        m_poolChecked = true;
        m_dmaOk = KodiRendersWithGles();
        Log(ADDON_LOG_INFO, "frame buffers: %s", m_dmaOk ? "dma-buf allowed (Kodi renders with GLES)" : "system memory (Kodi's renderer cannot take dma-bufs from add-ons)");
        if (m_dmaOk) return;
        VIDEOCODEC_PICTURE d{};
        d.videoFormat = real.videoFormat;
        d.width = 16; d.height = 16;
        d.stride[0] = 32; d.stride[1] = d.stride[2] = 16;
        d.planeOffsets[0] = 0; d.planeOffsets[1] = 512; d.planeOffsets[2] = 768;
        d.decodedDataSize = 4096;   // never a real picture's size
        if (GetFrameBuffer(d)) ReleaseFrameBuffer(d.videoBufferHandle);
#else
        (void)real;
#endif
    }
    bool KeepSysMemIfNeeded(VIDEOCODEC_PICTURE& picture)
    {
#if PM_KODI22 && defined(__linux__) && !defined(__ANDROID__)
        if (m_dmaOk) return true;
        for (int attempt = 0; attempt < 2; ++attempt) {
            VIDEOCODEC_PLATFORM_BUFFER pb{};
            if (!GetFrameBufferPlatformHandle(picture.videoBufferHandle, pb) || pb.type != VIDEOCODEC_PLATFORM_BUFFER_DRM_PRIME) return true;
            Log(ADDON_LOG_WARNING, "Kodi handed out a dma-buf frame buffer its renderer cannot take: asking for system memory");
            ReleaseFrameBuffer(picture.videoBufferHandle);
            picture.videoBufferHandle = nullptr;
            picture.decodedDataSize += 4096;   // a size no dma-buf pool is configured for
            if (!GetFrameBuffer(picture)) return false;
        }
        return true;
#else
        (void)picture;
        return true;
#endif
    }

    void ReleaseImports()
    {
        if (m_engine) {
            for (auto& kv : m_imports) m_engine->ReleaseImport(kv.second);
            for (auto& kv : m_hostImports) m_engine->ReleaseImport(kv.second);
        }
        m_imports.clear();
        m_hostImports.clear();
        m_layoutKnown = false;
    }

    // planar 10-bit (low bits) into Kodi's P010 buffer (high bits, U and V interleaved)
    void PackP010(const VIDEOCODEC_PICTURE& picture, int ow, int oh)
    {
        const int ocw = (ow + 1) / 2, och = (oh + 1) / 2;
        for (int y = 0; y < oh; ++y) {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(m_packTmp.data[0] + m_packTmp.stride[0] * y);
            uint16_t* d = reinterpret_cast<uint16_t*>(picture.decodedData + picture.planeOffsets[0] + size_t(picture.stride[0]) * size_t(y));
            for (int x = 0; x < ow; ++x) d[x] = uint16_t(s[x] << 6);
        }
        for (int y = 0; y < och; ++y) {
            const uint16_t* u = reinterpret_cast<const uint16_t*>(m_packTmp.data[1] + m_packTmp.stride[1] * y);
            const uint16_t* v = reinterpret_cast<const uint16_t*>(m_packTmp.data[2] + m_packTmp.stride[2] * y);
            uint16_t* d = reinterpret_cast<uint16_t*>(picture.decodedData + picture.planeOffsets[1] + size_t(picture.stride[1]) * size_t(y));
            for (int x = 0; x < ocw; ++x) { d[2 * x] = uint16_t(u[x] << 6); d[2 * x + 1] = uint16_t(v[x] << 6); }
        }
    }

    void Teardown()
    {
        if (m_zeroCopied) Log(ADDON_LOG_INFO, "zero copy: %llu pictures written in place", static_cast<unsigned long long>(m_zeroCopied));
        ReleaseImports();
#if defined(__ANDROID__) && defined(PM_AHB)
        m_ahb.reset();
#endif
        m_engine.reset();
        if (m_sws) sws_freeContext(m_sws);
        if (m_swsOut) sws_freeContext(m_swsOut);
        if (m_swsHdr) sws_freeContext(m_swsHdr);
        m_sws = m_swsOut = m_swsHdr = nullptr;
        av_frame_free(&m_frame);
        av_frame_free(&m_sw);
        av_packet_free(&m_pkt);
        avcodec_free_context(&m_ctx);
    }

    CPushInputStream* m_in = nullptr;
    AVCodecContext* m_ctx = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_sw = nullptr;
    AVPacket* m_pkt = nullptr;
    AVPixelFormat m_hwFmt = AV_PIX_FMT_NONE;
    bool m_hw = false;
    bool m_drained = false;
    SwsContext* m_sws = nullptr;
    SwsContext* m_swsOut = nullptr;
    SwsContext* m_swsHdr = nullptr;
    pmk::ToneMapper m_tm;
    bool m_toneMap = false;
    pmk::EngineCfg m_cfg;
    std::unique_ptr<pmk::IEngine> m_engine;
    bool HaveAhb() const
    {
#if defined(__ANDROID__) && defined(PM_AHB)
        return m_ahb != nullptr;
#else
        return false;
#endif
    }
    bool m_gpuFrames = false;   // frames reach the engine on the GPU (no CPU planes: real pictures come from the engine)
#if defined(__ANDROID__) && defined(PM_AHB)
    std::unique_ptr<pmk::AhbDecoder> m_ahb;
    pmk::AhbImporter m_importer;
#endif
    bool m_upscaled = false, m_test2x = false, m_warned = false;
    int m_w = 0, m_h = 0, m_bits = 8, m_k = 1, m_outW = 0, m_outH = 0;
    double m_frameUs = 0;
    Planes m_pic[2];
    Planes m_gen;
    Planes m_packTmp;
    Planes m_hdrTmp;
    pmk::OutLayout m_layout;
    std::map<std::pair<uint64_t, uint64_t>, int> m_imports;
    bool m_zcOff = false, m_zcNoted = false, m_poolChecked = false, m_dmaOk = false, m_hostOff = false, m_layoutKnown = false;
    std::map<std::pair<uint64_t, uint64_t>, int> m_hostImports;
    unsigned long long m_zeroCopied = 0;
    int m_packW = 0, m_packH = 0;
    int m_prevSlot = 0;
    int64_t m_prevPts = 0;
    bool m_havePrev = false;
    std::deque<Out> m_queue;
    unsigned long long m_emitted = 0, m_generated = 0;
    int m_packets = 0;
};

ADDON_STATUS CPushInputStream::CreateInstance(const kodi::addon::IInstanceInfo& instance, KODI_ADDON_INSTANCE_HDL& hdl)
{
    if (instance.IsType(ADDON_INSTANCE_VIDEOCODEC)) {
        hdl = new CPushVideoCodec(instance, this);
        return ADDON_STATUS_OK;
    }
    return ADDON_STATUS_NOT_IMPLEMENTED;
}

// ======================================================================================================= the add-on
class CPushAddon : public kodi::addon::CAddonBase
{
public:
    ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance, KODI_ADDON_INSTANCE_HDL& hdl) override
    {
        if (instance.IsType(ADDON_INSTANCE_INPUTSTREAM)) {
            hdl = new CPushInputStream(instance);
            return ADDON_STATUS_OK;
        }
        return ADDON_STATUS_NOT_IMPLEMENTED;
    }
};

} // namespace

ADDONCREATOR(CPushAddon)
