// pushmpc.Enhance, backend "apple": the Metal stages (Objective-C++, ARC). See apple_metal.h.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#include "apple_convert.h"
#include "apple_metal.h"

namespace pmvs::apple {

namespace {

const char* kShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct ConvP { uint inSlices; uint flags; uint w; uint h; };          // flags: 1 relu, 2 add skip
struct FirstP { uint w; uint h; float scale; uint pad; };              // luma = read * scale (normalised 0..1)
struct FinalP { uint inSlices; uint flags; uint w; uint h; float lo; float hi; uint out16; uint pad; };

// the first layer: one input channel (the Y plane), 4 output channels a slice
kernel void conv_first(texture2d<float, access::read> y [[texture(0)]],
                       texture2d_array<half, access::write> dst [[texture(1)]],
                       device const float4* W [[buffer(0)]], device const float4* B [[buffer(1)]],
                       constant FirstP& P [[buffer(2)]], uint3 g [[thread_position_in_grid]])
{
    if (g.x >= P.w || g.y >= P.h) return;
    float4 acc = B[g.z];
    for (int t = 0; t < 9; ++t) {
        int2 p = int2(g.xy) + int2(t % 3 - 1, t / 3 - 1);
        if (p.x < 0 || p.y < 0 || p.x >= int(P.w) || p.y >= int(P.h)) continue;
        float v = y.read(uint2(p)).r * P.scale;
        acc += W[(g.z * 9 + uint(t)) * 4] * v;          // column 0 (the only input channel)
    }
    dst.write(half4(acc), g.xy, g.z);
}

// a 3x3 layer: every input slice, 9 taps, a 4x4 weight block per (out slice, in slice, tap) as 4 columns
kernel void conv(texture2d_array<half, access::read> src [[texture(0)]],
                 texture2d_array<half, access::write> dst [[texture(1)]],
                 texture2d_array<half, access::read> skip [[texture(2)]],
                 device const float4* W [[buffer(0)]], device const float4* B [[buffer(1)]],
                 constant ConvP& P [[buffer(2)]], uint3 g [[thread_position_in_grid]])
{
    if (g.x >= P.w || g.y >= P.h) return;
    float4 acc = B[g.z];
    for (uint s = 0; s < P.inSlices; ++s)
        for (int t = 0; t < 9; ++t) {
            int2 p = int2(g.xy) + int2(t % 3 - 1, t / 3 - 1);
            if (p.x < 0 || p.y < 0 || p.x >= int(P.w) || p.y >= int(P.h)) continue;
            float4 v = float4(src.read(uint2(p), s));
            uint b = ((g.z * P.inSlices + s) * 9 + uint(t)) * 4;
            acc += W[b] * v.x + W[b + 1] * v.y + W[b + 2] * v.z + W[b + 3] * v.w;
        }
    if (P.flags & 2) acc += float4(skip.read(g.xy, g.z));
    if (P.flags & 1) acc = max(acc, 0.0f);
    dst.write(half4(acc), g.xy, g.z);
}

// the last layer (4 channels), depth-to-space x2 (DCR: channel = row parity * 2 + column parity) and the clip,
// written into the output Y plane (8-bit unorm, or 16-bit unorm holding 10 bits in the high bits)
kernel void conv_final(texture2d_array<half, access::read> src [[texture(0)]],
                       texture2d<float, access::write> y [[texture(1)]],
                       texture2d_array<half, access::read> skip [[texture(2)]],
                       device const float4* W [[buffer(0)]], device const float4* B [[buffer(1)]],
                       constant FinalP& P [[buffer(2)]], uint2 g [[thread_position_in_grid]])
{
    if (g.x >= P.w || g.y >= P.h) return;
    float4 acc = B[0];
    for (uint s = 0; s < P.inSlices; ++s)
        for (int t = 0; t < 9; ++t) {
            int2 p = int2(g) + int2(t % 3 - 1, t / 3 - 1);
            if (p.x < 0 || p.y < 0 || p.x >= int(P.w) || p.y >= int(P.h)) continue;
            float4 v = float4(src.read(uint2(p), s));
            uint b = (s * 9 + uint(t)) * 4;
            acc += W[b] * v.x + W[b + 1] * v.y + W[b + 2] * v.z + W[b + 3] * v.w;
        }
    if (P.flags & 2) acc += float4(skip.read(g, 0));
    if (P.flags & 1) acc = max(acc, 0.0f);
    acc = clamp(acc, P.lo, P.hi);
    if (P.out16 != 0) acc = round(acc * 1023.0f) * (64.0f / 65535.0f);
    uint2 o = g * 2;
    y.write(float4(acc.x), o);
    y.write(float4(acc.y), o + uint2(1, 0));
    y.write(float4(acc.z), o + uint2(0, 1));
    y.write(float4(acc.w), o + uint2(1, 1));
}

// chroma x2, bilinear (CbCr interleaved: rg)
kernel void chroma_up(texture2d<float, access::sample> src [[texture(0)]],
                      texture2d<float, access::write> dst [[texture(1)]],
                      constant uint4& P [[buffer(0)]], uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    float2 uv = (float2(g) + 0.5f) / float2(dst.get_width(), dst.get_height());
    float4 c = src.sample(s, uv);
    if (P.x != 0) c = round(c * (65535.0f / 64.0f)) * (64.0f / 65535.0f);
    dst.write(c, g);
}

// 10-bit recovery, one plane (C channels: 1 luma, 2 chroma). Codes: 16-bit unorm read * 65535 / 64 = 10-bit code;
// 8-bit unorm read * 255 = 8-bit code. flowScale: flow pixels -> this plane's pixels.
struct RecP { float t; float flowScaleX; float flowScaleY; uint hasFlow; uint chans; uint w; uint h; uint pad; };
static float4 code10(texture2d<float, access::read> tex, int2 p, uint w, uint h)
{
    p = clamp(p, int2(0), int2(int(w) - 1, int(h) - 1));
    return round(tex.read(uint2(p)) * (65535.0f / 64.0f));
}
static float4 resid(texture2d<float, access::read> tex, float2 q, uint w, uint h)
{
    int2 p0 = int2(floor(q));
    float2 f = q - float2(p0);
    float4 r[4];
    for (int i = 0; i < 4; ++i) {
        float4 c = code10(tex, p0 + int2(i & 1, i >> 1), w, h);
        float4 c8 = min(floor((c + 2.0f) / 4.0f), 255.0f);
        r[i] = c - 4.0f * c8;
    }
    return mix(mix(r[0], r[1], f.x), mix(r[2], r[3], f.x), f.y);
}
kernel void recover(texture2d<float, access::read> a [[texture(0)]],
                    texture2d<float, access::read> b [[texture(1)]],
                    texture2d<float, access::read> g8 [[texture(2)]],
                    texture2d<float, access::sample> flow [[texture(3)]],
                    texture2d<float, access::write> out [[texture(4)]],
                    constant RecP& P [[buffer(0)]], uint2 g [[thread_position_in_grid]])
{
    if (g.x >= P.w || g.y >= P.h) return;
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    float2 f = float2(0.0f);
    if (P.hasFlow != 0) {
        float2 uv = (float2(g) + 0.5f) / float2(P.w, P.h);
        f = flow.sample(s, uv).xy * float2(P.flowScaleX, P.flowScaleY);
    }
    const float t = P.t;
    float4 acc = float4(0.0f);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            float2 p = float2(int2(g) + int2(dx, dy));
            float wgt = (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f);
            acc += wgt * ((1.0f - t) * resid(a, p - t * f, P.w, P.h) + t * resid(b, p + (1.0f - t) * f, P.w, P.h));
        }
    float4 v = 4.0f * round(g8.read(g) * 255.0f) + acc / 16.0f;
    v = clamp(round(v), 0.0f, 1023.0f) * (64.0f / 65535.0f);
    out.write(v, g);
}

kernel void flow_scale(texture2d<float, access::read> src [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]],
                       constant float& s [[buffer(0)]], uint2 g [[thread_position_in_grid]])
{
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    dst.write(src.read(g) * s, g);
}
)MSL";

