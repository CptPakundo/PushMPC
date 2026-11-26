// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC portable GPU engine: the held-drawing repair's composite (pushmpc/src/PushCoreRegion.h CS_RepairY / UV in
// GLSL, on planar YUV: no RGB round trip). Each part (up to 3) is the generated picture of its pair at its phase, made
// at the source size (interp.comp, target 1: R16, code / maxV); one RGBA8 mask of the cell grid holds the parts' masks
// (r, g, b), bilinear, blurred over 5 x 5 taps of 0.75 x feather cells and through smoothstep(lo, hi): the feather.
// The masks are disjoint (HeldFrames.h AssignOwners), so the order of the parts does not matter. Luma gets detail
// matching (research P6): the part's own high-pass scaled to the slot's local energy (gain 0.8-1.6).
// Included by repair8.comp / repair16.comp with PLANE_FMT r8 / r16 (the slot's plane format).
layout(local_size_x = 16, local_size_y = 8) in;
layout(binding = 0, PLANE_FMT) uniform image2D slotY;
layout(binding = 1, PLANE_FMT) uniform image2D slotU;
layout(binding = 2, PLANE_FMT) uniform image2D slotV;
layout(binding = 3) uniform sampler2D p0Y;   // linear
layout(binding = 4) uniform sampler2D p0U;
layout(binding = 5) uniform sampler2D p0V;
layout(binding = 6) uniform sampler2D p1Y;
layout(binding = 7) uniform sampler2D p1U;
layout(binding = 8) uniform sampler2D p1V;
layout(binding = 9) uniform sampler2D p2Y;
layout(binding = 10) uniform sampler2D p2U;
layout(binding = 11) uniform sampler2D p2V;
layout(binding = 12) uniform sampler2D mask;   // RGBA8 UNORM, linear
layout(push_constant) uniform PC {
    int W, H, CW, CH, parts, plane, detail, pad;
    float msx, msy, lo, hi, kRaw, maxV;
    float s0x, s0y, s1x, s1y, s2x, s2y;
} pc;

vec3 maskAt(vec2 uv)
{
    vec3 m = vec3(0.0); float ws = 0.0;
    for (int j = -2; j <= 2; ++j) for (int i = -2; i <= 2; ++i) {
        float w = float((3 - abs(i)) * (3 - abs(j)));
        m += w * textureLod(mask, uv + vec2(i, j) * 0.75 * vec2(pc.msx, pc.msy), 0.0).rgb; ws += w;
    }
    m /= ws;
    return vec3(smoothstep(pc.lo, pc.hi, m.r), smoothstep(pc.lo, pc.hi, m.g), smoothstep(pc.lo, pc.hi, m.b));
}
vec2 shiftOf(int i) { return i == 0 ? vec2(pc.s0x, pc.s0y) : i == 1 ? vec2(pc.s1x, pc.s1y) : vec2(pc.s2x, pc.s2y); }
// a part's plane at p (pixels of that plane, centres at +0.5), in codes / maxV
float partAt(int i, int pl, vec2 p)
{
    vec2 sz = pl == 0 ? vec2(pc.W, pc.H) : vec2(pc.CW, pc.CH);
    vec2 uv = p / sz;
    // (opaque types cannot be selected with ?:)
    if (i == 0) { if (pl == 0) return textureLod(p0Y, uv, 0.0).r; if (pl == 1) return textureLod(p0U, uv, 0.0).r; return textureLod(p0V, uv, 0.0).r; }
    if (i == 1) { if (pl == 0) return textureLod(p1Y, uv, 0.0).r; if (pl == 1) return textureLod(p1U, uv, 0.0).r; return textureLod(p1V, uv, 0.0).r; }
    if (pl == 0) return textureLod(p2Y, uv, 0.0).r;
    if (pl == 1) return textureLod(p2U, uv, 0.0).r;
    return textureLod(p2V, uv, 0.0).r;
}
float slotYAt(ivec2 q) { return imageLoad(slotY, clamp(q, ivec2(0), ivec2(pc.W - 1, pc.H - 1))).r * pc.kRaw / pc.maxV; }
float energyPart(int i, vec2 p)
{
    float e = 0.0;
    for (int j = -2; j <= 2; j += 2) for (int k = -2; k <= 2; k += 2) {
        vec2 q = p + vec2(k, j);
        e += abs(partAt(i, 0, q) - 0.25 * (partAt(i, 0, q + vec2(1, 0)) + partAt(i, 0, q - vec2(1, 0)) + partAt(i, 0, q + vec2(0, 1)) + partAt(i, 0, q - vec2(0, 1))));
    }
    return e / 9.0;
}
float energySlot(ivec2 p)
{
    float e = 0.0;
    for (int j = -2; j <= 2; j += 2) for (int k = -2; k <= 2; k += 2) {
        ivec2 q = p + ivec2(k, j);
        e += abs(slotYAt(q) - 0.25 * (slotYAt(q + ivec2(1, 0)) + slotYAt(q - ivec2(1, 0)) + slotYAt(q + ivec2(0, 1)) + slotYAt(q - ivec2(0, 1))));
    }
    return e / 9.0;
}
void main()
{
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (pc.plane == 0) {
        if (id.x >= pc.W || id.y >= pc.H) return;
        vec3 m = maskAt((vec2(id) + 0.5) / vec2(pc.W, pc.H));
        if (max(max(m.r, m.g), m.b) <= 0.001) return;
        float c = imageLoad(slotY, id).r * pc.kRaw / pc.maxV;
        float detailRef = pc.detail != 0 ? energySlot(id) : 0.0;
        for (int i = 0; i < pc.parts; ++i) {
            float mi = i == 0 ? m.r : i == 1 ? m.g : m.b;
            if (mi <= 0.001) continue;
            vec2 p = vec2(id) + 0.5 - shiftOf(i);
            float r = partAt(i, 0, p);
            if (pc.detail != 0) {
                float hf = r - 0.25 * (partAt(i, 0, p + vec2(1, 0)) + partAt(i, 0, p - vec2(1, 0)) + partAt(i, 0, p + vec2(0, 1)) + partAt(i, 0, p - vec2(0, 1)));
                float g = clamp(sqrt((detailRef + 1e-4) / (energyPart(i, p) + 1e-4)), 0.8, 1.6);
                r += (g - 1.0) * hf;
            }
            c = mix(c, r, mi);
        }
        imageStore(slotY, id, vec4(clamp(floor(c * pc.maxV + 0.5), 0.0, pc.maxV) / pc.kRaw));
        return;
    }
    if (id.x >= pc.CW || id.y >= pc.CH) return;
    vec3 m = maskAt((vec2(id * 2) + 1.0) / vec2(pc.W, pc.H));
    if (max(max(m.r, m.g), m.b) <= 0.001) return;
    float u = imageLoad(slotU, id).r * pc.kRaw / pc.maxV, v = imageLoad(slotV, id).r * pc.kRaw / pc.maxV;
    for (int i = 0; i < pc.parts; ++i) {
        float mi = i == 0 ? m.r : i == 1 ? m.g : m.b;
        if (mi <= 0.001) continue;
        vec2 p = vec2(id) + 0.5 - shiftOf(i) * 0.5;   // the camera residual in chroma pixels
        u = mix(u, partAt(i, 1, p), mi);
        v = mix(v, partAt(i, 2, p), mi);
    }
    imageStore(slotU, id, vec4(clamp(floor(u * pc.maxV + 0.5), 0.0, pc.maxV) / pc.kRaw));
    imageStore(slotV, id, vec4(clamp(floor(v * pc.maxV + 0.5), 0.0, pc.maxV) / pc.kRaw));
}
