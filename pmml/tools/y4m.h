// SPDX-License-Identifier: GPL-3.0-or-later
// A minimal YUV4MPEG2 reader (4:2:0, 8 or 16 bit) for pmml_cli.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct Y4mFrame { std::vector<uint16_t> y, u, v; };

struct Y4m {
    int w = 0, h = 0, bits = 8;
    double fps = 0;
    std::vector<Y4mFrame> frames;

    bool read(const std::string& path, int limit, std::string* err) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { *err = "cannot open " + path; return false; }
        std::string header;
        for (int c; (c = std::fgetc(f)) != EOF && c != '\n';) header += char(c);
        if (header.rfind("YUV4MPEG2", 0) != 0) { std::fclose(f); *err = "not a Y4M file"; return false; }
        size_t p = 0;
        while ((p = header.find(' ', p)) != std::string::npos) {
            ++p;
            const char k = header[p];
            const std::string v = header.substr(p + 1, header.find(' ', p) - p - 1);
            if (k == 'W') w = std::atoi(v.c_str());
            else if (k == 'H') h = std::atoi(v.c_str());
            else if (k == 'F') { const auto c = v.find(':'); if (c != std::string::npos) fps = std::atof(v.substr(0, c).c_str()) / std::atof(v.substr(c + 1).c_str()); }
            else if (k == 'C') { if (v.find("p10") != std::string::npos) bits = 10; else if (v.find("p16") != std::string::npos) bits = 16; else if (v.find("p12") != std::string::npos) bits = 12;
                                 if (v.rfind("420", 0) != 0) { std::fclose(f); *err = "only 4:2:0 Y4M (" + v + ")"; return false; } }
        }
        const size_t bps = bits > 8 ? 2 : 1, ny = size_t(w) * h, nc = ny / 4;
        std::vector<uint8_t> buf((ny + 2 * nc) * bps);
        while (int(frames.size()) < limit) {
            std::string fh;
            int c;
            while ((c = std::fgetc(f)) != EOF && c != '\n') fh += char(c);
            if (c == EOF || std::fread(buf.data(), 1, buf.size(), f) != buf.size()) break;
            Y4mFrame fr;
            auto plane = [&](size_t off, size_t n, std::vector<uint16_t>& dst) {
                dst.resize(n);
                for (size_t i = 0; i < n; ++i) dst[i] = bps == 1 ? buf[off + i] : uint16_t(buf[(off + i) * 2] | (buf[(off + i) * 2 + 1] << 8));
            };
            plane(0, ny, fr.y);
            plane(ny, nc, fr.u);
            plane(ny + nc, nc, fr.v);
            frames.push_back(std::move(fr));
        }
        std::fclose(f);
        if (frames.empty()) { *err = "no frames in " + path; return false; }
        return true;
    }
};
