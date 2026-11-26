// SPDX-License-Identifier: GPL-3.0-or-later
// pmml's ONNX Runtime backend. ONNX Runtime is loaded at run time from <runtimeDir>/ort/<flavor>/ (one flavor per
// process); execution providers come from that build (CPU, DirectML, CoreML, MIGraphX) or are plugin EPs registered
// from <runtimeDir>/eps/<ep>/ep.json (TensorRT-RTX, OpenVINO, QNN, AMD GPU; ONNX Runtime >= 1.22 plugin-EP ABI).
// Three small graphs run around the PushFlow model on the same EP (make_graphs.py): pack (once per pair), addt
// (once per network run), synth (once per output frame). Intermediates stay in the EP's device memory (IoBinding
// with BindOutputToDevice): DirectML sessions share one D3D12 device and queue that pmml creates, so their
// tensors are interchangeable (and a D3D12 host such as pushcore can hand its own device in later).
#include "internal.h"

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if __has_include(<dml_provider_factory.h>) && __has_include(<DirectML.h>)
#define PMML_HAVE_DML 1
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectML.h>
#include <dml_provider_factory.h>
#endif
#endif
#ifndef PMML_HAVE_DML
#define PMML_HAVE_DML 0
#endif

namespace pmml::detail {

namespace {

std::basic_string<ORTCHAR_T> ortPath(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    for (auto& c : w) if (c == L'/') c = L'\\';
    return w;
#else
    return s;
#endif
}

std::string envStr(const char* name) { const char* v = std::getenv(name); return v ? v : ""; }

std::string absolutePath(const std::string& p) {
#ifdef _WIN32
    char buf[4096];
    const DWORD n = GetFullPathNameA(p.c_str(), sizeof buf, buf, nullptr);
    return n && n < sizeof buf ? std::string(buf, n) : p;
#else
    char* r = realpath(p.c_str(), nullptr);
    if (!r) return p;
    std::string s(r);
    std::free(r);
    return s;
#endif
}

// --- the process-wide ONNX Runtime -------------------------------------------------------------------------------------
struct Ort {
    Library lib;
    const OrtApi* api = nullptr;
    uint32_t version = 0;         // the API version obtained (the runtime's own, up to the header's)
    OrtEnv* env = nullptr;
    std::string flavor, dir, runtimeDir;
    std::set<std::string> registered;   // plugin EP libraries registered with env
};

std::mutex g_mutex;
Ort* g_ort = nullptr;

Ort* loadOrt(const std::string& runtimeDir, const std::string& flavor, std::string* err) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ort) {
        if (g_ort->flavor != flavor) { if (err) *err = "ONNX Runtime flavor '" + g_ort->flavor + "' is already loaded in this process (asked for '" + flavor + "'): one flavor per process"; return nullptr; }
        return g_ort;
    }
    auto o = std::make_unique<Ort>();
    o->flavor = flavor;
    o->runtimeDir = absolutePath(runtimeDir);   // plugin EP libraries must be given by absolute path
    o->dir = joinPath(joinPath(o->runtimeDir, "ort"), flavor);
    const std::string override_ = envStr("PMML_ORT_LIBRARY");
    const std::string path = override_.empty() ? joinPath(o->dir, ortLibraryName()) : override_;
    if (!o->lib.open(path, err)) return nullptr;
    using GetApiBase = const OrtApiBase* (ORT_API_CALL*)();
    auto base = reinterpret_cast<GetApiBase>(o->lib.symbol("OrtGetApiBase"));
    if (!base) { if (err) *err = path + " has no OrtGetApiBase"; return nullptr; }
    const OrtApiBase* b = base();
    // the newest table both sides know: the runtime refuses versions above its own
    for (uint32_t v = ORT_API_VERSION; v >= 14 && !o->api; --v) {
        o->api = b->GetApi(v);
        if (o->api) o->version = v;
    }
    if (!o->api) { if (err) *err = std::string("ONNX Runtime ") + b->GetVersionString() + " is too old (API 14+ needed)"; return nullptr; }
    const std::string level = envStr("PMML_ORT_LOG");
    const OrtLoggingLevel lv = level == "verbose" ? ORT_LOGGING_LEVEL_VERBOSE : level == "info" ? ORT_LOGGING_LEVEL_INFO : ORT_LOGGING_LEVEL_ERROR;
    if (OrtStatus* s = o->api->CreateEnv(lv, "pmml", &o->env)) {
        if (err) *err = std::string("CreateEnv: ") + o->api->GetErrorMessage(s);
        o->api->ReleaseStatus(s);
        return nullptr;
    }
    g_ort = o.release();
    return g_ort;
}

#define PMML_TRY(expr)                                                         \
    do {                                                                       \
        if (OrtStatus* status_ = (expr)) {                                     \
            if (err) *err = std::string(#expr).substr(0, 60) + ": " + ort->api->GetErrorMessage(status_); \
            ort->api->ReleaseStatus(status_);                                  \
            return false;                                                      \
        }                                                                      \
    } while (0)

// --- plugin EPs -------------------------------------------------------------------------------------------------------
struct PluginEp { std::string id, epName, library; };

std::vector<PluginEp> pluginEps(const std::string& runtimeDir) {
    std::vector<PluginEp> v;
    for (const char* id : {"trt-rtx", "openvino", "qnn", "amdgpu"}) {
        const std::string dir = joinPath(joinPath(runtimeDir, "eps"), id);
        const std::string json = readText(joinPath(dir, "ep.json"));
        if (json.empty()) continue;
        PluginEp p;
        p.id = id;
        p.epName = jsonString(json, "epName");
        const std::string lib = jsonString(json, "library");
        if (lib.empty() || p.epName.empty()) continue;
        p.library = (lib.find(':') != std::string::npos || lib[0] == '/') ? lib : joinPath(dir, lib);
        v.push_back(p);
    }
    return v;
}

#if ORT_API_VERSION >= 22
bool registerPlugins(Ort* ort, std::string* log) {
    if (ort->version < 22) return false;
    for (const auto& p : pluginEps(ort->runtimeDir)) {
        if (ort->registered.count(p.id)) continue;
        ort->registered.insert(p.id);
#ifdef _WIN32
        // the EP's own dependencies (OpenVINO's frontends, TBB, ...) sit next to it: put that folder on the DLL search
        // path while ONNX Runtime loads the library
        std::basic_string<ORTCHAR_T> dir = ortPath(p.library);
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        SetDllDirectoryW(dir.c_str());
#endif
        OrtStatus* regStatus = ort->api->RegisterExecutionProviderLibrary(ort->env, p.id.c_str(), ortPath(p.library).c_str());
#ifdef _WIN32
        SetDllDirectoryW(nullptr);
#endif
        if (OrtStatus* s = regStatus) {
            if (log) *log += "plugin EP " + p.id + " not registered: " + ort->api->GetErrorMessage(s) + "\n";
            ort->api->ReleaseStatus(s);
        } else if (log) {
            *log += "plugin EP " + p.id + " registered (" + p.library + ")\n";
        }
    }
    return true;
}

const char* deviceType(Ort* ort, const OrtEpDevice* d) {
    switch (ort->api->HardwareDevice_Type(ort->api->EpDevice_Device(d))) {
        case OrtHardwareDeviceType_GPU: return "GPU";
        case OrtHardwareDeviceType_NPU: return "NPU";
        default: return "CPU";
    }
}

// The EP devices of one EP name, of the wanted type ("" = any non-CPU first, then CPU).
std::vector<const OrtEpDevice*> epDevices(Ort* ort, const std::string& epName, const std::string& type) {
    std::vector<const OrtEpDevice*> out;
    const OrtEpDevice* const* devs = nullptr;
    size_t n = 0;
    if (OrtStatus* s = ort->api->GetEpDevices(ort->env, &devs, &n)) { ort->api->ReleaseStatus(s); return out; }
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < n; ++i) {
            if (epName != ort->api->EpDevice_EpName(devs[i])) continue;
            const std::string t = deviceType(ort, devs[i]);
            if (!type.empty() ? (pass == 0 && t == type) : ((pass == 0) == (t != "CPU"))) out.push_back(devs[i]);
        }
    return out;
}

std::string metadata(Ort* ort, const OrtKeyValuePairs* kv, const char* key) {
    if (!kv) return {};
    const char* v = ort->api->GetKeyValue(kv, key);
    return v ? v : "";
}
#endif

// --- DirectML: one D3D12 device + queue + DML device per process, shared by every session ----------------------------
#if PMML_HAVE_DML
struct DmlShared {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDMLDevice* dml = nullptr;
    int adapter = -1;
    std::string name;
    uint32_t vendorId = 0, deviceId = 0;
    std::string driver;
};
DmlShared g_dml;