std::string ErrText(NSError* e) { return e ? std::string(e.localizedDescription.UTF8String ?: "error") : std::string("unknown error"); }

bool Is16(uint32_t f) { return f == kx420 || f == kxf20; }

} // namespace

struct Metal::Net {
    struct L { int cin = 0, cout = 0, inSlices = 0, outSlices = 0; uint32_t flags = 0; float lo = 0, hi = 1; id<MTLBuffer> w, b; };
    std::vector<L> layers;
    int maxSlices = 1;
    std::string name;
    // per-size intermediates
    int tw = 0, th = 0;
    id<MTLTexture> a, b, skip;
};

struct Metal::Impl {
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    CVMetalTextureCacheRef cache = nullptr;
    id<MTLComputePipelineState> first, conv, fin, chroma, recover, flowScale;
    id<MTLTexture> dummyArray, dummy2d;
    std::mutex lock;

    id<MTLComputePipelineState> Pipe(id<MTLLibrary> lib, NSString* name, std::string& err)
    {
        NSError* e = nil;
        id<MTLFunction> f = [lib newFunctionWithName:name];
        if (!f) { err = std::string("no kernel ") + name.UTF8String; return nil; }
        id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:f error:&e];
        if (!p) err = std::string("pipeline ") + name.UTF8String + ": " + ErrText(e);
        return p;
    }
    // one plane of a pixel buffer as a texture (the CVMetalTexture is kept alive in keep until the GPU is done)
    id<MTLTexture> Plane(CVPixelBufferRef pb, int plane, NSMutableArray* keep, std::string& err)
    {
        const OSType f = CVPixelBufferGetPixelFormatType(pb);
        MTLPixelFormat mf;
        if (f == kCVPixelFormatType_TwoComponent16Half) mf = MTLPixelFormatRG16Float;
        else if (f == kCVPixelFormatType_OneComponent32Float) mf = MTLPixelFormatR32Float;
        else if (Is16(f)) mf = plane ? MTLPixelFormatRG16Unorm : MTLPixelFormatR16Unorm;
        else mf = plane ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm;
        const bool planar = CVPixelBufferIsPlanar(pb);
        const size_t w = planar ? CVPixelBufferGetWidthOfPlane(pb, size_t(plane)) : CVPixelBufferGetWidth(pb);
        const size_t h = planar ? CVPixelBufferGetHeightOfPlane(pb, size_t(plane)) : CVPixelBufferGetHeight(pb);
        CVMetalTextureRef t = nullptr;
        const CVReturn r = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, nullptr, mf, w, h, size_t(planar ? plane : 0), &t);
        if (r != kCVReturnSuccess || !t) { err = "CVMetalTextureCacheCreateTextureFromImage failed (" + std::to_string(int(r)) + ", format " + FourCCText(f) + ")"; return nil; }
        id<MTLTexture> tex = CVMetalTextureGetTexture(t);
        [keep addObject:(__bridge_transfer id)t];
        return tex;
    }
    void Dispatch(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> p, NSUInteger w, NSUInteger h, NSUInteger d)
    {
        [enc setComputePipelineState:p];
        const NSUInteger tw = p.threadExecutionWidth, th = std::max<NSUInteger>(1, p.maxTotalThreadsPerThreadgroup / tw);
        [enc dispatchThreads:MTLSizeMake(w, h, d) threadsPerThreadgroup:MTLSizeMake(tw, std::min<NSUInteger>(th, 16), 1)];
    }
    id<MTLTexture> Array(int w, int h, int slices)
    {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:NSUInteger(w) height:NSUInteger(h) mipmapped:NO];
        d.textureType = MTLTextureType2DArray;
        d.arrayLength = NSUInteger(std::max(1, slices));
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        d.storageMode = MTLStorageModePrivate;
        return [dev newTextureWithDescriptor:d];
    }
};

