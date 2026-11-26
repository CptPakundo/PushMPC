// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode, the Linux portal source: xdg-desktop-portal's ScreenCast interface (GNOME, KDE, wlroots,
// Hyprland, COSMIC; X11 desktops with a portal too) opens the desktop's own picker, the user picks the player's window
// once (the restore token remembers it: persist_mode 2), and the frames come over PipeWire:
//   - as a linear dma-buf when the GPU can import one (the engine reads it in place: no copy), else
//   - in shared memory (one copy into the engine's upload buffer).
// The cursor is left out (cursor_mode hidden). The newest buffer is held until the engine has read it, then given back
// (under the PipeWire thread loop's lock); older ones go back at once. Frame times: the buffer's PipeWire timestamp
// (CLOCK_MONOTONIC, the clock Now() reads) when it is plausible, else the arrival time.
#include "cap_platform.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dbus/dbus.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

namespace pmc {
namespace {

constexpr const char* kDest = "org.freedesktop.portal.Desktop";
constexpr const char* kPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";

// ------------------------------------------------------------------------------------------------ D-Bus helpers
// an a{sv} dictionary being appended
class Dict {
public:
    explicit Dict(DBusMessageIter* parent) : m_parent(parent) { dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, "{sv}", &m_arr); }
    void Str(const char* k, const char* v) { Entry(k, DBUS_TYPE_STRING_AS_STRING, DBUS_TYPE_STRING, &v); }
    void U32(const char* k, uint32_t v) { Entry(k, DBUS_TYPE_UINT32_AS_STRING, DBUS_TYPE_UINT32, &v); }
    void Bool(const char* k, bool b) { dbus_bool_t v = b ? TRUE : FALSE; Entry(k, DBUS_TYPE_BOOLEAN_AS_STRING, DBUS_TYPE_BOOLEAN, &v); }
    void Close() { dbus_message_iter_close_container(m_parent, &m_arr); }
private:
    void Entry(const char* k, const char* sig, int type, const void* v)
    {
        DBusMessageIter e, var;
        dbus_message_iter_open_container(&m_arr, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
        dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, sig, &var);
        dbus_message_iter_append_basic(&var, type, v);
        dbus_message_iter_close_container(&e, &var);
        dbus_message_iter_close_container(&m_arr, &e);
    }
    DBusMessageIter* m_parent;
    DBusMessageIter m_arr;
};

// walks an a{sv}: f(key, the variant's content)
template <class F> void ForEachEntry(DBusMessageIter* dict, F&& f)
{
    if (dbus_message_iter_get_arg_type(dict) != DBUS_TYPE_ARRAY) return;
    DBusMessageIter arr;
    dbus_message_iter_recurse(dict, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter e, var;
        dbus_message_iter_recurse(&arr, &e);
        const char* key = nullptr;
        dbus_message_iter_get_basic(&e, &key);
        dbus_message_iter_next(&e);
        if (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&e, &var);
            f(std::string(key ? key : ""), &var);
        }
        dbus_message_iter_next(&arr);
    }
}

std::string StringOf(DBusMessageIter* v)
{
    const int t = dbus_message_iter_get_arg_type(v);
    if (t != DBUS_TYPE_STRING && t != DBUS_TYPE_OBJECT_PATH) return {};
    const char* s = nullptr;
    dbus_message_iter_get_basic(v, &s);
    return s ? s : "";
}

std::string StateDir()
{
    const char* x = std::getenv("XDG_STATE_HOME");
    const char* h = std::getenv("HOME");
    std::string d = x && *x == '/' ? std::string(x) + "/pushmpc" : h ? std::string(h) + "/.local/state/pushmpc" : std::string();
    return d;
}

// the restore token's file: --restore-token, else one per player under the state folder
std::string TokenFile(const Options& o)
{
    if (!o.restoreToken.empty()) return o.restoreToken;
    const std::string d = StateDir();
    if (d.empty()) return {};
    std::string key = o.monitor ? "monitor" : o.player.empty() ? "window" : o.player;
    for (auto& c : key) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    return d + "/capture-" + key + ".token";
}

