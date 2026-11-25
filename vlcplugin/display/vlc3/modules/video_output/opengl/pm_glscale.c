// SPDX-License-Identifier: GPL-3.0-or-later
// pm_glscale: FSR 1 (EASU + RCAS) for PushMPC's OpenGL output fork; see pm_glscale.h.
// The two shaders follow AMD's FidelityFX Super Resolution 1.0 reference (ffx_fsr1.h, MIT licence, (c) Advanced Micro
// Devices): the same 12-tap EASU kernel (direction and anisotropy from the 4 nearest bilinear quads, a Lanczos-2-like
// window stretched along the edge, ringing clamped to the 2x2 neighbourhood) and the same RCAS lobe limit, written out
// for fragment shaders without gather, integer ops or half floats so that GLSL 1.20 and ES 1.00 compile them.
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pm_glscale.h"

#ifndef GL_FRAMEBUFFER
# define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
# define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
# define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_FRAMEBUFFER_BINDING
# define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_CURRENT_PROGRAM
# define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_RGBA16
# define GL_RGBA16 0x805B
#endif
#ifndef APIENTRY
# define APIENTRY
#endif

typedef void (APIENTRY *pm_GenFramebuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *pm_DeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *pm_BindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *pm_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *pm_CheckFramebufferStatus)(GLenum);

struct pm_glscale {
    vlc_object_t *obj;
    const opengl_vtable_t *vt;
    pm_GenFramebuffers GenFramebuffers;
    pm_DeleteFramebuffers DeleteFramebuffers;
    pm_BindFramebuffer BindFramebuffer;
    pm_FramebufferTexture2D FramebufferTexture2D;
    pm_CheckFramebufferStatus CheckFramebufferStatus;
    bool desktop;
    GLuint easu, rcas, vbo;
    GLint easu_pos, easu_src, easu_in, easu_out;
    GLint rcas_pos, rcas_src, rcas_size, rcas_origin, rcas_sharp;
    float sharp;                 /* RCAS con.x = exp2(-stops) */
    GLuint fbo[2], tex[2];       /* 0: the source-size picture, 1: EASU's output at the placement size */
    unsigned w[2], h[2];
    bool failed;                 /* textures/FBO not usable: stay with VLC's path */
    /* the frame being drawn */
    GLint prev_fbo, vp[4];
    unsigned src_w, src_h;
    bool logged;
};

static const char vs_src[] =
    "attribute vec2 pm_pos;\n"
    "void main() { gl_Position = vec4(pm_pos, 0.0, 1.0); }\n";