Metal::Metal() : m(std::make_unique<Impl>()) {}
Metal::~Metal() { if (m->cache) CFRelease(m->cache); }

Metal* Metal::Shared(std::string& err)
{
    static Metal* inst = nullptr;
    static std::string initErr;
    static std::once_flag once;
    std::call_once(once, [] {
        @autoreleasepool {
            auto* mt = new Metal();
            Impl& d = *mt->m;
            d.dev = MTLCreateSystemDefaultDevice();
            if (!d.dev) { initErr = "no Metal device"; delete mt; return; }
            d.queue = [d.dev newCommandQueue];
            if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, d.dev, nullptr, &d.cache) != kCVReturnSuccess) { initErr = "CVMetalTextureCacheCreate failed"; delete mt; return; }
            NSError* e = nil;
            MTLCompileOptions* o = [MTLCompileOptions new];
            id<MTLLibrary> lib = [d.dev newLibraryWithSource:[NSString stringWithUTF8String:kShaders] options:o error:&e];
            if (!lib) { initErr = "Metal kernels did not compile: " + ErrText(e); delete mt; return; }
            std::string pe;
            d.first = d.Pipe(lib, @"conv_first", pe); d.conv = d.Pipe(lib, @"conv", pe); d.fin = d.Pipe(lib, @"conv_final", pe);
            d.chroma = d.Pipe(lib, @"chroma_up", pe); d.recover = d.Pipe(lib, @"recover", pe); d.flowScale = d.Pipe(lib, @"flow_scale", pe);
            if (!pe.empty()) { initErr = pe; delete mt; return; }
            d.dummyArray = d.Array(1, 1, 1);
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float width:1 height:1 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            d.dummy2d = [d.dev newTextureWithDescriptor:td];
            inst = mt;
        }
    });
    if (!inst) err = initErr;
    return inst;
}

