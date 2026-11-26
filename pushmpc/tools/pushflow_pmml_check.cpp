// pushflow_pmml_check: PushFlow (PushFPS's PushFlowBackend, as pushcore builds it) end to end on a D3D12 device, with
// the network either on PushFlow's own DirectML session or on pmml's (VEYRA_PUSHFLOW_PMML = "<pmml runtime>|<backend>",
// read by PushFlowBackend::initialize). Two synthetic frames (a textured picture and a moving block) are packed, the
// network runs, and the generated frame at t = 0.5 (rebuilt at full size by PushFlowReconstruct) is read back and
// written as raw RGBA, so CI can compare the paths pixel by pixel. Runs on WARP (hosted runners: DirectML works there).
//
// usage: pushflow_pmml_check <runtime dir with pushflow\> <shader dir> <out.rgba> [--warp 1] [--size WxH] [--tier 0..2]
//   prints PROVIDER <what PushFlow says it runs on> and RESULT ok|failed
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "veyra/RuntimePaths.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/pushflow/PushFlowBackend.h"

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) { std::cout << "usage: pushflow_pmml_check <runtime dir> <shader dir> <out.rgba> [--warp 1] [--size WxH] [--tier N]\n"; return 2; }
    const std::wstring runtimeDir = argv[1], shaderDir = argv[2], outFile = argv[3];
    bool warp = false;
    uint32_t w = 512, h = 288, tier = 0, frames = 1;
    for (int i = 4; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i], v = argv[i + 1];
        if (k == L"--warp") warp = v == L"1";
        else if (k == L"--size") { w = uint32_t(_wtoi(v.c_str())); h = uint32_t(_wtoi(v.substr(v.find(L'x') + 1).c_str())); }
        else if (k == L"--tier") tier = uint32_t(_wtoi(v.c_str()));
        else if (k == L"--frames") frames = std::max(1u, uint32_t(_wtoi(v.c_str())));
    }
    veyra::runtime::setShaderDirectoryOverride(shaderDir);

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { std::cout << "RESULT failed (DXGI)\n"; return 1; }
    ComPtr<IDXGIAdapter1> adapter;
    if (warp) factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    else factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    ComPtr<ID3D12Device> device;
    if (!adapter || FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) { std::cout << "RESULT failed (no D3D12 device)\n"; return 1; }
    { DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d); std::wcout << L"ADAPTER " << d.Description << L"\n"; }

    ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> alloc; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) || FAILED(list->Close()) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { std::cout << "RESULT failed (queue)\n"; return 1; }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t value = 0;
    auto begin = [&] { return SUCCEEDED(alloc->Reset()) && SUCCEEDED(list->Reset(alloc.Get(), nullptr)); };
    auto submitWait = [&] {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* l[] = {list.Get()};
        queue->ExecuteCommandLists(1, l);
        if (FAILED(queue->Signal(fence.Get(), ++value))) return false;
        if (fence->GetCompletedValue() < value) { fence->SetEventOnCompletion(value, event); WaitForSingleObject(event, 300000); }
        return fence->GetCompletedValue() >= value;
    };
    const UINT pitch = (w * 4 + 255) & ~255u;
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; x.Transition.StateBefore = a; x.Transition.StateAfter = b;
        list->ResourceBarrier(1, &x);
    };
    // a textured picture (hashed noise, fine lines) with a bright block that moves `shift` pixels
    auto frameTexture = [&](int shift) -> ComPtr<ID3D12Resource> {
        auto tex = veyra::pipeline::makeTexture(device.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false);
        auto upload = veyra::pipeline::makeUploadBuffer(device.Get(), uint64_t(pitch) * h);
        if (!tex || !upload) return nullptr;
        uint8_t* m = nullptr;
        upload->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                uint8_t* p = m + size_t(y) * pitch + size_t(x) * 4;
                const uint32_t hsh = (x * 73856093u) ^ (y * 19349663u);
                int v = 70 + int(((hsh ^ (hsh >> 13)) * 1274126177u) >> 26) + ((x % 23 == 0 || y % 19 == 0) ? 60 : 0);
                const int bx = int(x) - (100 + shift), by = int(y) - int(h / 3);
                if (bx >= 0 && bx < 64 && by >= 0 && by < 48) v = 230 - ((bx / 8 + by / 8) % 2) * 120;
                p[0] = uint8_t(v); p[1] = uint8_t(v * 9 / 10 + 12); p[2] = uint8_t(v * 8 / 10 + 25); p[3] = 255;
            }
        upload->Unmap(0, nullptr);
        if (!begin()) return nullptr;
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, pitch};
        to.pResource = tex.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        barrier(tex.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!submitWait()) return nullptr;
        return tex;
    };
    auto a = frameTexture(0), b = frameTexture(24);
    auto output = veyra::pipeline::makeTexture(device.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    if (!a || !b || !output) { std::cout << "RESULT failed (textures)\n"; return 1; }

    veyra::pushflow::PushFlowBackend flow;
    if (!flow.initialize(device.Get(), queue.Get(), runtimeDir, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, veyra::pushflow::Encoding::Encoded, tier)) {
        std::cout << "RESULT failed (PushFlow did not start)\n";
        return 1;
    }
    const auto& st = flow.status();
    std::cout << "PROVIDER " << st.provider << "\nMODEL " << st.model << " " << st.precision << " network " << st.modelW << "x" << st.modelH << " padded " << st.padW << "x"
              << st.padH << " rebuild " << (st.rebuild ? 1 : 0) << " grids " << (st.grids ? 1 : 0) << "\n";
    // --frames N: the same pair N times, every frame must be made (a pmml EP that fails mid-stream must hand over to
    // DirectML within the frame: PMML_TEST_FAIL_RUN_AFTER); the last one is written
    bool ok = true;
    uint32_t failed = 0;
    for (uint32_t f = 0; f < frames && ok; ++f) {
        const bool made = begin() && flow.pack(list.Get(), a.Get(), b.Get(), 0.5f) && submitWait() && flow.infer();
        if (!made) { ++failed; std::cout << "FRAME " << f << " failed\n"; ok = false; break; }
        ok = begin();
        if (ok) {
            if (f == 0) barrier(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);   // later frames: still COPY_DEST
            ok = flow.resolve(list.Get(), a.Get(), b.Get(), output.Get()) && submitWait();
        }
    }
    std::cout << "FRAMES " << frames << " failed " << failed << "\nPROVIDER-AFTER " << flow.status().provider << "\n";
    // read the generated frame back (the resolve leaves the output as a copy destination it wrote into)
    std::vector<uint8_t> pixels;
    if (ok) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = uint64_t(pitch) * h; bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        ok = SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))) && begin();
        if (ok) {
            barrier(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = output.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource = rb.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, pitch};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            ok = submitWait();
        }
        if (ok) {
            uint8_t* m = nullptr;
            rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
            pixels.resize(size_t(w) * h * 4);
            for (uint32_t y = 0; y < h; ++y) std::memcpy(&pixels[size_t(y) * w * 4], m + size_t(y) * pitch, size_t(w) * 4);
            rb->Unmap(0, nullptr);
        }
    }
    if (ok) {
        if (FILE* f = _wfopen(outFile.c_str(), L"wb")) { std::fwrite(pixels.data(), 1, pixels.size(), f); std::fclose(f); }
        else ok = false;
    }
    std::cout << "RESULT " << (ok ? "ok" : "failed") << " " << w << "x" << h << "\n";
    return ok ? 0 : 1;
}