static const char easu_src[] =
    "uniform sampler2D pm_src;\n"
    "uniform vec2 pm_in;\n"
    "uniform vec2 pm_out;\n"
    "vec3 T(vec2 fp, float x, float y) { return texture2D(pm_src, (fp + vec2(x, y) + 0.5) / pm_in).rgb; }\n"
    "float L(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
    /* one bilinear quad's contribution to the edge direction and length; A above, B left, C centre, D right, E below */
    "void S(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
    "    float dc = lD - lC, cb = lC - lB;\n"
    "    float lenX = max(abs(dc), abs(cb)); lenX = lenX > 0.0 ? 1.0 / lenX : 0.0;\n"
    "    float dirX = lD - lB;\n"
    "    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0); lenX *= lenX;\n"
    "    float ec = lE - lC, ca = lC - lA;\n"
    "    float lenY = max(abs(ec), abs(ca)); lenY = lenY > 0.0 ? 1.0 / lenY : 0.0;\n"
    "    float dirY = lE - lA;\n"
    "    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0); lenY *= lenY;\n"
    "    dir += vec2(dirX, dirY) * w;\n"
    "    len += (lenX + lenY) * w;\n"
    "}\n"
    "void A(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
    "    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;\n"
    "    float d2 = min(v.x * v.x + v.y * v.y, clp);\n"
    "    float wB = 0.4 * d2 - 1.0, wA = lob * d2 - 1.0;\n"
    "    wB *= wB; wA *= wA;\n"
    "    wB = 1.5625 * wB - 0.5625;\n"
    "    float w = wB * wA;\n"
    "    aC += c * w; aW += w;\n"
    "}\n"
    "void main() {\n"
    "    vec2 pp = gl_FragCoord.xy * (pm_in / pm_out) - 0.5;\n"
    "    vec2 fp = floor(pp); pp -= fp;\n"
    /*      b c
     *    e f g h
     *    i j k l
     *      n o      (f = fp) */
    "    vec3 b = T(fp, 0.0, -1.0), c = T(fp, 1.0, -1.0);\n"
    "    vec3 e = T(fp, -1.0, 0.0), f = T(fp, 0.0, 0.0), g = T(fp, 1.0, 0.0), h = T(fp, 2.0, 0.0);\n"
    "    vec3 i = T(fp, -1.0, 1.0), j = T(fp, 0.0, 1.0), k = T(fp, 1.0, 1.0), l = T(fp, 2.0, 1.0);\n"
    "    vec3 n = T(fp, 0.0, 2.0), o = T(fp, 1.0, 2.0);\n"
    "    float bL = L(b), cL = L(c), eL = L(e), fL = L(f), gL = L(g), hL = L(h);\n"
    "    float iL = L(i), jL = L(j), kL = L(k), lL = L(l), nL = L(n), oL = L(o);\n"
    "    vec2 dir = vec2(0.0); float len = 0.0;\n"
    "    S(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
    "    S(dir, len, pp.x * (1.0 - pp.y),         cL, fL, gL, hL, kL);\n"
    "    S(dir, len, (1.0 - pp.x) * pp.y,         fL, iL, jL, kL, nL);\n"
    "    S(dir, len, pp.x * pp.y,                 gL, jL, kL, lL, oL);\n"
    "    vec2 dir2 = dir * dir;\n"
    "    float dirR = dir2.x + dir2.y;\n"
    "    bool zro = dirR < (1.0 / 32768.0);\n"
    "    dirR = zro ? 1.0 : inversesqrt(dirR);\n"
    "    dir.x = zro ? 1.0 : dir.x;\n"
    "    dir *= dirR;\n"
    "    len = len * 0.5; len *= len;\n"
    "    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));\n"
    "    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
    "    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;\n"
    "    float clp = 1.0 / lob;\n"
    "    vec3 aC = vec3(0.0); float aW = 0.0;\n"
    "    A(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);\n"
    "    A(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);\n"
    "    A(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);\n"
    "    A(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);\n"
    "    A(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);\n"
    "    A(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);\n"
    "    A(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);\n"
    "    A(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);\n"
    "    A(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);\n"
    "    A(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);\n"
    "    A(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);\n"
    "    A(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);\n"
    "    vec3 mn = min(min(f, g), min(j, k)), mx = max(max(f, g), max(j, k));\n"
    "    gl_FragColor = vec4(clamp(aC / aW, mn, mx), 1.0);\n"
    "}\n";

static const char rcas_src[] =
    "uniform sampler2D pm_src;\n"
    "uniform vec2 pm_size;\n"
    "uniform vec2 pm_origin;\n"
    "uniform float pm_sharp;\n"
    "vec3 T(vec2 p, float x, float y) { return texture2D(pm_src, (p + vec2(x, y)) / pm_size).rgb; }\n"
    "void main() {\n"
    "    vec2 p = floor(gl_FragCoord.xy - pm_origin) + 0.5;\n"
    /*      b
     *    d e f
     *      h */
    "    vec3 b = T(p, 0.0, -1.0), d = T(p, -1.0, 0.0), e = T(p, 0.0, 0.0), f = T(p, 1.0, 0.0), h = T(p, 0.0, 1.0);\n"
    "    vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
    "    vec3 hitMin = mn4 / (4.0 * mx4 + 1.0e-5);\n"
    "    vec3 hitMax = (1.0 - mx4) / (4.0 * mn4 - 4.0 - 1.0e-5);\n"
    "    vec3 lobeRGB = max(-hitMin, hitMax);\n"
    "    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pm_sharp;\n"
    "    vec3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);\n"
    "    gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
    "}\n";

static GLuint Compile(pm_glscale *s, GLenum type, const char *head, const char *body)
{
    const opengl_vtable_t *vt = s->vt;
    GLuint sh = vt->CreateShader(type);
    const char *src[2] = { head, body };
    vt->ShaderSource(sh, 2, src, NULL);
    vt->CompileShader(sh);
    GLint ok = 0;
    vt->GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        vt->GetShaderInfoLog(sh, sizeof(log), NULL, log);
        msg_Warn(s->obj, "PushMPC FSR: shader compile failed: %s", log);
        vt->DeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint Program(pm_glscale *s, const char *head, const char *fs)
{
    const opengl_vtable_t *vt = s->vt;
    GLuint v = Compile(s, GL_VERTEX_SHADER, head, vs_src), f = Compile(s, GL_FRAGMENT_SHADER, head, fs);
    if (!v || !f) {
        if (v) vt->DeleteShader(v);
        if (f) vt->DeleteShader(f);
        return 0;
    }
    GLuint p = vt->CreateProgram();
    vt->AttachShader(p, v);
    vt->AttachShader(p, f);
    vt->LinkProgram(p);
    vt->DeleteShader(v);
    vt->DeleteShader(f);
    GLint ok = 0;
    vt->GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        vt->GetProgramInfoLog(p, sizeof(log), NULL, log);
        msg_Warn(s->obj, "PushMPC FSR: program link failed: %s", log);
        vt->DeleteProgram(p);
        return 0;
    }
    return p;
}

