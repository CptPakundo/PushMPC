// mpv's frame-0 colour from the dummy frame its vapoursynth filter hands out while the script loads (no VapourSynth,
// no mpv headers: unit-tested in sched_test).
//
// mpv 0.39 to 0.42 (video/filter/vf_vapoursynth.c): the script loads when frame 0 reaches the filter, after
// p->fmt_in = mpi->params; a frame requested during the load is a black dummy made with
// mp_image_set_params(&dummy, &p->fmt_in), whose props carry _MP_IMAGE, a byte copy of the whole struct mp_image
// (sizeof(*img)), plus _SARNum/_SARDen (params.p_w/p_h), _ColorSpace (pl_system_to_av(repr.sys)) and _ColorRange
// (repr.levels == LIMITED, only when levels is set). mpv 0.38 and older set no _MP_IMAGE.
//
// struct mp_image { int w, h; struct mp_image_params params; ... } and the params prefix, the same at the tags
// v0.39.0, v0.40.0, v0.41.0 and v0.42.0 (video/mp_image.h, read at each tag):
//   enum mp_imgfmt imgfmt; const char *imgfmt_name; enum mp_imgfmt hw_subfmt; int w, h; int p_w, p_h;
//   bool force_window; struct pl_color_space color; struct pl_color_repr repr; ...
// libplacebo (src/include/libplacebo/colorspace.h, the same from v6.338.2, mpv 0.39's minimum, to master):
//   pl_color_space { enum primaries; enum transfer; struct pl_hdr_metadata hdr; }, hdr = 148 bytes (8 + 4 + 4 + 3 + 1
//   floats, pl_hdr_bezier { 18 floats; uint8_t } padded to 76, 2 floats); pl_color_repr { enum sys; enum levels;
//   enum alpha; struct pl_bit_encoding { 3 ints }; const struct pl_dovi_metadata *dovi; } (pointer-aligned).
// So from params.w (the anchor): primaries at +20, transfer at +24 on every ABI (force_window is padded to 4), and
// repr.sys at round_up(anchor + 176, sizeof(void*)) from the blob's start (the blob starts at struct mp_image, which
// is pointer-aligned), repr.levels 4 bytes later.
// The enum values used here are the same in every libplacebo from v6.338.2 on (new ones were added at the end;
// pl_color_system gained YCGCO_RE / YCGCO_RO before RGB in v7.360, handled below).
//
// No fixed offset is trusted: the anchor is found by value ([w, h, p_w, p_h] = the frame's size and _SARNum /
// _SARDen, unique in the blob), and the result is accepted only when the enums are in range and repr.sys /
// repr.levels at the computed place agree with the same frame's _ColorSpace / _ColorRange.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pmvs {

struct MpFrameColour {
    bool ok = false;
    std::string why;                  // the reason when not ok
    int plPrimaries = -1, plTransfer = -1, plSystem = -1, plLevels = -1;
    // H.273 / FFmpeg codes for the plugin's arguments; -1 = not known (the plugin then keeps its fallback)
    int matrix = -1, transfer = -1, primaries = -1;
    int range = -1;                   // the arguments' convention: 0 full, 1 limited
    size_t anchor = 0;
};

// libplacebo -> H.273 (libplacebo's own pl_primaries_to_av / pl_transfer_to_av, src/include/libplacebo/utils/
// libav_internal.h); an SDR curve H.273 has no code for maps to BT.1886 (1), so a trusted SDR frame never falls back
// to the format's guess (PQ at UHD)
inline int MpPrimariesToH273(int p)
{
    static const int t[] = { -1, 6, 5, 1, 4, 22, 9, -1, -1, -1, 10, 11, 12, -1, -1, 8, -1, -1 };
    return p >= 0 && p < int(sizeof t / sizeof t[0]) ? t[p] : -1;
}
inline int MpTransferToH273(int c)
{
    // UNKNOWN, BT_1886, SRGB, LINEAR, GAMMA18, GAMMA20, GAMMA22, GAMMA24, GAMMA26, GAMMA28, PRO_PHOTO, ST428, PQ, HLG,
    // V_LOG, S_LOG1, S_LOG2, SCRGB
    static const int t[] = { -1, 1, 13, 8, 1, 1, 4, 1, 1, 5, 1, 17, 16, 18, 1, 1, 1, 1 };
    return c >= 0 && c < int(sizeof t / sizeof t[0]) ? t[c] : -1;
}
inline constexpr int kPlPrimCount = 18, kPlTrcCountMax = 18, kPlSystemCountMax = 14, kPlLevelsCount = 3;

