#pragma once
// pushcore's side of the ZLUDA VSR worker (pushmpc/include/pushmpc/zluda_vsr.h): starts pushcore_zluda_vsr_worker.exe in
// a job object, hands it shared RGBA8 textures and fences, and runs one VSR per source frame with no CPU wait on the
// GPU. One frame is in flight: the reply of frame n is read before frame n+1 is sent.
//
// When the worker fails, exits or hangs (no reply within frameTimeoutMs), the client is "lost": the worker is killed,
// the done fence is signalled from the CPU so no queue waits forever, and run() returns false. The graph then takes its
// plain resize for that frame and every later one (never a black frame), and pushcore switches the stream to FSR at its
// next rebuild.
//
// Only D3D12 and Win32: pushmpc\tools\zluda_vsr_test.cpp drives it with the stub worker on WARP.
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "pushmpc/zluda_vsr.h"

namespace pmz {

class WorkerClient {
public:
    struct Config {
        std::wstring worker;                    // pushcore_zluda_vsr_worker.exe
        std::wstring zluda, vsr, cache, log;    // the Init paths (log: the worker's log file)
        uint32_t inW = 0, inH = 0, outW = 0, outH = 0, quality = 1, layout = 0;
        DWORD initTimeoutMs = 180000;           // Init includes loading ZLUDA and VSR's first create
        DWORD frameTimeoutMs = 3000;            // a frame's reply later than this = the worker hangs
    };
    WorkerClient() = default;
    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;
    ~WorkerClient() { Close(); }

