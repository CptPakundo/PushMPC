// SPDX-License-Identifier: GPL-3.0-or-later
// pmml's ncnn backend: the same PushFlow models (converted by pnnx, pmml/scripts/ncnn_models.py, one per padded network
// size) on ncnn's Vulkan compute (any Vulkan 1.1 GPU: RADV, ANV, NVK, NVIDIA, MoltenVK) or its CPU path.
// The network runs on the GPU; the colour conversion, the downscale and the full-size rebuild (synthCpu) run on the
// CPU here, multithreaded. A Vulkan rebuild sharing ncnn's VkDevice is the next step (track-ml-runtime.md).
#include "internal.h"

#include <ncnn/gpu.h>
#include <ncnn/net.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

namespace pmml::detail {

namespace {

int threadsHint() { return int(std::max(1u, std::thread::hardware_concurrency())); }

class NcnnInterpolator final : public Interpolator {
public:
    ~NcnnInterpolator() override { net_.clear(); }

    bool init(const Options& o, std::string* err) {
        o_ = o;
        if (!findModel(o, &model_, err)) return false;   // facts (pad, mask) from the ONNX model's side files
        n_ = networkSize(o.width, o.height, o.tier, o.motionHeight, model_.pad);
        affine_ = gridAffine(o.width, o.height, n_);
        const std::string dir = o.ncnnModelsDir.empty() ? joinPath(o.runtimeDir, "ncnn-models") : o.ncnnModelsDir;
        const std::string stem = joinPath(dir, "pushflow-" + modelRole(o.tier) + "-" + std::to_string(n_.padW) + "x" + std::to_string(n_.padH));
        if (!fileExists(stem + ".ncnn.param")) { if (err) *err = "no ncnn model " + stem + ".ncnn.param (pnnx converts one per padded network size)"; return false; }
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
        if (net_.load_param((stem + ".ncnn.param").c_str()) != 0 || net_.load_model((stem + ".ncnn.bin").c_str()) != 0) { if (err) *err = "ncnn cannot load " + stem; return false; }
        rgbA_.resize(size_t(3) * o.width * o.height);
        rgbB_.resize(rgbA_.size());
        out_.resize(rgbA_.size());
        std::ostringstream d;
        d << "pmml " << kVersion << " ncnn " << (vulkan ? "Vulkan on " + device_ : std::string("CPU")) << "; PushFlow " << o.tier << " network " << n_.w << "x" << n_.h
          << " (padded " << n_.padW << "x" << n_.padH << ") for " << o.width << "x" << o.height << ", rebuild on the CPU";
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
        stats_.packMs = nowMs() - t0;
        return true;
    }

    bool render(float t, const OutFrame& out, std::string* err) override {
        const bool once = phases_ > 1;
        const float tn = once ? o_.tRef : t;
        if (netT_ != tn) {
            const double t0 = nowMs();
            ncnn::Mat in(n_.padW, n_.padH, 7);
            for (int c = 0; c < 7; ++c) {
                float* p = in.channel(c);
                if (c < 6) std::memcpy(p, &small_[size_t(c) * n_.padW * n_.padH], size_t(n_.padW) * n_.padH * 4);
                else std::fill(p, p + size_t(n_.padW) * n_.padH, tn);
            }
            ncnn::Extractor ex = net_.create_extractor();
            ex.input("in0", in);
            ncnn::Mat g0, g1, m;
            if (ex.extract("out1", g0) != 0 || ex.extract("out2", g1) != 0 || ex.extract("out3", m) != 0) { if (err) *err = "ncnn extract failed"; return false; }
            // grids [1, ph, pw, 2] arrive as (w = 2, h = pw, c = ph); the mask [1, 1, ph, pw] as (w = pw, h = ph, c = 1)
            auto flat = [&](const ncnn::Mat& mat, std::vector<float>* dst, size_t want) -> bool {
                dst->resize(want);
                const size_t per = size_t(mat.w) * mat.h * (mat.dims == 4 ? mat.d : 1);
                if (size_t(mat.c) * per != want) return false;
                for (int c = 0; c < mat.c; ++c) std::memcpy(dst->data() + size_t(c) * per, mat.channel(c), per * 4);
                return true;
            };
            const size_t cells = size_t(n_.padW) * n_.padH;
            if (!flat(g0, &g0_, cells * 2) || !flat(g1, &g1_, cells * 2) || !flat(m, &mask_, cells)) {
                if (err) *err = "unexpected ncnn output shapes (" + std::to_string(g0.w) + "x" + std::to_string(g0.h) + "x" + std::to_string(g0.c) + ")";
                return false;
            }
            stats_.netMs = nowMs() - t0;
            ++stats_.networkRuns;
            netT_ = tn;
        }
        const double t0 = nowMs();
        const float s0 = once ? t / o_.tRef : 1.f, s1 = once ? (1.f - t) / (1.f - o_.tRef) : 1.f;
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
