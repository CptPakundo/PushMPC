// comb_probe <gray8 raw file> <width> <height>: PushMPC's CombMeter (pushmpc/content_detect.h) on a whole clip, for the
// deinterlace probe (.github/workflows/deint-probe.yml). Prints the meter's verdict on the first frames (the rule every
// route uses) and the share of combed moving frames over the whole clip (residual combing a deinterlacer left).
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "pushmpc/content_detect.h"
int main(int argc, char** argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: comb_probe <gray8 raw> <w> <h>\n"); return 2; }
    const int w = std::atoi(argv[2]), h = std::atoi(argv[3]);
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::vector<uint8_t> y(size_t(w) * h);
    pushmpc::content::CombMeter first;
    int frames = 0, verdictAt = -1, windows = 0, combedWindows = 0;
    pushmpc::content::CombMeter win;   // the whole clip in windows of 24 moving frames
    while (std::fread(y.data(), 1, y.size(), f) == y.size()) {
        ++frames;
        if (first.verdict() == pushmpc::content::CombMeter::kMeasuring && first.Push(y.data(), w, w, h, 0) != pushmpc::content::CombMeter::kMeasuring) verdictAt = frames;
        if (win.Push(y.data(), w, w, h, 0) != pushmpc::content::CombMeter::kMeasuring) {
            ++windows; combedWindows += win.combed();
            win.Reset();
        }
    }
    std::fclose(f);
    const char* v = first.verdict() == pushmpc::content::CombMeter::kInterlaced ? "interlaced" : first.verdict() == pushmpc::content::CombMeter::kProgressive ? "progressive" : "no verdict";
    std::printf("combmeter: %s (%d of %d moving frames combed, decided at frame %d); whole clip: %d of %d moving frames combed\n",
                v, first.combed(), first.measured(), verdictAt, combedWindows, windows * pushmpc::content::CombMeter::kNeed);
    return 0;
}
