// SHA-256 (FIPS 180-4), small and dependency-free: the identity check of the VFG library before the Ampere compat
// touches it, and the per-file check of the fetched runtime. nvl_test.cpp checks the FIPS test vectors.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace pmvs::nvl {

class Sha256 {
public:
    Sha256() { Reset(); }
    void Reset()
    {
        static const uint32_t h0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        std::memcpy(m_h, h0, sizeof(m_h)); m_len = 0; m_fill = 0;
    }
    void Update(const void* data, size_t n)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        m_len += uint64_t(n);
        while (n) {
            const size_t take = std::min<size_t>(64 - m_fill, n);
            std::memcpy(m_buf + m_fill, p, take); m_fill += take; p += take; n -= take;
            if (m_fill == 64) { Block(m_buf); m_fill = 0; }
        }
    }
    std::string HexUpper()
    {
        const uint64_t bits = m_len * 8;
        const uint8_t pad = 0x80; Update(&pad, 1);
        const uint8_t zero = 0; while (m_fill != 56) Update(&zero, 1);
        uint8_t l[8]; for (int i = 0; i < 8; ++i) l[i] = uint8_t(bits >> (56 - 8 * i));
        Update(l, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) snprintf(out + 8 * i, 9, "%08X", m_h[i]);
        return std::string(out, 64);
    }
    static bool File(const std::string& path, std::string& hex, uint64_t& size)
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        Sha256 s; size = 0;
        static thread_local uint8_t buf[1 << 16];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { s.Update(buf, n); size += n; }
        std::fclose(f);
        hex = s.HexUpper();
        return true;
    }

private:
    static uint32_t R(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void Block(const uint8_t* b)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | uint32_t(b[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = R(w[i - 15], 7) ^ R(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = R(w[i - 2], 17) ^ R(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = m_h[0], bb = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4], f = m_h[5], g = m_h[6], h = m_h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (R(e, 6) ^ R(e, 11) ^ R(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (R(a, 2) ^ R(a, 13) ^ R(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
        }
        m_h[0] += a; m_h[1] += bb; m_h[2] += c; m_h[3] += d; m_h[4] += e; m_h[5] += f; m_h[6] += g; m_h[7] += h;
    }
    uint32_t m_h[8];
    uint8_t m_buf[64];
    size_t m_fill = 0;
    uint64_t m_len = 0;
};

} // namespace pmvs::nvl