void MkdirP(const std::string& dir)
{
    for (size_t i = 1; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0700);
}

// ------------------------------------------------------------------------------------------------ the source
class PortalSource final : public Source {
public:
    explicit PortalSource(const Options& o) : m_o(o) {}
    ~PortalSource() override;
    bool Start(std::string& err) override;
    bool Next(SourceFrame& f, int timeoutMs) override;
    bool Alive(std::string& why) override;
    std::string Describe() const override;
    std::string PlayerName() const override { return m_o.player; }
    bool DmabufCapable() const override { return m_dmabuf; }

    // PipeWire's thread
    void OnParam(uint32_t id, const spa_pod* param);
    void OnProcess();
    void OnState(pw_stream_state s, const char* error);
    void OnRemoveBuffer(pw_buffer* b);

private:
    bool Connect(std::string& err);
    uint32_t Version();
    // a portal method that answers through a Request object: args appends the arguments (token: the handle_token to
    // put into the options); results gets the Response's a{sv}. response: 0 ok, 1 cancelled, 2 other
    bool Request(const char* method, const std::function<void(DBusMessageIter*, const char* token)>& args,
                 const std::function<void(DBusMessageIter*)>& results, uint32_t& response, std::string& err, int timeoutMs);
    bool StartPipeWire(int fd, std::string& err);
    void Requeue(pw_buffer* b);

    Options m_o;
    DBusConnection* m_bus = nullptr;
    std::string m_sender, m_session, m_restore;
    uint32_t m_node = 0, m_sourceType = 0;
    int m_counter = 0, m_streamW = 0, m_streamH = 0;
    bool m_closed = false;

    pw_thread_loop* m_loop = nullptr;
    pw_context* m_ctx = nullptr;
    pw_core* m_core = nullptr;
    pw_stream* m_stream = nullptr;
    spa_hook m_listener{};
    pw_stream_events m_events{};

    std::mutex m_mx;
    std::condition_variable m_cv;
    struct Ready { pw_buffer* b = nullptr; double t = 0; };
    Ready m_ready;
    int m_lent = 0;
    bool m_failed = false, m_dmabuf = false, m_formatDmabuf = false, m_pq = false;
    std::string m_why;
    uint32_t m_format = 0;
    int m_w = 0, m_h = 0;
    std::map<int, std::pair<void*, size_t>> m_maps;   // dma-buf fd -> its read-only mapping (host engines, checks)
    uint64_t m_frames = 0, m_dmabufFrames = 0;
};

void Param(void* d, uint32_t id, const spa_pod* p) { static_cast<PortalSource*>(d)->OnParam(id, p); }
void Process(void* d) { static_cast<PortalSource*>(d)->OnProcess(); }
void State(void* d, pw_stream_state, pw_stream_state s, const char* e) { static_cast<PortalSource*>(d)->OnState(s, e); }
void RemoveBuffer(void* d, pw_buffer* b) { static_cast<PortalSource*>(d)->OnRemoveBuffer(b); }

PortalSource::~PortalSource()
{
    if (m_loop) {
        pw_thread_loop_lock(m_loop);
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (m_ready.b && m_stream) pw_stream_queue_buffer(m_stream, m_ready.b);
            m_ready = {};
        }
        if (m_stream) { pw_stream_disconnect(m_stream); pw_stream_destroy(m_stream); }
        if (m_core) pw_core_disconnect(m_core);
        pw_thread_loop_unlock(m_loop);
        pw_thread_loop_stop(m_loop);
        if (m_ctx) pw_context_destroy(m_ctx);
        pw_thread_loop_destroy(m_loop);
    }
    for (auto& m : m_maps) munmap(m.second.first, m.second.second);
    if (m_bus) {
        if (!m_session.empty()) {
            DBusMessage* msg = dbus_message_new_method_call(kDest, m_session.c_str(), "org.freedesktop.portal.Session", "Close");
            if (msg) { dbus_connection_send(m_bus, msg, nullptr); dbus_connection_flush(m_bus); dbus_message_unref(msg); }
        }
        dbus_connection_close(m_bus);
        dbus_connection_unref(m_bus);
    }
}