std::string Metal::DeviceName() const { return m->dev ? std::string(m->dev.name.UTF8String) : std::string(); }

std::shared_ptr<Metal::Net> Metal::LoadNet(const NnModel& model, std::string& err)
{
    if (!model.Valid(err)) return nullptr;
    auto n = std::make_shared<Net>();
    n->name = model.name;
    for (const NnLayer& L : model.layers) {
        Net::L l;
        l.cin = L.cin; l.cout = L.cout; l.flags = L.flags; l.lo = L.lo; l.hi = L.hi;
        l.inSlices = (L.cin + 3) / 4; l.outSlices = (L.cout + 3) / 4;
        n->maxSlices = std::max({ n->maxSlices, l.inSlices, l.outSlices });
        // [out slice][in slice][tap][column c (input 4 is + c)] = float4 over rows r (output 4 os + r)
        std::vector<float> w(size_t(l.outSlices) * size_t(l.inSlices) * 9 * 16, 0.f), b(size_t(l.outSlices) * 4, 0.f);
        for (int oc = 0; oc < L.cout; ++oc) {
            b[size_t(oc)] = L.b[size_t(oc)];
            for (int ic = 0; ic < L.cin; ++ic)
                for (int t = 0; t < 9; ++t) {
                    const size_t os = size_t(oc / 4), r = size_t(oc % 4), is = size_t(ic / 4), c = size_t(ic % 4);
                    w[(((os * size_t(l.inSlices) + is) * 9 + size_t(t)) * 4 + c) * 4 + r] = L.w[(size_t(oc) * size_t(L.cin) + size_t(ic)) * 9 + size_t(t)];
                }
        }
        l.w = [m->dev newBufferWithBytes:w.data() length:w.size() * 4 options:MTLResourceStorageModeShared];
        l.b = [m->dev newBufferWithBytes:b.data() length:b.size() * 4 options:MTLResourceStorageModeShared];
        if (!l.w || !l.b) { err = "Metal buffer allocation failed"; return nullptr; }
        n->layers.push_back(l);
    }
    return n;
}

void* Metal::NewCommandBuffer() { return (__bridge_retained void*)[m->queue commandBuffer]; }

bool Metal::Commit(void* cmd, bool wait, std::string& err)
{
    id<MTLCommandBuffer> cb = (__bridge_transfer id<MTLCommandBuffer>)cmd;
    [cb commit];
    if (!wait) return true;
    [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusError) { err = "Metal: " + ErrText(cb.error); return false; }
    return true;
}

namespace {
struct ConvP { uint32_t inSlices, flags, w, h; };
struct FirstP { uint32_t w, h; float scale; uint32_t pad; };
struct FinalP { uint32_t inSlices, flags, w, h; float lo, hi; uint32_t out16, pad; };
struct RecP { float t, fsx, fsy; uint32_t hasFlow, chans, w, h, pad; };
}

