// The output frame's _MP_IMAGE told the colour the plugin outputs (mp_params.h PatchMpImage), for routes that change
// it: mpv reads an output frame's colour only from that blob, which the plugin copies from the source frame.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include "VapourSynth4.h"
#include "mp_params.h"

namespace pmvs {

// p: the output frame's props right after the source frame's were copied (before the backend writes its own _ColorRange
// / _Range: the blob is validated against the source's). H.273 codes; range: 0 full, 1 limited. false + why when the
// blob is absent (not mpv) or does not validate; the frame then keeps the source's blob, as before.
inline bool TagMpOutput(const VSAPI* a, VSMap* p, int transfer, int primaries, int matrix, int range, float maxLuma, std::string* why)
{
    int e = 0;
    const char* blob = a->mapGetData(p, "_MP_IMAGE", 0, &e);
    if (e || !blob) { if (why) *why = "no _MP_IMAGE (not mpv)"; return false; }
    const int n = a->mapGetDataSize(p, "_MP_IMAGE", 0, &e);
    if (e || n <= 0) { if (why) *why = "no _MP_IMAGE (not mpv)"; return false; }
    auto prop = [&](const char* k, int64_t def) { int pe = 0; const int64_t v = a->mapGetInt(p, k, 0, &pe); return pe ? def : v; };
    std::vector<uint8_t> b(reinterpret_cast<const uint8_t*>(blob), reinterpret_cast<const uint8_t*>(blob) + n);
    MpOutColour o;
    o.plTransfer = H273TransferToPl(transfer); o.plPrimaries = H273PrimariesToPl(primaries); o.plSystem = H273MatrixToPlSystem(matrix);
    o.plLevels = range == 0 ? 2 : 1; o.maxLuma = maxLuma;
    if (!PatchMpImage(b, int(prop("_SARNum", 0)), int(prop("_SARDen", 0)), int(prop("_ColorSpace", -1)), int(prop("_ColorRange", -1)), o, why)) return false;
    a->mapSetData(p, "_MP_IMAGE", reinterpret_cast<const char*>(b.data()), int(b.size()), dtBinary, maReplace);
    a->mapSetInt(p, "_ColorSpace", PlSystemToAv(o.plSystem), maReplace);   // the blob and mpv's own props agree again
    a->mapSetInt(p, "_ColorRange", o.plLevels == 1 ? 1 : 0, maReplace);
    return true;
}

// CI only: PUSHMPC_TEST_OUT_COLOUR=<transfer>,<primaries>,<matrix>,<range> tags every output of the portable backend
// (which never changes the colour) as that, so mpv's own "[vf] [out]" line can prove the blob writer on a runner
struct MpTestOutColour { bool on = false; int transfer = 0, primaries = 0, matrix = 0, range = 1; };
inline const MpTestOutColour& TestOutColour()
{
    static const MpTestOutColour t = [] {
        MpTestOutColour r;
        if (const char* v = std::getenv("PUSHMPC_TEST_OUT_COLOUR"); v && *v) {
            int x[4] = { 0, 0, 0, 1 }, i = 0;
            for (const char* c = v; *c && i < 4; ++c) {
                if (*c == ',') { ++i; continue; }
                if (*c >= '0' && *c <= '9') { if (c == v || c[-1] == ',') x[i] = 0; x[i] = x[i] * 10 + (*c - '0'); }
            }
            r.on = i == 3; r.transfer = x[0]; r.primaries = x[1]; r.matrix = x[2]; r.range = x[3];
        }
        return r;
    }();
    return t;
}

} // namespace pmvs
