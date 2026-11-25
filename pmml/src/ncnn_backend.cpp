// SPDX-License-Identifier: GPL-3.0-or-later
// pmml's ncnn backend: the RIFE releases PushFlow's tiers use (4.25 lite for Fast/Balanced, 4.26 for Quality) in their
// ncnn conversions (VapourSynth-RIFE-ncnn-Vulkan's model set, MIT, pinned in scripts/ml-runtime-pin.json) with the
// rife.Warp layer (ncnn_warp.cpp), on ncnn's Vulkan compute (any Vulkan 1.1 GPU: RADV, ANV, NVK, NVIDIA, MoltenVK) or
// its CPU path. The network's final flows and blend mask are taken from the graph (the last two warps' flow inputs and
// the sigmoid) and turned into PushFlow's grids, so the full-size rebuild and "flow once" are the same as with ONNX
// Runtime. (PushFlow's own ONNX exports do not convert: pnnx refuses their batch-of-two warps.)
// On Vulkan the network and the full-size rebuild (pmml.Rebuild, ncnn_rebuild.cpp: PushFlowReconstruct as a second
// net on the same VkDevice) run on the GPU: the flows and the mask stay in device memory between the two nets, the
// full-size frames go up once per pair and only the finished picture comes back. The colour conversion and the
// downscale run on the CPU. PMML_NCNN_CPU_REBUILD=1 (and the CPU path) rebuild with synthCpu instead.
#include "internal.h"

#if __has_include(<ncnn/net.h>)
#include <ncnn/gpu.h>
#include <ncnn/net.h>
#else   // the release package's CMake config puts include/ncnn itself on the path
#include <gpu.h>
#include <net.h>
#endif
#if __has_include(<ncnn/datareader.h>)
#include <ncnn/datareader.h>
#else
#include <datareader.h>
#endif

#include "ncnn_rebuild.h"
#include "ncnn_warp.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <sstream>
#include <thread>

namespace pmml::detail {

namespace {

// weights of a net that has none (the rebuild net): ncnn still wants a model load to create the pipelines
class EmptyReader : public ncnn::DataReader {
public:
    int scan(const char*, void*) const override { return 0; }
    size_t read(void* buf, size_t size) const override { std::memset(buf, 0, size); return size; }
};

int threadsHint() { return int(std::max(1u, std::thread::hardware_concurrency())); }

class NcnnInterpolator final : public Interpolator {
public:
    ~NcnnInterpolator() override {
#if NCNN_VULKAN
        faVk_.release(); fbVk_.release(); vf0_.release(); vf1_.release(); vm_.release();
        rebuild_.clear();
#endif
        net_.clear();
#if NCNN_VULKAN
        delete blobAlloc_;
        delete stagingAlloc_;
#endif
    }