// the layers from a Y texture (scale: read -> 0..1) into the Y plane of dst (out16: 10 bits high in 16)
static bool EncodeNet(Metal::Impl& d, Metal::Net& n, id<MTLCommandBuffer> cb, id<MTLTexture> yIn, float scale, id<MTLTexture> yOut, bool out16, int w, int h, std::string& err)
{
    if (n.tw != w || n.th != h || !n.a) {
        n.a = d.Array(w, h, n.maxSlices); n.b = d.Array(w, h, n.maxSlices); n.skip = d.Array(w, h, n.maxSlices);
        n.tw = w; n.th = h;
        if (!n.a || !n.b || !n.skip) { err = "Metal: intermediate textures"; return false; }
    }
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    id<MTLTexture> cur = nil;
    for (size_t i = 0; i < n.layers.size(); ++i) {
        const auto& l = n.layers[i];
        const bool save = l.flags & NnLayer::kSaveSkip;
        if (l.flags & NnLayer::kFinal) {
            FinalP p{ uint32_t(l.inSlices), l.flags & 3u, uint32_t(w), uint32_t(h), l.lo, l.hi, out16 ? 1u : 0u, 0 };
            [enc setTexture:cur atIndex:0]; [enc setTexture:yOut atIndex:1]; [enc setTexture:n.skip atIndex:2];
            [enc setBuffer:l.w offset:0 atIndex:0]; [enc setBuffer:l.b offset:0 atIndex:1]; [enc setBytes:&p length:sizeof(p) atIndex:2];
            d.Dispatch(enc, d.fin, NSUInteger(w), NSUInteger(h), 1);
            break;
        }
        // ping-pong between a and b; the layer that feeds the long skip writes the skip tensor (read later as well)
        id<MTLTexture> out = save ? n.skip : (cur == n.a ? n.b : n.a);
        if (i == 0) {
            FirstP p{ uint32_t(w), uint32_t(h), scale, 0 };
            [enc setTexture:yIn atIndex:0]; [enc setTexture:out atIndex:1];
            [enc setBuffer:l.w offset:0 atIndex:0]; [enc setBuffer:l.b offset:0 atIndex:1]; [enc setBytes:&p length:sizeof(p) atIndex:2];
            d.Dispatch(enc, d.first, NSUInteger(w), NSUInteger(h), NSUInteger(l.outSlices));
        } else {
            ConvP p{ uint32_t(l.inSlices), l.flags & 3u, uint32_t(w), uint32_t(h) };
            [enc setTexture:cur atIndex:0]; [enc setTexture:out atIndex:1]; [enc setTexture:n.skip atIndex:2];
            [enc setBuffer:l.w offset:0 atIndex:0]; [enc setBuffer:l.b offset:0 atIndex:1]; [enc setBytes:&p length:sizeof(p) atIndex:2];
            d.Dispatch(enc, d.conv, NSUInteger(w), NSUInteger(h), NSUInteger(l.outSlices));
        }
        cur = out;
    }
    [enc endEncoding];
    return true;
}

bool Metal::EncodeArtCnn(Net& n, void* cmd, CVPixelBufferRef src, CVPixelBufferRef dst, std::string& err)
{
    std::lock_guard<std::mutex> lk(m->lock);
    @autoreleasepool {
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmd;
        NSMutableArray* keep = [NSMutableArray array];
        const int w = int(CVPixelBufferGetWidth(src)), h = int(CVPixelBufferGetHeight(src));
        if (int(CVPixelBufferGetWidth(dst)) != 2 * w || int(CVPixelBufferGetHeight(dst)) != 2 * h) { err = "ArtCNN: the output must be 2x the input"; return false; }
        const bool in16 = Is16(CVPixelBufferGetPixelFormatType(src)), out16 = Is16(CVPixelBufferGetPixelFormatType(dst));
        id<MTLTexture> yi = m->Plane(src, 0, keep, err), ci = m->Plane(src, 1, keep, err), yo = m->Plane(dst, 0, keep, err), co = m->Plane(dst, 1, keep, err);
        if (!yi || !ci || !yo || !co) return false;
        // 16-bit words hold 10-bit codes in the high bits: code = read * 65535 / 64; normalise by 1023
        const float scale = in16 ? float(65535.0 / 64.0 / 1023.0) : 1.0f;
        if (!EncodeNet(*m, n, cb, yi, scale, yo, out16, w, h, err)) return false;
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        const uint32_t p[4] = { out16 ? 1u : 0u, 0, 0, 0 };
        [enc setTexture:ci atIndex:0]; [enc setTexture:co atIndex:1]; [enc setBytes:p length:sizeof(p) atIndex:0];
        m->Dispatch(enc, m->chroma, co.width, co.height, 1);
        [enc endEncoding];
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) { (void)keep.count; }];
        return true;
    }
}