std::vector<IDXGIAdapter1*> dxgiAdapters(bool warp) {
    std::vector<IDXGIAdapter1*> v;
    IDXGIFactory6* f = nullptr;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory6), reinterpret_cast<void**>(&f)))) return v;
    if (warp) {
        IDXGIAdapter1* a = nullptr;
        if (SUCCEEDED(f->EnumWarpAdapter(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&a)))) v.push_back(a);
    } else {
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* a = nullptr;
            if (FAILED(f->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&a)))) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            bool seen = false;   // the same adapter can be listed once per output
            for (auto* o : v) { DXGI_ADAPTER_DESC1 e{}; o->GetDesc1(&e); seen = seen || (e.AdapterLuid.LowPart == d.AdapterLuid.LowPart && e.AdapterLuid.HighPart == d.AdapterLuid.HighPart); }
            if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || seen) { a->Release(); continue; }
            v.push_back(a);
        }
    }
    f->Release();
    return v;
}

std::string narrowW(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 1)) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void describeAdapter(IDXGIAdapter1* a, std::string* name, uint32_t* ven, uint32_t* dev, std::string* driver) {
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    *name = narrowW(d.Description);
    *ven = d.VendorId; *dev = d.DeviceId;
    LARGE_INTEGER umd{};
    if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
        std::ostringstream ss;
        ss << HIWORD(umd.HighPart) << "." << LOWORD(umd.HighPart) << "." << HIWORD(umd.LowPart) << "." << LOWORD(umd.LowPart);
        *driver = ss.str();
    }
}

bool dmlShared(Ort* ort, int adapter, std::string* err) {
    if (g_dml.dml) {
        if (g_dml.adapter != adapter) { if (err) *err = "DirectML already runs on adapter " + std::to_string(g_dml.adapter); return false; }
        return true;
    }
    const bool warp = envStr("PMML_DML_WARP") == "1";
    auto adapters = dxgiAdapters(warp);
    if (adapter < 0 || size_t(adapter) >= adapters.size()) { if (err) *err = "no D3D12 adapter " + std::to_string(adapter); for (auto* a : adapters) a->Release(); return false; }
    IDXGIAdapter1* a = adapters[size_t(adapter)];
    describeAdapter(a, &g_dml.name, &g_dml.vendorId, &g_dml.deviceId, &g_dml.driver);
    HRESULT hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&g_dml.device));
    for (auto* x : adapters) x->Release();
    if (FAILED(hr)) { if (err) *err = "D3D12CreateDevice failed"; return false; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(g_dml.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&g_dml.queue)))) { if (err) *err = "CreateCommandQueue failed"; return false; }
    // DirectML.dll from the flavor folder (the redistributable next to onnxruntime.dll), not System32's
    static Library dmlLib;
    if (!dmlLib.loaded() && !dmlLib.open(joinPath(ort->dir, "DirectML.dll"), err)) return false;
    using Create = HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**);
    auto create = reinterpret_cast<Create>(dmlLib.symbol("DMLCreateDevice"));
    if (!create || FAILED(create(g_dml.device, DML_CREATE_DEVICE_FLAG_NONE, __uuidof(IDMLDevice), reinterpret_cast<void**>(&g_dml.dml)))) { if (err) *err = "DMLCreateDevice failed"; return false; }
    g_dml.adapter = adapter;
    return true;
}
#endif

// --- one EP on a session options object -------------------------------------------------------------------------------
struct EpChoice {
    std::string describe;
    const OrtMemoryInfo* deviceMemory = nullptr;   // where intermediates live (nullptr = host)
    OrtMemoryInfo* ownedMemory = nullptr;
    bool gpu = false;
};

void parseOptions(const std::string& text, std::vector<std::string>* keys, std::vector<std::string>* vals) {
    std::stringstream ss(text);
    for (std::string kv; std::getline(ss, kv, ';');) {
        const auto eq = kv.find('=');
        if (eq != std::string::npos) { keys->push_back(kv.substr(0, eq)); vals->push_back(kv.substr(eq + 1)); }
    }
}