pm_glscale *PM_GlScaleCreate(vlc_object_t *obj, vlc_gl_t *gl, const opengl_vtable_t *vt, unsigned glsl_version,
                             const char *precision_header, float sharpness)
{
    pm_glscale *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->obj = obj;
    s->vt = vt;
    s->GenFramebuffers = (pm_GenFramebuffers)vlc_gl_GetProcAddress(gl, "glGenFramebuffers");
    s->DeleteFramebuffers = (pm_DeleteFramebuffers)vlc_gl_GetProcAddress(gl, "glDeleteFramebuffers");
    s->BindFramebuffer = (pm_BindFramebuffer)vlc_gl_GetProcAddress(gl, "glBindFramebuffer");
    s->FramebufferTexture2D = (pm_FramebufferTexture2D)vlc_gl_GetProcAddress(gl, "glFramebufferTexture2D");
    s->CheckFramebufferStatus = (pm_CheckFramebufferStatus)vlc_gl_GetProcAddress(gl, "glCheckFramebufferStatus");
    if (!s->GenFramebuffers || !s->DeleteFramebuffers || !s->BindFramebuffer || !s->FramebufferTexture2D ||
        !s->CheckFramebufferStatus) {
        msg_Warn(obj, "PushMPC FSR: no framebuffer objects in this OpenGL: VLC's bilinear scaling");
        free(s);
        return NULL;
    }
    s->desktop = glsl_version >= 110 && glsl_version != 100 && glsl_version != 300;
    char head[256];
    snprintf(head, sizeof(head), "#version %u\n%s\n", glsl_version, precision_header ? precision_header : "");
    s->easu = Program(s, head, easu_src);
    s->rcas = s->easu ? Program(s, head, rcas_src) : 0;
    if (!s->easu || !s->rcas) {
        PM_GlScaleDestroy(s);
        return NULL;
    }
    s->easu_pos = vt->GetAttribLocation(s->easu, "pm_pos");
    s->easu_src = vt->GetUniformLocation(s->easu, "pm_src");
    s->easu_in = vt->GetUniformLocation(s->easu, "pm_in");
    s->easu_out = vt->GetUniformLocation(s->easu, "pm_out");
    s->rcas_pos = vt->GetAttribLocation(s->rcas, "pm_pos");
    s->rcas_src = vt->GetUniformLocation(s->rcas, "pm_src");
    s->rcas_size = vt->GetUniformLocation(s->rcas, "pm_size");
    s->rcas_origin = vt->GetUniformLocation(s->rcas, "pm_origin");
    s->rcas_sharp = vt->GetUniformLocation(s->rcas, "pm_sharp");
    /* one triangle that covers the viewport */
    static const GLfloat tri[] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };
    vt->GenBuffers(1, &s->vbo);
    vt->BindBuffer(GL_ARRAY_BUFFER, s->vbo);
    vt->BufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    vt->BindBuffer(GL_ARRAY_BUFFER, 0);
    /* sharpness 0..1 -> RCAS stops 2..0 (AMD: 0 = maximum) */
    if (sharpness < 0.f) sharpness = 0.f;
    if (sharpness > 1.f) sharpness = 1.f;
    s->sharp = exp2f(-2.f * (1.f - sharpness));
    msg_Dbg(obj, "PushMPC FSR upscaler ready (GLSL %u, sharpness %.2f)", glsl_version, sharpness);
    return s;
}

void PM_GlScaleDestroy(pm_glscale *s)
{
    if (!s)
        return;
    const opengl_vtable_t *vt = s->vt;
    for (int t = 0; t < 2; t++) {
        if (s->fbo[t]) s->DeleteFramebuffers(1, &s->fbo[t]);
        if (s->tex[t]) vt->DeleteTextures(1, &s->tex[t]);
    }
    if (s->vbo) vt->DeleteBuffers(1, &s->vbo);
    if (s->easu) vt->DeleteProgram(s->easu);
    if (s->rcas) vt->DeleteProgram(s->rcas);
    free(s);
}

