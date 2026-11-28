// pm_deint.cpp: see pm_deint.h. The YADIF algorithm (Michael Niedermayer's "yet another deinterlacing filter", as
// FFmpeg's vf_yadif runs it in mode 1, send_field, with its spatial check): for each line of the missing field, the
// spatial prediction from the lines above and below along the best of five directions, kept within the range the
// temporal neighbours of that pixel allow (the missing field's lines one field before and one field after, and how
// much the picture moved there). Written here from the algorithm for the module's planes; the picture's left and right
// edges read their nearest pixels, the top and bottom lines the nearest line of the kept field.
// Licence: GPL-3.0-or-later.
#include "pm_deint.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace {

struct Plane { int w = 0, h = 0; std::vector<uint16_t> px; };   // 16-bit samples whatever the depth
struct Frame { Plane p[3]; };

template <class T>
void Load(Plane& d, const uint8_t* src, ptrdiff_t stride)
{
    for (int y = 0; y < d.h; ++y) {
        const T* s = reinterpret_cast<const T*>(src + ptrdiff_t(y) * stride);
        uint16_t* o = d.px.data() + size_t(y) * d.w;
        for (int x = 0; x < d.w; ++x) o[x] = uint16_t(s[x]);
    }
}
template <class T>
void Store(const std::vector<uint16_t>& v, int w, int h, uint8_t* dst, ptrdiff_t stride)
{
    for (int y = 0; y < h; ++y) {
        T* o = reinterpret_cast<T*>(dst + ptrdiff_t(y) * stride);
        const uint16_t* s = v.data() + size_t(y) * w;
        for (int x = 0; x < w; ++x) o[x] = T(s[x]);
    }
}

// the YADIF prediction of one pixel: rows a (above) and b (below) of cur, prev, next; p2 / n2 rows y, y-2, y+2.
// X(r, i) reads row r at column x + i (the caller makes x + i valid)
template <class Get>
inline int Predict(Get X, const uint16_t* pa, const uint16_t* pb, const uint16_t* ca, const uint16_t* cb, const uint16_t* na, const uint16_t* nb,
                   const uint16_t* p2, const uint16_t* n2, const uint16_t* p2m, const uint16_t* n2m, const uint16_t* p2p, const uint16_t* n2p)
{
    const int c = X(ca, 0), e = X(cb, 0);
    const int d = (X(p2, 0) + X(n2, 0)) >> 1;
    const int td0 = std::abs(X(p2, 0) - X(n2, 0));
    const int td1 = (std::abs(X(pa, 0) - c) + std::abs(X(pb, 0) - e)) >> 1;
    const int td2 = (std::abs(X(na, 0) - c) + std::abs(X(nb, 0) - e)) >> 1;
    int diff = std::max({ td0 >> 1, td1, td2 });
    int pred = (c + e) >> 1;
    int score = std::abs(X(ca, -1) - X(cb, -1)) + std::abs(c - e) + std::abs(X(ca, 1) - X(cb, 1)) - 1;
    // the edge-directed check: along -1 then -2, and +1 then +2 (the second only when the first was better)
    auto check = [&](int j) {
        const int s = std::abs(X(ca, -1 + j) - X(cb, -1 - j)) + std::abs(X(ca, j) - X(cb, -j)) + std::abs(X(ca, 1 + j) - X(cb, 1 - j));
        if (s < score) { score = s; pred = (X(ca, j) + X(cb, -j)) >> 1; return true; }
        return false;
    };
    if (check(-1)) check(-2);
    if (check(1)) check(2);
    // the spatial check: the temporal range widened by how the neighbouring lines moved
    const int b = (X(p2m, 0) + X(n2m, 0)) >> 1, f = (X(p2p, 0) + X(n2p, 0)) >> 1;
    const int mx = std::max({ d - e, d - c, std::min(b - c, f - e) });
    const int mn = std::min({ d - e, d - c, std::max(b - c, f - e) });
    diff = std::max({ diff, mn, -mx });
    return std::max(0, std::clamp(pred, d - diff, d + diff));
}