bool appendEp(Ort* ort, OrtSessionOptions* so, const Options& o, EpChoice* choice, std::string* err) {
    const std::string& ep = o.backend.ep;
    std::vector<std::string> keys, vals;
    parseOptions(envStr("PMML_EP_OPTIONS"), &keys, &vals);
    auto cstrs = [](const std::vector<std::string>& v) { std::vector<const char*> c; for (auto& s : v) c.push_back(s.c_str()); return c; };
    if (ep == "cpu") { choice->describe = "CPU"; return true; }
    choice->gpu = true;
    PMML_TRY(ort->api->SetIntraOpNumThreads(so, 1));
    PMML_TRY(ort->api->AddSessionConfigEntry(so, "session.intra_op.allow_spinning", "0"));
    if (ep == "dml") {
#if PMML_HAVE_DML
        const OrtDmlApi* dml = nullptr;
        PMML_TRY(ort->api->GetExecutionProviderApi("DML", ort->version, reinterpret_cast<const void**>(&dml)));
        if (!dmlShared(ort, o.backend.device, err)) return false;
        PMML_TRY(ort->api->DisableMemPattern(so));
        PMML_TRY(ort->api->SetSessionExecutionMode(so, ORT_SEQUENTIAL));
        PMML_TRY(dml->SessionOptionsAppendExecutionProvider_DML1(so, g_dml.dml, g_dml.queue));
        if (!choice->ownedMemory) PMML_TRY(ort->api->CreateMemoryInfo("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault, &choice->ownedMemory));
        choice->deviceMemory = choice->ownedMemory;
        choice->describe = "DirectML on " + g_dml.name + " (driver " + g_dml.driver + ")";
        return true;
#else
        if (err) *err = "this pmml was built without DirectML";
        return false;
#endif
    }
    if (ep == "coreml") {
        keys.insert(keys.begin(), {"ModelFormat", "MLComputeUnits"});
        vals.insert(vals.begin(), {"MLProgram", "ALL"});
        auto k = cstrs(keys), v = cstrs(vals);
        PMML_TRY(ort->api->SessionOptionsAppendExecutionProvider(so, "CoreML", k.data(), v.data(), k.size()));
        choice->describe = "CoreML (all compute units)";
        return true;
    }
    if (ep == "migraphx") {
        auto k = cstrs(keys), v = cstrs(vals);
        if (OrtStatus* s = ort->api->SessionOptionsAppendExecutionProvider(so, "MIGraphX", k.data(), v.data(), k.size())) {
            ort->api->ReleaseStatus(s);
            OrtMIGraphXProviderOptions mo{};
            mo.device_id = o.backend.device;
            PMML_TRY(ort->api->SessionOptionsAppendExecutionProvider_MIGraphX(so, &mo));
        }
        choice->describe = "MIGraphX (ROCm) device " + std::to_string(o.backend.device);
        return true;
    }
    // plugin EPs
    const auto plugins = pluginEps(ort->runtimeDir);
    const std::string id = ep == "qnn-gpu" ? "qnn" : ep;
    auto it = std::find_if(plugins.begin(), plugins.end(), [&](const PluginEp& p) { return p.id == id; });
#if ORT_API_VERSION >= 22
    if (it != plugins.end() && ort->version >= 22) {
        registerPlugins(ort, nullptr);
        const std::string type = ep == "qnn" ? "NPU" : "GPU";
        auto devs = epDevices(ort, it->epName, type);
        if (devs.empty()) { if (err) *err = it->epName + " reports no " + type + " device here"; return false; }
        if (o.backend.device < 0 || size_t(o.backend.device) >= devs.size()) { if (err) *err = it->epName + " has no device " + std::to_string(o.backend.device); return false; }
        const OrtEpDevice* d = devs[size_t(o.backend.device)];
        if (ep == "trt-rtx" && !o.cacheDir.empty()) {
            // TensorRT-RTX's runtime cache keeps the JIT-compiled kernels across runs (key per the EP's docs)
            if (std::find(keys.begin(), keys.end(), "nv_runtime_cache_path") == keys.end()) { keys.push_back("nv_runtime_cache_path"); vals.push_back(joinPath(o.cacheDir, "trt-rtx")); }
        }
        auto k = cstrs(keys), v = cstrs(vals);
        PMML_TRY(ort->api->SessionOptionsAppendExecutionProvider_V2(so, ort->env, &d, 1, k.data(), v.data(), k.size()));
#if ORT_API_VERSION >= 23
        if (ort->version >= 23) choice->deviceMemory = ort->api->EpDevice_MemoryInfo(d, OrtDeviceMemoryType_DEFAULT);
#endif
        const OrtHardwareDevice* hw = ort->api->EpDevice_Device(d);
        choice->describe = it->epName + " on " + ort->api->HardwareDevice_Vendor(hw) + " " + metadata(ort, ort->api->HardwareDevice_Metadata(hw), "Description") +
                           " (" + deviceType(ort, d) + ")";
        return true;
    }
#endif
    // the same EPs built into the ONNX Runtime library itself (older ABI)
    const char* legacy = ep == "openvino" ? "OpenVINO" : (ep == "qnn" || ep == "qnn-gpu") ? "QNN" : ep == "trt-rtx" ? "NvTensorRtRtx" : nullptr;
    if (!legacy) { if (err) *err = "unknown EP '" + ep + "'"; return false; }
    if (ep == "openvino" && std::find(keys.begin(), keys.end(), "device_type") == keys.end()) { keys.push_back("device_type"); vals.push_back("GPU"); }
    if (ep == "qnn" || ep == "qnn-gpu") { keys.push_back("backend_type"); vals.push_back(ep == "qnn" ? "htp" : "gpu"); }
    auto k = cstrs(keys), v = cstrs(vals);
    PMML_TRY(ort->api->SessionOptionsAppendExecutionProvider(so, legacy, k.data(), v.data(), k.size()));
    choice->describe = std::string(legacy) + " (built in)";
    return true;
}

// --- the interpolator -------------------------------------------------------------------------------------------------
class OrtInterpolator final : public Interpolator {
public:
    ~OrtInterpolator() override { release(); }

    bool init(const Options& o, std::string* err) {
        o_ = o;
        ort = loadOrt(o.runtimeDir, o.backend.flavor, err);
        if (!ort) return false;
        if (!findModel(o, &model_, err)) return false;
        net_ = networkSize(o.width, o.height, o.tier, o.motionHeight, model_.pad);
        affine_ = gridAffine(o.width, o.height, net_);
        colour_ = o.colour.packed();
        const bool yuv = o.format == PlaneFormat::Yuv420;
        fmt_ = yuv ? "yuv" : "rgb";
        PMML_TRY(ort->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &cpu_));
        PMML_TRY(ort->api->GetAllocatorWithDefaultOptions(&alloc_));
        const double t0 = nowMs();
        // the network: free dimensions fixed to the padded size
        std::vector<std::pair<std::string, int64_t>> netDims;
        if (model_.dims.size() == 4) {
            const int64_t vals[4] = {1, 7, net_.padH, net_.padW};
            for (int i = 0; i < 4; ++i)
                if (!model_.dims[size_t(i)].empty() && !std::isdigit(static_cast<unsigned char>(model_.dims[size_t(i)][0]))) netDims.push_back({model_.dims[size_t(i)], vals[i]});
        }
        if (!session(model_.path, netDims, {}, &net, err)) return false;
        const std::vector<std::pair<std::string, int64_t>> dims = {{"pmml_h", o.height}, {"pmml_w", o.width}, {"pmml_ch", o.height / 2},
                                                                   {"pmml_cw", o.width / 2}, {"pmml_ph", net_.padH}, {"pmml_pw", net_.padW}};
        const std::vector<std::pair<std::string, std::vector<int64_t>>> sizes = {
            {"nh", {net_.h}}, {"nw", {net_.w}}, {"padh", {net_.padH - net_.h}}, {"padw", {net_.padW - net_.w}}};
        if (!session(joinPath(o.graphsDir, "pack-" + fmt_ + ".onnx"), dims,
                     sizes, &pack, err)) return false;
        if (!session(joinPath(o.graphsDir, "addt.onnx"), dims, {}, &addt, err)) return false;
        if (!session(joinPath(o.graphsDir, "synth-" + fmt_ + ".onnx"), dims, sizes, &synth, err)) return false;
        // optional: the per-pixel phase graph (renderMap); graph folders made before it run without
        {
            const std::string mapPath = joinPath(o.graphsDir, "synth_map-" + fmt_ + ".onnx");
            if (FILE* f = std::fopen(mapPath.c_str(), "rb")) {
                std::fclose(f);
                std::string e;
                if (!session(mapPath, dims, sizes, &synthMap, &e)) synthMap = nullptr;
            }
        }
        initMs_ = nowMs() - t0;
        for (OrtSession* s : {pack, addt, net, synth}) {
            OrtIoBinding* b = nullptr;
            PMML_TRY(ort->api->CreateIoBinding(s, &b));
            bindings_.push_back(b);
        }
        if (synthMap) {
            OrtIoBinding* b = nullptr;
            PMML_TRY(ort->api->CreateIoBinding(synthMap, &b));
            bindings_.push_back(b);   // [4]
        }
        if (o.hostIntermediates) choice_.deviceMemory = nullptr;
        std::ostringstream d;
        d << "pmml " << kVersion << " ORT " << ort->flavor << " (API " << ort->version << "), " << choice_.describe << "; PushFlow " << o.tier << " ("
          << model_.path.substr(model_.path.find_last_of("/\\") + 1) << ") network " << net_.w << "x" << net_.h << " (padded " << net_.padW << "x" << net_.padH
          << ") for " << o.width << "x" << o.height << ", intermediates " << (choice_.deviceMemory ? "on the device" : "in host memory") << ", sessions in "
          << int(initMs_) << " ms";
        describe_ = d.str();
        return true;
    }

    bool setPair(const Frame& a, const Frame& b, int phases, std::string* err) override {
        phases_ = phases;
        netT_ = -1.f;
        const int w = o_.width, h = o_.height;
        const bool yuv = o_.format == PlaneFormat::Yuv420;
        OrtIoBinding* bind = bindings_[0];
        ort->api->ClearBoundInputs(bind);
        ort->api->ClearBoundOutputs(bind);
        releaseValues(&inputs_);
        static const char* namesRgb[2][3] = {{"a_r", "a_g", "a_b"}, {"b_r", "b_g", "b_b"}};
        static const char* namesYuv[2][3] = {{"a_y", "a_u", "a_v"}, {"b_y", "b_u", "b_v"}};
        staging_.resize(6);
        for (int f = 0; f < 2; ++f) {
            const Frame& fr = f == 0 ? a : b;
            for (int p = 0; p < 3; ++p) {
                const int pw = (yuv && p) ? w / 2 : w, ph = (yuv && p) ? h / 2 : h;
                const uint16_t* src = fr.plane[p];
                if (fr.stride[p] != pw) {   // contiguous copy
                    auto& s = staging_[size_t(f * 3 + p)];
                    s.resize(size_t(pw) * ph);
                    for (int y = 0; y < ph; ++y) std::memcpy(&s[size_t(y) * pw], src + y * fr.stride[p], size_t(pw) * 2);
                    src = s.data();
                }
                OrtValue* v = nullptr;
                if (!tensor(const_cast<uint16_t*>(src), {1, 1, ph, pw}, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16, &v, err)) return false;
                inputs_.push_back(v);
                PMML_TRY(ort->api->BindInput(bind, (yuv ? namesYuv : namesRgb)[f][p], v));
            }
        }
        OrtValue* c = nullptr;
        if (!tensor(colour_.data(), {8}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &c, err)) return false;
        inputs_.push_back(c);
        PMML_TRY(ort->api->BindInput(bind, "colour", c));
        if (!bindOut(bind, "small_ab") || !bindOut(bind, "frame_a") || !bindOut(bind, "frame_b")) { if (err) *err = "BindOutput (pack) failed"; return false; }
        const double t0 = nowMs();
        PMML_TRY(ort->api->RunWithBinding(pack, nullptr, bind));
        if (o_.syncTiming) PMML_TRY(ort->api->SynchronizeBoundOutputs(bind));
        stats_.packMs = nowMs() - t0;
        return outputs(bind, &packed_, 3, err);
    }

    bool render(float t, const OutFrame& out, std::string* err) override {
        const bool once = phases_ > 1;
        const float tn = once ? o_.tRef : t;
        if (netT_ != tn) {
            if (!runNetwork(tn, err)) return false;
            netT_ = tn;
        }
        const float s0 = once ? t / o_.tRef : 1.f, s1 = once ? (1.f - t) / (1.f - o_.tRef) : 1.f;
        const float scale[2] = {s0, s1};
        std::vector<OrtValue*> tmp;
        OrtValue* sv = nullptr;
        { std::string e; if (tensor(const_cast<float*>(scale), {2}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &sv, &e)) tmp.push_back(sv); }
        return runSynth(synth, bindings_[3], {{"scale", sv}}, tmp, out, err);
    }

    // per-pixel phase: t per cell of a mapW x mapH grid over the picture (bilinear at the network's pixel centres; the
    // padding takes the nearest edge), the network once at tRef, synth_map with s0 = t / tRef, s1 = (1 - t) / (1 - tRef)
    bool renderMap(const float* tMap, int mapW, int mapH, const OutFrame& out, std::string* err) override {
        if (!synthMap) { if (err) *err = "synth_map-" + fmt_ + ".onnx is not in the graphs folder (make_graphs.py)"; return false; }
        if (!tMap || mapW < 1 || mapH < 1) { if (err) *err = "renderMap: no phase map"; return false; }
        if (netT_ != o_.tRef) {
            if (!runNetwork(o_.tRef, err)) return false;
            netT_ = o_.tRef;
        }
        const int pw = net_.padW, ph = net_.padH;
        map0_.resize(size_t(pw) * ph); map1_.resize(size_t(pw) * ph);
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) {
                const float fx = std::clamp((float(std::min(x, net_.w - 1)) + 0.5f) / float(net_.w) * float(mapW) - 0.5f, 0.f, float(mapW - 1));
                const float fy = std::clamp((float(std::min(y, net_.h - 1)) + 0.5f) / float(net_.h) * float(mapH) - 0.5f, 0.f, float(mapH - 1));
                const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, mapW - 1), y1 = std::min(y0 + 1, mapH - 1);
                const float ax = fx - float(x0), ay = fy - float(y0);
                const float t = (tMap[size_t(y0) * mapW + x0] * (1 - ax) + tMap[size_t(y0) * mapW + x1] * ax) * (1 - ay)
                              + (tMap[size_t(y1) * mapW + x0] * (1 - ax) + tMap[size_t(y1) * mapW + x1] * ax) * ay;
                map0_[size_t(y) * pw + x] = t / o_.tRef;
                map1_[size_t(y) * pw + x] = (1.f - t) / (1.f - o_.tRef);
            }
        std::vector<OrtValue*> tmp;
        OrtValue *m0 = nullptr, *m1 = nullptr;
        if (!tensor(map0_.data(), {1, ph, pw, 1}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &m0, err)) return false;
        tmp.push_back(m0);
        if (!tensor(map1_.data(), {1, ph, pw, 1}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &m1, err)) { for (OrtValue* v : tmp) ort->api->ReleaseValue(v); return false; }
        tmp.push_back(m1);
        return runSynth(synthMap, bindings_[4], {{"scale0_map", m0}, {"scale1_map", m1}}, tmp, out, err);
    }

    // a synth graph (synth or synth_map) with its own scale inputs; tmp: values to release afterwards (taken over)
    bool runSynth(OrtSession* graph, OrtIoBinding* bind, const std::vector<std::pair<const char*, OrtValue*>>& scaleIn, std::vector<OrtValue*>& tmp,
                  const OutFrame& out, std::string* err) {
        ort->api->ClearBoundInputs(bind);
        ort->api->ClearBoundOutputs(bind);
        auto host = [&](const float* data, int64_t n) -> OrtValue* {
            OrtValue* v = nullptr;
            std::string e;
            if (!tensor(const_cast<float*>(data), {n}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v, &e)) return nullptr;
            tmp.push_back(v);
            return v;
        };
        bool scalesOk = true;
        for (const auto& s : scaleIn) scalesOk = scalesOk && s.second;
        OrtValue* av = host(affine_.data(), 4);
        OrtValue* cv = host(colour_.data(), 8);
        bool ok = scalesOk && av && cv;
        auto bindIn = [&](const char* name, OrtValue* v) {
            if (!ok) return;
            if (OrtStatus* s = ort->api->BindInput(bind, name, v)) { ok = false; if (err) *err = std::string("BindInput ") + name + ": " + ort->api->GetErrorMessage(s); ort->api->ReleaseStatus(s); }
        };
        bindIn("frame_a", packed_[1]); bindIn("frame_b", packed_[2]);
        bindIn("grid0", netOut_[1]); bindIn("grid1", netOut_[2]); bindIn("mask", netOut_[3]);
        for (const auto& s : scaleIn) bindIn(s.first, s.second);
        bindIn("affine", av); bindIn("colour", cv);
        // outputs straight into the caller's planes when contiguous
        const bool yuv = o_.format == PlaneFormat::Yuv420;
        static const char* outRgb[3] = {"out_r", "out_g", "out_b"};
        static const char* outYuv[3] = {"out_y", "out_u", "out_v"};
        outStaging_.resize(3);
        uint16_t* dst[3];
        for (int p = 0; p < 3 && ok; ++p) {
            const int pw = (yuv && p) ? o_.width / 2 : o_.width, ph = (yuv && p) ? o_.height / 2 : o_.height;
            dst[p] = out.plane[p];
            if (out.stride[p] != pw) { outStaging_[size_t(p)].resize(size_t(pw) * ph); dst[p] = outStaging_[size_t(p)].data(); }
            OrtValue* v = nullptr;
            ok = tensor(dst[p], {1, 1, ph, pw}, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16, &v, err);
            if (ok) {
                tmp.push_back(v);
                if (OrtStatus* s = ort->api->BindOutput(bind, (yuv ? outYuv : outRgb)[p], v)) { ok = false; if (err) *err = ort->api->GetErrorMessage(s); ort->api->ReleaseStatus(s); }
            }
        }
        if (ok) {
            const double t0 = nowMs();
            if (OrtStatus* s = ort->api->RunWithBinding(graph, nullptr, bind)) { ok = false; if (err) *err = std::string("synth: ") + ort->api->GetErrorMessage(s); ort->api->ReleaseStatus(s); }
            stats_.synthMs = nowMs() - t0;
            ++stats_.synthRuns;
        }
        for (int p = 0; p < 3 && ok; ++p) {
            if (dst[p] == out.plane[p]) continue;
            const int pw = (yuv && p) ? o_.width / 2 : o_.width, ph = (yuv && p) ? o_.height / 2 : o_.height;
            for (int y = 0; y < ph; ++y) std::memcpy(out.plane[p] + y * out.stride[p], dst[p] + size_t(y) * pw, size_t(pw) * 2);
        }
        for (OrtValue* v : tmp) ort->api->ReleaseValue(v);
        return ok;
    }

    Stats stats() const override {
        Stats s = stats_;
        s.netW = net_.w; s.netH = net_.h; s.padW = net_.padW; s.padH = net_.padH;
        return s;
    }
    std::string describe() const override { return describe_; }

    Ort* ort = nullptr;
    OrtSession* pack = nullptr;
    OrtSession* addt = nullptr;
    OrtSession* net = nullptr;
    OrtSession* synth = nullptr;
    OrtSession* synthMap = nullptr;   // synth_map-<fmt>.onnx (renderMap), when the graphs folder has it