static bool Target(pm_glscale *s, int t, unsigned w, unsigned h)
{
    if (s->tex[t] && s->w[t] == w && s->h[t] == h)
        return true;
    const opengl_vtable_t *vt = s->vt;
    if (!s->tex[t]) {
        vt->GenTextures(1, &s->tex[t]);
        s->GenFramebuffers(1, &s->fbo[t]);
    }
    vt->ActiveTexture(GL_TEXTURE0);
    vt->BindTexture(GL_TEXTURE_2D, s->tex[t]);
    vt->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    vt->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    vt->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    vt->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    s->BindFramebuffer(GL_FRAMEBUFFER, s->fbo[t]);
    GLenum st = 0;
    if (s->desktop) {   /* 16 bits per channel keeps 10-bit sources intact between the passes */
        vt->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, w, h, 0, GL_RGBA, GL_UNSIGNED_SHORT, NULL);
        s->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex[t], 0);
        st = s->CheckFramebufferStatus(GL_FRAMEBUFFER);
    }
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        vt->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        s->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex[t], 0);
        st = s->CheckFramebufferStatus(GL_FRAMEBUFFER);
    }
    s->BindFramebuffer(GL_FRAMEBUFFER, (GLuint)s->prev_fbo);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        msg_Warn(s->obj, "PushMPC FSR: framebuffer incomplete (0x%x): VLC's bilinear scaling", st);
        s->failed = true;
        return false;
    }
    s->w[t] = w;
    s->h[t] = h;
    return true;
}

bool PM_GlScaleBegin(pm_glscale *s, unsigned src_w, unsigned src_h)
{
    if (!s || s->failed || src_w == 0 || src_h == 0)
        return false;
    const opengl_vtable_t *vt = s->vt;
    vt->GetIntegerv(GL_VIEWPORT, s->vp);
    /* upscaling only (downscaling and 1:1 keep VLC's path) */
    if (s->vp[2] <= 0 || s->vp[3] <= 0 || ((unsigned)s->vp[2] <= src_w && (unsigned)s->vp[3] <= src_h))
        return false;
    vt->GetIntegerv(GL_FRAMEBUFFER_BINDING, &s->prev_fbo);
    if (!Target(s, 0, src_w, src_h) || !Target(s, 1, (unsigned)s->vp[2], (unsigned)s->vp[3]))
        return false;
    if (!s->logged) {
        msg_Dbg(s->obj, "PushMPC FSR: %ux%u -> %dx%d", src_w, src_h, s->vp[2], s->vp[3]);
        s->logged = true;
    }
    s->src_w = src_w;
    s->src_h = src_h;
    s->BindFramebuffer(GL_FRAMEBUFFER, s->fbo[0]);
    vt->Viewport(0, 0, src_w, src_h);
    return true;
}

static void Pass(pm_glscale *s, GLuint prog, GLint pos, GLuint src_tex)
{
    const opengl_vtable_t *vt = s->vt;
    vt->ActiveTexture(GL_TEXTURE0);
    vt->BindTexture(GL_TEXTURE_2D, src_tex);
    vt->BindBuffer(GL_ARRAY_BUFFER, s->vbo);
    vt->EnableVertexAttribArray(pos);
    vt->VertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
    vt->DrawArrays(GL_TRIANGLES, 0, 3);
    (void)prog;
}

void PM_GlScaleEnd(pm_glscale *s)
{
    const opengl_vtable_t *vt = s->vt;
    GLint prog = 0;
    vt->GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    /* EASU: source-size picture -> placement-size texture */
    s->BindFramebuffer(GL_FRAMEBUFFER, s->fbo[1]);
    vt->Viewport(0, 0, s->vp[2], s->vp[3]);
    vt->UseProgram(s->easu);
    vt->Uniform1i(s->easu_src, 0);
    vt->Uniform2f(s->easu_in, (float)s->src_w, (float)s->src_h);
    vt->Uniform2f(s->easu_out, (float)s->vp[2], (float)s->vp[3]);
    Pass(s, s->easu, s->easu_pos, s->tex[0]);
    /* RCAS: into the window, at VLC's placement */
    s->BindFramebuffer(GL_FRAMEBUFFER, (GLuint)s->prev_fbo);
    vt->Viewport(s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    vt->UseProgram(s->rcas);
    vt->Uniform1i(s->rcas_src, 0);
    vt->Uniform2f(s->rcas_size, (float)s->vp[2], (float)s->vp[3]);
    vt->Uniform2f(s->rcas_origin, (float)s->vp[0], (float)s->vp[1]);
    vt->Uniform1f(s->rcas_sharp, s->sharp);
    Pass(s, s->rcas, s->rcas_pos, s->tex[1]);
    vt->BindBuffer(GL_ARRAY_BUFFER, 0);
    vt->UseProgram((GLuint)prog);
}
