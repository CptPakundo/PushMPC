// PushMPC: Intel video methods glue (see PushCoreIntel.h) and their pushcore_selfTest entries (PushCoreSelfTest.h).
#define PUSHCORE_BUILD 1
#include "PushCoreIntel.h"
#include "PushCoreIntelVsr.h"
#include "PushCoreSelfTest.h"
#include "pushmpc/unlocks.h"
#include "veyra/pipeline/PushMpcCutGuard.h"
#include "veyra/Log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>

using Microsoft::WRL::ComPtr;

namespace pushmpc {
namespace {
std::string narrowW(const std::wstring& s)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring widen(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring r(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n);
    return r;
}
double numberAfter(const std::wstring& s, const wchar_t* key)
{
    const auto p = s.find(key);
    if (p == std::wstring::npos) return -1;
    return std::wcstod(s.c_str() + p + wcslen(key), nullptr);
}
uint64_t luidOf(const LUID& l) { return (uint64_t(uint32_t(l.HighPart)) << 32) | l.LowPart; }
}  // namespace

bool isIntelArcDiscrete(uint32_t vendor, uint32_t device, const std::wstring& name)
{
    if (vendor != 0x8086) return false;
    // Alchemist (DG2: A310-A770, A-series mobile) 0x5690-0x56BF; Battlemage (BMG: B570 / B580) 0xE200-0xE21F
    if ((device >= 0x5690 && device <= 0x56BF) || (device >= 0xE200 && device <= 0xE21F)) return true;
    std::wstring a = name; for (auto& c : a) c = wchar_t(towupper(c));
    return a.find(L"ARC(TM) A") != std::wstring::npos || a.find(L"ARC(TM) B") != std::wstring::npos || a.find(L"ARC A") != std::wstring::npos ||
           a.find(L"ARC B") != std::wstring::npos;
}

IntelCaps intelCaps(const DXGI_ADAPTER_DESC1& d, const std::wstring& driver, bool runTests)
{
    IntelCaps c;
    const bool intel = d.VendorId == 0x8086;
    const std::wstring key = pmu::SelfTestKey(d.VendorId, d.DeviceId, driver);
    // ---- Intel Video Super Resolution (the driver's video processor extension)
    {
        std::wstring src;
        const pmu::UnlockMode mode = pmu::ReadUnlockMode(pmu::Unlock::IntelVsr, &src);
        pmu::SelfTest t = pmu::ReadSelfTest(pmu::Unlock::IntelVsr, key);
        pmu::UnlockDecision u = pmu::DecideUnlock(pmu::Unlock::IntelVsr, mode, intel, L"needs an Intel GPU with the driver's video super resolution", t, false, src);
        if (u.runSelfTest && runTests) {
            D3D11Bridge b; std::string why;
            IvsrSelfTest r;
            if (b.init(nullptr, luidOf(d.AdapterLuid), why)) r = intelVsrSelfTest(b);
            else { r.ran = true; r.reason = "fail: " + why; }
            pmu::WriteSelfTest(pmu::Unlock::IntelVsr, key, r.pass, widen(r.reason));
            veyra::log::info("pushcore", "Intel VSR self-test: " + r.reason);
            t = pmu::ReadSelfTest(pmu::Unlock::IntelVsr, key);
            if (!t.known) { t.known = true; t.passed = r.pass; t.reason = widen(r.reason); }
            u = pmu::DecideUnlock(pmu::Unlock::IntelVsr, mode, intel, L"", t, false, src);
        }
        c.ivsrHw = intel && mode != pmu::UnlockMode::Off;
        c.ivsrOn = u.on;
        c.ivsrMs4k = t.known ? intelVsrCostFromReason(t.reason) : -1;
        c.ivsrWhy = !intel ? "needs an Intel GPU with the driver's video super resolution"
                  : mode == pmu::UnlockMode::Off ? "switched off (PUSHMPC_DISABLE_INTELVSR / Unlocks IntelVsr = 0)" : narrowW(u.why);
        veyra::log::info("pushcore", "unlock " + narrowW(u.why));
    }
    // ---- Intel VPL AI frame interpolation
    {
        std::wstring src;
        const pmu::UnlockMode mode = pmu::ReadUnlockMode(pmu::Unlock::VplFi, &src);
        pmu::SelfTest t = pmu::ReadSelfTest(pmu::Unlock::VplFi, key);
        const bool eligible = intel && VplFi::built();
        const std::wstring inel = !intel ? L"needs an Intel Arc or Xe GPU (Intel VPL runtime in the driver)" : L"this pushcore was built without Intel VPL";
        pmu::UnlockDecision u = pmu::DecideUnlock(pmu::Unlock::VplFi, mode, eligible, inel, t, false, src);
        if (u.runSelfTest && runTests) {
            const VplSelfTest r = vplFiSelfTest(luidOf(d.AdapterLuid), false, 20000);
            pmu::WriteSelfTest(pmu::Unlock::VplFi, key, r.pass, widen(r.reason));
            veyra::log::info("pushcore", "Intel VPL frame interpolation self-test: " + r.reason);
            t = pmu::ReadSelfTest(pmu::Unlock::VplFi, key);
            if (!t.known) { t.known = true; t.passed = r.pass; t.reason = widen(r.reason); }
            u = pmu::DecideUnlock(pmu::Unlock::VplFi, mode, eligible, inel, t, false, src);
        }
        c.vplHw = eligible && mode != pmu::UnlockMode::Off;
        c.vplOn = u.on;
        if (t.known) {
            c.vplMs1080 = numberAfter(t.reason, L"ms=");
            c.vplPsnr = numberAfter(t.reason, L"psnr=");
            const double m = numberAfter(t.reason, L"maxM=");
            c.vplMaxMultiplier = m >= 2 && m <= 8 ? uint32_t(m) : 2;
            c.vplGate = t.passed && t.reason.find(L"gate=pass") != std::wstring::npos && isIntelArcDiscrete(d.VendorId, d.DeviceId, d.Description);
        }
        c.vplWhy = !intel ? "needs an Intel Arc or Xe GPU (Intel VPL runtime in the driver)" : !VplFi::built() ? "this pushcore was built without Intel VPL"
                 : mode == pmu::UnlockMode::Off ? "switched off (PUSHMPC_DISABLE_VPLFI / Unlocks VplFi = 0)" : narrowW(u.why);
        veyra::log::info("pushcore", "unlock " + narrowW(u.why));
    }
    return c;
}

// ------------------------------------------------------------------------------------------------ VplEgress
bool VplEgress::init(D3D11Bridge& b, ID3D12Device* d12, ID3D12CommandQueue* queue, ID3D11Device1* caller, ID3D11Fence* callerFence,
                     ID3D12Fence* fence12, uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t wantMultiplier, double fps, bool cutGuard,
                     std::string& why)
{
    shutdown();
    b_ = &b; d12_ = d12; queue_ = queue; fence12_ = fence12;
    if (!fi_.init(b, w, h, format, wantMultiplier, fps, why)) return false;
    if (!b.importFence12(fence12, fence12in11_, why) || !b.importFence11(callerFence, callerFenceIn11_, why) || !b.createFence11(done_, why)) { shutdown(); return false; }
    slots_.resize(size_t(fi_.multiplier() - 1) * 3);
    for (auto& s : slots_)
        if (!b.createFrom12(w, h, format, s, why) || !b.openOn(caller, s, why)) { shutdown(); return false; }
    for (auto& hd : held_) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1;
        td.MipLevels = 1; td.Format = format; td.SampleDesc.Count = 1;
        if (FAILED(d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&hd)))) { why = "held frame texture"; shutdown(); return false; }
    }
    for (auto& a : alloc_)
        if (FAILED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) { why = "command allocator"; shutdown(); return false; }
    if (FAILED(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0].Get(), nullptr, IID_PPV_ARGS(&list_)))) { why = "command list"; shutdown(); return false; }
    list_->Close();
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (cutGuard && (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM)) {
        guard_ = std::make_unique<veyra::pipeline::CutGuard>();
        if (!guard_->initialize(d12, w, h, format, true, "vpl-fi")) { veyra::log::warn("cut-guard", "vpl-fi: the cut guard did not start; VPL's own scene detection only"); guard_.reset(); }
    }
    quit_ = false;
    thread_ = std::thread([this] { worker(); });
    return true;
}