private:
    std::vector<float> map0_, map1_;   // renderMap's scale maps at the padded network size
    bool runNetwork(float t, std::string* err) {
        OrtIoBinding* ba = bindings_[1];
        ort->api->ClearBoundInputs(ba);
        ort->api->ClearBoundOutputs(ba);
        OrtValue* tv = nullptr;
        float tt = t;
        if (!tensor(&tt, {1}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &tv, err)) return false;
        bool ok = true;
        std::string e;
        if (OrtStatus* s = ort->api->BindInput(ba, "small_ab", packed_[0])) { e = ort->api->GetErrorMessage(s); ort->api->ReleaseStatus(s); ok = false; }
        if (ok) if (OrtStatus* s = ort->api->BindInput(ba, "t", tv)) { e = ort->api->GetErrorMessage(s); ort->api->ReleaseStatus(s); ok = false; }
        if (ok) ok = bindOut(ba, "net_input");
        double t0 = nowMs();
        if (ok) if (OrtStatus* s = ort->api->RunWithBinding(addt, nullptr, ba)) { e = ort->api->GetErrorMessage(s); ort->api->ReleaseStatus(s); ok = false; }
        std::vector<OrtValue*> netIn;
        if (ok) ok = outputs(ba, &netIn, 1, &e);
        ort->api->ReleaseValue(tv);
        if (!ok) { if (err) *err = "addt: " + e; return false; }
        OrtIoBinding* bn = bindings_[2];
        ort->api->ClearBoundInputs(bn);
        ort->api->ClearBoundOutputs(bn);
        if (netInName_.empty()) {
            char* name = nullptr;
            PMML_TRY(ort->api->SessionGetInputName(net, 0, alloc_, &name));
            netInName_ = name;
            alloc_->Free(alloc_, name);
            size_t n = 0;
            PMML_TRY(ort->api->SessionGetOutputCount(net, &n));
            if (n < 4) { if (err) *err = "the PushFlow model has no grid/mask outputs (" + std::to_string(n) + " outputs)"; return false; }
            for (size_t i = 0; i < 4; ++i) {
                PMML_TRY(ort->api->SessionGetOutputName(net, i, alloc_, &name));
                netOutNames_.push_back(name);
                alloc_->Free(alloc_, name);
            }
        }
        PMML_TRY(ort->api->BindInput(bn, netInName_.c_str(), netIn[0]));
        for (auto& n : netOutNames_) if (!bindOut(bn, n.c_str())) { if (err) *err = "BindOutput " + n; return false; }
        PMML_TRY(ort->api->RunWithBinding(net, nullptr, bn));
        if (o_.syncTiming) PMML_TRY(ort->api->SynchronizeBoundOutputs(bn));
        stats_.netMs = nowMs() - t0;
        ++stats_.networkRuns;
        releaseValues(&netIn);
        if (!outputs(bn, &netOut_, 4, err)) return false;
        if (!model_.maskForFirst) { if (err) *err = "models whose mask weights the second frame are not supported yet"; return false; }
        return true;
    }

    bool session(const std::string& path, const std::vector<std::pair<std::string, int64_t>>& dims,
                 const std::vector<std::pair<std::string, std::vector<int64_t>>>& inits, OrtSession** out, std::string* err) {
        if (!fileExists(path)) { if (err) *err = "missing " + path; return false; }
        OrtSessionOptions* so = nullptr;
        PMML_TRY(ort->api->CreateSessionOptions(&so));
        struct Guard { Ort* o; OrtSessionOptions* s; ~Guard() { o->api->ReleaseSessionOptions(s); } } guard{ort, so};
        PMML_TRY(ort->api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL));
        for (auto& d : dims) PMML_TRY(ort->api->AddFreeDimensionOverrideByName(so, d.first.c_str(), d.second));
        EpChoice c;
        c.ownedMemory = choice_.ownedMemory;
        if (!appendEp(ort, so, o_, &c, err)) return false;
        choice_ = c;
        if (inits.empty()) {
            PMML_TRY(ort->api->CreateSession(ort->env, ortPath(path).c_str(), so, out));
            return true;
        }
        // size constants: make_graphs.py writes unique int64 sentinels, replaced here in the model bytes, so they are
        // true constants for every EP (DirectML ignores AddInitializer overrides of the file's own initializers)
        std::string bytes = readText(path);
        for (auto& i : inits) {
            int hits = 0;
            uint64_t sentinel = 0x504D4D4C00000000ull | uint64_t(i.first == "nh" ? 1 : i.first == "nw" ? 2 : i.first == "padh" ? 3 : 4);
            char pat[8], val[8];
            const int64_t v = i.second.front();
            for (int k = 0; k < 8; ++k) { pat[k] = char((sentinel >> (8 * k)) & 0xff); val[k] = char((uint64_t(v) >> (8 * k)) & 0xff); }
            for (size_t pos = 0; (pos = bytes.find(std::string(pat, 8), pos)) != std::string::npos; pos += 8) { bytes.replace(pos, 8, val, 8); ++hits; }
            (void)hits;
        }
        PMML_TRY(ort->api->CreateSessionFromArray(ort->env, bytes.data(), bytes.size(), so, out));
        return true;
    }

    bool tensor(void* data, std::vector<int64_t> shape, ONNXTensorElementDataType type, OrtValue** out, std::string* err) {
        size_t n = 1;
        for (auto d : shape) n *= size_t(d);
        const size_t bytes = n * (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16 ? 2 : 4);
        PMML_TRY(ort->api->CreateTensorWithDataAsOrtValue(cpu_, data, bytes, shape.data(), shape.size(), type, out));
        return true;
    }

    bool bindOut(OrtIoBinding* b, const char* name) {
        OrtStatus* s = ort->api->BindOutputToDevice(b, name, choice_.deviceMemory ? choice_.deviceMemory : cpu_);
        if (s) { ort->api->ReleaseStatus(s); return false; }
        return true;
    }

    bool outputs(OrtIoBinding* b, std::vector<OrtValue*>* dst, size_t want, std::string* err) {
        releaseValues(dst);
        OrtValue** vals = nullptr;
        size_t n = 0;
        PMML_TRY(ort->api->GetBoundOutputValues(b, alloc_, &vals, &n));
        for (size_t i = 0; i < n; ++i) dst->push_back(vals[i]);
        if (vals) alloc_->Free(alloc_, vals);
        if (n < want) { if (err) *err = "expected " + std::to_string(want) + " outputs, got " + std::to_string(n); return false; }
        return true;
    }

    void releaseValues(std::vector<OrtValue*>* v) {
        if (!ort) return;
        for (OrtValue* x : *v) ort->api->ReleaseValue(x);
        v->clear();
    }

    void release() {
        if (!ort) return;
        releaseValues(&inputs_);
        releaseValues(&packed_);
        releaseValues(&netOut_);
        for (auto* b : bindings_) ort->api->ReleaseIoBinding(b);
        for (OrtSession* s : {pack, addt, net, synth, synthMap}) if (s) ort->api->ReleaseSession(s);
        if (cpu_) ort->api->ReleaseMemoryInfo(cpu_);
        if (choice_.ownedMemory) ort->api->ReleaseMemoryInfo(choice_.ownedMemory);
    }

    Options o_;
    ModelFiles model_;
    NetworkSize net_;
    std::array<float, 4> affine_{};
    std::array<float, 8> colour_{};
    std::string fmt_, describe_, netInName_;
    std::vector<std::string> netOutNames_;
    EpChoice choice_;
    OrtMemoryInfo* cpu_ = nullptr;
    OrtAllocator* alloc_ = nullptr;
    std::vector<OrtIoBinding*> bindings_;
    std::vector<OrtValue*> inputs_, packed_, netOut_;
    std::vector<std::vector<uint16_t>> staging_, outStaging_;
    int phases_ = 0;
    float netT_ = -1.f;
    double initMs_ = 0;
    Stats stats_;
};

