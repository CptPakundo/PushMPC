// SPDX-License-Identifier: GPL-3.0-or-later
#include "ncnn_rebuild.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

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

const char kPushFlowRebuildParam[] =
    "7767517\n"
    "6 6\n"
    "Input fa 0 1 fa\n"
    "Input fb 0 1 fb\n"
    "Input f0 0 1 f0\n"
    "Input f1 0 1 f1\n"
    "Input m 0 1 m\n"
    "pmml.Rebuild rebuild 5 1 fa fb f0 f1 m out\n";

#if NCNN_VULKAN
static const char kRebuildComp[] = R"GLSL(
#version 450
#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
#if NCNN_fp16_arithmetic
#extension GL_EXT_shader_explicit_arithmetic_types_float16: require
#endif
layout (binding = 0) readonly buffer fa_blob { sfp fa_data[]; };
layout (binding = 1) readonly buffer fb_blob { sfp fb_data[]; };
layout (binding = 2) readonly buffer f0_blob { sfp f0_data[]; };
layout (binding = 3) readonly buffer f1_blob { sfp f1_data[]; };
layout (binding = 4) readonly buffer m_blob { sfp m_data[]; };
layout (binding = 5) writeonly buffer out_blob { sfp out_data[]; };
layout (push_constant) uniform parameter {
    int w; int h; int cstep;
    int pw; int ph; int fstep;
    int mstep; int nw; int nh;
    float s0; float s1;
} p;
float fieldAt(int which, int ch, float x, float y)
{
    x = clamp(x, 0.0, float(p.pw - 1)); y = clamp(y, 0.0, float(p.ph - 1));
    int x0 = int(x), y0 = int(y);
    int x1 = min(x0 + 1, p.pw - 1), y1 = min(y0 + 1, p.ph - 1);
    float fx = x - float(x0), fy = y - float(y0);
    float v00, v01, v10, v11;
    if (which == 0) {
        int b = ch * p.fstep;
        v00 = float(buffer_ld1(f0_data, b + y0 * p.pw + x0)); v01 = float(buffer_ld1(f0_data, b + y0 * p.pw + x1));
        v10 = float(buffer_ld1(f0_data, b + y1 * p.pw + x0)); v11 = float(buffer_ld1(f0_data, b + y1 * p.pw + x1));
    } else if (which == 1) {
        int b = ch * p.fstep;
        v00 = float(buffer_ld1(f1_data, b + y0 * p.pw + x0)); v01 = float(buffer_ld1(f1_data, b + y0 * p.pw + x1));
        v10 = float(buffer_ld1(f1_data, b + y1 * p.pw + x0)); v11 = float(buffer_ld1(f1_data, b + y1 * p.pw + x1));
    } else {
        v00 = float(buffer_ld1(m_data, y0 * p.pw + x0)); v01 = float(buffer_ld1(m_data, y0 * p.pw + x1));
        v10 = float(buffer_ld1(m_data, y1 * p.pw + x0)); v11 = float(buffer_ld1(m_data, y1 * p.pw + x1));
    }
    return (v00 * (1.0 - fx) + v01 * fx) * (1.0 - fy) + (v10 * (1.0 - fx) + v11 * fx) * fy;
}
float frameAt(int which, int ch, float x, float y)
{
    x = clamp(x, 0.0, float(p.w - 1)); y = clamp(y, 0.0, float(p.h - 1));
    int x0 = int(x), y0 = int(y);
    int x1 = min(x0 + 1, p.w - 1), y1 = min(y0 + 1, p.h - 1);
    float fx = x - float(x0), fy = y - float(y0);
    int b = ch * p.cstep;
    float v00, v01, v10, v11;
    if (which == 0) {
        v00 = float(buffer_ld1(fa_data, b + y0 * p.w + x0)); v01 = float(buffer_ld1(fa_data, b + y0 * p.w + x1));
        v10 = float(buffer_ld1(fa_data, b + y1 * p.w + x0)); v11 = float(buffer_ld1(fa_data, b + y1 * p.w + x1));
    } else {
        v00 = float(buffer_ld1(fb_data, b + y0 * p.w + x0)); v01 = float(buffer_ld1(fb_data, b + y0 * p.w + x1));
        v10 = float(buffer_ld1(fb_data, b + y1 * p.w + x0)); v11 = float(buffer_ld1(fb_data, b + y1 * p.w + x1));
    }
    return (v00 * (1.0 - fx) + v01 * fx) * (1.0 - fy) + (v10 * (1.0 - fx) + v11 * fx) * fy;
}
void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    if (gx >= p.w || gy >= p.h) return;
    float qx = clamp((float(gx) + 0.5) * float(p.nw) / float(p.w) - 0.5, 0.0, float(p.nw - 1));
    float qy = clamp((float(gy) + 0.5) * float(p.nh) / float(p.h) - 0.5, 0.0, float(p.nh - 1));
    float m = fieldAt(2, 0, qx, qy);
    float ax = (qx + fieldAt(0, 0, qx, qy) * p.s0 + 0.5) * float(p.w) / float(p.nw) - 0.5;
    float ay = (qy + fieldAt(0, 1, qx, qy) * p.s0 + 0.5) * float(p.h) / float(p.nh) - 0.5;
    float bx = (qx + fieldAt(1, 0, qx, qy) * p.s1 + 0.5) * float(p.w) / float(p.nw) - 0.5;
    float by = (qy + fieldAt(1, 1, qx, qy) * p.s1 + 0.5) * float(p.h) / float(p.nh) - 0.5;
    for (int c = 0; c < 3; c++) {
        float a = frameAt(0, c, ax, ay);
        float b = frameAt(1, c, bx, by);
        buffer_st1(out_data, c * p.cstep + gy * p.w + gx, afp(b + (a - b) * m));
    }
}
)GLSL";
#endif

