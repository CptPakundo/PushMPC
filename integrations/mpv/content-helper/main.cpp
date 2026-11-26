// pushmpc-content: the image model for plain mpv (pushmpc.lua when no PushMPC engine runs). pushmpc.lua starts it once
// per mpv (the subprocess command) and ends it with the player; it also ends by itself once that mpv is gone. It loads
// the content model once (pushmpc/content_model.h: CLIP ViT-B/32's image encoder through pmml, DirectML / CoreML / the
// CPU), takes the pictures pushmpc.lua hands over (content_helper.h: the protocol), integrates the evidence with
// ContentDetector's tracker semantics and writes the class, the CG sub-flag and the policy to the status file.
//   pushmpc-content --pid <mpv pid> [--tag <name>] [--dir <folder>]
// Files: <folder>/pushmpc-cd-<pid><tag>.in (pictures in), .st (status out); <folder>: TEMP / TMP (Windows), TMPDIR or
// /tmp elsewhere, as the lua's. --pid 0: no player to watch (tests). Without the model or its runtime the status says
// state=failed with the reason and the helper exits: the lua falls back to its own measure.
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include "content_helper.h"
#include "pushmpc/content_model.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace pushmpc::content;
using namespace pushmpc::content::mpvhelper;

namespace {

// the player this helper belongs to (pid 0: none)
class ParentWatch {
public:
    explicit ParentWatch(long pid) : m_pid(pid)
    {
#if defined(_WIN32)
        if (pid > 0) m_h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
#endif
    }
    ~ParentWatch()
    {
#if defined(_WIN32)
        if (m_h) CloseHandle(m_h);
#endif
    }
    bool alive() const
    {
        if (m_pid <= 0) return true;
#if defined(_WIN32)
        return m_h && WaitForSingleObject(m_h, 0) == WAIT_TIMEOUT;
#else
        return kill(pid_t(m_pid), 0) == 0 || errno == EPERM;
#endif
    }
private:
    long m_pid;
#if defined(_WIN32)
    HANDLE m_h = nullptr;
#endif
};

fs::path TempDir()
{
#if defined(_WIN32)
    for (const wchar_t* n : { L"TEMP", L"TMP" }) if (const wchar_t* v = _wgetenv(n); v && *v) return fs::path(v);
    return fs::temp_directory_path();
#else
    if (const char* v = std::getenv("TMPDIR"); v && *v) return fs::path(v);
    return fs::path("/tmp");
#endif
}

// the newest result with the CG probability when this ContentModel has it (Take(p, pts, pCg)), else without it
template <class M>
bool TakeResult(M& m, double& p, double& pts, double& pCg)
{
    if constexpr (requires { m.Take(p, pts, pCg); }) return m.Take(p, pts, pCg);
    else { pCg = -1; return m.Take(p, pts); }
}

int Run(const fs::path& dir, long pid, const std::string& tag)
{
    ParentWatch parent(pid);
    if (!parent.alive()) return 2;
    // never ahead of the player (the CPU fallback of the model uses several cores for a moment each picture)
#if defined(_WIN32)
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
    if (nice(10) == -1) { /* stays at the normal priority */ }
#endif
    const std::string base = "pushmpc-cd-" + std::to_string(pid) + tag;
    const fs::path in = dir / (base + ".in"), st = dir / (base + ".st");
    std::error_code ec;
    fs::remove(in, ec);

    StatusInfo info;
    ImageTracker tracker;
    CutMeter cuts;
    std::mutex lm;
    std::string logReady, logFail;
    ContentModel model([&](const std::string& m) {
        std::lock_guard<std::mutex> lk(lm);
        if (m.find(" not used (") != std::string::npos || m.find(" failed (") != std::string::npos) logFail = m; else logReady = m;
    });
    bool dirty = true;
    long long inFlight = -1;
    unsigned gen = 0, inFlightGen = 0;
    for (;;) {
        if (!parent.alive()) break;
        {
            std::lock_guard<std::mutex> lk(lm);
            if (!logReady.empty() && info.state == "loading") {
                const std::string pre = "content: image model ";
                info.state = "ready"; info.model = logReady.rfind(pre, 0) == 0 ? logReady.substr(pre.size()) : logReady; dirty = true;
            }
            if (model.Failed()) {
                info.state = "failed"; info.reason = logFail.empty() ? "the image model failed" : logFail;
                if (const size_t k = info.reason.find(": the hand-built cues decide"); k != std::string::npos) info.reason.erase(k);   // (the lua names its fallback)
                for (int k = 0; k < 40 && !WriteAtomic(st, StatusText(info, tracker)); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                return 0;
            }
        }
        if (fs::exists(in, ec)) {
            std::string bytes, err;
            const bool got = ReadFile(in, bytes);
            fs::remove(in, ec);   // consumed: the lua may send the next one
            Picture p;
            if (got && ParsePicture(bytes, p, &err)) {
                ++info.pictures;
                if (p.cut == 2) { tracker.Reset(); cuts.Reset(); ++gen; }   // a new file: a new decision
                const std::vector<float> rgb = Thumbnail(p);
                const bool cut = cuts.Push(rgb);
                info.cutL1 = cuts.last();
                if (p.cut == 1 || cut) tracker.Cut();
                if (inFlight >= 0) ++info.dropped;   // the previous one is still running
                else { model.Offer(ModelInput(rgb), p.pts); inFlight = p.seq; inFlightGen = gen; }
            } else if (got) { info.reason = "picture not used: " + err; dirty = true; }
        }
        double pr, pts, pcg;
        if (inFlight >= 0 && TakeResult(model, pr, pts, pcg)) {
            if (inFlightGen == gen) { tracker.Evidence(pr, pts, pcg); info.seq = inFlight; info.pImage = pr; info.pCg = pcg; }
            info.ms = model.LastMs();
            inFlight = -1; dirty = true;
        }
        if (dirty) dirty = !WriteAtomic(st, StatusText(info, tracker));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    fs::remove(in, ec); fs::remove(st, ec);
    return 0;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
{
    auto narrow = [](const std::wstring& w) { std::string s; for (wchar_t c : w) s += c < 128 ? char(c) : '_'; return s; };
    long pid = -1; std::string tag; fs::path dir;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i], v = argv[i + 1];
        if (k == L"--pid") pid = std::wcstol(v.c_str(), nullptr, 10);
        else if (k == L"--tag") tag = narrow(v);
        else if (k == L"--dir") dir = fs::path(v);
    }
#else
int main(int argc, char** argv)
{
    long pid = -1; std::string tag; fs::path dir;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--pid") pid = std::strtol(v.c_str(), nullptr, 10);
        else if (k == "--tag") tag = v;
        else if (k == "--dir") dir = fs::path(v);
    }
#endif
    if (pid < 0) { std::fprintf(stderr, "usage: pushmpc-content --pid <player pid> [--tag <name>] [--dir <folder>]\n"); return 1; }
    std::string t;
    for (char c : tag) if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') t += c;
    if (!t.empty()) t = "-" + t;
    return Run(dir.empty() ? TempDir() : dir, pid, t);
}