    bool Open(ID3D12Device* device, const Config& cfg)
    {
        Close();
        cfg_ = cfg;
        dev_ = device;
        lost_ = false; error_.clear(); sent_ = 0; replied_ = 0; lastMs_ = -1;
        if (!device) return Fail("no device");
        if (!ValidExtent(cfg.inW, cfg.inH, cfg.outW, cfg.outH)) return Fail(std::format("size {}x{} -> {}x{} is outside RTX VSR's range", cfg.inW, cfg.inH, cfg.outW, cfg.outH));
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto tex = [&](UINT w, UINT h, Microsoft::WRL::ComPtr<ID3D12Resource>& r) {
            D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)));
        };
        if (!tex(cfg.inW, cfg.inH, in_) || !tex(cfg.outW, cfg.outH, out_)) return Fail("shared VSR textures could not be created");
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&producer_))) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&done_))))
            return Fail("shared VSR fences could not be created");
        // pipes (only the child's ends are inherited), the worker suspended until its job holds it
        HANDLE readChild = nullptr, writeChild = nullptr;
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        if (!CreatePipe(&readChild, &toWorker_, &sa, 65536) || !CreatePipe(&fromWorker_, &writeChild, &sa, 65536)) { Drop(readChild); Drop(writeChild); return Fail("worker pipes could not be created"); }
        SetHandleInformation(toWorker_, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(fromWorker_, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si);
        SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> storage(bytes);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        HANDLE inherit[] = { readChild, writeChild };
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes) ||
            !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)) {
            Drop(readChild); Drop(writeChild); return Fail("worker handle list failed");
        }
        std::wstring cmd = std::format(L"\"{}\" {} {}", cfg.worker, uintptr_t(readChild), uintptr_t(writeChild));
        PROCESS_INFORMATION pi{};
        const bool created = CreateProcessW(cfg.worker.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                            nullptr, nullptr, &si.StartupInfo, &pi) != FALSE;
        const DWORD launchError = GetLastError();
        DeleteProcThreadAttributeList(si.lpAttributeList); Drop(readChild); Drop(writeChild);
        if (!created) return Fail(std::format("the worker did not start (win32 {})", launchError));
        process_ = pi.hProcess; pid_ = pi.dwProcessId;
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{}; lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &lim, sizeof(lim)) || !AssignProcessToJobObject(job_, process_)) {
            TerminateProcess(process_, 126); Drop(pi.hThread); return Fail("the worker could not be put in its job");
        }
        Init init = MakeInit();
        const LUID l = device->GetAdapterLuid();
        init.adapterLuid = (uint64_t(uint32_t(l.HighPart)) << 32) | l.LowPart;
        init.inW = cfg.inW; init.inH = cfg.inH; init.outW = cfg.outW; init.outH = cfg.outH; init.quality = cfg.quality; init.layout = cfg.layout;
        PutPath(init.zluda, cfg.zluda); PutPath(init.vsr, cfg.vsr); PutPath(init.cache, cfg.cache); PutPath(init.log, cfg.log);
        ID3D12DeviceChild* objs[4] = { in_.Get(), out_.Get(), producer_.Get(), done_.Get() };
        for (int i = 0; i < 4; ++i) {
            HANDLE local = nullptr, remote = nullptr;
            const HRESULT hr = device->CreateSharedHandle(objs[i], nullptr, GENERIC_ALL, nullptr, &local);
            const bool ok = SUCCEEDED(hr) && DuplicateHandle(GetCurrentProcess(), local, process_, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS);
            Drop(local);
            if (!ok) { TerminateProcess(process_, 126); Drop(pi.hThread); return Fail(std::format("shared handle {} could not be passed (hr 0x{:08X})", i, unsigned(hr))); }
            init.handles[i] = uintptr_t(remote);
        }
        ResumeThread(pi.hThread); Drop(pi.hThread);
        if (!WriteAll(&init, sizeof(init))) return Fail("the worker did not take its initialisation");
        Reply r;
        if (!ReadReply(r, cfg.initTimeoutMs)) return Fail("the worker did not answer its initialisation" + ExitNote());
        if (r.result != 0 || r.h.sequence != 0) return Fail("the worker refused: " + std::string(r.message.data()));
        reader_ = std::thread([this] { ReaderLoop(); });
        watchdog_ = std::thread([this] { WatchLoop(); });
        open_ = true;
        return true;
    }
    // src: RGBA8 in COPY_SOURCE, the size of the input
    bool CopyIn(ID3D12GraphicsCommandList* list, ID3D12Resource* src)
    {
        if (!open_ || lost_) return false;
        Transition(list, in_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(in_.Get(), src);
        Transition(list, in_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        return true;
    }
    // after the list holding CopyIn() was submitted on this queue: producer signal, the worker's VSR, done wait (GPU)
    bool Run(ID3D12CommandQueue* queue)
    {
        if (!open_ || lost_) return false;
        const uint64_t n = sent_ + 1;
        // one frame in flight: frame n-1's reply first (the reader thread has it, or the watchdog gives up on it)
        {
            std::unique_lock lk(m_);
            if (!cv_.wait_for(lk, std::chrono::milliseconds(cfg_.frameTimeoutMs), [&] { return lost_.load() || replied_ >= n - 1; })) {
                lk.unlock(); MarkLost(std::format("no answer for frame {} within {} ms", n - 1, cfg_.frameTimeoutMs)); return false;
            }
        }
        if (lost_) return false;
        if (FAILED(queue->Signal(producer_.Get(), n))) { MarkLost("producer fence signal failed"); return false; }
        const Evaluate e = MakeEvaluate(n);
        {
            std::lock_guard lk(m_);
            sent_ = n; sentAt_ = std::chrono::steady_clock::now();
        }
        if (!WriteAll(&e, sizeof(e))) { MarkLost("the worker pipe is closed" + ExitNote()); return false; }
        cv_.notify_all();
        if (FAILED(queue->Wait(done_.Get(), n))) { MarkLost("done fence wait failed"); return false; }
        return true;
    }
    // dst: RGBA8 in COPY_DEST, the size of the output
    bool CopyOut(ID3D12GraphicsCommandList* list, ID3D12Resource* dst)
    {
        if (!open_) return false;
        Transition(list, out_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(dst, out_.Get());
        Transition(list, out_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        return true;
    }
    bool Lost() const { return lost_; }
    bool IsOpen() const { return open_; }
    std::string Error() const { std::lock_guard lk(m_); return error_; }
    double LastMs() const { return lastMs_; }
    uint64_t Frames() const { return replied_; }
    DWORD Pid() const { return pid_; }
    void Close()
    {
        if (open_ && !lost_ && process_) {
            // drain the last frame, then a clean stop
            {
                std::unique_lock lk(m_);
                cv_.wait_for(lk, std::chrono::milliseconds(cfg_.frameTimeoutMs), [&] { return lost_.load() || replied_ >= sent_; });
            }
            quit_ = true;   // the reader sees the pipe close as the end, not as a lost worker
            const Header stop = MakeStop(sent_ + 1);
            WriteAll(&stop, sizeof(stop));
            if (WaitForSingleObject(process_, 3000) != WAIT_OBJECT_0) TerminateProcess(process_, 125);
        } else if (process_) {
            TerminateProcess(process_, 125);
        }
        quit_ = true;
        cv_.notify_all();
        Drop(toWorker_);
        if (process_) WaitForSingleObject(process_, 3000);
        Drop(fromWorker_);   // ends the reader's ReadFile
        if (reader_.joinable()) reader_.join();
        if (watchdog_.joinable()) watchdog_.join();
        UnblockQueue();
        Drop(process_); Drop(job_);
        in_.Reset(); out_.Reset(); producer_.Reset(); done_.Reset();
        open_ = false; quit_ = false;
    }

private:
    static void Drop(HANDLE& h) { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); h = nullptr; }
    static void Transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = r;
        br.Transition.StateBefore = a; br.Transition.StateAfter = b; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        l->ResourceBarrier(1, &br);
    }
    bool Fail(const std::string& e) { { std::lock_guard lk(m_); error_ = e; } Close(); open_ = false; return false; }
    std::string ExitNote() const
    {
        DWORD code = STILL_ACTIVE;
        if (process_ && GetExitCodeProcess(process_, &code) && code != STILL_ACTIVE) return std::format(" (the worker exited with code {})", code);
        return {};
    }
    bool WriteAll(const void* d, DWORD n) { DWORD k = 0; return toWorker_ && WriteFile(toWorker_, d, n, &k, nullptr) && k == n; }
    // a reply within timeoutMs (the initialisation only; frames go through the reader thread)
    bool ReadReply(Reply& r, DWORD timeoutMs)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        DWORD got = 0;
        while (got < sizeof(r)) {
            DWORD avail = 0;
            if (!PeekNamedPipe(fromWorker_, nullptr, 0, nullptr, &avail, nullptr)) return false;
            if (!avail) {
                if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) { PeekNamedPipe(fromWorker_, nullptr, 0, nullptr, &avail, nullptr); if (!avail) return false; }
                else if (std::chrono::steady_clock::now() > end) return false;
                else { Sleep(2); continue; }
            }
            DWORD k = 0;
            if (!ReadFile(fromWorker_, reinterpret_cast<char*>(&r) + got, sizeof(r) - got, &k, nullptr) || !k) return false;
            got += k;
        }
        return ValidHeader(r.h, Op::Reply, Bytes<Reply>());
    }
    void ReaderLoop()
    {
        for (;;) {
            Reply r; DWORD got = 0; bool ok = true;
            while (got < sizeof(r)) {
                DWORD k = 0;
                if (!ReadFile(fromWorker_, reinterpret_cast<char*>(&r) + got, sizeof(r) - got, &k, nullptr) || !k) { ok = false; break; }
                got += k;
            }
            if (quit_) return;
            if (!ok) { MarkLost("the worker stopped" + ExitNote()); return; }
            if (!ValidHeader(r.h, Op::Reply, Bytes<Reply>())) { MarkLost("malformed reply"); return; }
            if (r.result != 0) { MarkLost(std::format("frame {}: {}", r.h.sequence, r.message.data())); return; }
            {
                std::lock_guard lk(m_);
                replied_ = std::max<uint64_t>(replied_, r.h.sequence);
                lastMs_ = r.ms;
            }
            cv_.notify_all();
        }
    }
    void WatchLoop()
    {
        std::unique_lock lk(m_);
        while (!quit_ && !lost_) {
            cv_.wait_for(lk, std::chrono::milliseconds(200));
            if (quit_ || lost_) break;
            if (sent_ > replied_ && std::chrono::steady_clock::now() - sentAt_ > std::chrono::milliseconds(cfg_.frameTimeoutMs)) {
                const uint64_t f = sent_;
                lk.unlock(); MarkLost(std::format("frame {} took longer than {} ms: the worker was stopped", f, cfg_.frameTimeoutMs)); lk.lock();
                break;
            }
        }
    }
    void MarkLost(const std::string& why)
    {
        bool expected = false;
        if (!lost_.compare_exchange_strong(expected, true)) return;
        { std::lock_guard lk(m_); if (error_.empty()) error_ = why; }
        if (process_) TerminateProcess(process_, 124);
        UnblockQueue();
        cv_.notify_all();
    }
    // nothing may wait on the done fence for a frame the worker will never finish
    void UnblockQueue()
    {
        if (!done_) return;
        uint64_t s; { std::lock_guard lk(m_); s = sent_; }
        if (done_->GetCompletedValue() < s) done_->Signal(s);
    }

    Config cfg_;
    ID3D12Device* dev_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> in_, out_;
    Microsoft::WRL::ComPtr<ID3D12Fence> producer_, done_;
    HANDLE toWorker_ = nullptr, fromWorker_ = nullptr, process_ = nullptr, job_ = nullptr;
    DWORD pid_ = 0;
    std::thread reader_, watchdog_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> lost_{ false }, quit_{ false };
    bool open_ = false;
    uint64_t sent_ = 0, replied_ = 0;
    std::chrono::steady_clock::time_point sentAt_{};
    double lastMs_ = -1;
    std::string error_;
};

