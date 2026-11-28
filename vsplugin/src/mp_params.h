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

} // namespace pmvs
