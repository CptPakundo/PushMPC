// SPDX-License-Identifier: GPL-3.0-or-later
// The "rife.Warp" custom layer the RIFE ncnn models use (rife-ncnn-vulkan by nihui and its v4.x model sets, MIT):
// out(x, y) = image sampled bilinearly at (x + flow_x, y + flow_y), coordinates clamped to the picture. Written for pmml
// after the published behaviour of nihui's layer (same arithmetic, including the weights taken after the clamp), CPU and
// Vulkan (elempack 1).
#pragma once
#if __has_include(<ncnn/layer.h>)
#include <ncnn/layer.h>
#else
#include <layer.h>
#endif

namespace pmml::detail {

class RifeWarp : public ncnn::Layer {
public:
    RifeWarp();
    int create_pipeline(const ncnn::Option& opt) override;
    int destroy_pipeline(const ncnn::Option& opt) override;
    int forward(const std::vector<ncnn::Mat>& bottom, std::vector<ncnn::Mat>& top, const ncnn::Option& opt) const override;
#if NCNN_VULKAN
    int forward(const std::vector<ncnn::VkMat>& bottom, std::vector<ncnn::VkMat>& top, ncnn::VkCompute& cmd, const ncnn::Option& opt) const override;
#endif
private:
#if NCNN_VULKAN
    ncnn::Pipeline* pipeline_ = nullptr;
#endif
};

ncnn::Layer* RifeWarpCreator(void* userdata);

}  // namespace pmml::detail