// whether libplacebo's system value s gives the AVColorSpace mpv put in _ColorSpace (pl_system_to_av), under either
// numbering (before v7.360: ... YCGCO 9, RGB 10, XYZ 11; from v7.360: YCGCO 9, YCGCO_RE 10, YCGCO_RO 11, RGB 12, XYZ 13)
inline bool MpSystemMatchesAv(int s, int av)
{
    switch (s) {
    case 0: return av == 2;
    case 1: return av == 6;
    case 2: return av == 1;
    case 3: return av == 7;
    case 4: return av == 9;
    case 5: return av == 10;
    case 6: case 7: return av == 14;
    case 8: return av == 2;
    case 9: return av == 8;
    case 10: return av == 0 || av == 16 || av == 2;   // RGB (old) / YCGCO_RE (new)
    case 11: return av == 2 || av == 17;               // XYZ (old) / YCGCO_RO (new)
    case 12: return av == 0;                           // RGB (new)
    case 13: return av == 2;                           // XYZ (new)
    default: return false;
    }
}

inline int32_t MpReadI32(const uint8_t* b, size_t off) { int32_t v; std::memcpy(&v, b + off, 4); return v; }

// blob: _MP_IMAGE's bytes; w, h: the frame's size; sarNum / sarDen: _SARNum / _SARDen; colorSpace: _ColorSpace (-1
// absent); colorRange: _ColorRange (-1 absent: levels unknown); ptrSize: sizeof(void*) of this process (mpv's)
inline MpFrameColour ParseMpImage(const uint8_t* blob, size_t n, int w, int h, int sarNum, int sarDen, int colorSpace, int colorRange, size_t ptrSize = sizeof(void*))
{
    MpFrameColour r;
    if (!blob || n < 64) { r.why = "no _MP_IMAGE (mpv older than 0.39?)"; return r; }
    if (colorSpace < 0) { r.why = "no _ColorSpace on the frame"; return r; }
    size_t found = 0, hits = 0;
    for (size_t o = 8; o + 16 <= n; o += 4)
        if (MpReadI32(blob, o) == w && MpReadI32(blob, o + 4) == h && MpReadI32(blob, o + 8) == sarNum && MpReadI32(blob, o + 12) == sarDen) { found = o; ++hits; }
    if (hits != 1) { r.why = hits ? "the size / aspect anchor is not unique in _MP_IMAGE" : "no size / aspect anchor in _MP_IMAGE"; return r; }
    const size_t sysOff = (found + 176 + ptrSize - 1) / ptrSize * ptrSize;
    if (found + 28 > n || sysOff + 8 > n) { r.why = "_MP_IMAGE is shorter than mpv 0.39's layout"; return r; }
    r.anchor = found;
    r.plPrimaries = MpReadI32(blob, found + 20);
    r.plTransfer = MpReadI32(blob, found + 24);
    r.plSystem = MpReadI32(blob, sysOff);
    r.plLevels = MpReadI32(blob, sysOff + 4);
    if (r.plPrimaries < 0 || r.plPrimaries >= kPlPrimCount || r.plTransfer < 0 || r.plTransfer >= kPlTrcCountMax) { r.why = "primaries / transfer out of libplacebo's range"; return r; }
    if (r.plSystem < 0 || r.plSystem >= kPlSystemCountMax || r.plLevels < 0 || r.plLevels >= kPlLevelsCount) { r.why = "colour system / levels out of libplacebo's range"; return r; }
    if (!MpSystemMatchesAv(r.plSystem, colorSpace)) { r.why = "the colour system in _MP_IMAGE disagrees with _ColorSpace"; return r; }
    const bool levelsAgree = colorRange < 0 ? r.plLevels == 0 : (r.plLevels != 0 && (colorRange == 1) == (r.plLevels == 1));
    if (!levelsAgree) { r.why = "the levels in _MP_IMAGE disagree with _ColorRange"; return r; }
    r.ok = true;
    r.primaries = MpPrimariesToH273(r.plPrimaries);
    r.transfer = MpTransferToH273(r.plTransfer);
    r.matrix = colorSpace != 2 ? colorSpace : -1;
    r.range = r.plLevels == 1 ? 1 : r.plLevels == 2 ? 0 : -1;
    return r;
}