bool PortalSource::Connect(std::string& err)
{
    DBusError e;
    dbus_error_init(&e);
    m_bus = dbus_bus_get_private(DBUS_BUS_SESSION, &e);
    if (!m_bus) { err = std::string("no session bus (") + (e.message ? e.message : "?") + ")"; dbus_error_free(&e); return false; }
    dbus_connection_set_exit_on_disconnect(m_bus, FALSE);
    const char* unique = dbus_bus_get_unique_name(m_bus);
    m_sender = unique ? unique + (unique[0] == ':' ? 1 : 0) : "";
    for (auto& c : m_sender) if (c == '.') c = '_';
    const std::string closed = "type='signal',interface='org.freedesktop.portal.Session',member='Closed'";
    dbus_bus_add_match(m_bus, closed.c_str(), nullptr);
    return true;
}

uint32_t PortalSource::Version()
{
    DBusMessage* msg = dbus_message_new_method_call(kDest, kPath, "org.freedesktop.DBus.Properties", "Get");
    const char* iface = kScreenCast;
    const char* prop = "version";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    DBusError e;
    dbus_error_init(&e);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(m_bus, msg, 5000, &e);
    dbus_message_unref(msg);
    uint32_t v = 0;
    if (r) {
        DBusMessageIter it, var;
        if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&it, &var);
            if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_UINT32) dbus_message_iter_get_basic(&var, &v);
        }
        dbus_message_unref(r);
    }
    dbus_error_free(&e);
    return v;
}

bool PortalSource::Request(const char* method, const std::function<void(DBusMessageIter*, const char*)>& args,
                           const std::function<void(DBusMessageIter*)>& results, uint32_t& response, std::string& err, int timeoutMs)
{
    const std::string token = "pushmpc" + std::to_string(++m_counter);
    std::string handle = "/org/freedesktop/portal/desktop/request/" + m_sender + "/" + token;
    // subscribe before the call (the answer can come before the reply)
    const std::string match = "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" + handle + "'";
    dbus_bus_add_match(m_bus, match.c_str(), nullptr);
    DBusMessage* msg = dbus_message_new_method_call(kDest, kPath, kScreenCast, method);
    DBusMessageIter it;
    dbus_message_iter_init_append(msg, &it);
    args(&it, token.c_str());
    DBusError e;
    dbus_error_init(&e);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_bus, msg, 30000, &e);
    dbus_message_unref(msg);
    if (!reply) {
        err = std::string("the screen-capture portal refused ") + method + " (" + (e.message ? e.message : "no answer") + ")";
        dbus_error_free(&e);
        dbus_bus_remove_match(m_bus, match.c_str(), nullptr);
        return false;
    }
    const char* path = nullptr;
    if (dbus_message_get_args(reply, &e, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID) && path && handle != path) {
        // an old portal names the request itself
        dbus_bus_remove_match(m_bus, match.c_str(), nullptr);
        handle = path;
        dbus_bus_add_match(m_bus, ("type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" + handle + "'").c_str(), nullptr);
    }
    dbus_error_free(&e);
    dbus_message_unref(reply);
    const double end = Now() + timeoutMs * 1e-3;
    bool got = false;
    while (!got && Now() < end) {
        dbus_connection_read_write(m_bus, 100);
        while (DBusMessage* m = dbus_connection_pop_message(m_bus)) {
            if (dbus_message_is_signal(m, "org.freedesktop.portal.Request", "Response") && dbus_message_get_path(m) && handle == dbus_message_get_path(m)) {
                DBusMessageIter ri;
                if (dbus_message_iter_init(m, &ri) && dbus_message_iter_get_arg_type(&ri) == DBUS_TYPE_UINT32) {
                    dbus_message_iter_get_basic(&ri, &response);
                    dbus_message_iter_next(&ri);
                    if (response == 0) results(&ri);
                    got = true;
                }
            } else if (dbus_message_is_signal(m, "org.freedesktop.portal.Session", "Closed")) {
                m_closed = true;
            }
            dbus_message_unref(m);
        }
    }
    dbus_bus_remove_match(m_bus, ("type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" + handle + "'").c_str(), nullptr);
    if (!got) { err = std::string("the screen-capture portal did not answer ") + method; return false; }
    return true;
}

