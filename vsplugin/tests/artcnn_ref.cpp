// artcnn_ref: apple_nn.h's ArtCNN float reference on a raw picture, for the parity checks against onnxruntime (CI,
// tests/ci/artcnn_parity.py) and against the generated fragment shaders in mpv.
//   artcnn_ref <model.pmnn> <in.f32> <width> <height> <out.f32>
// in.f32: width x height float32 luma in [0, 1], rows packed; out.f32: 2 width x 2 height.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "apple_nn.h"

int main(int argc, char** argv)
{
    if (argc != 6) { std::fprintf(stderr, "usage: artcnn_ref <model.pmnn> <in.f32> <width> <height> <out.f32>\n"); return 2; }
    pmvs::apple::NnModel m;
    std::string err;
    if (!m.Load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const int w = std::atoi(argv[3]), h = std::atoi(argv[4]);
    std::vector<float> in(size_t(w) * size_t(h)), out(in.size() * 4);
    FILE* f = std::fopen(argv[2], "rb");
    if (!f || std::fread(in.data(), 4, in.size(), f) != in.size()) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    std::fclose(f);
    pmvs::apple::NnReference(m, in.data(), w, h, out.data());
    f = std::fopen(argv[5], "wb");
    if (!f || std::fwrite(out.data(), 4, out.size(), f) != out.size()) { std::fprintf(stderr, "cannot write %s\n", argv[5]); return 1; }
    std::fclose(f);
    std::printf("%s: %zu parameters, %dx%d -> %dx%d\n", m.name.c_str(), m.Parameters(), w, h, 2 * w, 2 * h);
    return 0;
}
