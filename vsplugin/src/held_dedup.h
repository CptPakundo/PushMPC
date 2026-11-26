// held_dedup: PushFPS's duplicate test on the CPU (its portable Dedup.h compareForDuplicate, as mpvnative's
// cpu_backend.cpp runs it) for the routes whose engine has no GPU test of its own: the VapourSynth CPU route, the mpv
// apple backend, and the CPU engine behind pm_engine (VLC, Kodi). A frame that repeats the last DISTINCT frame (anime
// drawn on twos, threes, fours) is held back; when the next distinct frame comes within kLookahead repeats, the
// outputs of the whole held span come from that one pair, each at its own phase; a longer run is a still (shown as it
// is, only its last interval moves). Pure C++, header only.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace pmvs::held {

inline constexpr int kLookahead = 3;   // twos, threes and fours (HeldFrames.h kLookahead; the GPU routes' cadence.h)

// the luma at quarter size: 4x4 means, 8 bits. bytesPerSample 1 or 2 (little endian 16-bit containers); shift: what
// takes a sample to 8 bits (0 for 8-bit, bits - 8 for LSB-aligned 9..16 bits, 8 for P010's MSB-aligned words)
inline void QuarterLuma(const uint8_t* y, ptrdiff_t stride, int w, int h, int bytesPerSample, int shift, std::vector<uint8_t>& q)
{
    const int qw = w / 4, qh = h / 4;
    q.resize(size_t(qw) * size_t(qh));
    for (int r = 0; r < qh; ++r)
        for (int x = 0; x < qw; ++x) {
            uint32_t s = 0;
            for (int j = 0; j < 4; ++j) {
                const uint8_t* row = y + (r * 4 + j) * stride;
                for (int i = 0; i < 4; ++i)
                    s += bytesPerSample > 1 ? uint32_t(uint32_t(row[2 * (x * 4 + i)] | (row[2 * (x * 4 + i) + 1] << 8)) >> shift) : row[x * 4 + i];
            }
            q[size_t(r) * size_t(qw) + size_t(x)] = uint8_t(std::min<uint32_t>((s + 8) / 16, 255));
        }
}

// PushFPS's DedupSettings: no 4x4 block of the quarter-size luma changed by 3 codes on average, mean change under 0.8
inline bool QuarterDuplicate(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int w, int h)
{
    const int qw = w / 4, qh = h / 4;
    if (a.size() != b.size() || !qw || !qh || a.size() != size_t(qw) * size_t(qh)) return false;
    double total = 0;
    float worst = 0;
    for (int by = 0; by < (qh + 3) / 4; ++by)
        for (int bx = 0; bx < (qw + 3) / 4; ++bx) {
            int sum = 0, n = 0;
            for (int y = by * 4; y < std::min(by * 4 + 4, qh); ++y)
                for (int x = bx * 4; x < std::min(bx * 4 + 4, qw); ++x) { sum += std::abs(int(a[size_t(y) * qw + x]) - int(b[size_t(y) * qw + x])); ++n; }
            total += sum;
            worst = std::max(worst, float(sum) / float(n));
        }
    return worst < 3.0f && total / double(a.size()) < 0.8;
}

} // namespace pmvs::held