    bool init(const Options& o, std::string* err) {
        o_ = o;
        if (!findModel(o, &model_, err)) return false;   // facts (pad, mask) from the ONNX model's side files
        n_ = networkSize(o.width, o.height, o.tier, o.motionHeight, model_.pad);
        affine_ = gridAffine(o.width, o.height, n_);
        const std::string dir = o.ncnnModelsDir.empty() ? joinPath(o.runtimeDir, "ncnn-models") : o.ncnnModelsDir;
        const std::string stem = joinPath(joinPath(dir, modelRole(o.tier) == "quality" ? "rife-v4.26_ensembleFalse" : "rife-v4.25-lite_ensembleFalse"), "flownet");
        if (!fileExists(stem + ".param")) { if (err) *err = "no ncnn model " + stem + ".param"; return false; }
        if (!findBlobs(stem + ".param", err)) return false;
        const bool vulkan = o.backend.ep != "cpu";
        net_.opt.use_vulkan_compute = vulkan;
        net_.opt.num_threads = threadsHint();
        net_.opt.use_fp16_packed = o.fp16;
        net_.opt.use_fp16_storage = o.fp16;
        net_.opt.use_fp16_arithmetic = false;
        if (vulkan) {
            if (ncnn::get_gpu_count() <= o.backend.device) { if (err) *err = "ncnn sees no Vulkan device " + std::to_string(o.backend.device); return false; }
            net_.set_vulkan_device(o.backend.device);
            device_ = ncnn::get_gpu_info(o.backend.device).device_name();
        }
        net_.register_custom_layer("rife.Warp", RifeWarpCreator);
        if (net_.load_param((stem + ".param").c_str()) != 0 || net_.load_model((stem + ".bin").c_str()) != 0) { if (err) *err = "ncnn cannot load " + stem; return false; }
#if NCNN_VULKAN
        const char* cpuRebuild = std::getenv("PMML_NCNN_CPU_REBUILD");
        gpuRebuild_ = vulkan && !(cpuRebuild && std::string(cpuRebuild) == "1");
        if (gpuRebuild_) {
            const ncnn::VulkanDevice* vkdev = net_.vulkan_device();
            blobAlloc_ = new ncnn::VkBlobAllocator(vkdev);
            stagingAlloc_ = new ncnn::VkStagingAllocator(vkdev);
            rebuild_.opt = net_.opt;
            rebuild_.set_vulkan_device(o.backend.device);
            rebuild_.register_custom_layer("pmml.Rebuild", PushFlowRebuildCreator);
            EmptyReader empty;
            if (rebuild_.load_param_mem(kPushFlowRebuildParam) != 0 || rebuild_.load_model(empty) != 0) { if (err) *err = "ncnn: the rebuild net did not load"; return false; }
            for (ncnn::Layer* l : rebuild_.layers())
                if (auto* r = dynamic_cast<PushFlowRebuild*>(l)) rebuildLayer_ = r;
            if (!rebuildLayer_) { if (err) *err = "ncnn: no pmml.Rebuild layer"; return false; }
            rebuildLayer_->nw = n_.w; rebuildLayer_->nh = n_.h;
            vkOpt_ = net_.opt;
            vkOpt_.blob_vkallocator = blobAlloc_;
            vkOpt_.workspace_vkallocator = blobAlloc_;
            vkOpt_.staging_vkallocator = stagingAlloc_;
        }
#endif
        rgbA_.resize(size_t(3) * o.width * o.height);
        rgbB_.resize(rgbA_.size());
        out_.resize(rgbA_.size());
        std::ostringstream d;
        d << "pmml " << kVersion << " ncnn " << (vulkan ? "Vulkan on " + device_ : std::string("CPU")) << "; PushFlow " << o.tier << " network " << n_.w << "x" << n_.h
          << " (padded " << n_.padW << "x" << n_.padH << ") for " << o.width << "x" << o.height << (gpuRebuild_ ? ", rebuild on the GPU" : ", rebuild on the CPU");
        describe_ = d.str();
        return true;
    }

    bool setPair(const Frame& a, const Frame& b, int phases, std::string* err) override {
        phases_ = phases;
        netT_ = -1.f;
        const double t0 = nowMs();
        planesToRgb(a, o_.format, o_.colour, o_.width, o_.height, rgbA_.data());
        planesToRgb(b, o_.format, o_.colour, o_.width, o_.height, rgbB_.data());
        // both frames at the network size, padded by repeating the edge (pack-*.onnx)
        small_.assign(size_t(6) * n_.padW * n_.padH, 0.f);
        std::vector<float> tmp(size_t(3) * n_.w * n_.h);
        for (int f = 0; f < 2; ++f) {
            resizeLinear(f ? rgbB_.data() : rgbA_.data(), 3, o_.height, o_.width, n_.h, n_.w, tmp.data());
            for (int c = 0; c < 3; ++c)
                for (int y = 0; y < n_.padH; ++y)
                    for (int x = 0; x < n_.padW; ++x)
                        small_[(size_t(f * 3 + c) * n_.padH + y) * n_.padW + x] = tmp[(size_t(c) * n_.h + std::min(y, n_.h - 1)) * n_.w + std::min(x, n_.w - 1)];
        }
#if NCNN_VULKAN
        if (gpuRebuild_) {   // the full-size frames go up once per pair
            ncnn::Mat fa(o_.width, o_.height, 3), fb(o_.width, o_.height, 3);
            const size_t plane = size_t(o_.width) * o_.height;
            for (int c = 0; c < 3; ++c) {
                std::memcpy(fa.channel(c), &rgbA_[size_t(c) * plane], plane * 4);
                std::memcpy(fb.channel(c), &rgbB_[size_t(c) * plane], plane * 4);
            }
            ncnn::VkCompute cmd(net_.vulkan_device());
            cmd.record_upload(fa, faVk_, vkOpt_);
            cmd.record_upload(fb, fbVk_, vkOpt_);
            if (cmd.submit_and_wait() != 0) { if (err) *err = "ncnn: frame upload failed"; return false; }
        }
#endif
        stats_.packMs = nowMs() - t0;
        return true;
    }

