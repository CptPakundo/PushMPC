#pragma once
// PushMPC: the Intel video methods as pushcore uses them (glue between PushCoreGpu.cpp and the bridge modules).
//   - capabilities: Intel VSR (PUSHCORE_CAP_IVSR) and VPL frame interpolation (PUSHCORE_CAP_VPL_FI): the hardware rule,
//     the off switches (unlocks IntelVsr / VplFi), the self-test results cached per adapter + driver (run here when
//     Auto and not tested yet on this adapter + driver);
//   - VplEgress: VPL frame interpolation after the graph (worker thread on the bridge, the cut guard and the hold on the
//     D3D12 queue, generated frames in shared textures the caller's D3D11 device reads).
#include "PushCoreD3D11Bridge.h"
#include "PushCoreVplFi.h"
#include "pushmpc/pushcore_abi.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace veyra::pipeline { class CutGuard; }

namespace pushmpc {

struct IntelCaps {
    bool ivsrHw = false, ivsrOn = false;    // hardware: an Intel GPU (and not switched off); on: self-test passed or forced
    std::string ivsrWhy;                    // the reason when not on (or the decision line when on)
    double ivsrMs4k = -1;                   // measured 1080p -> 4K cost (self-test)
    bool vplHw = false, vplOn = false, vplGate = false;
    std::string vplWhy;
    double vplMs1080 = -1, vplPsnr = -1;
    uint32_t vplMaxMultiplier = 0;
};
// adapter = the DXGI adapter; driver = its user-mode driver version; runTests = run a missing self-test now
IntelCaps intelCaps(const DXGI_ADAPTER_DESC1& adapter, const std::wstring& driver, bool runTests);
// Intel discrete Arc (Alchemist / Battlemage) by PCI device ID or name
bool isIntelArcDiscrete(uint32_t vendor, uint32_t device, const std::wstring& name);

class VplEgress {
public:
    ~VplEgress() { shutdown(); }
    // Opens VPL on the bridge for w x h frames of `format` (the graph's output); caller = the caller's D3D11 device (the
    // generated frames are opened there); callerFence = pushcore's shared D3D11 fence the caller's presentation signals;
    // queue = the graph's D3D12 queue; slots = generated textures in flight.
    bool init(D3D11Bridge& b, ID3D12Device* d12, ID3D12CommandQueue* queue, ID3D11Device1* caller, ID3D11Fence* callerFence,
              ID3D12Fence* fence12, uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t wantMultiplier, double fps, bool cutGuard,
              std::string& why);
    void shutdown();
    bool ready() const { return fi_.ready(); }
    uint32_t multiplier() const { return fi_.multiplier(); }
    std::string describe() const { return fi_.describe() + (guard_ ? ", cut guard on" : ", no cut guard"); }
    bool cutGuardActive() const { return guard_ != nullptr; }
    uint64_t cuts() const;

    // A job for one real frame (graph output real12, ready at fence12 value readyValue). false = busy (no free slots: the
    // caller must return generated frames first) or failed (why set).
    bool start(ID3D12Resource* real12, uint64_t readyValue, bool discontinuity, std::string& why);
    bool busy() const;                    // a job is in flight
    bool waitDone(uint32_t ms);           // true when no job is in flight
    // After the job: the D3D12 side (cut guard test, hold over the generated frames, the held copy) and fence12's value
    // covering it (value12 is pushcore's counter for fence12). gens = the generated frames for the caller, in time order.
    struct Gen { ID3D11Texture2D* texture; int slot; };
    bool finish(uint64_t& value12, std::vector<Gen>& gens, std::string& why);
    // the caller is done with a generated frame (fence value of pushcore's caller fence after its draw; 0 = not drawn)
    void released(int slot, uint64_t callerFenceValue);
    void dropJobs();                      // seek / flush / teardown: wait for the job, forget its frames
    double lastMs() const { return lastMs_; }

private:
    void worker();
    D3D11Bridge* b_ = nullptr;
    VplFi fi_;
    Microsoft::WRL::ComPtr<ID3D12Device> d12_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence12in11_, callerFenceIn11_;
    D3D11Bridge::Fence11 done_;           // bridge signals after VPL's copies; the D3D12 queue waits on it
    std::vector<D3D11Bridge::Texture> slots_;
    std::unordered_map<ID3D12Resource*, Microsoft::WRL::ComPtr<ID3D11Texture2D>> opened_;
    Microsoft::WRL::ComPtr<ID3D12Resource> held_[2];
    std::unique_ptr<veyra::pipeline::CutGuard> guard_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc_[4];
    uint64_t allocValue_[4]{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    uint32_t allocNext_ = 0;
    HANDLE event_ = nullptr;
    uint32_t parity_ = 0;
    bool haveHeld_ = false;
    double lastMs_ = -1;
    // the job
    struct Job {
        ID3D12Resource* real12 = nullptr;
        ID3D11Texture2D* real11 = nullptr;
        uint64_t readyValue = 0;
        bool discontinuity = false;
        std::vector<int> slots;
        bool pending = false, taken = false, done = false, ok = false;
        uint32_t produced = 0;
        uint64_t doneValue = 0;
        std::string why;
    } job_;
    std::thread thread_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false;
};

}  // namespace pushmpc