class OrtRawSession final : public RawSession {
public:
    ~OrtRawSession() override {
        if (!ort) return;
        if (session_) ort->api->ReleaseSession(session_);
        if (cpu_) ort->api->ReleaseMemoryInfo(cpu_);
    }
    bool init(const std::string& runtimeDir, const Backend& b, const std::string& model, const std::vector<std::pair<std::string, int64_t>>& dims,
              const std::string& cacheDir, std::string* err) {
        ort = loadOrt(runtimeDir, b.flavor, err);
        if (!ort) return false;
        Options o;
        o.runtimeDir = runtimeDir;
        o.backend = b;
        o.cacheDir = cacheDir;
        OrtSessionOptions* so = nullptr;
        PMML_TRY(ort->api->CreateSessionOptions(&so));
        struct Guard { Ort* o; OrtSessionOptions* s; ~Guard() { o->api->ReleaseSessionOptions(s); } } guard{ort, so};
        PMML_TRY(ort->api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL));
        for (auto& d : dims) PMML_TRY(ort->api->AddFreeDimensionOverrideByName(so, d.first.c_str(), d.second));
        EpChoice c;
        if (!appendEp(ort, so, o, &c, err)) return false;
        describe_ = c.describe;
        PMML_TRY(ort->api->CreateSession(ort->env, ortPath(model).c_str(), so, &session_));
        PMML_TRY(ort->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &cpu_));
        OrtAllocator* alloc = nullptr;
        PMML_TRY(ort->api->GetAllocatorWithDefaultOptions(&alloc));
        char* name = nullptr;
        PMML_TRY(ort->api->SessionGetInputName(session_, 0, alloc, &name));
        input_ = name;
        alloc->Free(alloc, name);
        size_t n = 0;
        PMML_TRY(ort->api->SessionGetOutputCount(session_, &n));
        for (size_t i = 0; i < n; ++i) {
            PMML_TRY(ort->api->SessionGetOutputName(session_, i, alloc, &name));
            outputs_.push_back(name);
            alloc->Free(alloc, name);
        }
        return true;
    }
    size_t outputCount() const override { return outputs_.size(); }
    bool run(const float* input, const std::vector<int64_t>& shape, const std::vector<float*>& outs, const std::vector<size_t>& bytes, std::string* err) override {
        // tests: PMML_TEST_FAIL_RUN_AFTER=N makes every run after the first N fail (a host's mid-stream fallback)
        if (failAfter_ < 0) { const char* e = std::getenv("PMML_TEST_FAIL_RUN_AFTER"); failAfter_ = e && *e ? std::max(0, std::atoi(e)) : 0x7fffffff; }
        if (runs_++ >= failAfter_) { if (err) *err = "Run: failure injected by PMML_TEST_FAIL_RUN_AFTER"; return false; }
        size_t count = 1;
        for (auto d : shape) count *= size_t(d);
        OrtValue* in = nullptr;
        PMML_TRY(ort->api->CreateTensorWithDataAsOrtValue(cpu_, const_cast<float*>(input), count * 4, shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in));
        std::vector<const char*> names;
        for (auto& n : outputs_) names.push_back(n.c_str());
        std::vector<OrtValue*> got(outputs_.size(), nullptr);
        const char* inName = input_.c_str();
        OrtStatus* s = ort->api->Run(session_, nullptr, &inName, &in, 1, names.data(), names.size(), got.data());
        ort->api->ReleaseValue(in);
        bool ok = true;
        if (s) { if (err) *err = std::string("Run: ") + ort->api->GetErrorMessage(s); ort->api->ReleaseStatus(s); ok = false; }
        for (size_t i = 0; i < got.size(); ++i) {
            if (ok && i < outs.size() && outs[i]) {
                void* data = nullptr;
                OrtTensorTypeAndShapeInfo* info = nullptr;
                size_t elems = 0;
                OrtStatus* s1 = ort->api->GetTensorMutableData(got[i], &data);
                OrtStatus* s2 = s1 ? nullptr : ort->api->GetTensorTypeAndShape(got[i], &info);
                if (s1 || s2) { ok = false; if (err) *err = "output " + std::to_string(i) + " unreadable"; if (s1) ort->api->ReleaseStatus(s1); if (s2) ort->api->ReleaseStatus(s2); }
                else {
                    if (OrtStatus* s3 = ort->api->GetTensorShapeElementCount(info, &elems)) ort->api->ReleaseStatus(s3);
                    ort->api->ReleaseTensorTypeAndShapeInfo(info);
                    if (elems * 4 > bytes[i]) { ok = false; if (err) *err = "output " + std::to_string(i) + " is larger than its buffer"; }
                    else std::memcpy(outs[i], data, elems * 4);
                }
            }
            if (got[i]) ort->api->ReleaseValue(got[i]);
        }
        return ok;
    }
    std::string describe() const override { return describe_; }