bool PortalSource::Start(std::string& err)
{
    if (!Connect(err)) return false;
    const uint32_t version = Version();
    if (version == 0) { err = "no screen-capture portal (install xdg-desktop-portal and your desktop's portal backend)"; return false; }
    uint32_t resp = 0;
    // 1. the session
    if (!Request("CreateSession", [&](DBusMessageIter* it, const char* token) {
            Dict d(it); d.Str("handle_token", token); d.Str("session_handle_token", ("pushmpcs" + std::to_string(m_counter)).c_str()); d.Close();
        }, [&](DBusMessageIter* r) {
            ForEachEntry(r, [&](const std::string& k, DBusMessageIter* v) { if (k == "session_handle") m_session = StringOf(v); });
        }, resp, err, 30000)) return false;
    if (resp != 0 || m_session.empty()) { err = "the screen-capture portal did not open a session"; return false; }
    // 2. what to capture: one window (or a monitor: --monitor), no cursor, remembered for next time
    std::string token;
    const std::string tokenFile = TokenFile(m_o);
    if (!tokenFile.empty()) { std::ifstream f(tokenFile); std::getline(f, token); }
    const char* session = m_session.c_str();
    if (!Request("SelectSources", [&](DBusMessageIter* it, const char* tk) {
            dbus_message_iter_append_basic(it, DBUS_TYPE_OBJECT_PATH, &session);
            Dict d(it);
            d.Str("handle_token", tk);
            d.U32("types", m_o.monitor ? 1u : 2u);   // MONITOR 1, WINDOW 2
            d.Bool("multiple", false);
            if (version >= 2) d.U32("cursor_mode", 1u);   // hidden
            if (version >= 4) { d.U32("persist_mode", 2u); if (!token.empty()) d.Str("restore_token", token.c_str()); }
            d.Close();
        }, [](DBusMessageIter*) {}, resp, err, 30000)) return false;
    if (resp != 0) { err = "the screen-capture portal did not take the request (this desktop may not offer window capture: try --monitor)"; return false; }
    // 3. the picker (the user's choice; up to 5 minutes)
    if (!Request("Start", [&](DBusMessageIter* it, const char* tk) {
            const char* parent = "";
            dbus_message_iter_append_basic(it, DBUS_TYPE_OBJECT_PATH, &session);
            dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &parent);
            Dict d(it); d.Str("handle_token", tk); d.Close();
        }, [&](DBusMessageIter* r) {
            ForEachEntry(r, [&](const std::string& k, DBusMessageIter* v) {
                if (k == "restore_token") m_restore = StringOf(v);
                if (k != "streams" || dbus_message_iter_get_arg_type(v) != DBUS_TYPE_ARRAY) return;
                DBusMessageIter arr, st;
                dbus_message_iter_recurse(v, &arr);
                if (dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_STRUCT) return;
                dbus_message_iter_recurse(&arr, &st);
                if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_UINT32) dbus_message_iter_get_basic(&st, &m_node);
                dbus_message_iter_next(&st);
                ForEachEntry(&st, [&](const std::string& pk, DBusMessageIter* pv) {
                    if (pk == "source_type" && dbus_message_iter_get_arg_type(pv) == DBUS_TYPE_UINT32) dbus_message_iter_get_basic(pv, &m_sourceType);
                    if (pk == "size" && dbus_message_iter_get_arg_type(pv) == DBUS_TYPE_STRUCT) {
                        DBusMessageIter s;
                        dbus_message_iter_recurse(pv, &s);
                        int32_t w = 0, h = 0;
                        if (dbus_message_iter_get_arg_type(&s) == DBUS_TYPE_INT32) dbus_message_iter_get_basic(&s, &w);
                        dbus_message_iter_next(&s);
                        if (dbus_message_iter_get_arg_type(&s) == DBUS_TYPE_INT32) dbus_message_iter_get_basic(&s, &h);
                        m_streamW = w; m_streamH = h;
                    }
                });
            });
        }, resp, err, 300000)) return false;
    if (resp == 1) { err = "no window was picked (the picker was closed)"; return false; }
    if (resp != 0 || m_node == 0) { err = "the screen-capture portal did not start the capture"; return false; }
    if (!tokenFile.empty() && !m_restore.empty()) {
        MkdirP(tokenFile.substr(0, tokenFile.rfind('/')));
        std::ofstream f(tokenFile, std::ios::trunc);
        f << m_restore << "\n";
    }
    // 4. PipeWire
    DBusMessage* msg = dbus_message_new_method_call(kDest, kPath, kScreenCast, "OpenPipeWireRemote");
    DBusMessageIter it;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &session);
    { Dict d(&it); d.Close(); }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_bus, msg, 30000, &e);
    dbus_message_unref(msg);
    int fd = -1;
    if (reply) { dbus_message_get_args(reply, &e, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID); dbus_message_unref(reply); }
    if (fd < 0) { err = std::string("the screen-capture portal gave no PipeWire connection (") + (e.message ? e.message : "?") + ")"; dbus_error_free(&e); return false; }
    dbus_error_free(&e);
    return StartPipeWire(fd, err);
}