// ------------------------------------------------------------------------------------------------ the self-test run
// Starts the worker's self-test (pushcore_zluda_vsr_worker.exe --self-test ... --record <key>) as a detached, low
// priority process; the worker writes HKCU\Software\PushMPC\SelfTest\ZludaVsr itself when it is done. One at a time per
// user session (named mutex held by the worker's process lifetime through the job of this call: the mutex is created
// here and the handle inherited). Returns false with why when it could not start.
inline bool StartSelfTest(const std::wstring& worker, uint64_t luid, const std::wstring& zluda, const std::wstring& vsr, const std::wstring& cache,
                          const std::wstring& log, const std::wstring& recordKey, const std::wstring& regBase, std::string& why, HANDLE* processOut = nullptr,
                          const std::wstring& extraArgs = L"")
{
    HANDLE probe = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\PushMPC-ZludaVsr-SelfTest");
    if (probe) { CloseHandle(probe); why = "a ZludaVsr self-test is already running"; return false; }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE mtx = CreateMutexW(&sa, FALSE, L"Local\\PushMPC-ZludaVsr-SelfTest");
    if (!mtx) { why = "self-test mutex failed"; return false; }
    std::wstring cmd = std::format(L"\"{}\" --self-test --luid {:X} --zluda \"{}\" --vsr \"{}\" --cache \"{}\" --log \"{}\" --record \"{}\"", worker, luid, zluda, vsr, cache, log, recordKey);
    if (!regBase.empty()) cmd += std::format(L" --regbase \"{}\"", regBase);
    if (!extraArgs.empty()) cmd += L" " + extraArgs;   // --width / --height / --scale of the stream that asked for it
    STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si);
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    HANDLE inherit[] = { mtx };
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(worker.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT,
                                   nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(mtx);   // the worker's inherited handle keeps the mutex alive for its lifetime
    if (!ok) { why = std::format("the self-test did not start (win32 {})", GetLastError()); return false; }
    CloseHandle(pi.hThread);
    if (processOut) *processOut = pi.hProcess; else CloseHandle(pi.hProcess);
    return true;
}

} // namespace pmz
