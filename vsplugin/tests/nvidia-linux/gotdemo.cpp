// libnvl_gotdemo.so: calls cuDeviceGetAttribute / cuModuleLoadData through its own GOT, as NVIDIA's
// libnvVFXVideoFrameGeneration.so calls them from libcuda.so.1 (nvl_got_test patches this library's slots only).
extern "C" {
int cuDeviceGetAttribute(int*, int, int);
int cuModuleLoadData(void**, const void*);
__attribute__((visibility("default"))) int demo_minor(int device) { int v = -1; cuDeviceGetAttribute(&v, 76, device); return v; }
__attribute__((visibility("default"))) int demo_load(const void* image) { void* m = nullptr; return cuModuleLoadData(&m, image); }
}