// SMPTE ST 2084 (PQ) in enum spa_video_transfer_function: named in PipeWire's headers from 1.6, the same value
// before (what Sunshine's PipeWire capture defines for older headers)
constexpr uint32_t kTransferPq = 14;

// hdr: 10-bit R'G'B' in BT.2020 PQ (colour primaries and transfer mandatory: only a compositor that really delivers
// PQ takes it); else 8-bit sRGB
const spa_pod* BuildFormat(spa_pod_builder* b, bool linear, bool hdr)
{
    spa_rectangle def{ 1920, 1080 }, mn{ 1, 1 }, mx{ 16384, 16384 };
    spa_fraction f0{ 0, 1 }, fmax{ 360, 1 };
    spa_pod_frame f;
    spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        0);
    if (hdr) {
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_xBGR_210LE, SPA_VIDEO_FORMAT_xBGR_210LE,
                            SPA_VIDEO_FORMAT_ABGR_210LE, SPA_VIDEO_FORMAT_xRGB_210LE, SPA_VIDEO_FORMAT_ARGB_210LE), 0);
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_colorPrimaries, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_id(b, SPA_VIDEO_COLOR_PRIMARIES_BT2020);
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_transferFunction, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_id(b, kTransferPq);
    } else {
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                            SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA), 0);
    }
    spa_pod_builder_add(b,
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def, &mn, &mx),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&f0, &f0, &fmax),
        0);
    if (linear) {   // a linear dma-buf (DRM_FORMAT_MOD_LINEAR = 0): what the engine's storage-buffer import reads
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_long(b, 0);
    }
    return static_cast<const spa_pod*>(spa_pod_builder_pop(b, &f));
}

