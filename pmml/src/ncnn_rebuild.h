// SPDX-License-Identifier: GPL-3.0-or-later
// "pmml.Rebuild": PushFlowReconstruct as an ncnn layer (CPU and Vulkan), so the ncnn backend keeps the full-size rebuild
// on the GPU. Inputs: the two full-size frames (fa, fb: 3 x H x W, RGB 0..1), the network's final flows (f0, f1:
// 2 x ph x pw, pixels at the padded network size) and blend mask (m: 1 x ph x pw, the weight of fa). The arithmetic is
// synthCpu's with PushFlow's grids: every output pixel samples the fields at its network position, scales the
// displacement by s0 / s1 ("flow once") and reads the full-size frames there (bilinear, coordinates clamped).
// The per-run values (s0, s1, the network's own size) are set on the layer before an extraction.
#pragma once
#if __has_include(<ncnn/layer.h>)
#include <ncnn/layer.h>
#else
#include <layer.h>
#endif

namespace pmml::detail {

class PushFlowRebuild : public ncnn::Layer {
public:
    PushFlowRebuild();
    int create_pipeline(const ncnn::Option& opt) override;
    int destroy_pipeline(const ncnn::Option& opt) override;
    int forward(const std::vector<ncnn::Mat>& bottom, std::vector<ncnn::Mat>& top, const ncnn::Option& opt) const override;
#if NCNN_VULKAN
    int forward(const std::vector<ncnn::VkMat>& bottom, std::vector<ncnn::VkMat>& top, ncnn::VkCompute& cmd, const ncnn::Option& opt) const override;
#endif
    float s0 = 1.f, s1 = 1.f;   // displacement factors
    int nw = 0, nh = 0;         // the network's own size inside the padded fields

private:
#if NCNN_VULKAN
    ncnn::Pipeline* pipeline_ = nullptr;
#endif
};

ncnn::Layer* PushFlowRebuildCreator(void* userdata);

// The rebuild as a net of its own: five inputs (fa, fb, f0, f1, m) and the layer; output blob "out".
extern const char kPushFlowRebuildParam[];

}  // namespace pmml::detail
