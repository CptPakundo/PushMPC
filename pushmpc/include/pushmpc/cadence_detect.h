// Telecine (2:3 pulldown) measured in the picture, never from flags or names: 24p film carried as 60i repeats fields in
// a cycle of five frames, AA BB BC CD DD, so two neighbouring frames of every five weave fields of two different film
// frames (combed) and three do not. CombMeter (content_detect.h) counts those 2 of 5 combed frames as progressive (it
// needs half or more), and a deinterlacer would halve the film frames' detail; the remedy is field matching and
// decimation, which restores the film frames exactly (deint-probe: fieldmatch + decimate, 96 of 96 frames bit-exact).
//
// The measure per frame is CombMeter's: R = sum |Y[y-1] - 2Y[y] + Y[y+1]| / sum |Y[y-2] - 2Y[y] + Y[y+2]| over pixels
// that moved against the frame before, combed when R > 1 (the same centre crop of at most 1280 x 720, field parity
// kept). The verdict looks at the phase of each combed frame in the cycle of five: telecine when, over at least
// kNeed moving frames, between a quarter and three fifths of them are combed and at least 80 % of the combed ones fall
// on two neighbouring phases (the BC and CD frames), each of those two phases combed at least 70 % of the time it is
// seen. Interlaced video combs nearly every moving frame (more than three fifths), progressive video almost none; a
// broken cadence (edits, a video section) spreads the combed frames over the phases. kGiveUp frames without a verdict:
// no telecine (the caller keeps what it did). Pure C++, header only, no other PushMPC header.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace pushmpc::content {

class CadenceMeter {
public:
    static constexpr int kNeed = 40;       // moving frames for a verdict (8 cycles)
    static constexpr int kGiveUp = 240;    // frames
    static constexpr float kCombR = 1.0f;  // CombMeter::kCombR
    enum Verdict { kMeasuring = 0, kNone = 1, kTelecine = 2 };
    void Reset() { *this = CadenceMeter(); }
    Verdict verdict() const { return m_v; }
    int measured() const { return m_n; }
    int combed() const { return m_comb; }
    int frames() const { return m_frames; }
    // the two neighbouring phases holding the combed frames, and their share of all combed frames (after a verdict)
    int phase() const { return m_phase; }
    double phaseShare() const { return m_share; }
    // the field order measured on the first combed frame of each cycle (true: top field first; after a telecine verdict)
    bool topFieldFirst() const { return m_topRep[size_t(m_phase)] >= m_botRep[size_t(m_phase)]; }
    // one frame's luma (8-bit, or 16-bit words: shift = bits to drop for 8-bit codes); frames in presentation order
    template <class T>
    Verdict Push(const T* y, ptrdiff_t stride, int w, int h, int shift = 0)
    {
        if (m_v != kMeasuring || !y || w < 64 || h < 64) return m_v;
        const int pos = m_frames++ % 5;
        const int cw = std::min(w, 1280), chh = std::min(h, 720) & ~1, x0 = (w - cw) / 2, y0 = ((h - chh) / 2) & ~1;
        std::vector<uint8_t> cur(size_t(cw) * chh);
        for (int r = 0; r < chh; ++r) { const T* s = y + ptrdiff_t(y0 + r) * stride + x0; for (int c = 0; c < cw; ++c) cur[size_t(r) * cw + c] = uint8_t(std::min<int>(255, int(s[c]) >> shift)); }
        if (m_prev.size() == cur.size()) {
            double d1 = 0, d2 = 0; size_t moving = 0;
            for (int r = 2; r + 2 < chh; ++r)
                for (int c = 0; c < cw; ++c) {
                    const size_t i = size_t(r) * cw + c;
                    if (std::abs(int(cur[i]) - int(m_prev[i])) <= 6) continue;
                    ++moving;
                    d1 += std::abs(int(cur[i - cw]) - 2 * int(cur[i]) + int(cur[i + cw]));
                    d2 += std::abs(int(cur[i - 2 * size_t(cw)]) - 2 * int(cur[i]) + int(cur[i + 2 * size_t(cw)]));
                }
            if (moving >= 2000) {
                const bool combed = d1 / (d2 + 1e-6) > kCombR;
                ++m_n; m_seen[size_t(pos)]++;
                if (combed) {
                    ++m_comb; m_combAt[size_t(pos)]++;
                    // the field order: in the first combed frame of the pair one field repeats the frame before's field of
                    // the same parity (TFF: the top one, BB then BC). Which field is closer to the frame before
                    double se = 0, so = 0;
                    for (int r = 0; r < chh; ++r) {
                        double s = 0;
                        for (int c = 0; c < cw; ++c) s += std::abs(int(cur[size_t(r) * cw + c]) - int(m_prev[size_t(r) * cw + c]));
                        (r & 1 ? so : se) += s;
                    }
                    if (se < 0.5 * so) m_topRep[size_t(pos)]++;
                    else if (so < 0.5 * se) m_botRep[size_t(pos)]++;
                }
                if (m_n >= kNeed) Decide(false);
            }
        }
        m_prev.swap(cur);
        if (m_v == kMeasuring && m_frames >= kGiveUp) Decide(true);
        return m_v;
    }
private:
    void Decide(bool last)
    {
        m_phase = 0; m_share = 0;
        for (int p = 0; p < 5; ++p) {
            const int q = (p + 1) % 5, both = m_combAt[size_t(p)] + m_combAt[size_t(q)];
            const double share = m_comb ? double(both) / m_comb : 0.0;
            if (share > m_share) { m_share = share; m_phase = p; }
        }
        const int p = m_phase, q = (p + 1) % 5;
        auto rate = [&](int i) { return m_seen[size_t(i)] ? double(m_combAt[size_t(i)]) / m_seen[size_t(i)] : 0.0; };
        const double combedShare = m_n ? double(m_comb) / m_n : 0.0;
        const bool telecine = m_n >= kNeed && combedShare >= 0.25 && combedShare <= 0.6 && m_share >= 0.8 && rate(p) >= 0.7 && rate(q) >= 0.7;
        if (telecine) m_v = kTelecine;
        // clearly not: nearly every frame combed (60i video), nearly none (progressive), or the combed frames spread over the
        // cycle (noise, edits: telecine puts them on two neighbouring phases); or looked long enough
        else if (last || combedShare > 0.6 || combedShare < 0.15 || m_share < 0.6 || m_n >= 3 * kNeed) m_v = kNone;
    }
    std::vector<uint8_t> m_prev;
    std::array<int, 5> m_seen{}, m_combAt{}, m_topRep{}, m_botRep{};
    int m_n = 0, m_comb = 0, m_frames = 0, m_phase = 0;
    double m_share = 0;
    Verdict m_v = kMeasuring;
};

} // namespace pushmpc::content