bool PortalSource::StartPipeWire(int fd, std::string& err)
{
    pw_init(nullptr, nullptr);
    m_loop = pw_thread_loop_new("pushmpc-capture", nullptr);
    if (!m_loop) { close(fd); err = "PipeWire: no thread loop"; return false; }
    m_ctx = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
    if (!m_ctx || pw_thread_loop_start(m_loop) < 0) { close(fd); err = "PipeWire: no context"; return false; }
    pw_thread_loop_lock(m_loop);
    m_core = pw_context_connect_fd(m_ctx, fcntl(fd, F_DUPFD_CLOEXEC, 3), nullptr, 0);
    close(fd);
    if (!m_core) { pw_thread_loop_unlock(m_loop); err = "PipeWire: cannot connect to the portal's remote"; return false; }
    m_stream = pw_stream_new(m_core, "pushmpc-capture",
                             pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    if (!m_stream) { pw_thread_loop_unlock(m_loop); err = "PipeWire: no stream"; return false; }
    m_events = pw_stream_events{};
    m_events.version = PW_VERSION_STREAM_EVENTS;
    m_events.state_changed = State;
    m_events.param_changed = Param;
    m_events.process = Process;
    m_events.remove_buffer = RemoveBuffer;
    pw_stream_add_listener(m_stream, &m_listener, &m_events, this);
    uint8_t buf[4096];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const spa_pod* params[4];
    uint32_t n = 0;
    // 10-bit BT.2020 PQ first (an HDR desktop's compositor that delivers PQ), then 8-bit sRGB (what KWin, Mutter and
    // wlroots deliver today); each as a linear dma-buf first (zero copy when the GPU imports it), then shared memory.
    // An HDR capture on an SDR overlay is brought down to SDR by the presenter. PUSHMPC_CAPTURE_SDR=1: 8 bit only.
    const bool dmabuf = !std::getenv("PUSHMPC_CAPTURE_NO_DMABUF"), hdr = !std::getenv("PUSHMPC_CAPTURE_SDR");
    for (const bool h : { true, false }) {
        if (h && !hdr) continue;
        if (dmabuf) params[n++] = BuildFormat(&b, true, h);
        params[n++] = BuildFormat(&b, false, h);
    }
    const int r = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, m_node, static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, n);
    pw_thread_loop_unlock(m_loop);
    if (r < 0) { err = std::string("PipeWire: cannot connect the stream (") + spa_strerror(r) + ")"; return false; }
    return true;
}

void PortalSource::OnParam(uint32_t id, const spa_pod* param)
{
    if (!param || id != SPA_PARAM_Format) return;
    uint32_t mt = 0, mst = 0;
    if (spa_format_parse(param, &mt, &mst) < 0 || mt != SPA_MEDIA_TYPE_video || mst != SPA_MEDIA_SUBTYPE_raw) return;
    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(param, &info) < 0) return;
    const bool dmabuf = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr;
    const bool ten = info.format == SPA_VIDEO_FORMAT_xBGR_210LE || info.format == SPA_VIDEO_FORMAT_ABGR_210LE || info.format == SPA_VIDEO_FORMAT_xRGB_210LE
                     || info.format == SPA_VIDEO_FORMAT_ARGB_210LE;
    const bool pq = ten && uint32_t(info.transfer_function) == kTransferPq && info.color_primaries == SPA_VIDEO_COLOR_PRIMARIES_BT2020;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_format = info.format; m_w = int(info.size.width); m_h = int(info.size.height); m_formatDmabuf = dmabuf; m_pq = pq;
    }
    std::fprintf(stderr, "pushmpc-capture: the portal's stream: %dx%d, %s%s\n", int(info.size.width), int(info.size.height),
                 pq ? "10-bit BT.2020 PQ (HDR)" : ten ? "10 bit without PQ (shown as SDR)" : "8-bit sRGB (SDR)", dmabuf ? ", dma-buf" : ", shared memory");
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const spa_pod* params[3];
    const int types = dmabuf ? (1 << SPA_DATA_DmaBuf) : ((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd));
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(6, 3, 16),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(types)));
    params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(int(sizeof(spa_meta_header)))));
    params[2] = static_cast<const spa_pod*>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop), SPA_PARAM_META_size, SPA_POD_Int(int(sizeof(spa_meta_region)))));
    pw_stream_update_params(m_stream, params, 3);
}