    bool render(float t, const OutFrame& out, std::string* err) override {
        const bool once = phases_ > 1;
        const float tn = once ? o_.tRef : t;
        if (netT_ != tn) {
            const double t0 = nowMs();
            const size_t cells = size_t(n_.padW) * n_.padH;
            ncnn::Mat in0(n_.padW, n_.padH, 3), in1(n_.padW, n_.padH, 3), in2(n_.padW, n_.padH, 1);
            for (int c = 0; c < 3; ++c) {
                std::memcpy(in0.channel(c), &small_[size_t(c) * cells], cells * 4);
                std::memcpy(in1.channel(c), &small_[size_t(c + 3) * cells], cells * 4);
            }
            in2.fill(tn);
            ncnn::Extractor ex = net_.create_extractor();
            ex.input("in0", in0);
            ex.input("in1", in1);
            ex.input("in2", in2);
#if NCNN_VULKAN
            if (gpuRebuild_) {   // the flows and the mask stay on the GPU for the rebuild net
                ex.set_blob_vkallocator(blobAlloc_);
                ex.set_workspace_vkallocator(blobAlloc_);
                ex.set_staging_vkallocator(stagingAlloc_);
                ncnn::VkCompute cmd(net_.vulkan_device());
                if (ex.extract(flow0_.c_str(), vf0_, cmd) != 0 || ex.extract(flow1_.c_str(), vf1_, cmd) != 0 || ex.extract(mask_name_.c_str(), vm_, cmd) != 0 ||
                    cmd.submit_and_wait() != 0) { if (err) *err = "ncnn extract (Vulkan) failed"; return false; }
                if (vf0_.w != n_.padW || vf0_.h != n_.padH || vm_.w != n_.padW || vm_.h != n_.padH) {
                    if (err) *err = "unexpected ncnn output shapes (" + std::to_string(vf0_.w) + "x" + std::to_string(vf0_.h) + ")";
                    return false;
                }
                stats_.netMs = nowMs() - t0;
                ++stats_.networkRuns;
                netT_ = tn;
            }
#endif
        }
        if (netT_ != tn) {
            const double t0 = nowMs();
            const size_t cells = size_t(n_.padW) * n_.padH;
            ncnn::Mat in0(n_.padW, n_.padH, 3), in1(n_.padW, n_.padH, 3), in2(n_.padW, n_.padH, 1);
            for (int c = 0; c < 3; ++c) {
                std::memcpy(in0.channel(c), &small_[size_t(c) * cells], cells * 4);
                std::memcpy(in1.channel(c), &small_[size_t(c + 3) * cells], cells * 4);
            }
            in2.fill(tn);
            ncnn::Extractor ex = net_.create_extractor();
            ex.input("in0", in0);
            ex.input("in1", in1);
            ex.input("in2", in2);
            ncnn::Mat f0, f1, m;
            if (ex.extract(flow0_.c_str(), f0) != 0 || ex.extract(flow1_.c_str(), f1) != 0 || ex.extract(mask_name_.c_str(), m) != 0) { if (err) *err = "ncnn extract failed"; return false; }
            if (f0.w != n_.padW || f0.h != n_.padH || f0.c < 2 || m.w != n_.padW || m.h != n_.padH) {
                if (err) *err = "unexpected ncnn output shapes (" + std::to_string(f0.w) + "x" + std::to_string(f0.h) + "x" + std::to_string(f0.c) + ")";
                return false;
            }
            // flows (pixels at the padded network size) -> PushFlow's grids (normalised, align_corners = 1) and the mask
            g0_.resize(cells * 2); g1_.resize(cells * 2); mask_.resize(cells);
            const float sx = 2.f / float(std::max(n_.padW - 1, 1)), sy = 2.f / float(std::max(n_.padH - 1, 1));
            for (int y = 0; y < n_.padH; ++y) {
                const float* ax = f0.channel(0).row(y); const float* ay = f0.channel(1).row(y);
                const float* bx = f1.channel(0).row(y); const float* by = f1.channel(1).row(y);
                const float* mm = m.channel(0).row(y);
                for (int x = 0; x < n_.padW; ++x) {
                    const size_t i = size_t(y) * n_.padW + x;
                    g0_[i * 2] = (float(x) + ax[x]) * sx - 1.f; g0_[i * 2 + 1] = (float(y) + ay[x]) * sy - 1.f;
                    g1_[i * 2] = (float(x) + bx[x]) * sx - 1.f; g1_[i * 2 + 1] = (float(y) + by[x]) * sy - 1.f;
                    mask_[i] = mm[x];
                }
            }
            stats_.netMs = nowMs() - t0;
            ++stats_.networkRuns;
            netT_ = tn;
        }
        const double t0 = nowMs();
        const float s0 = once ? t / o_.tRef : 1.f, s1 = once ? (1.f - t) / (1.f - o_.tRef) : 1.f;
#if NCNN_VULKAN
        if (gpuRebuild_) {
            rebuildLayer_->s0 = s0; rebuildLayer_->s1 = s1;
            ncnn::Extractor ex = rebuild_.create_extractor();
            ex.set_blob_vkallocator(blobAlloc_);
            ex.set_workspace_vkallocator(blobAlloc_);
            ex.set_staging_vkallocator(stagingAlloc_);
            ex.input("fa", faVk_); ex.input("fb", fbVk_); ex.input("f0", vf0_); ex.input("f1", vf1_); ex.input("m", vm_);
            ncnn::VkCompute cmd(net_.vulkan_device());
            ncnn::VkMat vout;
            ncnn::Mat picture;
            if (ex.extract("out", vout, cmd) != 0) { if (err) *err = "ncnn rebuild failed"; return false; }
            cmd.record_download(vout, picture, vkOpt_);
            if (cmd.submit_and_wait() != 0 || picture.w != o_.width || picture.h != o_.height || picture.c != 3) { if (err) *err = "ncnn rebuild download failed"; return false; }
            const size_t plane = size_t(o_.width) * o_.height;
            for (int c = 0; c < 3; ++c) std::memcpy(&out_[size_t(c) * plane], picture.channel(c), plane * 4);
            rgbToPlanes(out_.data(), o_.format, o_.colour, o_.width, o_.height, out);
            stats_.synthMs = nowMs() - t0;
            ++stats_.synthRuns;
            return true;
        }
#endif
        synthCpu(rgbA_.data(), rgbB_.data(), o_.width, o_.height, g0_.data(), g1_.data(), mask_.data(), n_, affine_, s0, s1, out_.data(), threadsHint());
        rgbToPlanes(out_.data(), o_.format, o_.colour, o_.width, o_.height, out);
        stats_.synthMs = nowMs() - t0;
        ++stats_.synthRuns;
        return true;
    }

