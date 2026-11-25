// SPDX-License-Identifier: GPL-3.0-or-later
#include "ncnn_warp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#if NCNN_VULKAN
#if __has_include(<ncnn/gpu.h>)
#include <ncnn/command.h>
#include <ncnn/gpu.h>
#include <ncnn/pipeline.h>
#else
#include <command.h>
#include <gpu.h>
#include <pipeline.h>
#endif
#endif

namespace pmml::detail {

#if NCNN_VULKAN
static const char kWarpComp[] = R"GLSL(
#version 450
#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
#if NCNN_fp16_arithmetic
#extension GL_EXT_shader_explicit_arithmetic_types_float16: require
#endif
layout (binding = 0) readonly buffer image_blob { sfp image_data[]; };
layout (binding = 1) readonly buffer flow_blob { sfp flow_data[]; };
layout (binding = 2) writeonly buffer top_blob { sfp top_data[]; };
layout (push_constant) uniform parameter { int w; int h; int c; int cstep; } p;
void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);
    if (gx >= p.w || gy >= p.h || gz >= p.c) return;
    afp sx = afp(gx) + buffer_ld1(flow_data, gy * p.w + gx);
    afp sy = afp(gy) + buffer_ld1(flow_data, p.cstep + gy * p.w + gx);
    int x0 = int(floor(sx)), y0 = int(floor(sy));
    int x1 = clamp(x0 + 1, 0, p.w - 1), y1 = clamp(y0 + 1, 0, p.h - 1);
    x0 = clamp(x0, 0, p.w - 1); y0 = clamp(y0, 0, p.h - 1);
    afp a = sx - afp(x0), b = sy - afp(y0);
    int base = gz * p.cstep;
    afp v0 = buffer_ld1(image_data, base + y0 * p.w + x0);
    afp v1 = buffer_ld1(image_data, base + y0 * p.w + x1);
    afp v2 = buffer_ld1(image_data, base + y1 * p.w + x0);
    afp v3 = buffer_ld1(image_data, base + y1 * p.w + x1);
    afp v = (v0 * (afp(1.f) - a) + v1 * a) * (afp(1.f) - b) + (v2 * (afp(1.f) - a) + v3 * a) * b;
    buffer_st1(top_data, base + gy * p.w + gx, v);
}
)GLSL";
#endif

RifeWarp::RifeWarp() {
    one_blob_only = false;
    support_inplace = false;
#if NCNN_VULKAN
    support_vulkan = true;
#endif
    support_packing = false;   // elempack 1 only: one shader
}

int RifeWarp::create_pipeline(const ncnn::Option& opt) {
#if NCNN_VULKAN
    if (!vkdev) return 0;
    static std::vector<uint32_t> spirv;
    static std::mutex lock;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (spirv.empty() && ncnn::compile_spirv_module(kWarpComp, int(sizeof(kWarpComp) - 1), opt, spirv) != 0) return -1;
    }
    std::vector<ncnn::vk_specialization_type> specializations;
    pipeline_ = new ncnn::Pipeline(vkdev);
    pipeline_->set_optimal_local_size_xyz();
    pipeline_->create(spirv.data(), spirv.size() * 4, specializations);
#endif
    return 0;
}

int RifeWarp::destroy_pipeline(const ncnn::Option&) {
#if NCNN_VULKAN
    delete pipeline_;
    pipeline_ = nullptr;
#endif
    return 0;
}

int RifeWarp::forward(const std::vector<ncnn::Mat>& bottom, std::vector<ncnn::Mat>& top, const ncnn::Option& opt) const {
    const ncnn::Mat& image = bottom[0];
    const ncnn::Mat& flow = bottom[1];
    const int w = image.w, h = image.h, c = image.c;
    ncnn::Mat& out = top[0];
    out.create(w, h, c, size_t(4), opt.blob_allocator);
    if (out.empty()) return -100;
    const float* fx = flow.channel(0);
    const float* fy = flow.channel(1);
    #pragma omp parallel for num_threads(opt.num_threads)
    for (int q = 0; q < c; ++q) {
        const ncnn::Mat ch = image.channel(q);
        float* o = out.channel(q);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                const float sx = float(x) + fx[i], sy = float(y) + fy[i];
                int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
                const int x1 = std::clamp(x0 + 1, 0, w - 1), y1 = std::clamp(y0 + 1, 0, h - 1);
                x0 = std::clamp(x0, 0, w - 1);
                y0 = std::clamp(y0, 0, h - 1);
                const float a = sx - float(x0), b = sy - float(y0);
                const float v0 = ch.row(y0)[x0], v1 = ch.row(y0)[x1], v2 = ch.row(y1)[x0], v3 = ch.row(y1)[x1];
                o[i] = (v0 * (1 - a) + v1 * a) * (1 - b) + (v2 * (1 - a) + v3 * a) * b;
            }
    }
    return 0;
}

#if NCNN_VULKAN
int RifeWarp::forward(const std::vector<ncnn::VkMat>& bottom, std::vector<ncnn::VkMat>& top, ncnn::VkCompute& cmd, const ncnn::Option& opt) const {
    const ncnn::VkMat& image = bottom[0];
    const ncnn::VkMat& flow = bottom[1];
    ncnn::VkMat& out = top[0];
    out.create(image.w, image.h, image.c, image.elemsize, image.elempack, opt.blob_vkallocator);
    if (out.empty()) return -100;
    std::vector<ncnn::VkMat> bindings = {image, flow, out};
    std::vector<ncnn::vk_constant_type> constants(4);
    constants[0].i = out.w;
    constants[1].i = out.h;
    constants[2].i = out.c;
    constants[3].i = int(out.cstep);
    cmd.record_pipeline(pipeline_, bindings, constants, out);
    return 0;
}
#endif

ncnn::Layer* RifeWarpCreator(void*) { return new RifeWarp(); }

}  // namespace pmml::detail