void VplEgress::shutdown()
{
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    job_ = {};
    if (queue_ && fence12_ && event_ && list_) {
        // the queue may still read our textures
        uint64_t last = 0; for (auto v : allocValue_) last = std::max(last, v);
        if (last && fence12_->GetCompletedValue() < last) { fence12_->SetEventOnCompletion(last, event_); WaitForSingleObject(event_, 5000); }
    }
    guard_.reset();
    fi_.close();
    slots_.clear(); opened_.clear();
    for (auto& h : held_) h.Reset();
    for (auto& a : alloc_) a.Reset();
    for (auto& v : allocValue_) v = 0;
    list_.Reset();
    fence12in11_.Reset(); callerFenceIn11_.Reset(); done_ = {};
    if (event_) { CloseHandle(event_); event_ = nullptr; }
    haveHeld_ = false; parity_ = 0;
    d12_.Reset(); queue_.Reset(); fence12_.Reset();
}

uint64_t VplEgress::cuts() const { return guard_ ? guard_->cuts() : 0; }

bool VplEgress::busy() const { std::lock_guard<std::mutex> lk(m_); return job_.pending && !job_.done; }
bool VplEgress::waitDone(uint32_t ms)
{
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return !job_.pending || job_.done; });
}

bool VplEgress::start(ID3D12Resource* real12, uint64_t readyValue, bool discontinuity, std::string& why)
{
    std::lock_guard<std::mutex> lk(m_);
    if (job_.pending) return false;
    // free slots for the frames of this pair
    std::vector<int> pick;
    for (int i = 0; i < int(slots_.size()) && pick.size() + 1 < fi_.multiplier(); ++i) if (!slots_[size_t(i)].held) pick.push_back(i);
    if (pick.size() + 1 < fi_.multiplier()) return false;   // busy: the caller holds them
    auto it = opened_.find(real12);
    if (it == opened_.end()) {
        ComPtr<ID3D11Texture2D> t;
        if (!b_->open12(real12, t, why)) return false;
        it = opened_.emplace(real12, t).first;
    }
    for (int i : pick) slots_[size_t(i)].held = true;
    job_ = {};
    job_.real12 = real12; job_.real11 = it->second.Get(); job_.readyValue = readyValue; job_.discontinuity = discontinuity;
    job_.slots = pick; job_.pending = true;
    cv_.notify_all();
    return true;
}

