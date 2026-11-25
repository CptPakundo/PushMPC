// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc.Enhance, portable GPU engine (see gpu_engine.h).
#include "gpu_engine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include "NIS_Config.h"
#include "spv_all.h"

namespace pmvs::gpu {

namespace {

double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
VkDeviceSize Align(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

enum P { PPyr0, PPyrDown, PDedup, PMe, PMeFinal, PMad, PFb, PInterp, PNis, PEasu, PRcas, PPack, POfConv, PCount };
constexpr VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           SBD = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SMP = VK_DESCRIPTOR_TYPE_SAMPLER,
                           SIM = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
struct PipeDesc { const uint32_t* code; size_t bytes; std::vector<VkDescriptorType> types; uint32_t push; };
std::vector<PipeDesc> Descs()
{
    return {
        { kSpv_pyr0, sizeof(kSpv_pyr0), { CIS, SI }, 16 },
        { kSpv_pyrdown, sizeof(kSpv_pyrdown), { SI, SI }, 16 },
        { kSpv_dedup, sizeof(kSpv_dedup), { SI, SI, SBD }, 8 },
        { kSpv_me, sizeof(kSpv_me), { SI, SI, SB, SB, SB, SB }, 64 },
        { kSpv_mefinal, sizeof(kSpv_mefinal), { SB, SB, SB, SBD }, 20 },
        { kSpv_mad, sizeof(kSpv_mad), { SI, SI, SBD }, 8 },
        { kSpv_fbcheck, sizeof(kSpv_fbcheck), { SB, SB, SBD }, 12 },
        { kSpv_interp, sizeof(kSpv_interp), { CIS, CIS, CIS, CIS, CIS, CIS, SB, SB, SBD, SI, SI, SI, SB }, 104 },
        { kSpv_nis, sizeof(kSpv_nis), { UB, SMP, SIM, SIM, SIM, SI }, 0 },
        { kSpv_easu, sizeof(kSpv_easu), { CIS, SI }, 72 },
        { kSpv_rcas, sizeof(kSpv_rcas), { CIS, SI }, 24 },
        { kSpv_pack, sizeof(kSpv_pack), { CIS, CIS, CIS, SBD }, 60 },
        { kSpv_ofconv, sizeof(kSpv_ofconv), { SB, SI, SI, SB, SB, SBD }, 40 },
    };
}

struct Cell { int32_t vx, vy, cost, zero; float subx, suby; };   // me.comp's Cell (std430: 24 bytes)

uint32_t AsU(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

} // namespace

struct Engine::Impl {
    std::shared_ptr<Context> ctx;
    Config cfg;
    int outW = 0, outH = 0, cw = 0, ch = 0, ocw = 0, och = 0, bps = 1;
    float kRaw = 255.f, maxV = 255.f;
    int levels = 4, finest = 1;
    std::vector<int> lw, lh;        // pyramid level sizes
    int fbx = 0, fby = 0, fbs = 16; // the finest grid (the field)
    bool useOf = false;
    Timing times;

    struct Pipe { VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline p = VK_NULL_HANDLE; std::vector<VkDescriptorType> types; uint32_t push = 0; };
    Pipe pipes[PCount];
    VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t value = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    struct Job { VkCommandBuffer cb = VK_NULL_HANDLE; VkDescriptorPool dp = VK_NULL_HANDLE; uint64_t value = 0; };
    std::vector<Job> jobs;
    size_t nextJob = 0;

    struct Slot { Image y, u, v; std::vector<Image> pyr; Image ofLuma; bool used = false; uint64_t lastUse = 0; };
    std::vector<Slot> slots;
    struct Pair { bool used = false; int a = -1, b = -1; Buffer fieldF, fieldK, costF, costK, stats, hold; bool holdOn = false; int zx = 0, zy = 0, zw = 0, zh = 0; uint64_t lastUse = 0; };
    std::vector<Pair> pairs;
    std::vector<Buffer> sweep1, sweep2;   // per level
    Buffer staging[2]; uint64_t stagingUse[2] = { 0, 0 }; int nextStaging = 0;
    VkDeviceSize stOffU = 0, stOffV = 0;
    Buffer readback; VkDeviceSize outStride = 0, offU = 0, offV = 0; int pitchY = 0, pitchC = 0;
    std::vector<bool> outUsed;
    Buffer dedupStats; int nextDedup = 0;
    Buffer dummy;
    Image midY, midU, midV, upTmp, upY, coefScale, coefUsm;
    Buffer nisUbo;
    // optical flow
    VkOpticalFlowSessionNV ofSession = VK_NULL_HANDLE;
    Image ofA, ofB, ofFlowF, ofFlowB;
    Buffer ofBufF, ofBufB;
    int ofGrid = 4, ofGw = 0, ofGh = 0;
    VkCommandPool ofPool = VK_NULL_HANDLE;
    VkCommandBuffer ofCb = VK_NULL_HANDLE;

    ~Impl() { Free(); }
    void Free();
    bool MakePipes(std::string& err);
    Job& Begin(std::string& err, bool& ok);
    uint64_t Submit(Job& j, std::string& err, uint64_t waitValue = 0);
    bool WaitValue(uint64_t v, std::string& err) const;
    void Barrier(VkCommandBuffer cb) const;
    void ToGeneral(VkCommandBuffer cb, VkImage img) const;
    // descriptor writing + dispatch for one pipeline
    struct Bind {
        Impl& m; Job& j; P p;
        std::vector<VkWriteDescriptorSet> w;
        std::deque<VkDescriptorImageInfo> ii;
        std::deque<VkDescriptorBufferInfo> bi;
        std::vector<uint32_t> dyn;
        Bind(Impl& mm, Job& jj, P pp) : m(mm), j(jj), p(pp) {}
        Bind& Img(uint32_t b, VkImageView v, VkSampler s = VK_NULL_HANDLE)
        {
            const VkDescriptorType t = m.pipes[p].types[b];
            ii.push_back({ t == CIS || t == SMP ? (s ? s : m.nearest) : VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL });
            VkWriteDescriptorSet x{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            x.dstBinding = b; x.descriptorCount = 1; x.descriptorType = t; x.pImageInfo = &ii.back();
            w.push_back(x);
            return *this;
        }
        Bind& Buf(uint32_t b, const Buffer& buf, VkDeviceSize off = 0, VkDeviceSize range = VK_WHOLE_SIZE)
        {
            const VkDescriptorType t = m.pipes[p].types[b];
            if (t == SBD) { dyn.push_back(uint32_t(off)); bi.push_back({ buf.buf, 0, range }); }
            else bi.push_back({ buf.buf, off, range });
            VkWriteDescriptorSet x{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            x.dstBinding = b; x.descriptorCount = 1; x.descriptorType = t; x.pBufferInfo = &bi.back();
            w.push_back(x);
            return *this;
        }
        bool Run(const void* push, uint32_t gx, uint32_t gy, std::string& err)
        {
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = j.dp; ai.descriptorSetCount = 1; ai.pSetLayouts = &m.pipes[p].dsl;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(m.ctx->Dev(), &ai, &set) != VK_SUCCESS) { err = "vkAllocateDescriptorSets failed"; return false; }
            for (auto& x : w) x.dstSet = set;
            vkUpdateDescriptorSets(m.ctx->Dev(), uint32_t(w.size()), w.data(), 0, nullptr);
            vkCmdBindPipeline(j.cb, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipes[p].p);
            vkCmdBindDescriptorSets(j.cb, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipes[p].pl, 0, 1, &set, uint32_t(dyn.size()), dyn.data());
            if (m.pipes[p].push && push) vkCmdPushConstants(j.cb, m.pipes[p].pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, m.pipes[p].push, push);
            vkCmdDispatch(j.cb, std::max(1u, gx), std::max(1u, gy), 1);
            return true;
        }
    };
    bool Interp(Job& j, int sa, int sb, const Pair* pr, float t, int mode, int target, int out, std::string& err);
    bool UpscaleAndPack(Job& j, int out, std::string& err);
    bool MotionDir(Job& j, int sa, int sb, const Pair* prev, bool forward, Pair& pr, std::string& err);
    bool MotionOf(int sa, int sb, Pair& pr, std::string& err);
    int FreeOut() const { for (size_t i = 0; i < outUsed.size(); ++i) if (!outUsed[i]) return int(i); return -1; }
};

void Engine::Impl::Free()
{
    if (!ctx) return;
    const VkDevice d = ctx->Dev();
    vkDeviceWaitIdle(d);
    for (auto& s : slots) { ctx->Destroy(s.y); ctx->Destroy(s.u); ctx->Destroy(s.v); for (auto& i : s.pyr) ctx->Destroy(i); ctx->Destroy(s.ofLuma); }
    slots.clear();
    for (auto& p : pairs) { ctx->Destroy(p.fieldF); ctx->Destroy(p.fieldK); ctx->Destroy(p.costF); ctx->Destroy(p.costK); ctx->Destroy(p.stats); ctx->Destroy(p.hold); }
    pairs.clear();
    for (auto& b : sweep1) ctx->Destroy(b);
    for (auto& b : sweep2) ctx->Destroy(b);
    sweep1.clear(); sweep2.clear();
    for (auto& b : staging) ctx->Destroy(b);
    ctx->Destroy(readback); ctx->Destroy(dedupStats); ctx->Destroy(dummy); ctx->Destroy(nisUbo);
    for (Image* i : { &midY, &midU, &midV, &upTmp, &upY, &coefScale, &coefUsm, &ofA, &ofB, &ofFlowF, &ofFlowB }) ctx->Destroy(*i);
    ctx->Destroy(ofBufF); ctx->Destroy(ofBufB);
    if (ofSession) vkDestroyOpticalFlowSessionNV(d, ofSession, nullptr);
    ofSession = VK_NULL_HANDLE;
    if (ofPool) vkDestroyCommandPool(d, ofPool, nullptr);
    ofPool = VK_NULL_HANDLE;
    for (auto& j : jobs) if (j.dp) vkDestroyDescriptorPool(d, j.dp, nullptr);
    jobs.clear();
    if (pool) vkDestroyCommandPool(d, pool, nullptr);
    pool = VK_NULL_HANDLE;
    if (timeline) vkDestroySemaphore(d, timeline, nullptr);
    timeline = VK_NULL_HANDLE;
    for (auto& p : pipes) {
        if (p.p) vkDestroyPipeline(d, p.p, nullptr);
        if (p.pl) vkDestroyPipelineLayout(d, p.pl, nullptr);
        if (p.dsl) vkDestroyDescriptorSetLayout(d, p.dsl, nullptr);
        p = {};
    }
    if (nearest) vkDestroySampler(d, nearest, nullptr);
    if (linear) vkDestroySampler(d, linear, nullptr);
    nearest = linear = VK_NULL_HANDLE;
    outUsed.clear();
    ctx.reset();
}

bool Engine::Impl::MakePipes(std::string& err)
{
    const VkDevice d = ctx->Dev();
    const auto descs = Descs();
    for (int i = 0; i < PCount; ++i) {
        const PipeDesc& pd = descs[size_t(i)];
        Pipe& p = pipes[i];
        p.types = pd.types; p.push = pd.push;
        std::vector<VkDescriptorSetLayoutBinding> b(pd.types.size());
        for (size_t k = 0; k < b.size(); ++k) { b[k].binding = uint32_t(k); b[k].descriptorType = pd.types[k]; b[k].descriptorCount = 1; b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = uint32_t(b.size()); li.pBindings = b.data();
        if (vkCreateDescriptorSetLayout(d, &li, nullptr, &p.dsl) != VK_SUCCESS) { err = "vkCreateDescriptorSetLayout failed"; return false; }
        VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, pd.push };
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1; pli.pSetLayouts = &p.dsl; pli.pushConstantRangeCount = pd.push ? 1 : 0; pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(d, &pli, nullptr, &p.pl) != VK_SUCCESS) { err = "vkCreatePipelineLayout failed"; return false; }
        VkShaderModuleCreateInfo si{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        si.codeSize = pd.bytes; si.pCode = pd.code;
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(d, &si, nullptr, &mod) != VK_SUCCESS) { err = "vkCreateShaderModule failed (shader " + std::to_string(i) + ")"; return false; }
        VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr };
        ci.layout = p.pl;
        const VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &p.p);
        vkDestroyShaderModule(d, mod, nullptr);
        if (r != VK_SUCCESS) { err = "vkCreateComputePipelines failed (shader " + std::to_string(i) + ", " + std::to_string(int(r)) + ")"; return false; }
    }
    VkSamplerCreateInfo s{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    s.magFilter = s.minFilter = VK_FILTER_NEAREST; s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; s.maxLod = 0;
    if (vkCreateSampler(d, &s, nullptr, &nearest) != VK_SUCCESS) { err = "vkCreateSampler failed"; return false; }
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(d, &s, nullptr, &linear) != VK_SUCCESS) { err = "vkCreateSampler failed"; return false; }
    return true;
}

void Engine::Impl::Barrier(VkCommandBuffer cb) const
{
    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void Engine::Impl::ToGeneral(VkCommandBuffer cb, VkImage img) const
{
    VkImageMemoryBarrier ib{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    ib.srcAccessMask = 0; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img; ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
}

bool Engine::Impl::WaitValue(uint64_t v, std::string& err) const
{
    if (v == 0) return true;
    VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1; wi.pSemaphores = &timeline; wi.pValues = &v;
    const VkResult r = vkWaitSemaphores(ctx->Dev(), &wi, 10ull * 1000 * 1000 * 1000);
    if (r != VK_SUCCESS) { err = r == VK_TIMEOUT ? "the GPU did not finish within 10 s" : "vkWaitSemaphores failed (" + std::to_string(int(r)) + ")"; return false; }
    return true;
}

Engine::Impl::Job& Engine::Impl::Begin(std::string& err, bool& ok)
{
    Job& j = jobs[nextJob];
    nextJob = (nextJob + 1) % jobs.size();
    ok = WaitValue(j.value, err);
    vkResetDescriptorPool(ctx->Dev(), j.dp, 0);
    vkResetCommandBuffer(j.cb, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(j.cb, &bi);
    Barrier(j.cb);   // after everything submitted before (the scratch images and buffers are shared)
    return j;
}

uint64_t Engine::Impl::Submit(Job& j, std::string& err, uint64_t waitValue)
{
    Barrier(j.cb);   // the host reads what this job wrote
    vkEndCommandBuffer(j.cb);
    const uint64_t v = ++value;
    VkTimelineSemaphoreSubmitInfo ts{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    ts.signalSemaphoreValueCount = 1; ts.pSignalSemaphoreValues = &v;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (waitValue) { ts.waitSemaphoreValueCount = 1; ts.pWaitSemaphoreValues = &waitValue; }
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext = &ts;
    si.commandBufferCount = 1; si.pCommandBuffers = &j.cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &timeline;
    if (waitValue) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &timeline; si.pWaitDstStageMask = &waitStage; }
    const VkResult r = ctx->Submit(si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) { err = "vkQueueSubmit failed (" + std::to_string(int(r)) + ")"; return 0; }
    j.value = v;
    return v;
}

Engine::Engine() : m(std::make_unique<Impl>()) {}
Engine::~Engine() = default;
const Config& Engine::Cfg() const { return m->cfg; }
const DeviceInfo& Engine::Device() const { static DeviceInfo none; return m->ctx ? m->ctx->Info() : none; }
bool Engine::OpticalFlowActive() const { return m->useOf; }
int Engine::OutWidth() const { return m->outW; }
int Engine::OutHeight() const { return m->outH; }
const Engine::Timing& Engine::Times() const { return m->times; }
bool Engine::Done(uint64_t ticket) const
{
    uint64_t v = 0;
    return vkGetSemaphoreCounterValue(m->ctx->Dev(), m->timeline, &v) == VK_SUCCESS && v >= ticket;
}
bool Engine::Wait(uint64_t ticket, std::string& err) const
{
    const double t0 = NowMs();
    const bool ok = m->WaitValue(ticket, err);
    m->times.waitMs += NowMs() - t0;
    return ok;
}

bool Engine::Configure(const Config& c, const DeviceRequest& req, std::string& err)
{
    m->Free();
    if (c.width < 64 || c.height < 64 || c.bits < 8 || c.bits > 16) { err = "GPU engine: the picture must be at least 64x64, 8..16 bits"; return false; }
    m->ctx = Context::Get(req, err);
    if (!m->ctx) return false;
    Impl& I = *m;
    I.cfg = c;
    I.outW = c.outWidth > 0 ? c.outWidth : c.width; I.outH = c.outHeight > 0 ? c.outHeight : c.height;
    if (I.outW == c.width && I.outH == c.height) I.cfg.upscaler = Upscaler::None;
    if (I.cfg.upscaler == Upscaler::None) { I.outW = c.width; I.outH = c.height; }
    I.cw = (c.width + 1) / 2; I.ch = (c.height + 1) / 2; I.ocw = (I.outW + 1) / 2; I.och = (I.outH + 1) / 2;
    I.bps = c.bits > 8 ? 2 : 1;
    I.kRaw = c.bits > 8 ? 65535.f : 255.f;
    I.maxV = float((1 << c.bits) - 1);
    // the pyramid: PushFPS's rule (halve while the level above is wider and taller than 8 px)
    const int want = c.levels > 0 ? c.levels : (c.height > 1200 ? 5 : 4);
    I.lw = { c.width }; I.lh = { c.height };
    while (int(I.lw.size()) < want && I.lw.back() > 8 && I.lh.back() > 8) { I.lw.push_back((I.lw.back() + 1) / 2); I.lh.push_back((I.lh.back() + 1) / 2); }
    I.levels = int(I.lw.size());
    I.finest = std::clamp(c.quality <= 0 ? 2 : c.quality == 1 ? 1 : 0, 0, I.levels - 1);
    I.fbx = (I.lw[size_t(I.finest)] + 7) / 8; I.fby = (I.lh[size_t(I.finest)] + 7) / 8; I.fbs = 8 << I.finest;
    const VkDevice d = I.ctx->Dev();
    if (!I.MakePipes(err)) return false;

    VkSemaphoreTypeCreateInfo st{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; st.initialValue = 0;
    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sci.pNext = &st;
    if (vkCreateSemaphore(d, &sci, nullptr, &I.timeline) != VK_SUCCESS) { err = "vkCreateSemaphore failed"; return false; }
    I.value = 0;
    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = I.ctx->Family();
    if (vkCreateCommandPool(d, &pci, nullptr, &I.pool) != VK_SUCCESS) { err = "vkCreateCommandPool failed"; return false; }
    I.jobs.resize(size_t(std::max(12, c.outSlots + 6)));
    for (auto& j : I.jobs) {
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = I.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(d, &ai, &j.cb) != VK_SUCCESS) { err = "vkAllocateCommandBuffers failed"; return false; }
        const VkDescriptorPoolSize sizes[] = { { CIS, 256 }, { SI, 256 }, { SB, 256 }, { SBD, 128 }, { UB, 16 }, { SMP, 16 }, { SIM, 64 } };
        VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = 96; dpi.poolSizeCount = uint32_t(std::size(sizes)); dpi.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(d, &dpi, nullptr, &j.dp) != VK_SUCCESS) { err = "vkCreateDescriptorPool failed"; return false; }
    }
    I.nextJob = 0;

    const VkFormat planeFmt = c.bits > 8 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
    const VkImageUsageFlags planeUse = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags pyrUse = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    I.useOf = I.ctx->OpticalFlow() && c.quality >= 0;
    if (c.blockMatcherOnly) I.useOf = false;
    if (const char* v = std::getenv("PUSHMPC_GPU_MOTION")) { if (std::strcmp(v, "blocks") == 0) I.useOf = false; }
    I.slots.resize(size_t(std::max(3, c.sourceSlots)));
    for (auto& s : I.slots) {
        if (!I.ctx->CreateImage(c.width, c.height, planeFmt, planeUse, false, s.y, err) || !I.ctx->CreateImage(I.cw, I.ch, planeFmt, planeUse, false, s.u, err)
            || !I.ctx->CreateImage(I.cw, I.ch, planeFmt, planeUse, false, s.v, err)) return false;
        s.pyr.resize(size_t(I.levels));
        for (int l = 0; l < I.levels; ++l) if (!I.ctx->CreateImage(I.lw[size_t(l)], I.lh[size_t(l)], VK_FORMAT_R8_UINT, pyrUse, false, s.pyr[size_t(l)], err)) return false;
    }
    auto host = [&](VkDeviceSize n, VkBufferUsageFlags u, Buffer& b) { return I.ctx->CreateBuffer(n, u | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, b, err); };
    auto dev = [&](VkDeviceSize n, Buffer& b) { return I.ctx->CreateBuffer(n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, b, err); };
    const VkDeviceSize fieldBytes = VkDeviceSize(I.fbx) * VkDeviceSize(I.fby) * 8, costBytes = VkDeviceSize(I.fbx) * VkDeviceSize(I.fby) * 4;
    I.pairs.resize(size_t(std::max(3, c.sourceSlots)));
    for (auto& p : I.pairs)
        if (!dev(fieldBytes, p.fieldF) || !dev(fieldBytes, p.fieldK) || !dev(costBytes, p.costF) || !dev(costBytes, p.costK) || !host(1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, p.stats)) return false;
    I.sweep1.resize(size_t(I.levels)); I.sweep2.resize(size_t(I.levels));
    for (int l = 0; l < I.levels; ++l) {
        const VkDeviceSize n = VkDeviceSize((I.lw[size_t(l)] + 7) / 8) * VkDeviceSize((I.lh[size_t(l)] + 7) / 8) * sizeof(Cell);
        if (!dev(n, I.sweep1[size_t(l)]) || !dev(n, I.sweep2[size_t(l)])) return false;
    }
    // staging: one source frame, planes at 256-byte offsets
    const VkDeviceSize ySz = VkDeviceSize(c.width) * VkDeviceSize(c.height) * VkDeviceSize(I.bps), cSz = VkDeviceSize(I.cw) * VkDeviceSize(I.ch) * VkDeviceSize(I.bps);
    I.stOffU = Align(ySz, 256); I.stOffV = Align(I.stOffU + cSz, 256);
    for (auto& b : I.staging) if (!host(I.stOffV + cSz, 0, b)) return false;
    // readback: packed rows, 4-byte pitch, planes at 256-byte offsets; one slot per output in flight
    I.pitchY = int(Align(VkDeviceSize(I.outW) * VkDeviceSize(I.bps), 4)); I.pitchC = int(Align(VkDeviceSize(I.ocw) * VkDeviceSize(I.bps), 4));
    const VkDeviceSize align = std::max<VkDeviceSize>(256, I.ctx->Limits().minStorageBufferOffsetAlignment);
    I.offU = Align(VkDeviceSize(I.pitchY) * VkDeviceSize(I.outH), align); I.offV = Align(I.offU + VkDeviceSize(I.pitchC) * VkDeviceSize(I.och), align);
    I.outStride = Align(I.offV + VkDeviceSize(I.pitchC) * VkDeviceSize(I.och), align);
    I.outUsed.assign(size_t(std::max(2, c.outSlots)), false);
    if (!host(I.outStride * I.outUsed.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, I.readback)) return false;
    if (!host(256 * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, I.dedupStats)) return false;
    if (!dev(256, I.dummy)) return false;
    // intermediates for the upscaler (1x1 placeholders without one: interp.comp binds them either way)
    const VkImageUsageFlags midUse = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const bool up = I.cfg.upscaler != Upscaler::None;
    if (!I.ctx->CreateImage(up ? c.width : 1, up ? c.height : 1, VK_FORMAT_R16_UNORM, midUse, true, I.midY, err)
        || !I.ctx->CreateImage(up ? I.cw : 1, up ? I.ch : 1, VK_FORMAT_R16_UNORM, midUse, false, I.midU, err)
        || !I.ctx->CreateImage(up ? I.cw : 1, up ? I.ch : 1, VK_FORMAT_R16_UNORM, midUse, false, I.midV, err)) return false;
    if (up) {
        if (!I.ctx->CreateImage(I.outW, I.outH, VK_FORMAT_R16_UNORM, midUse, false, I.upTmp, err) || !I.ctx->CreateImage(I.outW, I.outH, VK_FORMAT_R16_UNORM, midUse, false, I.upY, err)) return false;
        if (I.cfg.upscaler == Upscaler::Nis) {
            if (!I.ctx->CreateImage(kFilterSize / 4, kPhaseCount, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, I.coefScale, err)
                || !I.ctx->CreateImage(kFilterSize / 4, kPhaseCount, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, I.coefUsm, err)) return false;
            if (!host(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, I.nisUbo)) return false;
            NISConfig nc{};
            if (!NVScalerUpdateConfig(nc, std::clamp(I.cfg.sharpness, 0.f, 1.f), 0, 0, uint32_t(c.width), uint32_t(c.height), uint32_t(c.width), uint32_t(c.height), 0, 0,
                                      uint32_t(I.outW), uint32_t(I.outH), uint32_t(I.outW), uint32_t(I.outH), NISHDRMode::None)) { err = "NIS: unsupported scale"; return false; }
            std::memcpy(I.nisUbo.map, &nc, 28 * 4);
        }
    }
    // optical flow: the luma copies it reads, its flow images and their readable copies
    if (I.useOf) {
        std::string ofErr;
        I.ofGrid = 4;
        I.ofGw = (c.width + I.ofGrid - 1) / I.ofGrid; I.ofGh = (c.height + I.ofGrid - 1) / I.ofGrid;
        const uint32_t inUse = VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, outUse = VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV;
        bool ok = I.ctx->CreateImage(c.width, c.height, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false, I.ofA, ofErr, inUse)
               && I.ctx->CreateImage(c.width, c.height, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false, I.ofB, ofErr, inUse)
               && I.ctx->CreateImage(I.ofGw, I.ofGh, VK_FORMAT_R16G16_S10_5_NV, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false, I.ofFlowF, ofErr, outUse)
               && I.ctx->CreateImage(I.ofGw, I.ofGh, VK_FORMAT_R16G16_S10_5_NV, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false, I.ofFlowB, ofErr, outUse)
               && dev(VkDeviceSize(I.ofGw) * VkDeviceSize(I.ofGh) * 4, I.ofBufF) && dev(VkDeviceSize(I.ofGw) * VkDeviceSize(I.ofGh) * 4, I.ofBufB);
        if (ok) {
            VkOpticalFlowSessionCreateInfoNV oi{ VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV };
            oi.width = uint32_t(c.width); oi.height = uint32_t(c.height);
            oi.imageFormat = VK_FORMAT_R8_UNORM; oi.flowVectorFormat = VK_FORMAT_R16G16_S10_5_NV;
            oi.outputGridSize = VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV;
            oi.performanceLevel = VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV;
            oi.flags = VK_OPTICAL_FLOW_SESSION_CREATE_BOTH_DIRECTIONS_BIT_NV;
            ok = vkCreateOpticalFlowSessionNV(d, &oi, nullptr, &I.ofSession) == VK_SUCCESS;
            if (!ok) ofErr = "vkCreateOpticalFlowSessionNV failed";
        }
        if (ok) {
            ok = vkBindOpticalFlowSessionImageNV(d, I.ofSession, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV, I.ofA.view, VK_IMAGE_LAYOUT_GENERAL) == VK_SUCCESS
              && vkBindOpticalFlowSessionImageNV(d, I.ofSession, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV, I.ofB.view, VK_IMAGE_LAYOUT_GENERAL) == VK_SUCCESS
              && vkBindOpticalFlowSessionImageNV(d, I.ofSession, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV, I.ofFlowF.view, VK_IMAGE_LAYOUT_GENERAL) == VK_SUCCESS
              && vkBindOpticalFlowSessionImageNV(d, I.ofSession, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_BACKWARD_FLOW_VECTOR_NV, I.ofFlowB.view, VK_IMAGE_LAYOUT_GENERAL) == VK_SUCCESS;
            if (!ok) ofErr = "vkBindOpticalFlowSessionImageNV failed";
        }
        if (ok) {
            VkCommandPoolCreateInfo opi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            opi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; opi.queueFamilyIndex = I.ctx->OfFamily();
            VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ok = vkCreateCommandPool(d, &opi, nullptr, &I.ofPool) == VK_SUCCESS;
            ai.commandPool = I.ofPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
            ok = ok && vkAllocateCommandBuffers(d, &ai, &I.ofCb) == VK_SUCCESS;
        }
        if (!ok) {   // the block matcher instead
            for (Image* i : { &I.ofA, &I.ofB, &I.ofFlowF, &I.ofFlowB }) I.ctx->Destroy(*i);
            I.ctx->Destroy(I.ofBufF); I.ctx->Destroy(I.ofBufB);
            if (I.ofSession) vkDestroyOpticalFlowSessionNV(d, I.ofSession, nullptr);
            I.ofSession = VK_NULL_HANDLE;
            I.useOf = false;
        }
    }

    // every image to GENERAL; the NIS tables
    bool ok = true;
    Impl::Job& j = I.Begin(err, ok);
    if (!ok) return false;
    auto gen = [&](const Image& i) { if (i.img) I.ToGeneral(j.cb, i.img); };
    for (auto& s : I.slots) { gen(s.y); gen(s.u); gen(s.v); for (auto& p : s.pyr) gen(p); }
    for (const Image* i : { &I.midY, &I.midU, &I.midV, &I.upTmp, &I.upY, &I.coefScale, &I.coefUsm, &I.ofA, &I.ofB, &I.ofFlowF, &I.ofFlowB }) gen(*i);
    Buffer coefStage;
    if (I.coefScale.img) {
        if (!host(sizeof(coef_scale_fp16) + sizeof(coef_usm_fp16), 0, coefStage)) return false;
        std::memcpy(coefStage.map, coef_scale_fp16, sizeof(coef_scale_fp16));
        std::memcpy(static_cast<uint8_t*>(coefStage.map) + sizeof(coef_scale_fp16), coef_usm_fp16, sizeof(coef_usm_fp16));
        VkBufferImageCopy r{};
        r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; r.imageExtent = { uint32_t(kFilterSize / 4), uint32_t(kPhaseCount), 1 };
        vkCmdCopyBufferToImage(j.cb, coefStage.buf, I.coefScale.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        r.bufferOffset = sizeof(coef_scale_fp16);
        vkCmdCopyBufferToImage(j.cb, coefStage.buf, I.coefUsm.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    }
    vkCmdFillBuffer(j.cb, I.dummy.buf, 0, VK_WHOLE_SIZE, 0);
    const uint64_t v = I.Submit(j, err);
    if (!v || !I.WaitValue(v, err)) { I.ctx->Destroy(coefStage); return false; }
    I.ctx->Destroy(coefStage);
    I.times = {};
    return true;
}

int Engine::FreeSlots() const { int n = 0; for (auto& s : m->slots) n += s.used ? 0 : 1; return n; }
void Engine::ReleaseSlot(int slot) { if (slot >= 0 && slot < int(m->slots.size())) m->slots[size_t(slot)].used = false; }
void Engine::ReleasePair(int pair) { if (pair >= 0 && pair < int(m->pairs.size())) m->pairs[size_t(pair)].used = false; }
int Engine::FreeOuts() const { int n = 0; for (bool u : m->outUsed) n += u ? 0 : 1; return n; }
void Engine::ReleaseOut(int out) { if (out >= 0 && out < int(m->outUsed.size())) m->outUsed[size_t(out)] = false; }

int Engine::Upload(const YuvView& v, std::string& err)
{
    Impl& I = *m;
    if (v.width != I.cfg.width || v.height != I.cfg.height || v.bits != I.cfg.bits) { err = "GPU engine: the frame's size or depth changed"; return -1; }
    int slot = -1;
    for (size_t i = 0; i < I.slots.size(); ++i) if (!I.slots[i].used) { slot = int(i); break; }
    if (slot < 0) { err = "GPU engine: no free picture slot"; return -1; }
    const double t0 = NowMs();
    const int sb = I.nextStaging;
    I.nextStaging ^= 1;
    if (!I.WaitValue(I.stagingUse[sb], err)) return -1;
    Buffer& st = I.staging[sb];
    uint8_t* base = static_cast<uint8_t*>(st.map);
    for (int p = 0; p < 3; ++p) {
        const int w = p ? I.cw : I.cfg.width, h = p ? I.ch : I.cfg.height;
        const size_t row = size_t(w) * size_t(I.bps);
        uint8_t* dst = base + (p == 0 ? 0 : p == 1 ? I.stOffU : I.stOffV);
        for (int y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * row, v.data[p] + ptrdiff_t(y) * v.stride[p], row);
    }
    bool ok = true;
    Impl::Job& j = I.Begin(err, ok);
    if (!ok) return -1;
    Impl::Slot& s = I.slots[size_t(slot)];
    const Image* planes[3] = { &s.y, &s.u, &s.v };
    for (int p = 0; p < 3; ++p) {
        VkBufferImageCopy r{};
        r.bufferOffset = p == 0 ? 0 : p == 1 ? I.stOffU : I.stOffV;
        r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.imageExtent = { uint32_t(planes[p]->w), uint32_t(planes[p]->h), 1 };
        vkCmdCopyBufferToImage(j.cb, st.buf, planes[p]->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    }
    I.Barrier(j.cb);
    struct { int w, h, shift; float kRaw; } p0{ I.cfg.width, I.cfg.height, std::max(0, I.cfg.bits - 8), I.kRaw };
    if (!Impl::Bind(I, j, PPyr0).Img(0, s.y.view).Img(1, s.pyr[0].view).Run(&p0, uint32_t((I.cfg.width + 15) / 16), uint32_t((I.cfg.height + 7) / 8), err)) return -1;
    for (int l = 1; l < I.levels; ++l) {
        I.Barrier(j.cb);
        struct { int sw, sh, dw, dh; } pd{ I.lw[size_t(l - 1)], I.lh[size_t(l - 1)], I.lw[size_t(l)], I.lh[size_t(l)] };
        if (!Impl::Bind(I, j, PPyrDown).Img(0, s.pyr[size_t(l - 1)].view).Img(1, s.pyr[size_t(l)].view).Run(&pd, uint32_t((pd.dw + 15) / 16), uint32_t((pd.dh + 7) / 8), err)) return -1;
    }
    const uint64_t val = I.Submit(j, err);
    if (!val) return -1;
    I.stagingUse[sb] = val;
    s.used = true; s.lastUse = val;
    I.times.uploads++;
    I.times.uploadMs += NowMs() - t0;
    return slot;
}

bool Engine::Duplicate(int prevSlot, int slot, DedupStats& st, std::string& err)
{
    Impl& I = *m;
    st = {};
    const int lv = std::min(2, I.levels - 1);
    const VkDeviceSize off = VkDeviceSize(I.nextDedup) * 256;
    I.nextDedup = (I.nextDedup + 1) % 64;
    bool ok = true;
    Impl::Job& j = I.Begin(err, ok);
    if (!ok) return false;
    vkCmdFillBuffer(j.cb, I.dedupStats.buf, off, 256, 0);
    I.Barrier(j.cb);
    struct { int w, h; } pc{ I.lw[size_t(lv)], I.lh[size_t(lv)] };
    if (!Impl::Bind(I, j, PDedup).Img(0, I.slots[size_t(prevSlot)].pyr[size_t(lv)].view).Img(1, I.slots[size_t(slot)].pyr[size_t(lv)].view).Buf(2, I.dedupStats, off, 256)
             .Run(&pc, uint32_t(((pc.w + 3) / 4 + 7) / 8), uint32_t(((pc.h + 3) / 4 + 7) / 8), err)) return false;
    const uint64_t v = I.Submit(j, err);
    if (!v || !I.WaitValue(v, err)) return false;
    const uint32_t* r = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(I.dedupStats.map) + off);
    st.maxBlock = float(r[0]) / 1024.f;
    st.mean = float(double(r[1]) / (double(pc.w) * double(pc.h)));
    st.duplicate = st.maxBlock < 3.0f && st.mean < 0.8f;   // PushFPS's DedupSettings
    return true;
}

bool Engine::Impl::MotionDir(Job& j, int sa, int sb, const Pair* prev, bool forward, Pair& pr, std::string& err)
{
    const Slot& A = slots[size_t(sa)];
    const Slot& B = slots[size_t(sb)];
    const int coarsest = levels - 1;
    const int range = std::clamp(cfg.searchRange, 1, 32);
    const bool usePrev = prev != nullptr;
    const Buffer* parent = &dummy;
    int pbx = 1, pby = 1;
    for (int level = coarsest; level >= finest; --level) {
        const int W = lw[size_t(level)], H = lh[size_t(level)], bx = (W + 7) / 8, by = (H + 7) / 8;
        struct {
            int w, h, bx, by, pbx, pby, flags, range;
            float lambda, scale;
            int prevBx, prevBy, prevBs, limX, limY, pad;
        } pc{ W, H, bx, by, pbx, pby, 0, range, std::max(0.f, cfg.smoothness), float(1 << level), fbx, fby, fbs, W / 2 + range * 2, H / 2 + range * 2, 0 };
        const bool last = level == finest;
        const int base = (level == coarsest ? 1 : 0) | (usePrev ? 8 : 0);
        const Buffer& prevField = usePrev ? (forward ? prev->fieldF : prev->fieldK) : dummy;
        const int sweeps = std::clamp(cfg.sweeps, 1, 2);
        pc.flags = base | (last && sweeps == 1 ? 2 : 0);
        if (!Bind(*this, j, PMe).Img(0, A.pyr[size_t(level)].view).Img(1, B.pyr[size_t(level)].view).Buf(2, *parent).Buf(3, dummy).Buf(4, prevField).Buf(5, sweep1[size_t(level)])
                 .Run(&pc, uint32_t(bx), uint32_t(by), err)) return false;
        Barrier(j.cb);
        const Buffer* outGrid = &sweep1[size_t(level)];
        if (sweeps == 2) {
            pc.flags = base | 4 | (cfg.extended ? 16 : 0) | (last ? 2 : 0);
            if (!Bind(*this, j, PMe).Img(0, A.pyr[size_t(level)].view).Img(1, B.pyr[size_t(level)].view).Buf(2, *parent).Buf(3, sweep1[size_t(level)]).Buf(4, prevField).Buf(5, sweep2[size_t(level)])
                     .Run(&pc, uint32_t(bx), uint32_t(by), err)) return false;
            Barrier(j.cb);
            outGrid = &sweep2[size_t(level)];
        }
        parent = outGrid; pbx = bx; pby = by;
    }
    struct { int bx, by; float scale, cutFloor, cutRatio; } pf{ fbx, fby, float(1 << finest), 5.0f, 0.6f };
    if (!Bind(*this, j, PMeFinal).Buf(0, *parent).Buf(1, forward ? pr.fieldF : pr.fieldK).Buf(2, forward ? pr.costF : pr.costK).Buf(3, pr.stats, forward ? 0 : 256, 256)
             .Run(&pf, uint32_t((fbx + 7) / 8), uint32_t((fby + 7) / 8), err)) return false;
    Barrier(j.cb);
    return true;
}

bool Engine::Impl::MotionOf(int sa, int sb, Pair& pr, std::string& err)
{
    // 1: the two luma pictures into the session's input images (compute queue)
    bool ok = true;
    Job& j1 = Begin(err, ok);
    if (!ok) return false;
    VkImageCopy ic{};
    ic.srcSubresource = ic.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    ic.extent = { uint32_t(cfg.width), uint32_t(cfg.height), 1 };
    vkCmdCopyImage(j1.cb, slots[size_t(sa)].pyr[0].img, VK_IMAGE_LAYOUT_GENERAL, ofA.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    vkCmdCopyImage(j1.cb, slots[size_t(sb)].pyr[0].img, VK_IMAGE_LAYOUT_GENERAL, ofB.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    const uint64_t v1 = Submit(j1, err);
    if (!v1) return false;
    // 2: the flow, both directions (optical-flow queue)
    vkResetCommandBuffer(ofCb, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(ofCb, &bi);
    VkOpticalFlowExecuteInfoNV ei{ VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV };
    vkCmdOpticalFlowExecuteNV(ofCb, ofSession, &ei);
    vkEndCommandBuffer(ofCb);
    const uint64_t v2 = ++value;
    VkTimelineSemaphoreSubmitInfo ts{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    ts.waitSemaphoreValueCount = 1; ts.pWaitSemaphoreValues = &v1; ts.signalSemaphoreValueCount = 1; ts.pSignalSemaphoreValues = &v2;
    const VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext = &ts; si.waitSemaphoreCount = 1; si.pWaitSemaphores = &timeline; si.pWaitDstStageMask = &ws;
    si.commandBufferCount = 1; si.pCommandBuffers = &ofCb; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &timeline;
    if (ctx->SubmitOf(si) != VK_SUCCESS) { err = "optical flow: submit failed"; return false; }
    // 3: the flow into the engine's fields and measures (compute queue)
    Job& j3 = Begin(err, ok);
    if (!ok) return false;
    VkBufferImageCopy r{};
    r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; r.imageExtent = { uint32_t(ofGw), uint32_t(ofGh), 1 };
    vkCmdCopyImageToBuffer(j3.cb, ofFlowF.img, VK_IMAGE_LAYOUT_GENERAL, ofBufF.buf, 1, &r);
    vkCmdCopyImageToBuffer(j3.cb, ofFlowB.img, VK_IMAGE_LAYOUT_GENERAL, ofBufB.buf, 1, &r);
    Barrier(j3.cb);
    struct { int w, h, bx, by, bs, gw, gh, grid; float cutFloor, cutRatio; } pc{ cfg.width, cfg.height, fbx, fby, fbs, ofGw, ofGh, ofGrid, 5.0f, 0.6f };
    if (!Bind(*this, j3, POfConv).Buf(0, ofBufF).Img(1, slots[size_t(sa)].pyr[0].view).Img(2, slots[size_t(sb)].pyr[0].view).Buf(3, pr.fieldF).Buf(4, pr.costF).Buf(5, pr.stats, 0, 256)
             .Run(&pc, uint32_t((fbx + 7) / 8), uint32_t((fby + 7) / 8), err)) return false;
    if (!Bind(*this, j3, POfConv).Buf(0, ofBufB).Img(1, slots[size_t(sb)].pyr[0].view).Img(2, slots[size_t(sa)].pyr[0].view).Buf(3, pr.fieldK).Buf(4, pr.costK).Buf(5, pr.stats, 256, 256)
             .Run(&pc, uint32_t((fbx + 7) / 8), uint32_t((fby + 7) / 8), err)) return false;
    Barrier(j3.cb);
    struct { int w, h; } pm{ lw[size_t(finest)], lh[size_t(finest)] };
    if (!Bind(*this, j3, PMad).Img(0, slots[size_t(sa)].pyr[size_t(finest)].view).Img(1, slots[size_t(sb)].pyr[size_t(finest)].view).Buf(2, pr.stats, 512, 256)
             .Run(&pm, uint32_t((pm.w + 15) / 16), uint32_t((pm.h + 15) / 16), err)) return false;
    struct { int bx, by, bs; } pb{ fbx, fby, fbs };
    if (!Bind(*this, j3, PFb).Buf(0, pr.fieldF).Buf(1, pr.fieldK).Buf(2, pr.stats, 768, 256).Run(&pb, uint32_t((fbx + 7) / 8), uint32_t((fby + 7) / 8), err)) return false;
    const uint64_t v3 = Submit(j3, err, v2);
    return v3 && WaitValue(v3, err);
}

int Engine::Estimate(int slotA, int slotB, int prevPair, PairStats& st, std::string& err)
{
    Impl& I = *m;
    st = {};
    int pi = -1;
    for (size_t i = 0; i < I.pairs.size(); ++i) if (!I.pairs[i].used) { pi = int(i); break; }
    if (pi < 0) { err = "GPU engine: no free pair"; return -1; }
    Impl::Pair& pr = I.pairs[size_t(pi)];
    const Impl::Pair* prev = prevPair >= 0 && prevPair < int(I.pairs.size()) && I.pairs[size_t(prevPair)].used ? &I.pairs[size_t(prevPair)] : nullptr;
    const double t0 = NowMs();
    std::memset(pr.stats.map, 0, 1024);
    pr.holdOn = false;
    if (I.useOf) {
        if (!I.MotionOf(slotA, slotB, pr, err)) return -1;
    } else {
        bool ok = true;
        Impl::Job& j = I.Begin(err, ok);
        if (!ok) return -1;
        if (!I.MotionDir(j, slotA, slotB, prev, true, pr, err) || !I.MotionDir(j, slotB, slotA, prev, false, pr, err)) return -1;
        struct { int w, h; } pm{ I.lw[size_t(I.finest)], I.lh[size_t(I.finest)] };
        if (!Impl::Bind(I, j, PMad).Img(0, I.slots[size_t(slotA)].pyr[size_t(I.finest)].view).Img(1, I.slots[size_t(slotB)].pyr[size_t(I.finest)].view).Buf(2, pr.stats, 512, 256)
                 .Run(&pm, uint32_t((pm.w + 15) / 16), uint32_t((pm.h + 15) / 16), err)) return -1;
        struct { int bx, by, bs; } pb{ I.fbx, I.fby, I.fbs };
        if (!Impl::Bind(I, j, PFb).Buf(0, pr.fieldF).Buf(1, pr.fieldK).Buf(2, pr.stats, 768, 256).Run(&pb, uint32_t((I.fbx + 7) / 8), uint32_t((I.fby + 7) / 8), err)) return -1;
        const uint64_t v = I.Submit(j, err);
        if (!v || !I.WaitValue(v, err)) return -1;
    }
    const uint32_t* s = static_cast<const uint32_t*>(pr.stats.map);
    const double n = std::max(1u, s[2]);
    st.blocks = int(s[2]);
    st.unexplainedF = float(s[0] / n); st.meanCostF = float(s[1] / 256.0 / n);
    st.unexplainedB = float(s[64] / std::max(1.0, double(s[66]))); st.meanCostB = float(s[65] / 256.0 / std::max(1.0, double(s[66])));
    st.zeroCost = float(double(s[128]) / (double(I.lw[size_t(I.finest)]) * double(I.lh[size_t(I.finest)])));
    st.consistent = float(s[192] / n); st.moving = float(s[193] / n);
    st.cutRule = double(s[0]) > n * 0.5 || double(s[64]) > std::max(1.0, double(s[66])) * 0.5;
    st.ms = NowMs() - t0;
    pr.used = true; pr.a = slotA; pr.b = slotB;
    I.times.pairs++;
    I.times.meMs += st.ms;
    return pi;
}

bool Engine::SetHold(int pair, const HoldPairYuv* hold, std::string& err)
{
    Impl& I = *m;
    Impl::Pair& pr = I.pairs[size_t(pair)];
    if (!I.WaitValue(pr.lastUse, err)) return false;   // the host rewrites the buffer: its earlier readers are done
    pr.holdOn = false;
    if (!hold || hold->held == 0) return true;
    const VkDeviceSize n = hold->data.size() * sizeof(float);
    // the previous use of this buffer is over: a pair's outputs are waited for before the pair is reused
    if (pr.hold.size < n) {
        I.ctx->Destroy(pr.hold);
        if (!I.ctx->CreateBuffer(n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, pr.hold, err)) return false;
    }
    std::memcpy(pr.hold.map, hold->data.data(), size_t(n));
    pr.holdOn = true; pr.zx = hold->x; pr.zy = hold->y; pr.zw = hold->w; pr.zh = hold->h;
    return true;
}

bool Engine::Impl::Interp(Job& j, int sa, int sb, const Pair* pr, float t, int mode, int target, int out, std::string& err)
{
    const Slot& A = slots[size_t(sa)];
    const Slot& B = slots[size_t(sb)];
    struct {
        int W, H, CW, CH, mode, plane, target, bps, pitchY, pitchC, offU, offV, fbx, fby, fbs, hold, zx, zy, zw, zh;
        float t, maxV, kRaw, errLow, errHigh;
        float pad;
    } pc{ cfg.width, cfg.height, cw, ch, mode, 0, target, bps, pitchY, pitchC, int(offU), int(offV), fbx, fby, fbs,
          pr && pr->holdOn && mode == 0 ? 1 : 0, pr ? pr->zx : 0, pr ? pr->zy : 0, pr ? pr->zw : 0, pr ? pr->zh : 0, t, maxV, kRaw, cfg.errorLow, cfg.errorHigh, 0 };
    const VkDeviceSize outOff = target == 0 ? outStride * VkDeviceSize(out) : 0;
    for (int plane = 0; plane < 2; ++plane) {
        pc.plane = plane;
        const int per = target == 0 ? 4 / bps : 1;
        const int w = plane ? cw : cfg.width, h = plane ? ch : cfg.height;
        Bind b(*this, j, PInterp);
        b.Img(0, A.y.view).Img(1, A.u.view).Img(2, A.v.view).Img(3, B.y.view).Img(4, B.u.view).Img(5, B.v.view)
         .Buf(6, pr ? pr->fieldF : dummy).Buf(7, pr ? pr->fieldK : dummy).Buf(8, readback, outOff, outStride).Img(9, midY.view).Img(10, midU.view).Img(11, midV.view)
         .Buf(12, pc.hold ? pr->hold : dummy);
        if (!b.Run(&pc, uint32_t(((w + per - 1) / per + 15) / 16), uint32_t((h + 7) / 8), err)) return false;
    }
    return true;
}

bool Engine::Impl::UpscaleAndPack(Job& j, int out, std::string& err)
{
    Barrier(j.cb);
    if (cfg.upscaler == Upscaler::Nis) {
        Bind b(*this, j, PNis);
        b.Buf(0, nisUbo, 0, 256).Img(1, VK_NULL_HANDLE, linear).Img(2, midY.viewRRR).Img(3, coefScale.view).Img(4, coefUsm.view).Img(5, upY.view);
        if (!b.Run(nullptr, uint32_t((outW + 31) / 32), uint32_t((outH + 23) / 24), err)) return false;
    } else {
        const float iw = float(cfg.width), ih = float(cfg.height), ow = float(outW), oh = float(outH);
        struct { uint32_t con[16]; int ow, oh; } pe{};
        pe.con[0] = AsU(iw / ow); pe.con[1] = AsU(ih / oh); pe.con[2] = AsU(0.5f * iw / ow - 0.5f); pe.con[3] = AsU(0.5f * ih / oh - 0.5f);
        pe.con[4] = AsU(1.f / iw); pe.con[5] = AsU(1.f / ih); pe.con[6] = AsU(1.f / iw); pe.con[7] = AsU(-1.f / ih);
        pe.con[8] = AsU(-1.f / iw); pe.con[9] = AsU(2.f / ih); pe.con[10] = AsU(1.f / iw); pe.con[11] = AsU(2.f / ih);
        pe.con[12] = AsU(0.f); pe.con[13] = AsU(4.f / ih); pe.con[14] = 0; pe.con[15] = 0;
        pe.ow = outW; pe.oh = outH;
        if (!Bind(*this, j, PEasu).Img(0, midY.view, linear).Img(1, upTmp.view).Run(&pe, uint32_t((outW + 7) / 8), uint32_t((outH + 7) / 8), err)) return false;
        Barrier(j.cb);
        struct { uint32_t con[4]; int w, h; } pr{};
        const float stops = 2.f * (1.f - std::clamp(cfg.sharpness, 0.f, 1.f));
        pr.con[0] = AsU(std::exp2(-stops)); pr.w = outW; pr.h = outH;
        if (!Bind(*this, j, PRcas).Img(0, upTmp.view).Img(1, upY.view).Run(&pr, uint32_t((outW + 7) / 8), uint32_t((outH + 7) / 8), err)) return false;
    }
    Barrier(j.cb);
    struct { int OW, OH, OCW, OCH, SW, SH, SCW, SCH, plane, bps, pitchY, pitchC, offU, offV; float maxV; } pp{
        outW, outH, ocw, och, cfg.width, cfg.height, cw, ch, 0, bps, pitchY, pitchC, int(offU), int(offV), maxV };
    const int per = 4 / bps;
    for (int plane = 0; plane < 2; ++plane) {
        pp.plane = plane;
        const int w = plane ? ocw : outW, h = plane ? och : outH;
        if (!Bind(*this, j, PPack).Img(0, upY.view).Img(1, midU.view).Img(2, midV.view).Buf(3, readback, outStride * VkDeviceSize(out), outStride)
                 .Run(&pp, uint32_t(((w + per - 1) / per + 15) / 16), uint32_t((h + 7) / 8), err)) return false;
    }
    return true;
}

bool Engine::Generate(int pair, float t, int mode, int& out, uint64_t& ticket, std::string& err)
{
    Impl& I = *m;
    out = I.FreeOut();
    if (out < 0) { err = "GPU engine: no free readback slot"; return false; }
    Impl::Pair& pr = I.pairs[size_t(pair)];
    bool ok = true;
    Impl::Job& j = I.Begin(err, ok);
    if (!ok) return false;
    const bool up = I.cfg.upscaler != Upscaler::None;
    const int sa = mode == 2 ? pr.b : pr.a, sb = mode == 1 ? pr.a : pr.b;
    if (!I.Interp(j, sa, sb, &pr, t, mode == 2 ? 1 : mode, up ? 1 : 0, out, err)) return false;
    if (up && !I.UpscaleAndPack(j, out, err)) return false;
    ticket = I.Submit(j, err);
    if (!ticket) return false;
    pr.lastUse = ticket;
    I.outUsed[size_t(out)] = true;
    I.times.outputs++;
    return true;
}

bool Engine::Real(int slot, int& out, uint64_t& ticket, std::string& err)
{
    Impl& I = *m;
    out = I.FreeOut();
    if (out < 0) { err = "GPU engine: no free readback slot"; return false; }
    bool ok = true;
    Impl::Job& j = I.Begin(err, ok);
    if (!ok) return false;
    const bool up = I.cfg.upscaler != Upscaler::None;
    if (!I.Interp(j, slot, slot, nullptr, 0.f, 1, up ? 1 : 0, out, err)) return false;
    if (up && !I.UpscaleAndPack(j, out, err)) return false;
    ticket = I.Submit(j, err);
    if (!ticket) return false;
    I.outUsed[size_t(out)] = true;
    I.times.outputs++;
    return true;
}

void Engine::Read(int out, const YuvOut& o)
{
    Impl& I = *m;
    const uint8_t* base = static_cast<const uint8_t*>(I.readback.map) + I.outStride * VkDeviceSize(out);
    for (int p = 0; p < 3; ++p) {
        const int w = p ? I.ocw : I.outW, h = p ? I.och : I.outH, pitch = p ? I.pitchC : I.pitchY;
        const uint8_t* src = base + (p == 0 ? 0 : p == 1 ? I.offU : I.offV);
        const size_t row = size_t(w) * size_t(I.bps);
        for (int y = 0; y < h; ++y) std::memcpy(o.data[p] + ptrdiff_t(y) * o.stride[p], src + size_t(y) * size_t(pitch), row);
    }
    I.outUsed[size_t(out)] = false;
}

bool Engine::InjectFields(int pair, const veyra::portable::MotionField& f, const veyra::portable::MotionField& k, std::string& err)
{
    Impl& I = *m;
    if (f.blocksX != I.fbx || f.blocksY != I.fby || f.blockSize != I.fbs || k.blocksX != I.fbx || k.blocksY != I.fby) {
        err = "InjectFields: the CPU field's grid differs from the engine's"; return false;
    }
    const size_t n = size_t(I.fbx) * size_t(I.fby);
    Buffer st;
    if (!I.ctx->CreateBuffer(n * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, st, err)) return false;
    float* p = static_cast<float*>(st.map);
    for (size_t i = 0; i < n; ++i) { p[2 * i] = f.vx[i]; p[2 * i + 1] = f.vy[i]; p[2 * n + 2 * i] = k.vx[i]; p[2 * n + 2 * i + 1] = k.vy[i]; }
    bool ok = true;
    Impl::Job& j = I.Begin(err, ok);
    if (!ok) { I.ctx->Destroy(st); return false; }
    VkBufferCopy c{ 0, 0, n * 8 };
    vkCmdCopyBuffer(j.cb, st.buf, I.pairs[size_t(pair)].fieldF.buf, 1, &c);
    c.srcOffset = n * 8;
    vkCmdCopyBuffer(j.cb, st.buf, I.pairs[size_t(pair)].fieldK.buf, 1, &c);
    const uint64_t v = I.Submit(j, err);
    ok = v && I.WaitValue(v, err);
    I.ctx->Destroy(st);
    return ok;
}

bool Engine::ReadFields(int pair, veyra::portable::MotionField& f, veyra::portable::MotionField& k, std::string& err)
{
    Impl& I = *m;
    const size_t n = size_t(I.fbx) * size_t(I.fby);
    Buffer st;
    if (!I.ctx->CreateBuffer(n * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, st, err)) return false;
    bool ok = true;
    Impl::Job& j = I.Begin(err, ok);
    if (!ok) { I.ctx->Destroy(st); return false; }
    VkBufferCopy c{ 0, 0, n * 8 };
    vkCmdCopyBuffer(j.cb, I.pairs[size_t(pair)].fieldF.buf, st.buf, 1, &c);
    c.dstOffset = n * 8;
    vkCmdCopyBuffer(j.cb, I.pairs[size_t(pair)].fieldK.buf, st.buf, 1, &c);
    const uint64_t v = I.Submit(j, err);
    ok = v && I.WaitValue(v, err);
    if (ok) {
        for (auto* m2 : { &f, &k }) {
            m2->blocksX = I.fbx; m2->blocksY = I.fby; m2->blockSize = I.fbs; m2->width = I.cfg.width; m2->height = I.cfg.height;
            m2->vx.resize(n); m2->vy.resize(n);
        }
        const float* p = static_cast<const float*>(st.map);
        for (size_t i = 0; i < n; ++i) { f.vx[i] = p[2 * i]; f.vy[i] = p[2 * i + 1]; k.vx[i] = p[2 * n + 2 * i]; k.vy[i] = p[2 * n + 2 * i + 1]; }
    }
    I.ctx->Destroy(st);
    return ok;
}

} // namespace pmvs::gpu
