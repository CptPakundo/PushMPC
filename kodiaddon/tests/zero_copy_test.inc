// zero copy: a dma-buf (udmabuf over a memfd, standing in for Kodi's DRM-PRIME frame buffer) imported into the GPU
// engine; the generated frame written in place must equal the one read back the normal way. Skips (with the reason)
// without /dev/udmabuf or dma-buf import; PM_REQUIRE_DMABUF=1 makes a skip a failure. Included by cadence_test.cpp.
#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/udmabuf.h>
#endif

static void ZeroCopySkip(const std::string& why, bool must)
{
    std::printf("zero copy: SKIP (%s)\n", why.c_str());
    if (must) { std::printf("FAIL zero copy required\n"); ++g_fail; }
}

static void ZeroCopy()
{
#if defined(__linux__) && !defined(__ANDROID__)
    const char* req = std::getenv("PM_REQUIRE_DMABUF");
    const bool must = req && req[0] == '1';
    pmk::EngineCfg c;
    c.width = W; c.height = H; c.bits = 8; c.threads = 2; c.quality = 1; c.backend = 2;
    std::string why;
    auto e = pmk::OpenEngine(c, why);
    if (!e) return ZeroCopySkip("no GPU engine: " + why, must);
    if (!e->CanImport()) return ZeroCopySkip("the Vulkan device has no dma-buf import", must);
    pmk::OutLayout L;
    CHECK(e->Layout(L));
    const int dev = open("/dev/udmabuf", O_RDWR);
    if (dev < 0) return ZeroCopySkip("no /dev/udmabuf", must);
    const long page = sysconf(_SC_PAGESIZE);
    const uint64_t size = (L.size + uint64_t(page) - 1) / uint64_t(page) * uint64_t(page);
    const int mfd = memfd_create("pm", MFD_ALLOW_SEALING);
    CHECK(mfd >= 0 && ftruncate(mfd, off_t(size)) == 0 && fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) == 0);
    udmabuf_create cr{};
    cr.memfd = uint32_t(mfd); cr.flags = UDMABUF_FLAGS_CLOEXEC; cr.offset = 0; cr.size = size;
    const int buf = ioctl(dev, UDMABUF_CREATE, &cr);
    if (buf < 0) { close(mfd); close(dev); return ZeroCopySkip("UDMABUF_CREATE failed", must); }
    std::string err;
    const int id = e->Import(buf, size, err);
    if (id < 0) { close(buf); close(mfd); close(dev); return ZeroCopySkip("import failed: " + err, must); }
    std::vector<unsigned char> y(size_t(W) * H), u(size_t(W / 2) * (H / 2)), v(u.size());
    pm_yuv_out o{{y.data(), u.data(), v.data()}, {W, W / 2, W / 2}};
    for (int f = 0; f < 3; ++f) {
        Frame(f, 40 + 8 * f, 220);
        pm_yuv sv{{Y[f & 1], U[f & 1], V[f & 1]}, {W, W / 2, W / 2}};
        CHECK(e->Push(sv, f > 0, err));
    }
    CHECK(e->Generate(0.5f, o, err));
    CHECK(e->GenerateInto(0.5f, id, err));
    void* mp = mmap(nullptr, size_t(size), PROT_READ, MAP_SHARED, mfd, 0);
    CHECK(mp != MAP_FAILED);
    if (mp != MAP_FAILED) {
        const unsigned char* m = static_cast<const unsigned char*>(mp);
        long diff = 0;
        for (int r = 0; r < H; ++r) diff += std::memcmp(m + size_t(r) * size_t(L.pitchY), y.data() + size_t(r) * W, W) != 0;
        for (int r = 0; r < H / 2; ++r) diff += std::memcmp(m + L.offU + size_t(r) * size_t(L.pitchC), u.data() + size_t(r) * (W / 2), W / 2) != 0;
        std::printf("zero copy: frame written in place into a dma-buf, %ld rows differ from the readback\n", diff);
        CHECK(diff == 0);
        munmap(mp, size_t(size));
    }
    e->ReleaseImport(id);
    close(buf); close(mfd); close(dev);
#endif
}