// "0.42.0" / "mpv 0.39.0-dev" / "v0.40.1": major and minor; false when none
inline bool MpVersion(const std::string& s, int& major, int& minor)
{
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') continue;
        size_t j = i; int a = 0; while (j < s.size() && s[j] >= '0' && s[j] <= '9') a = a * 10 + (s[j++] - '0');
        if (j >= s.size() || s[j] != '.') { i = j; continue; }
        size_t k = j + 1; int b = 0; bool any = false; while (k < s.size() && s[k] >= '0' && s[k] <= '9') { b = b * 10 + (s[k++] - '0'); any = true; }
        if (!any) { i = j; continue; }
        major = a; minor = b;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ the output's colour
// mpv takes an output frame's colour only from its _MP_IMAGE (vf_vapoursynth.c vs_frame_done: img = *mpi, then
// mp_image_new_copy -> mp_image_copy_attributes copies params.color, repr, light, and from 0.42 primaries_orig /
// transfer_orig / sys_orig); the plugin's _Transfer / _Primaries / _Matrix are not read. The plugin copies the source
// frame's props, so a route that changes the colour (a tone map to SDR, RTX Video HDR) must patch the blob, as mpv's own
// vf_format does (set_params: transfer changed -> hdr metadata cleared, light AUTO; *_orig = the new values).
// Further fields used here, after repr.sys at S (mp_params.h layout above), from the 0.39-0.42 struct and libplacebo:
//   pl_color_repr: sys S, levels S + 4, alpha S + 8, bits S + 12 .. S + 24, const pl_dovi_metadata *dovi at
//   round_up(S + 24, ptr); its size R = round_up(24 + ptr, ptr) (32 on 64-bit, 28 on 32-bit);
//   then enum primaries_orig S + R, transfer_orig S + R + 4, sys_orig S + R + 8, enum mp_csp_light light S + R + 12
//   (MP_CSP_LIGHT_AUTO 0 .. SCENE_1_2 4, the same at every tag);
//   pl_hdr_metadata at anchor + 28 .. + 176, max_luma at + 28 + 36.
struct MpOutColour {
    // libplacebo values. plTransfer and plPrimaries -1 together: the transfer is the source's (the HDR metadata, light
    // and Dolby Vision stay), only the matrix (system) and the levels are written
    int plPrimaries = -1, plTransfer = -1, plSystem = -1, plLevels = -1;
    float maxLuma = 0;                                                     // cd/m2 for an HDR output; 0 = mpv's default
};
// H.273 / FFmpeg codes -> libplacebo (the plugin's outputs only use these); -1 = no mapping
inline int H273TransferToPl(int t) { switch (t) { case 1: return 1; case 13: return 2; case 8: return 3; case 4: return 6; case 5: return 9; case 16: return 12; case 18: return 13; default: return -1; } }
inline int H273PrimariesToPl(int p) { switch (p) { case 6: return 1; case 5: return 2; case 1: return 3; case 4: return 4; case 9: return 6; case 11: return 11; case 12: return 12; default: return -1; } }
inline int H273MatrixToPlSystem(int m) { switch (m) { case 5: case 6: return 1; case 1: return 2; case 7: return 3; case 9: return 4; case 10: return 5; default: return -1; } }   // not RGB: its value moved in v7.360
inline int PlSystemToAv(int s) { switch (s) { case 1: return 6; case 2: return 1; case 3: return 7; case 4: return 9; case 5: return 10; default: return 2; } }
// the matrix pushmpc.vpy converts the plugin's RGB output to YUV 4:4:4 with (its _mat rule; keep both in step)
inline int VpyRgbMatrix(int w, int h, int bits) { return bits > 8 && w >= 3200 ? 9 : (w < 1024 && h < 600 ? 6 : 1); }

// patches a copy of a validated blob (the anchor search and checks of ParseMpImage, with the size from the blob's own
// mp_image w / h); false and why when it does not validate or a field to write holds an out-of-range value
inline bool PatchMpImage(std::vector<uint8_t>& b, int sarNum, int sarDen, int colorSpace, int colorRange, const MpOutColour& o, std::string* why, size_t ptrSize = sizeof(void*))
{
    auto no = [&](const char* m) { if (why) *why = m; return false; };
    const bool keepTransfer = o.plPrimaries < 0 && o.plTransfer < 0;
    if ((!keepTransfer && (o.plPrimaries < 0 || o.plTransfer < 0)) || o.plSystem < 0 || o.plLevels < 1 || o.plLevels > 2) return no("no libplacebo value for the output colour");
    if (b.size() < 64) return no("no _MP_IMAGE");
    const int w = MpReadI32(b.data(), 0), h = MpReadI32(b.data(), 4);
    const MpFrameColour src = ParseMpImage(b.data(), b.size(), w, h, sarNum, sarDen, colorSpace, colorRange, ptrSize);
    if (!src.ok) { if (why) *why = src.why; return false; }
    const size_t a = src.anchor, s = (a + 176 + ptrSize - 1) / ptrSize * ptrSize;
    const size_t dovi = (s + 24 + ptrSize - 1) / ptrSize * ptrSize, r = (24 + ptrSize + ptrSize - 1) / ptrSize * ptrSize;
    const size_t orig = s + r, light = orig + 12;
    if (light + 4 > b.size()) return no("_MP_IMAGE is shorter than mpv 0.39's layout");
    const int po = MpReadI32(b.data(), orig), to = MpReadI32(b.data(), orig + 4), so = MpReadI32(b.data(), orig + 8), li = MpReadI32(b.data(), light);
    if (po < 0 || po >= kPlPrimCount || to < 0 || to >= kPlTrcCountMax || so < 0 || so >= kPlSystemCountMax || li < 0 || li > 4) return no("the *_orig / light fields are out of range");
    auto put = [&](size_t off, int32_t v) { std::memcpy(b.data() + off, &v, 4); };
    if (keepTransfer) {   // the matrix and the levels only (sys_orig with them, as vf_format does)
        if (src.plSystem == 8) return true;   // Dolby Vision: mpv reshapes after the filter from repr.dovi; left as it is
        put(s, o.plSystem); put(s + 4, o.plLevels); put(orig + 8, o.plSystem);
        return true;
    }
    put(a + 20, o.plPrimaries);
    put(a + 24, o.plTransfer);
    std::memset(b.data() + a + 28, 0, 148);                                // the source's HDR metadata does not describe the output
    if (o.maxLuma > 0) std::memcpy(b.data() + a + 28 + 36, &o.maxLuma, 4);
    put(s, o.plSystem);
    put(s + 4, o.plLevels);
    std::memset(b.data() + dovi, 0, ptrSize);                              // no Dolby Vision reshaping of the output
    put(orig, o.plPrimaries); put(orig + 4, o.plTransfer); put(orig + 8, o.plSystem);
    put(light, 0);                                                         // MP_CSP_LIGHT_AUTO: mpv infers it from the transfer
    return true;
}

} // namespace pmvs
