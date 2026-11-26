#pragma once
// Fast start (PushMPC 1.0): the filter's own shaders compiled once and kept, so a new player session does not spend
// ~0.7 s in D3DCompile while MPC Video Renderer waits for the connection.
//
// The HLSL source is compiled per entry point in parallel on background threads as soon as the input pin connects
// (before MPC-VR asks for our device), and the bytecode is kept in %LOCALAPPDATA%\PushMPC\cache\filter-shaders-<key>.bin,
// keyed by the source, the filter version and the compiler: the next session reads the file instead of compiling.
// A missing, stale or damaged file is simply rebuilt; a shader not in the cache is compiled where it is used, as before.
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs {

inline uint64_t Fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// runs fn on its own thread with a reference on this DLL (a filter may be released while the thread still runs)
inline void Detach(std::function<void()> fn)
{
    struct C { std::function<void()> f; HMODULE self; };
    auto* c = new C{ std::move(fn), nullptr };
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Fnv1a), &c->self);
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        HMODULE self = nullptr;
        { std::unique_ptr<C> c(static_cast<C*>(p)); self = c->self; c->f(); }
        if (self) FreeLibraryAndExitThread(self, 0);
        return 0;
    }, c, 0, nullptr);
    if (t) CloseHandle(t);
    else { if (c->self) FreeLibrary(c->self); delete c; }
}

class ShaderCache {
public:
    using Entry = std::pair<std::string, std::string>;   // entry point, target
    using Logger = void (*)(const std::string&);

    // Starts once per process: loads the cache file, compiles what it lacks (in parallel) and rewrites it.
    void Start(const char* src, std::vector<Entry> entries, const std::wstring& dir, const char* version, Logger log)
    {
        std::lock_guard<std::mutex> lk(m_);
        if (started_) return;
        started_ = true;
        const size_t len = strlen(src);
        uint64_t key = Fnv1a(src, len);
        key = Fnv1a(version, strlen(version), key);
        key = Fnv1a(&kFormat, sizeof(kFormat), key);
        wchar_t sys[MAX_PATH] = {};
        GetSystemDirectoryW(sys, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA a = {};   // the compiler that made the bytecode (a Windows update can change it)
        if (GetFileAttributesExW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str(), GetFileExInfoStandard, &a)) {
            key = Fnv1a(&a.ftLastWriteTime, sizeof(a.ftLastWriteTime), key);
            key = Fnv1a(&a.nFileSizeLow, sizeof(a.nFileSizeLow), key);
        }
        if (!dir.empty()) { CreateDirectoryW(dir.c_str(), nullptr); file_ = dir + std::format(L"\\filter-shaders-{:016x}.bin", key); dir_ = dir; }
        Detach([this, src, entries = std::move(entries), log] { Run(src, entries, log); });
    }
    // The bytecode of an entry point: waits for the background work (at most `waitMs`); false = compile it yourself
    bool Get(const char* entry, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& out, DWORD waitMs = 3000)
    {
        std::unique_lock<std::mutex> lk(m_);
        if (!started_) return false;
        cv_.wait_for(lk, std::chrono::milliseconds(waitMs), [this] { return done_; });
        auto it = blobs_.find(std::string(entry) + "|" + target);
        if (it == blobs_.end()) return false;
        out = it->second;
        return true;
    }

private:
    static constexpr uint32_t kMagic = 0x53484D50, kFormat = 1;   // "PMHS"
    std::mutex m_;
    std::condition_variable cv_;
    bool started_ = false, done_ = false;
    std::wstring file_, dir_;
    std::map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>> blobs_;