private:
    Ort* ort = nullptr;
    OrtSession* session_ = nullptr;
    OrtMemoryInfo* cpu_ = nullptr;
    std::string input_, describe_;
    std::vector<std::string> outputs_;
    int failAfter_ = -1, runs_ = 0;
};

// --- the anime upscalers (pmml/upscale.h): sr_in -> model -> sr_out, intermediates on the device ----------------------
class OrtUpscaler final : public Upscaler {
public:
    ~OrtUpscaler() override { release(); }

    bool init(const UpscaleOptions& u, std::string* err) {
        u_ = u;
        m_ = findUpscaleModel(u.model);
        if (!m_) { if (err) *err = "unknown anime upscaler " + u.model; return false; }
        ort = loadOrt(u.runtimeDir, u.backend.flavor, err);
        if (!ort) return false;
        o_.runtimeDir = u.runtimeDir; o_.backend = u.backend; o_.cacheDir = u.cacheDir;
        rgba_ = u.io == UpscaleIo::Rgba8;
        recurrent_ = std::string(m_->kind) == "animesr";
        luma_ = std::string(m_->kind) == "luma";
        const int w = u.width, h = u.height, s = m_->scale;
        if (w < 8 || h < 8 || (!rgba_ && (w % 2 || h % 2))) { if (err) *err = "upscaler: bad input size"; return false; }
        outW_ = rgba_ && u.outWidth > 0 ? u.outWidth : w * s;
        outH_ = rgba_ && u.outHeight > 0 ? u.outHeight : h * s;
        inPw_ = rgba_ && u.inPitchPx > w ? u.inPitchPx : w;
        outPw_ = rgba_ && u.outPitchPx > outW_ ? u.outPitchPx : outW_;
        colour_ = u.colour.packed();
        PMML_TRY(ort->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &cpu_));
        PMML_TRY(ort->api->GetAllocatorWithDefaultOptions(&alloc_));
        const std::string dir = u.modelsDir.empty() ? joinPath(u.graphsDir, "anime-sr") : u.modelsDir;
        const std::string kind = luma_ ? "luma" : "rgb", io = rgba_ ? "rgba8" : "yuv";
        const int ch = rgba_ ? h : h / 2, cw = rgba_ ? w : w / 2;
        const std::vector<std::pair<std::string, int64_t>> dims = {
            {"pmml_h", h}, {"pmml_w", w}, {"pmml_ch", ch}, {"pmml_cw", cw}, {"H", h}, {"W", w}, {"H4", 4 * h}, {"W4", 4 * w},
            {"pmml_oh2", h * s}, {"pmml_ow2", w * s}, {"pmml_oh2c", h * s / 2}, {"pmml_ow2c", w * s / 2}, {"pmml_oh", outH_}, {"pmml_ow", outW_},
            {"pmml_ipw", inPw_}, {"pmml_opw", outPw_}};
        const double t0 = nowMs();
        if (!session(joinPath(u.graphsDir, "sr_in_" + kind + "-" + io + ".onnx"), dims, rgba_, &in_, err)) return false;
        if (!session(joinPath(dir, m_->graph), dims, false, &net_, err)) return false;
        if (!session(joinPath(u.graphsDir, "sr_out_" + kind + "-" + io + ".onnx"), dims, rgba_, &out_, err)) return false;
        for (OrtSession* x : {in_, net_, out_}) {
            OrtIoBinding* b = nullptr;
            PMML_TRY(ort->api->CreateIoBinding(x, &b));
            bind_.push_back(b);
        }
        char* name = nullptr;
        PMML_TRY(ort->api->SessionGetInputName(net_, 0, alloc_, &name)); netIn_ = name; alloc_->Free(alloc_, name);
        PMML_TRY(ort->api->SessionGetOutputName(net_, 0, alloc_, &name)); netOut_ = name; alloc_->Free(alloc_, name);
        if (recurrent_) {
            zeroFb_.assign(size_t(3) * 16 * w * h, 0.f);
            zeroState_.assign(size_t(64) * w * h, 0.f);
        }
        std::ostringstream d;
        d << "pmml " << kVersion << " ORT " << ort->flavor << ", " << choice_.describe << "; anime upscaler " << m_->id << " (" << m_->licence << ") "
          << w << "x" << h << " -> " << outW_ << "x" << outH_ << (rgba_ ? " RGBA8" : " YUV 4:2:0") << ", sessions in " << int(nowMs() - t0) << " ms";
        describe_ = d.str();
        return true;
    }

    int outWidth() const override { return outW_; }
    int outHeight() const override { return outH_; }
    void reset() override { releaseValues(&prevX_); releaseValues(&fb_); releaseValues(&state_); }
    UpscaleStats stats() const override { return stats_; }
    std::string describe() const override { return describe_; }

    bool run(const Frame& in, const OutFrame& out, std::string* err) override {
        if (rgba_) { if (err) *err = "this upscaler was created for RGBA8"; return false; }
        const int w = u_.width, h = u_.height;
        std::vector<OrtValue*> tmp;
        static const char* inNames[3] = {"in_y", "in_u", "in_v"};
        stagingIn_.resize(3);
        bool ok = true;
        for (int p = 0; p < 3 && ok; ++p) {
            const int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
            const uint16_t* src = in.plane[p];
            if (in.stride[p] != pw) {
                auto& st = stagingIn_[size_t(p)];
                st.resize(size_t(pw) * ph);
                for (int y = 0; y < ph; ++y) std::memcpy(&st[size_t(y) * pw], src + y * in.stride[p], size_t(pw) * 2);
                src = st.data();
            }
            OrtValue* v = nullptr;
            ok = tensor(const_cast<uint16_t*>(src), {1, 1, ph, pw}, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16, 2, &v, err);
            if (ok) tmp.push_back(v);
        }
        OrtValue* cv = nullptr;
        if (ok) { ok = tensor(colour_.data(), {8}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, 4, &cv, err); if (ok) tmp.push_back(cv); }
        std::vector<std::pair<const char*, OrtValue*>> ins;
        for (int p = 0; p < 3 && ok; ++p) ins.push_back({inNames[p], tmp[size_t(p)]});
        if (ok) ins.push_back({"colour", cv});
        // outputs into the caller's planes when contiguous
        static const char* outNames[3] = {"out_y", "out_u", "out_v"};
        stagingOut_.resize(3);
        uint16_t* dst[3] = {};
        std::vector<std::pair<const char*, OrtValue*>> outs;
        for (int p = 0; p < 3 && ok; ++p) {
            const int pw = p ? outW_ / 2 : outW_, ph = p ? outH_ / 2 : outH_;
            dst[p] = out.plane[p];
            if (out.stride[p] != pw) { stagingOut_[size_t(p)].resize(size_t(pw) * ph); dst[p] = stagingOut_[size_t(p)].data(); }
            OrtValue* v = nullptr;
            ok = tensor(dst[p], {1, 1, ph, pw}, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16, 2, &v, err);
            if (ok) { tmp.push_back(v); outs.push_back({outNames[p], v}); }
        }
        if (ok) ok = chain(ins, outs, err);
        for (int p = 0; p < 3 && ok; ++p) {
            if (dst[p] == out.plane[p]) continue;
            const int pw = p ? outW_ / 2 : outW_, ph = p ? outH_ / 2 : outH_;
            for (int y = 0; y < ph; ++y) std::memcpy(out.plane[p] + y * out.stride[p], dst[p] + size_t(y) * pw, size_t(pw) * 2);
        }
        for (OrtValue* v : tmp) ort->api->ReleaseValue(v);
        return ok;
    }

    bool runRgba(const uint8_t* in, ptrdiff_t inPitch, uint8_t* out, ptrdiff_t outPitch, std::string* err) override {
        if (!rgba_) { if (err) *err = "this upscaler was created for YUV planes"; return false; }
        const int w = u_.width, h = u_.height;
        const uint8_t* src = in;
        if (inPitch != ptrdiff_t(inPw_) * 4) {
            rgbaIn_.resize(size_t(inPw_) * h * 4);
            for (int y = 0; y < h; ++y) std::memcpy(&rgbaIn_[size_t(y) * inPw_ * 4], in + y * inPitch, size_t(w) * 4);
            src = rgbaIn_.data();
        }
        uint8_t* dst = out;
        if (outPitch != ptrdiff_t(outPw_) * 4) { rgbaOut_.resize(size_t(outPw_) * outH_ * 4); dst = rgbaOut_.data(); }
        std::vector<OrtValue*> tmp;
        OrtValue *iv = nullptr, *ov = nullptr;
        bool ok = tensor(const_cast<uint8_t*>(src), {1, h, inPw_, 4}, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8, 1, &iv, err);
        if (ok) { tmp.push_back(iv); ok = tensor(dst, {1, outH_, outPw_, 4}, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8, 1, &ov, err); }
        if (ok) { tmp.push_back(ov); ok = chain({{"in_rgba", iv}}, {{"out_rgba", ov}}, err); }
        if (ok && dst != out)
            for (int y = 0; y < outH_; ++y) std::memcpy(out + y * outPitch, dst + size_t(y) * outPw_ * 4, size_t(outW_) * 4);
        for (OrtValue* v : tmp) ort->api->ReleaseValue(v);
        return ok;
    }

    std::string deviceKind() const override {
        if (!rgba_ || !choice_.deviceMemory) return "host";
        return o_.backend.ep == "dml" ? "dml" : o_.backend.ep == "trt-rtx" ? "cuda" : "host";
    }