void VplEgress::worker()
{
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return quit_ || (job_.pending && !job_.taken); });
            if (quit_) return;
            job_.taken = true;
            j = job_;
        }
        // the graph's real frame is ready on the GPU (CPU wait: VPL's own work is CPU-synchronous anyway), then the slots
        // are free on the caller's side (GPU wait on the bridge), then VPL
        if (fence12_->GetCompletedValue() < j.readyValue) { fence12_->SetEventOnCompletion(j.readyValue, event_); WaitForSingleObject(event_, 5000); }
        std::vector<ID3D11Texture2D*> outs;
        for (int s : j.slots) { b_->wait(callerFenceIn11_.Get(), slots_[size_t(s)].releaseValue); outs.push_back(slots_[size_t(s)].tex.Get()); }
        uint32_t produced = 0;
        std::string why;
        const bool ok = fi_.push(j.real11, j.discontinuity, outs.data(), produced, why);
        const uint64_t v = ok ? b_->signal(done_) : 0;
        {
            std::lock_guard<std::mutex> lk(m_);
            job_.ok = ok && v; job_.produced = produced; job_.doneValue = v; job_.why = ok && !v ? std::string("bridge fence signal failed") : why; job_.done = true;
            lastMs_ = fi_.lastMs();
        }
        cv_.notify_all();
    }
}

