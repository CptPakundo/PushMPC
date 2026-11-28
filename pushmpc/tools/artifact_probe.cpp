// artifact_probe: run the artifact meter (pushmpc/artifact_meter.h) over a raw .y4m file (8-bit or 10/16-bit 4:2:0),
// for calibration against encode ladders. Prints one line: the medians over the clip (one shot unless the meter's own
// cut test fires), the per-shot lines, and the CPU time of Sample + Measure per sampled frame.
//   artifact_probe <file.y4m | -> [every=1] [label]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "pushmpc/artifact_meter.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace pushmpc::artifacts;

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: artifact_probe <file.y4m> [every] [label]\n"); return 2; }
    const bool in = std::strcmp(argv[1], "-") == 0;   // "-": a y4m pipe on stdin
#ifdef _WIN32
    if (in) _setmode(_fileno(stdin), _O_BINARY);
#endif
    FILE* f = in ? stdin : std::fopen(argv[1], "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    const int every = argc > 2 ? std::max(1, std::atoi(argv[2])) : 1;
    const char* label = argc > 3 ? argv[3] : argv[1];
    char hdr[1024] = {};
    if (!std::fgets(hdr, sizeof hdr, f) || std::strncmp(hdr, "YUV4MPEG2", 9) != 0) { std::fprintf(stderr, "not y4m\n"); return 2; }
    int w = 0, h = 0, bits = 8;
    for (char* p = std::strtok(hdr, " \n"); p; p = std::strtok(nullptr, " \n")) {
        if (p[0] == 'W') w = std::atoi(p + 1);
        else if (p[0] == 'H') h = std::atoi(p + 1);
        else if (p[0] == 'C') { const std::string c(p + 1); if (c.find("p10") != std::string::npos) bits = 10; else if (c.find("p12") != std::string::npos) bits = 12; else if (c.find("p16") != std::string::npos) bits = 16; }
    }
    if (w <= 0 || h <= 0) return 2;
    const size_t bps = bits > 8 ? 2 : 1, ySize = size_t(w) * h * bps, frameSize = ySize * 3 / 2;
    std::vector<uint8_t> buf(frameSize);
    ArtifactMeter m;
    char line[256];
    int n = 0, sampled = 0; double ms = 0, msS = 0;
    while (std::fgets(line, sizeof line, f) && std::strncmp(line, "FRAME", 5) == 0) {
        if (std::fread(buf.data(), 1, frameSize, f) != frameSize) break;
        if (n++ % every) continue;
        const auto t0 = std::chrono::steady_clock::now();
        const Tiles t = bps == 2 ? ArtifactMeter::Sample(reinterpret_cast<const uint16_t*>(buf.data()), ptrdiff_t(w), w, h, 0, bits, n / 50.0, uint32_t(sampled))
                                 : ArtifactMeter::Sample(buf.data(), ptrdiff_t(w), w, h, 0, 8, n / 50.0, uint32_t(sampled));
        const auto t1 = std::chrono::steady_clock::now();
        const FrameScores fs = ArtifactMeter::Measure(t);
        msS += std::chrono::duration<double, std::milli>(t1 - t0).count();
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        (void)fs;
        if (m.Push(t)) { const Shot& s = m.LastShot(); std::printf("  shot %.2f-%.2f s: block %.3f ring %.3f band %.4f detail %.3f\n", s.t0, s.t1, s.block, s.ring, s.band, s.detail); }
        ++sampled;
    }
    if (!in) std::fclose(f);
    const Shot r = m.Running(), st = m.Stream();
    const Shot& s = st.valid ? (r.valid ? r : st) : r;   // a single-shot clip: the running shot
    std::printf("RESULT %s %dx%d %d-bit frames %d sampled %d: block %.3f ring %.3f band %.4f detail %.3f conf %.2f/%.2f/%.2f/%.2f shots %u cpu %.2f ms/frame (Sample %.2f + Measure)\n",
                label, w, h, bits, n, sampled, s.block, s.ring, s.band, s.detail, s.confBlock, s.confRing, s.confBand, s.confDetail, m.shots(), sampled ? ms / sampled : 0.0, sampled ? msS / sampled : 0.0);
    return 0;
}