    Stats stats() const override {
        Stats s = stats_;
        s.netW = n_.w; s.netH = n_.h; s.padW = n_.padW; s.padH = n_.padH;
        return s;
    }
    std::string describe() const override { return describe_; }

private:
    // the blobs PushFlow's rebuild needs, read from the param text: the flow inputs of the last two rife.Warp layers
    // (frame 0, frame 1) and the Sigmoid's output (the blend weight of frame 0)
    bool findBlobs(const std::string& param, std::string* err) {
        std::ifstream f(param);
        std::string line, warpA, warpB;
        std::vector<std::pair<std::string, std::string>> warps;   // (image input, flow input)
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string type, name;
            int nin = 0, nout = 0;
            if (!(ss >> type >> name >> nin >> nout)) continue;
            std::vector<std::string> ins(size_t(std::max(nin, 0))), outs(size_t(std::max(nout, 0)));
            for (auto& b : ins) ss >> b;
            for (auto& b : outs) ss >> b;
            if (type == "rife.Warp" && ins.size() == 2) warps.push_back({ins[0], ins[1]});
            if (type == "Sigmoid" && !outs.empty()) mask_name_ = outs[0];
        }
        if (warps.size() < 2 || mask_name_.empty()) { if (err) *err = "not a RIFE v4 ncnn model (no final warps / sigmoid): " + param; return false; }
        const auto& a = warps[warps.size() - 2];
        const auto& b = warps[warps.size() - 1];
        if (a.first.rfind("in0", 0) != 0 || b.first.rfind("in1", 0) != 0) { if (err) *err = "unexpected final warps in " + param; return false; }
        flow0_ = a.second;
        flow1_ = b.second;
        return true;
    }

