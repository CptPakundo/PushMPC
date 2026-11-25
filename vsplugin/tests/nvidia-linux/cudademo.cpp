// libnvl_cudademo.so: three CUDA driver entry points with libcuda's names, for nvl_got_test (no GPU). The attribute
// call reports SM 8.6; the module load records the image it was given.
#include <cstdint>
#include <cstring>
#include <vector>

static std::vector<uint8_t> g_last;
extern "C" {
__attribute__((visibility("default"))) int cuDeviceGetAttribute(int* v, int attribute, int device)
{
    if (!v) return 1;
    *v = attribute == 75 ? 8 : attribute == 76 ? 6 : 0;
    (void)device;
    return 0;
}
__attribute__((visibility("default"))) int cuModuleLoadData(void** module, const void* image)
{
    uint64_t payload = 0;
    std::memcpy(&payload, static_cast<const uint8_t*>(image) + 8, 8);
    const auto* b = static_cast<const uint8_t*>(image);
    g_last.assign(b, b + 16 + payload);
    if (module) *module = reinterpret_cast<void*>(0x1234);
    return 0;
}
__attribute__((visibility("default"))) int cuCtxGetDevice(int* d) { if (d) *d = 0; return 0; }
// test access to what the last load received
__attribute__((visibility("default"))) const uint8_t* demo_last_image(size_t* n) { *n = g_last.size(); return g_last.data(); }
}