bool Metal::EncodeRecover(void* cmd, CVPixelBufferRef a16, CVPixelBufferRef b16, CVPixelBufferRef g8, CVPixelBufferRef flow, float t, CVPixelBufferRef out, std::string& err)
{
    std::lock_guard<std::mutex> lk(m->lock);
    @autoreleasepool {
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmd;
        NSMutableArray* keep = [NSMutableArray array];
        id<MTLTexture> ft = flow ? m->Plane(flow, 0, keep, err) : m->dummy2d;
        if (!ft) return false;
        const float fw = flow ? float(CVPixelBufferGetWidth(flow)) : 1.f, fh = flow ? float(CVPixelBufferGetHeight(flow)) : 1.f;
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        for (int plane = 0; plane < 2; ++plane) {
            id<MTLTexture> ta = m->Plane(a16, plane, keep, err), tb = m->Plane(b16, plane, keep, err), tg = m->Plane(g8, plane, keep, err), to = m->Plane(out, plane, keep, err);
            if (!ta || !tb || !tg || !to) { [enc endEncoding]; return false; }
            RecP p{ t, float(to.width) / fw, float(to.height) / fh, flow ? 1u : 0u, plane ? 2u : 1u, uint32_t(to.width), uint32_t(to.height), 0 };
            [enc setTexture:ta atIndex:0]; [enc setTexture:tb atIndex:1]; [enc setTexture:tg atIndex:2]; [enc setTexture:ft atIndex:3]; [enc setTexture:to atIndex:4];
            [enc setBytes:&p length:sizeof(p) atIndex:0];
            m->Dispatch(enc, m->recover, to.width, to.height, 1);
        }
        [enc endEncoding];
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) { (void)keep.count; }];
        return true;
    }
}

bool Metal::EncodeFlowScale(void* cmd, CVPixelBufferRef in, CVPixelBufferRef out, float scale, std::string& err)
{
    std::lock_guard<std::mutex> lk(m->lock);
    @autoreleasepool {
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmd;
        NSMutableArray* keep = [NSMutableArray array];
        id<MTLTexture> ti = m->Plane(in, 0, keep, err), to = m->Plane(out, 0, keep, err);
        if (!ti || !to) return false;
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setTexture:ti atIndex:0]; [enc setTexture:to atIndex:1]; [enc setBytes:&scale length:sizeof(scale) atIndex:0];
        m->Dispatch(enc, m->flowScale, to.width, to.height, 1);
        [enc endEncoding];
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) { (void)keep.count; }];
        return true;
    }
}

bool Metal::RunArtCnnFloat(Net& n, const float* in, int w, int h, float* out, std::string& err)
{
    std::lock_guard<std::mutex> lk(m->lock);
    @autoreleasepool {
        MTLTextureDescriptor* di = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:NSUInteger(w) height:NSUInteger(h) mipmapped:NO];
        di.usage = MTLTextureUsageShaderRead; di.storageMode = MTLStorageModeShared;
        MTLTextureDescriptor* dout = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:NSUInteger(2 * w) height:NSUInteger(2 * h) mipmapped:NO];
        dout.usage = MTLTextureUsageShaderWrite; dout.storageMode = MTLStorageModeShared;
        id<MTLTexture> ti = [m->dev newTextureWithDescriptor:di], to = [m->dev newTextureWithDescriptor:dout];
        if (!ti || !to) { err = "Metal: test textures"; return false; }
        [ti replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(w), NSUInteger(h)) mipmapLevel:0 withBytes:in bytesPerRow:NSUInteger(w) * 4];
        id<MTLCommandBuffer> cb = [m->queue commandBuffer];
        if (!EncodeNet(*m, n, cb, ti, 1.0f, to, false, w, h, err)) return false;
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status == MTLCommandBufferStatusError) { err = "Metal: " + ErrText(cb.error); return false; }
        [to getBytes:out bytesPerRow:NSUInteger(2 * w) * 4 fromRegion:MTLRegionMake2D(0, 0, NSUInteger(2 * w), NSUInteger(2 * h)) mipmapLevel:0];
        return true;
    }
}

std::string FindModel(const std::string& name)
{
    std::vector<std::string> dirs;
    if (const char* e = std::getenv("PUSHMPC_MODELS"); e && *e) dirs.push_back(e);
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&FindModel), &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        const size_t s = p.find_last_of('/');
        if (s != std::string::npos) { dirs.push_back(p.substr(0, s) + "/pushmpc-models"); dirs.push_back(p.substr(0, s) + "/../share/pushmpc/models"); }
    }
    if (const char* h = std::getenv("HOME"); h && *h) dirs.push_back(std::string(h) + "/.config/pushmpc/models");
    for (auto& d : dirs) {
        const std::string f = d + "/" + name + ".pmnn";
        if (FILE* fp = std::fopen(f.c_str(), "rb")) { std::fclose(fp); return f; }
    }
    return "";
}

} // namespace pmvs::apple