PushFlowRebuild::PushFlowRebuild() {
    one_blob_only = false;
    support_inplace = false;
#if NCNN_VULKAN
    support_vulkan = true;
#endif
    support_packing = false;   // elempack 1 in and out: one shader
}

int PushFlowRebuild::create_pipeline(const ncnn::Option& opt) {
#if NCNN_VULKAN
    if (!vkdev) return 0;
    static std::vector<uint32_t> spirv;
    static std::mutex lock;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (spirv.empty() && ncnn::compile_spirv_module(kRebuildComp, int(sizeof(kRebuildComp) - 1), opt, spirv) != 0) return -1;
    }
    std::vector<ncnn::vk_specialization_type> specializations;
    pipeline_ = new ncnn::Pipeline(vkdev);
    pipeline_->set_optimal_local_size_xyz(8, 8, 1);
    pipeline_->create(spirv.data(), spirv.size() * 4, specializations);
#endif
    return 0;
}

int PushFlowRebuild::destroy_pipeline(const ncnn::Option&) {
#if NCNN_VULKAN
    delete pipeline_;
    pipeline_ = nullptr;
#endif
    return 0;
}

static inline float clampSample(const float* p, int w, int h, float x, float y) {
    x = std::min(std::max(x, 0.f), float(w - 1));
    y = std::min(std::max(y, 0.f), float(h - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    return (p[y0 * w + x0] * (1 - fx) + p[y0 * w + x1] * fx) * (1 - fy) + (p[y1 * w + x0] * (1 - fx) + p[y1 * w + x1] * fx) * fy;
}

int PushFlowRebuild::forward(const std::vector<ncnn::Mat>& bottom, std::vector<ncnn::Mat>& top, const ncnn::Option& opt) const {
    const ncnn::Mat& fa = bottom[0];
    const ncnn::Mat& fb = bottom[1];
    const ncnn::Mat& f0 = bottom[2];
    const ncnn::Mat& f1 = bottom[3];
    const ncnn::Mat& m = bottom[4];
    const int w = fa.w, h = fa.h, pw = f0.w, ph = f0.h;
    ncnn::Mat& out = top[0];
    out.create(w, h, 3, size_t(4), opt.blob_allocator);
    if (out.empty()) return -100;
    #pragma omp parallel for num_threads(opt.num_threads)
    for (int y = 0; y < h; ++y) {
        const float qy = std::min(std::max((y + 0.5f) * float(nh) / float(h) - 0.5f, 0.f), float(nh - 1));
        for (int x = 0; x < w; ++x) {
            const float qx = std::min(std::max((x + 0.5f) * float(nw) / float(w) - 0.5f, 0.f), float(nw - 1));
            const float mm = clampSample(m.channel(0), pw, ph, qx, qy);
            const float ax = (qx + clampSample(f0.channel(0), pw, ph, qx, qy) * s0 + 0.5f) * float(w) / float(nw) - 0.5f;
            const float ay = (qy + clampSample(f0.channel(1), pw, ph, qx, qy) * s0 + 0.5f) * float(h) / float(nh) - 0.5f;
            const float bx = (qx + clampSample(f1.channel(0), pw, ph, qx, qy) * s1 + 0.5f) * float(w) / float(nw) - 0.5f;
            const float by = (qy + clampSample(f1.channel(1), pw, ph, qx, qy) * s1 + 0.5f) * float(h) / float(nh) - 0.5f;
            for (int c = 0; c < 3; ++c) {
                const float a = clampSample(fa.channel(c), w, h, ax, ay), b = clampSample(fb.channel(c), w, h, bx, by);
                out.channel(c)[y * w + x] = b + (a - b) * mm;
            }
        }
    }
    return 0;
}

#if NCNN_VULKAN
int PushFlowRebuild::forward(const std::vector<ncnn::VkMat>& bottom, std::vector<ncnn::VkMat>& top, ncnn::VkCompute& cmd, const ncnn::Option& opt) const {
    const ncnn::VkMat& fa = bottom[0];
    const ncnn::VkMat& f0 = bottom[2];
    const ncnn::VkMat& m = bottom[4];
    ncnn::VkMat& out = top[0];
    out.create(fa.w, fa.h, 3, fa.elemsize, 1, opt.blob_vkallocator);
    if (out.empty()) return -100;
    std::vector<ncnn::VkMat> bindings = {bottom[0], bottom[1], bottom[2], bottom[3], bottom[4], out};
    std::vector<ncnn::vk_constant_type> constants(11);
    constants[0].i = fa.w; constants[1].i = fa.h; constants[2].i = int(fa.cstep);
    constants[3].i = f0.w; constants[4].i = f0.h; constants[5].i = int(f0.cstep);
    constants[6].i = int(m.cstep); constants[7].i = nw; constants[8].i = nh;
    constants[9].f = s0; constants[10].f = s1;
    ncnn::VkMat dispatcher;
    dispatcher.w = fa.w; dispatcher.h = fa.h; dispatcher.c = 1;
    cmd.record_pipeline(pipeline_, bindings, constants, dispatcher);
    return 0;
}
#endif

ncnn::Layer* PushFlowRebuildCreator(void*) { return new PushFlowRebuild(); }

}  // namespace pmml::detail
