// pm_ivtc.cpp: see pm_ivtc.h. Licence: GPL-3.0-or-later.
#include "pm_ivtc.h"
#include <cstring>
#include <new>
#include <vector>

struct pm_ivtc {
    int w = 0, h = 0, bps = 1, phase = 0, pos = 0, tff = 1;
    std::vector<uint8_t> held[3], out[3];   // the held frame (BC) and the film frame ready, packed rows
};

namespace {
int PlaneW(const pm_ivtc* v, int i) { return i ? v->w / 2 : v->w; }
int PlaneH(const pm_ivtc* v, int i) { return i ? v->h / 2 : v->h; }
void Copy(const pm_ivtc* v, int i, const uint8_t* src, ptrdiff_t stride, std::vector<uint8_t>& dst)
{
    const size_t row = size_t(PlaneW(v, i)) * v->bps;
    dst.resize(row * PlaneH(v, i));
    for (int y = 0; y < PlaneH(v, i); ++y) std::memcpy(dst.data() + y * row, src + ptrdiff_t(y) * stride, row);
}
}

extern "C" pm_ivtc* pm_ivtc_new(int w, int h, int bits, int phase, int pos, int tff)
{
    if (w < 4 || h < 4 || bits < 8 || bits > 16 || phase < 0 || phase > 4) return nullptr;
    auto* v = new (std::nothrow) pm_ivtc();
    if (!v) return nullptr;
    v->w = w & ~1; v->h = h & ~1; v->bps = bits > 8 ? 2 : 1; v->phase = phase; v->pos = ((pos % 5) + 5) % 5; v->tff = tff ? 1 : 0;
    return v;
}
extern "C" void pm_ivtc_free(pm_ivtc* v) { delete v; }

extern "C" int pm_ivtc_push(pm_ivtc* v, const uint8_t* const plane[3], const ptrdiff_t stride[3])
{
    if (!v) return 0;
    const int pos = v->pos;
    v->pos = (v->pos + 1) % 5;
    try {
        if (pos == v->phase) {   // BC: held, its non-repeated field is half of C
            for (int i = 0; i < 3; ++i) Copy(v, i, plane[i], stride[i], v->held[i]);
            return 0;
        }
        for (int i = 0; i < 3; ++i) Copy(v, i, plane[i], stride[i], v->out[i]);
        if (pos == (v->phase + 1) % 5 && !v->held[0].empty()) {
            // CD: C's first field (TFF: top) is CD's own; its second field comes from BC (TFF: the bottom lines)
            const int from = v->tff ? 1 : 0;   // the line parity taken from BC
            for (int i = 0; i < 3; ++i) {
                const size_t row = size_t(PlaneW(v, i)) * v->bps;
                for (int y = from; y < PlaneH(v, i); y += 2) std::memcpy(v->out[i].data() + y * row, v->held[i].data() + y * row, row);
            }
        }
        return 1;
    } catch (...) { return 0; }
}

extern "C" void pm_ivtc_get(pm_ivtc* v, uint8_t* const plane[3], const ptrdiff_t stride[3])
{
    if (!v) return;
    for (int i = 0; i < 3; ++i) {
        const size_t row = size_t(PlaneW(v, i)) * v->bps;
        if (v->out[i].size() < row * PlaneH(v, i)) continue;
        for (int y = 0; y < PlaneH(v, i); ++y) std::memcpy(plane[i] + ptrdiff_t(y) * stride[i], v->out[i].data() + y * row, row);
    }
}