    std::string flow0_, flow1_, mask_name_;
    bool gpuRebuild_ = false;
#if NCNN_VULKAN
    ncnn::Net rebuild_;
    PushFlowRebuild* rebuildLayer_ = nullptr;
    ncnn::VkBlobAllocator* blobAlloc_ = nullptr;
    ncnn::VkStagingAllocator* stagingAlloc_ = nullptr;
    ncnn::Option vkOpt_;
    ncnn::VkMat faVk_, fbVk_, vf0_, vf1_, vm_;
#endif
    Options o_;
    ModelFiles model_;
    NetworkSize n_;
    std::array<float, 4> affine_{};
    ncnn::Net net_;
    std::string device_, describe_;
    std::vector<float> rgbA_, rgbB_, out_, small_, g0_, g1_, mask_;
    int phases_ = 0;
    float netT_ = -1.f;
    Stats stats_;
};

}  // namespace

std::unique_ptr<Interpolator> createNcnn(const Options& o, std::string* err) {
    if (o.backend.ep != "cpu") ncnn::create_gpu_instance();
    auto p = std::make_unique<NcnnInterpolator>();
    if (!p->init(o, err)) return nullptr;
    return p;
}

std::vector<DeviceInfo> listNcnn(const std::string& runtimeDir, std::string* log) {
    std::vector<DeviceInfo> v;
    ncnn::create_gpu_instance();
    const int n = ncnn::get_gpu_count();
    if (log) *log += "ncnn: " + std::to_string(n) + " Vulkan device(s)\n";
    for (int i = 0; i < n; ++i) {
        const ncnn::GpuInfo& g = ncnn::get_gpu_info(i);
        DeviceInfo d;
        d.backend.runtime = "ncnn"; d.backend.flavor = "ncnn"; d.backend.ep = "vulkan"; d.backend.device = i;
        d.name = g.device_name();
        d.vendorId = g.vendor_id();
        d.deviceId = g.device_id();
        d.type = g.type() == 3 ? "CPU" : "GPU";
        d.note = g.type() == 0 ? "discrete" : g.type() == 1 ? "integrated" : g.type() == 2 ? "virtual" : "cpu";
        v.push_back(d);
    }
    return v;
}

}  // namespace pmml::detail