// one output field of a plane: keep the lines of parity `keep` from cur, interpolate the others. p2 / n2: the frames
// holding the missing field one field before and one after (first field: prev and cur; second field: cur and next)
void Field(const Plane& prev, const Plane& cur, const Plane& next, const Plane& p2, const Plane& n2, int keep, std::vector<uint16_t>& out)
{
    const int w = cur.w, h = cur.h;
    out.resize(size_t(w) * h);
    auto row = [w](const Plane& p, int y) { return p.px.data() + size_t(y) * w; };
    for (int y = 0; y < h; ++y) {
        uint16_t* o = out.data() + size_t(y) * w;
        if ((y & 1) == keep) { std::memcpy(o, row(cur, y), size_t(w) * 2); continue; }
        // the kept field's lines above and below (mirrored at the picture's edges), two above / below for the check
        const int ym = y > 0 ? y - 1 : y + 1, yp = y + 1 < h ? y + 1 : y - 1;
        const int ym2 = y >= 2 ? y - 2 : y, yp2 = y + 2 < h ? y + 2 : y;
        const uint16_t *pa = row(prev, ym), *pb = row(prev, yp), *ca = row(cur, ym), *cb = row(cur, yp), *na = row(next, ym), *nb = row(next, yp);
        const uint16_t *q2 = row(p2, y), *m2 = row(n2, y), *q2m = row(p2, ym2), *m2m = row(n2, ym2), *q2p = row(p2, yp2), *m2p = row(n2, yp2);
        int x = 0;
        auto edge = [&](const uint16_t* r, int i) { return int(r[std::clamp(x + i, 0, w - 1)]); };
        auto inner = [&](const uint16_t* r, int i) { return int(r[x + i]); };
        for (; x < std::min(3, w); ++x) o[x] = uint16_t(Predict(edge, pa, pb, ca, cb, na, nb, q2, m2, q2m, m2m, q2p, m2p));
        for (; x < w - 3; ++x) o[x] = uint16_t(Predict(inner, pa, pb, ca, cb, na, nb, q2, m2, q2m, m2m, q2p, m2p));
        for (; x < w; ++x) o[x] = uint16_t(Predict(edge, pa, pb, ca, cb, na, nb, q2, m2, q2m, m2m, q2p, m2p));
    }
}

} // namespace

struct pm_deint {
    int w = 0, h = 0, bits = 8;
    Frame f[3];               // prev, cur, next (rotating)
    int have = 0, tff = 1;
    std::vector<uint16_t> out[2][3];
};

extern "C" pm_deint* pm_deint_new(int w, int h, int bits)
{
    if (w < 4 || h < 4 || bits < 8 || bits > 16) return nullptr;
    auto* d = new (std::nothrow) pm_deint();
    if (!d) return nullptr;
    d->w = w & ~1; d->h = h & ~1; d->bits = bits;
    try {
        for (auto& fr : d->f)
            for (int i = 0; i < 3; ++i) { fr.p[i].w = i ? d->w / 2 : d->w; fr.p[i].h = i ? d->h / 2 : d->h; fr.p[i].px.resize(size_t(fr.p[i].w) * fr.p[i].h); }
    } catch (...) { delete d; return nullptr; }
    return d;
}
extern "C" void pm_deint_free(pm_deint* d) { delete d; }
extern "C" void pm_deint_reset(pm_deint* d) { if (d) d->have = 0; }

extern "C" int pm_deint_push(pm_deint* d, const uint8_t* const plane[3], const ptrdiff_t stride[3], int tff)
{
    if (!d) return 0;
    try {
        // rotate: prev <- cur <- next <- the new frame
        std::swap(d->f[0], d->f[1]); std::swap(d->f[1], d->f[2]);
        for (int i = 0; i < 3; ++i) {
            if (d->bits > 8) Load<uint16_t>(d->f[2].p[i], plane[i], stride[i]);
            else Load<uint8_t>(d->f[2].p[i], plane[i], stride[i]);
        }
        if (d->have == 0) { d->f[1] = d->f[2]; d->f[0] = d->f[2]; }   // the first frame stands in for its own neighbours
        else if (d->have == 1) d->f[0] = d->f[1];
        d->have = std::min(d->have + 1, 2);
        d->tff = tff ? 1 : 0;
        if (d->have < 2) return 0;
        // the previous frame (now cur) has its next frame: both its fields
        const Frame &prev = d->f[0], &cur = d->f[1], &next = d->f[2];
        for (int i = 0; i < 3; ++i) {
            // the first field in time keeps the top lines (TFF) and is missing the bottom field, which exists one field
            // before (prev) and one after (cur); the second field keeps the bottom lines, missing top: cur, next
            Field(prev.p[i], cur.p[i], next.p[i], prev.p[i], cur.p[i], d->tff ? 0 : 1, d->out[0][i]);
            Field(prev.p[i], cur.p[i], next.p[i], cur.p[i], next.p[i], d->tff ? 1 : 0, d->out[1][i]);
        }
        return 2;
    } catch (...) { return 0; }
}

extern "C" void pm_deint_get(pm_deint* d, int field, uint8_t* const plane[3], const ptrdiff_t stride[3])
{
    if (!d || field < 0 || field > 1) return;
    for (int i = 0; i < 3; ++i) {
        const int w = i ? d->w / 2 : d->w, h = i ? d->h / 2 : d->h;
        if (d->bits > 8) Store<uint16_t>(d->out[field][i], w, h, plane[i], stride[i]);
        else Store<uint8_t>(d->out[field][i], w, h, plane[i], stride[i]);
    }
}