void PortalSource::OnState(pw_stream_state s, const char* error)
{
    std::lock_guard<std::mutex> lk(m_mx);
    if (s == PW_STREAM_STATE_ERROR) { m_failed = true; m_why = std::string("the capture stream failed (") + (error ? error : "?") + ")"; }
    else if (s == PW_STREAM_STATE_UNCONNECTED) { m_failed = true; if (m_why.empty()) m_why = "the capture stream ended (the window closed, or capture was stopped)"; }
    m_cv.notify_all();
}

void PortalSource::OnRemoveBuffer(pw_buffer* b)
{
    if (!b || !b->buffer || b->buffer->n_datas < 1) return;
    const spa_data& d = b->buffer->datas[0];
    if (d.type != SPA_DATA_DmaBuf) return;
    std::lock_guard<std::mutex> lk(m_mx);
    auto it = m_maps.find(int(d.fd));
    if (it != m_maps.end()) { munmap(it->second.first, it->second.second); m_maps.erase(it); }
}

void PortalSource::OnProcess()
{
    pw_buffer* newest = nullptr;
    while (pw_buffer* b = pw_stream_dequeue_buffer(m_stream)) {
        if (newest) pw_stream_queue_buffer(m_stream, newest);   // only the newest picture matters
        newest = b;
    }
    if (!newest) return;
    spa_buffer* sb = newest->buffer;
    auto* h = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(sb, SPA_META_Header, sizeof(spa_meta_header)));
    bool bad = sb->n_datas < 1 || (h && (h->flags & SPA_META_HEADER_FLAG_CORRUPTED));
    if (!bad) {
        const spa_data& d = sb->datas[0];
        if (d.type != SPA_DATA_DmaBuf && (!d.data || !d.chunk || d.chunk->size == 0)) bad = true;   // no new picture (cursor-only update)
#ifdef SPA_CHUNK_FLAG_CORRUPTED
        if (d.chunk && (d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) bad = true;
#endif
    }
    if (bad) { pw_stream_queue_buffer(m_stream, newest); return; }
    double t = Now();
    if (h && h->pts > 0) {
        const double p = double(h->pts) * 1e-9;
        if (std::fabs(p - t) < 0.5) t = p;
    }
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_ready.b) pw_stream_queue_buffer(m_stream, m_ready.b);   // the main thread did not take it in time
    m_ready = { newest, t };
    m_cv.notify_one();
}

void PortalSource::Requeue(pw_buffer* b)
{
    pw_thread_loop_lock(m_loop);
    if (m_stream) pw_stream_queue_buffer(m_stream, b);
    pw_thread_loop_unlock(m_loop);
    std::lock_guard<std::mutex> lk(m_mx);
    --m_lent;
}