#if PMML_HAVE_DML
    void* dmlDevice() const override { return deviceKind() == "dml" ? g_dml.device : nullptr; }
    void* dmlQueue() const override { return deviceKind() == "dml" ? g_dml.queue : nullptr; }
    void* wrapDml(void* resource, std::string* err) override {
        const OrtDmlApi* dml = nullptr;
        void* alloc = nullptr;
        if (failed(ort->api->GetExecutionProviderApi("DML", ort->version, reinterpret_cast<const void**>(&dml)), "DML API", err) || !dml) return nullptr;
        if (failed(dml->CreateGPUAllocationFromD3DResource(static_cast<ID3D12Resource*>(resource), &alloc), "CreateGPUAllocationFromD3DResource", err)) return nullptr;
        return alloc;
    }
    void freeDml(void* alloc) override {
        const OrtDmlApi* dml = nullptr;
        if (alloc && !ort->api->GetExecutionProviderApi("DML", ort->version, reinterpret_cast<const void**>(&dml)) && dml) dml->FreeGPUAllocation(alloc);
    }
#endif
    bool runRgbaDevice(void* in, void* out, std::string* err) override {
        if (deviceKind() == "host") { if (err) *err = "no zero copy on " + o_.backend.key(); return false; }
        std::vector<OrtValue*> tmp;
        const int64_t inShape[4] = {1, u_.height, inPw_, 4}, outShape[4] = {1, outH_, outPw_, 4};
        OrtValue *iv = nullptr, *ov = nullptr;
        PMML_TRY(ort->api->CreateTensorWithDataAsOrtValue(choice_.deviceMemory, in, size_t(u_.height) * inPw_ * 4, inShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8, &iv));
        tmp.push_back(iv);
        OrtStatus* s = ort->api->CreateTensorWithDataAsOrtValue(choice_.deviceMemory, out, size_t(outH_) * outPw_ * 4, outShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8, &ov);
        bool ok = !failed(s, "output tensor", err);
        if (ok) { tmp.push_back(ov); ok = chain({{"in_rgba", iv}}, {{"out_rgba", ov}}, err); }
        if (ok && deviceKind() == "cuda") ok = !failed(ort->api->SynchronizeBoundOutputs(bind_[2]), "sync", err);   // written when it returns
        for (OrtValue* v : tmp) ort->api->ReleaseValue(v);
        return ok;
    }