bool VplEgress::finish(uint64_t& value12, std::vector<Gen>& gens, std::string& why)
{
    Job j;
    { std::lock_guard<std::mutex> lk(m_); j = job_; }
    gens.clear();
    if (!j.pending || !j.done) { why = "no finished job"; return false; }
    auto release = [&](size_t from) { std::lock_guard<std::mutex> lk(m_); for (size_t k = from; k < j.slots.size(); ++k) slots_[size_t(j.slots[k])].held = false; job_ = {}; };
    if (!j.ok) { why = j.why; release(0); return false; }
    // the D3D12 side: wait for the bridge, then the pair test on the real frame, the hold over the generated frames of a
    // cut pair (A = the previous real frame, our own copy), and the copy of this real frame for the next pair
    const uint32_t a = allocNext_++ % 4;
    if (allocValue_[a] && fence12_->GetCompletedValue() < allocValue_[a]) { fence12_->SetEventOnCompletion(allocValue_[a], event_); WaitForSingleObject(event_, 5000); }
    if (FAILED(alloc_[a]->Reset()) || FAILED(list_->Reset(alloc_[a].Get(), nullptr))) { why = "command list reset"; release(0); return false; }
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES x, D3D12_RESOURCE_STATES y) {
        D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, x, y};
        list_->ResourceBarrier(1, &br);
    };
    const unsigned parity = parity_;
    const bool compare = haveHeld_ && !j.discontinuity;
    barrier(j.real12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (guard_) {
        guard_->captureAndTest(list_.Get(), j.real12, parity, compare);
        if (compare) {
            auto* A = held_[1 - parity].Get();
            barrier(A, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (uint32_t k = 0; k < j.produced; ++k) {
                auto* g = slots_[size_t(j.slots[k])].res.Get();
                barrier(g, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                const bool late = 2u * (k + 1) > fi_.multiplier();
                guard_->holdOnCut(list_.Get(), g, A, parity, j.real12, late);
                barrier(g, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            }
            barrier(A, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        barrier(held_[parity].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->CopyResource(held_[parity].Get(), j.real12);
        barrier(held_[parity].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    }
    barrier(j.real12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(list_->Close())) { why = "command list close"; release(0); return false; }
    queue_->Wait(done_.in12.Get(), j.doneValue);
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    if (FAILED(queue_->Signal(fence12_.Get(), ++value12))) { why = "D3D12 signal"; release(0); return false; }
    allocValue_[a] = value12;
    if (guard_) guard_->submitted(fence12_.Get(), value12);
    haveHeld_ = true; parity_ ^= 1u;
    for (uint32_t k = 0; k < j.produced; ++k) gens.push_back({slots_[size_t(j.slots[k])].ext.Get(), j.slots[k]});
    release(j.produced);   // slots VPL did not fill go back at once
    return true;
}

void VplEgress::released(int slot, uint64_t callerFenceValue)
{
    std::lock_guard<std::mutex> lk(m_);
    if (slot < 0 || size_t(slot) >= slots_.size()) return;
    if (callerFenceValue) slots_[size_t(slot)].releaseValue = callerFenceValue;
    slots_[size_t(slot)].held = false;
}

void VplEgress::dropJobs()
{
    waitDone(10000);
    std::lock_guard<std::mutex> lk(m_);
    if (job_.pending) for (int s : job_.slots) slots_[size_t(s)].held = false;
    job_ = {};
    haveHeld_ = false;
}

}  // namespace pushmpc

// ------------------------------------------------------------------------------------------------ self-tests
// The pushcore_selfTest entries (PushCoreSelfTest.h): Intel VSR "ivsr", VPL frame interpolation "vplfi", the bridge "bridge".
// Hardware results are recorded (FLAG_RECORD) under the key pushcore_capabilities reads; WARP results never.
namespace pushmpc {
namespace {
selftest::Result testIvsr(const selftest::Context& ctx)
{
    selftest::Result o;
    D3D11Bridge b; std::string why; IvsrSelfTest r;
    const bool up = ctx.warp ? b.initWarp(nullptr, why) : b.init(nullptr, ctx.luid, why);
    if (up) r = intelVsrSelfTest(b); else r.reason = "fail: " + why;
    o.pass = r.pass;
    o.verdict = r.reason;
    if (ctx.record && up)
        pmu::WriteSelfTest(pmu::Unlock::IntelVsr, pmu::SelfTestKey(ctx.adapter.VendorId, ctx.adapter.DeviceId, ctx.driver), r.pass, widen(r.reason));
    return o;
}
selftest::Result testVplFi(const selftest::Context& ctx)
{
    selftest::Result o;
    const VplSelfTest r = vplFiSelfTest(ctx.luid, ctx.warp, 60000);
    o.pass = r.pass;
    o.verdict = r.reason;
    o.fields = {{"built", VplFi::built() ? "true" : "false"}, {"material", vplMaterialBaselines()}};
    if (ctx.record && r.ran && r.reason.rfind("unavailable", 0) != 0)
        pmu::WriteSelfTest(pmu::Unlock::VplFi, pmu::SelfTestKey(ctx.adapter.VendorId, ctx.adapter.DeviceId, ctx.driver), r.pass, widen(r.reason));
    return o;
}
selftest::Result testBridge(const selftest::Context& ctx)
{
    selftest::Result o;
    bool ok = false;
    o.verdict = bridgeRoundTripTest(ctx.warp, ctx.luid, ok);
    o.pass = ok;
    return o;
}
const selftest::Registrar kIvsr({PUSHCORE_SELFTEST_IVSR, "ivsr", PUSHCORE_FEATURE_IVSR, &testIvsr});
const selftest::Registrar kVplFi({PUSHCORE_SELFTEST_VPL_FI, "vplfi", PUSHCORE_FEATURE_VPL_FI, &testVplFi});
const selftest::Registrar kBridge({PUSHCORE_SELFTEST_BRIDGE, "bridge", 0u, &testBridge});
}  // namespace
}  // namespace pushmpc