bool PortalSource::Next(SourceFrame& f, int timeoutMs)
{
    std::unique_lock<std::mutex> lk(m_mx);
    if (!m_ready.b) m_cv.wait_for(lk, std::chrono::milliseconds(std::max(0, timeoutMs)), [&] { return m_ready.b != nullptr || m_failed; });
    if (!m_ready.b) return false;
    const Ready r = m_ready;
    m_ready = {};
    spa_buffer* sb = r.b->buffer;
    const spa_data& d = sb->datas[0];
    int x = 0, y = 0, w = m_w, h = m_h;
    if (auto* crop = static_cast<spa_meta_region*>(spa_buffer_find_meta_data(sb, SPA_META_VideoCrop, sizeof(spa_meta_region)))) {
        if (crop->region.size.width > 0 && crop->region.size.height > 0) {
            x = crop->region.position.x; y = crop->region.position.y;
            w = int(crop->region.size.width); h = int(crop->region.size.height);
        }
    }
    const int stride = d.chunk && d.chunk->stride > 0 ? d.chunk->stride : m_w * 4;
    const uint32_t off = d.chunk ? d.chunk->offset : 0;
    x = std::clamp(x, 0, std::max(0, m_w - 1)); y = std::clamp(y, 0, std::max(0, m_h - 1));
    w = std::min(w, m_w - x); h = std::min(h, m_h - y);
    f = SourceFrame{};
    f.width = w; f.height = h;
    // xRGB_210LE = DRM XRGB2101010 (B in the low 10 bits), xBGR_210LE = XBGR2101010 (R in the low bits)
    if (m_pq) f.fmt = m_format == SPA_VIDEO_FORMAT_xBGR_210LE || m_format == SPA_VIDEO_FORMAT_ABGR_210LE ? PixFmt::XBGR10 : PixFmt::RGB10;
    else f.fmt = m_format == SPA_VIDEO_FORMAT_RGBx || m_format == SPA_VIDEO_FORMAT_RGBA ? PixFmt::RGBX : PixFmt::BGRX;
    f.pitch = stride;
    f.time = r.t;
    const uint64_t first = uint64_t(off) + uint64_t(y) * uint64_t(stride) + uint64_t(x) * 4;
    if (d.type == SPA_DATA_DmaBuf) {
        f.dmabufFd = int(d.fd);
        f.dmabufOffset = first;
        f.dmabufSize = std::max<uint64_t>(d.maxsize, uint64_t(off) + uint64_t(stride) * uint64_t(m_h));
        struct stat st {};
        if (fstat(f.dmabufFd, &st) == 0) f.dmabufInode = uint64_t(st.st_ino);
        // a read-only mapping for the host engines and the black-picture check (a linear buffer maps as it is)
        auto it = m_maps.find(f.dmabufFd);
        if (it == m_maps.end()) {
            const off_t total = lseek(f.dmabufFd, 0, SEEK_END);
            void* p = total > 0 ? mmap(nullptr, size_t(total), PROT_READ, MAP_SHARED, f.dmabufFd, 0) : MAP_FAILED;
            if (p != MAP_FAILED) it = m_maps.emplace(f.dmabufFd, std::make_pair(p, size_t(total))).first;
        }
        if (it != m_maps.end() && first + uint64_t(stride) * uint64_t(h) <= it->second.second) f.ptr = static_cast<const uint8_t*>(it->second.first) + first;
        ++m_dmabufFrames;
    } else {
        f.ptr = static_cast<const uint8_t*>(d.data) + first;
    }
    ++m_frames;
    ++m_lent;
    pw_buffer* b = r.b;
    f.release = [this, b] { Requeue(b); };
    m_dmabuf = m_formatDmabuf;
    return w > 0 && h > 0;
}

bool PortalSource::Alive(std::string& why)
{
    if (m_bus) {
        dbus_connection_read_write(m_bus, 0);
        while (DBusMessage* m = dbus_connection_pop_message(m_bus)) {
            if (dbus_message_is_signal(m, "org.freedesktop.portal.Session", "Closed")) m_closed = true;
            dbus_message_unref(m);
        }
    }
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_closed) { why = "capture was stopped (the desktop closed the screen-capture session)"; return false; }
    if (m_failed) { why = m_why; return false; }
    return true;
}

std::string PortalSource::Describe() const
{
    char b[256];
    std::snprintf(b, sizeof(b), "the screen-capture portal (%s, PipeWire node %u), %dx%d, %s", m_sourceType == 1 ? "a monitor" : "a window", m_node, m_streamW, m_streamH,
                  m_formatDmabuf ? (m_pq ? "linear dma-buf, 10-bit BT.2020 PQ" : "linear dma-buf") : (m_pq ? "shared memory, 10-bit BT.2020 PQ" : "shared memory"));
    return b;
}

} // namespace

std::unique_ptr<Source> MakePortalSource(const Options& o) { return std::make_unique<PortalSource>(o); }

} // namespace pmc