    void Run(const char* src, const std::vector<Entry>& entries, Logger log)
    {
        const int64_t t0 = Now();
        std::map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>> got;
        const bool fromFile = Load(got);
        std::vector<const Entry*> missing;
        for (const auto& e : entries) if (!got.count(e.first + "|" + e.second)) missing.push_back(&e);
        size_t compiled = 0;
        if (!missing.empty()) {
            std::vector<Microsoft::WRL::ComPtr<ID3DBlob>> out(missing.size());
            std::vector<std::thread> pool;
            for (size_t i = 0; i < missing.size(); ++i)
                pool.emplace_back([&, i] {
                    Microsoft::WRL::ComPtr<ID3DBlob> err;
                    D3DCompile(src, strlen(src), "pushmpcfilter", nullptr, nullptr, missing[i]->first.c_str(), missing[i]->second.c_str(),
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out[i], &err);
                });
            for (auto& t : pool) t.join();
            for (size_t i = 0; i < missing.size(); ++i)
                if (out[i]) { got[missing[i]->first + "|" + missing[i]->second] = out[i]; ++compiled; }
            if (compiled) Save(got);
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            blobs_ = std::move(got);
            done_ = true;
        }
        cv_.notify_all();
        if (log) log(std::format("shaders ready in {:.0f} ms: {} of {} from the cache file{}, {} compiled", Ms(Now() - t0),
                                 entries.size() - missing.size(), entries.size(), fromFile ? "" : " (none yet)", compiled));
    }
    bool Load(std::map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>>& got)
    {
        if (file_.empty()) return false;
        HANDLE h = CreateFileW(file_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size = {};
        std::vector<uint8_t> b;
        if (GetFileSizeEx(h, &size) && size.QuadPart > 16 && size.QuadPart < (16 << 20)) {
            b.resize(size_t(size.QuadPart));
            DWORD n = 0;
            if (!ReadFile(h, b.data(), DWORD(b.size()), &n, nullptr) || n != b.size()) b.clear();
        }
        CloseHandle(h);
        // magic, format, count, then per entry: name length, name, size, bytes; a checksum over all of it at the end
        size_t p = 0;
        auto rd = [&](void* d, size_t n) { if (p + n > b.size()) return false; memcpy(d, b.data() + p, n); p += n; return true; };
        uint32_t magic = 0, format = 0, count = 0;
        if (b.size() < 20 || !rd(&magic, 4) || !rd(&format, 4) || !rd(&count, 4) || magic != kMagic || format != kFormat || count > 256) return false;
        uint64_t sum = 0;
        memcpy(&sum, b.data() + b.size() - 8, 8);
        if (sum != Fnv1a(b.data(), b.size() - 8)) return false;
        std::map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>> r;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t nl = 0, bl = 0;
            if (!rd(&nl, 4) || nl > 256 || p + nl > b.size()) return false;
            std::string name(reinterpret_cast<const char*>(b.data() + p), nl); p += nl;
            if (!rd(&bl, 4) || bl > (4 << 20) || p + bl > b.size() - 8) return false;
            Microsoft::WRL::ComPtr<ID3DBlob> blob;
            if (FAILED(D3DCreateBlob(bl, &blob))) return false;
            memcpy(blob->GetBufferPointer(), b.data() + p, bl); p += bl;
            r[name] = blob;
        }
        got = std::move(r);
        return true;
    }
    void Save(const std::map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>>& got)
    {
        if (file_.empty()) return;
        std::vector<uint8_t> b;
        auto wr = [&](const void* d, size_t n) { const auto* q = static_cast<const uint8_t*>(d); b.insert(b.end(), q, q + n); };
        const uint32_t count = uint32_t(got.size());
        wr(&kMagic, 4); wr(&kFormat, 4); wr(&count, 4);
        for (const auto& kv : got) {
            const uint32_t nl = uint32_t(kv.first.size()), bl = uint32_t(kv.second->GetBufferSize());
            wr(&nl, 4); wr(kv.first.data(), nl); wr(&bl, 4); wr(kv.second->GetBufferPointer(), bl);
        }
        const uint64_t sum = Fnv1a(b.data(), b.size());
        wr(&sum, 8);
        // older cache files (another version) go; the new one is written beside and moved into place
        WIN32_FIND_DATAW fd = {};
        HANDLE f = FindFirstFileW((dir_ + L"\\filter-shaders-*.bin").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do { const std::wstring p = dir_ + L"\\" + fd.cFileName; if (_wcsicmp(p.c_str(), file_.c_str()) != 0) DeleteFileW(p.c_str()); } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        const std::wstring tmp = file_ + std::format(L".{}.tmp", GetCurrentProcessId());
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD n = 0;
        const bool ok = WriteFile(h, b.data(), DWORD(b.size()), &n, nullptr) && n == b.size();
        CloseHandle(h);
        if (!ok || !MoveFileExW(tmp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
    }
    static int64_t Now() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
    static double Ms(int64_t d) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 * double(d) / double(f.QuadPart); }
};

inline ShaderCache& Shaders() { static ShaderCache c; return c; }

}  // namespace fs