private:
    struct Release {   // releases a list of values at scope end
        OrtUpscaler* s;
        std::vector<OrtValue*>* v;
        ~Release() { s->releaseValues(v); }
    };

    bool failed(OrtStatus* s, const char* what, std::string* err) {
        if (!s) return false;
        if (err) *err = std::string(what) + ": " + ort->api->GetErrorMessage(s);
        ort->api->ReleaseStatus(s);
        return true;
    }

    // sr_in (host inputs) -> the model (device) -> sr_out (outputs into host tensors)
    bool chain(const std::vector<std::pair<const char*, OrtValue*>>& ins, const std::vector<std::pair<const char*, OrtValue*>>& outs, std::string* err) {
        for (auto* b : bind_) { ort->api->ClearBoundInputs(b); ort->api->ClearBoundOutputs(b); }
        // 1. the picture -> x (+ cbcr)
        for (auto& i : ins) if (failed(ort->api->BindInput(bind_[0], i.first, i.second), "sr_in input", err)) return false;
        if (!bindOut(bind_[0], "x") || (luma_ && !bindOut(bind_[0], "cbcr"))) { if (err) *err = "sr_in: BindOutput failed"; return false; }
        double t0 = nowMs();
        if (failed(ort->api->RunWithBinding(in_, nullptr, bind_[0]), "sr_in", err)) return false;
        if (u_.syncTiming && failed(ort->api->SynchronizeBoundOutputs(bind_[0]), "sync", err)) return false;
        stats_.inMs = nowMs() - t0;
        std::vector<OrtValue*> pre, net, host;
        Release relPre{this, &pre}, relNet{this, &net}, relHost{this, &host};
        if (!outputs(bind_[0], &pre, luma_ ? 2 : 1, err)) return false;
        // 2. the model
        OrtIoBinding* bn = bind_[1];
        if (recurrent_) {
            OrtValue* prev = prevX_.empty() ? pre[0] : prevX_[0];
            OrtValue* fb = fb_.empty() ? nullptr : fb_[0];
            OrtValue* st = state_.empty() ? nullptr : state_[0];
            if (!fb) { if (!tensor(zeroFb_.data(), {1, 3, 4 * u_.height, 4 * u_.width}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, 4, &fb, err)) return false; host.push_back(fb); }
            if (!st) { if (!tensor(zeroState_.data(), {1, 64, u_.height, u_.width}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, 4, &st, err)) return false; host.push_back(st); }
            const std::pair<const char*, OrtValue*> netIns[5] = {{"prev", prev}, {"cur", pre[0]}, {"next", pre[0]}, {"fb", fb}, {"state", st}};
            for (auto& i : netIns) if (failed(ort->api->BindInput(bn, i.first, i.second), "model input", err)) return false;
            if (!bindOut(bn, "y") || !bindOut(bn, "state_out")) { if (err) *err = "model: BindOutput failed"; return false; }
        } else {
            if (failed(ort->api->BindInput(bn, netIn_.c_str(), pre[0]), "model input", err)) return false;
            if (!bindOut(bn, netOut_.c_str())) { if (err) *err = "model: BindOutput failed"; return false; }
        }
        t0 = nowMs();
        if (failed(ort->api->RunWithBinding(net_, nullptr, bn), "model", err)) return false;
        if (u_.syncTiming && failed(ort->api->SynchronizeBoundOutputs(bn), "sync", err)) return false;
        stats_.netMs = nowMs() - t0;
        if (!outputs(bn, &net, recurrent_ ? 2 : 1, err)) return false;
        // 3. -> the picture
        OrtIoBinding* bo = bind_[2];
        if (failed(ort->api->BindInput(bo, "y", net[0]), "sr_out input", err)) return false;
        if (luma_ && failed(ort->api->BindInput(bo, "cbcr", pre[1]), "sr_out cbcr", err)) return false;
        if (!rgba_) {
            OrtValue* cv = nullptr;
            if (!tensor(colour_.data(), {8}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, 4, &cv, err)) return false;
            host.push_back(cv);
            if (failed(ort->api->BindInput(bo, "colour", cv), "sr_out colour", err)) return false;
        }
        for (auto& o : outs) if (failed(ort->api->BindOutput(bo, o.first, o.second), "sr_out output", err)) return false;
        t0 = nowMs();
        if (failed(ort->api->RunWithBinding(out_, nullptr, bo), "sr_out", err)) return false;
        stats_.outMs = nowMs() - t0;
        ++stats_.runs;
        if (recurrent_) {   // carried to the next frame: this frame, its output and the state
            releaseValues(&prevX_); prevX_.push_back(pre[0]); pre[0] = nullptr;
            releaseValues(&fb_); fb_.push_back(net[0]); net[0] = nullptr;
            releaseValues(&state_); state_.push_back(net[1]); net[1] = nullptr;
        }
        return true;
    }

    bool session(const std::string& path, const std::vector<std::pair<std::string, int64_t>>& dims, bool outSize, OrtSession** out, std::string* err) {
        if (!fileExists(path)) { if (err) *err = "missing " + path; return false; }
        OrtSessionOptions* so = nullptr;
        PMML_TRY(ort->api->CreateSessionOptions(&so));
        struct Guard { Ort* o; OrtSessionOptions* s; ~Guard() { o->api->ReleaseSessionOptions(s); } } guard{ort, so};
        PMML_TRY(ort->api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL));
        for (auto& d : dims) PMML_TRY(ort->api->AddFreeDimensionOverrideByName(so, d.first.c_str(), d.second));
        EpChoice c;
        c.ownedMemory = choice_.ownedMemory;
        if (!appendEp(ort, so, o_, &c, err)) return false;
        choice_ = c;
        if (!outSize) {   // by path: the model graphs' weights are external data next to them
            PMML_TRY(ort->api->CreateSession(ort->env, ortPath(path).c_str(), so, out));
            return true;
        }
        // sr_out_*-rgba8: the output size sentinels (anime_sr_graphs.py SENTINEL_OUT) replaced in the bytes
        std::string bytes = readText(path);
        for (int k = 0; k < 4; ++k) {   // outh, outw, inw, opad
            const uint64_t sentinel = 0x504D4D4C00000000ull | uint64_t(5 + k);
            const int64_t v = k == 0 ? outH_ : k == 1 ? outW_ : k == 2 ? u_.width : outPw_ - outW_;
            char pat[8], val[8];
            for (int i = 0; i < 8; ++i) { pat[i] = char((sentinel >> (8 * i)) & 0xff); val[i] = char((uint64_t(v) >> (8 * i)) & 0xff); }
            for (size_t pos = 0; (pos = bytes.find(std::string(pat, 8), pos)) != std::string::npos; pos += 8) bytes.replace(pos, 8, val, 8);
        }
        PMML_TRY(ort->api->CreateSessionFromArray(ort->env, bytes.data(), bytes.size(), so, out));
        return true;
    }

    bool tensor(void* data, std::vector<int64_t> shape, ONNXTensorElementDataType type, size_t elem, OrtValue** out, std::string* err) {
        size_t n = 1;
        for (auto d : shape) n *= size_t(d);
        PMML_TRY(ort->api->CreateTensorWithDataAsOrtValue(cpu_, data, n * elem, shape.data(), shape.size(), type, out));
        return true;
    }

    bool bindOut(OrtIoBinding* b, const char* name) {
        OrtStatus* s = ort->api->BindOutputToDevice(b, name, choice_.deviceMemory ? choice_.deviceMemory : cpu_);
        if (s) { ort->api->ReleaseStatus(s); return false; }
        return true;
    }

    bool outputs(OrtIoBinding* b, std::vector<OrtValue*>* dst, size_t want, std::string* err) {
        releaseValues(dst);
        OrtValue** vals = nullptr;
        size_t n = 0;
        PMML_TRY(ort->api->GetBoundOutputValues(b, alloc_, &vals, &n));
        for (size_t i = 0; i < n; ++i) dst->push_back(vals[i]);
        if (vals) alloc_->Free(alloc_, vals);
        if (n < want) { if (err) *err = "expected " + std::to_string(want) + " outputs, got " + std::to_string(n); return false; }
        return true;
    }

    void releaseValues(std::vector<OrtValue*>* v) {
        if (!ort) return;
        for (OrtValue* x : *v) if (x) ort->api->ReleaseValue(x);
        v->clear();
    }

    void release() {
        if (!ort) return;
        reset();
        for (auto* b : bind_) ort->api->ReleaseIoBinding(b);
        bind_.clear();
        for (OrtSession* s : {in_, net_, out_}) if (s) ort->api->ReleaseSession(s);
        if (cpu_) ort->api->ReleaseMemoryInfo(cpu_);
        if (choice_.ownedMemory) ort->api->ReleaseMemoryInfo(choice_.ownedMemory);
    }

    Ort* ort = nullptr;
    UpscaleOptions u_;
    Options o_;
    const UpscaleModel* m_ = nullptr;
    bool rgba_ = false, recurrent_ = false, luma_ = true;
    int outW_ = 0, outH_ = 0, inPw_ = 0, outPw_ = 0;
    std::array<float, 8> colour_{};
    OrtSession *in_ = nullptr, *net_ = nullptr, *out_ = nullptr;
    std::vector<OrtIoBinding*> bind_;
    std::string netIn_, netOut_, describe_;
    EpChoice choice_;
    OrtMemoryInfo* cpu_ = nullptr;
    OrtAllocator* alloc_ = nullptr;
    std::vector<OrtValue*> prevX_, fb_, state_;   // AnimeSR's carry
    std::vector<float> zeroFb_, zeroState_;
    std::vector<std::vector<uint16_t>> stagingIn_, stagingOut_;
    std::vector<uint8_t> rgbaIn_, rgbaOut_;
    UpscaleStats stats_;
};

}  // namespace

std::unique_ptr<RawSession> createOrtRaw(const std::string& runtimeDir, const Backend& b, const std::string& model,
                                         const std::vector<std::pair<std::string, int64_t>>& dims, const std::string& cacheDir, std::string* err) {
    auto p = std::make_unique<OrtRawSession>();
    if (!p->init(runtimeDir, b, model, dims, cacheDir, err)) return nullptr;
    return p;
}

std::unique_ptr<Interpolator> createOrt(const Options& o, std::string* err) {
    auto p = std::make_unique<OrtInterpolator>();
    if (!p->init(o, err)) return nullptr;
    return p;
}

std::unique_ptr<Upscaler> createOrtUpscaler(const UpscaleOptions& o, std::string* err) {
    auto p = std::make_unique<OrtUpscaler>();
    if (!p->init(o, err)) return nullptr;
    return p;
}

std::vector<DeviceInfo> listOrt(const std::string& runtimeDir, std::string* log) {
    std::vector<DeviceInfo> v;
    std::string flavor = envStr("PMML_FLAVOR");
    if (flavor.empty()) flavor = "core";
    std::string err;
    Ort* ort = loadOrt(runtimeDir, flavor, &err);
    if (!ort) { if (log) *log += "ort/" + flavor + ": " + err + "\n"; return v; }
    if (log) *log += "ort/" + flavor + ": API " + std::to_string(ort->version) + "\n";
    DeviceInfo cpu;
    cpu.backend.flavor = flavor; cpu.backend.ep = "cpu"; cpu.type = "CPU"; cpu.name = "ONNX Runtime CPU";
    v.push_back(cpu);
#if PMML_HAVE_DML
    if (flavor == "directml") {
        auto adapters = dxgiAdapters(envStr("PMML_DML_WARP") == "1");
        for (size_t i = 0; i < adapters.size(); ++i) {
            DeviceInfo d;
            d.backend.flavor = flavor; d.backend.ep = "dml"; d.backend.device = int(i); d.type = "GPU";
            describeAdapter(adapters[i], &d.name, &d.vendorId, &d.deviceId, &d.driver);
            adapters[i]->Release();
            v.push_back(d);
        }
    }
#endif
#if defined(__APPLE__)
    if (flavor == "core") { DeviceInfo d; d.backend.ep = "coreml"; d.type = "GPU"; d.name = "CoreML (ANE / GPU / CPU)"; v.push_back(d); }
#endif
    if (flavor == "migraphx") { DeviceInfo d; d.backend.flavor = flavor; d.backend.ep = "migraphx"; d.type = "GPU"; d.name = "MIGraphX (ROCm)"; v.push_back(d); }
#if ORT_API_VERSION >= 22
    if (registerPlugins(ort, log)) {
        for (const auto& p : pluginEps(runtimeDir)) {
            for (const char* type : {"GPU", "NPU"}) {
                auto devs = epDevices(ort, p.epName, type);
                for (size_t i = 0; i < devs.size(); ++i) {
                    const OrtHardwareDevice* hw = ort->api->EpDevice_Device(devs[i]);
                    DeviceInfo d;
                    d.backend.flavor = flavor;
                    d.backend.ep = (p.id == "qnn" && std::string(type) == "GPU") ? "qnn-gpu" : p.id;
                    d.backend.device = int(i);
                    d.type = type;
                    d.vendor = ort->api->HardwareDevice_Vendor(hw);
                    d.vendorId = ort->api->HardwareDevice_VendorId(hw);
                    d.deviceId = ort->api->HardwareDevice_DeviceId(hw);
                    d.name = metadata(ort, ort->api->HardwareDevice_Metadata(hw), "Description");
                    if (d.name.empty()) d.name = d.vendor;
                    d.driver = metadata(ort, ort->api->HardwareDevice_Metadata(hw), "DriverVersion");
                    d.note = p.epName;
                    v.push_back(d);
                }
            }
        }
    }
#endif
    return v;
}

}  // namespace pmml::detail
