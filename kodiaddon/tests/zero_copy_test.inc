// zero copy: a dma-buf (udmabuf over a memfd, standing in for Kodi's DRM-PRIME frame buffer) imported into the GPU
// engine; the generated frame written in place must equal the one read back the normal way. Skips (with the reason)
// without /dev/udmabuf or dma-buf import; PM_REQUIRE_DMABUF=1 makes a skip a failure. Included by cadence_test.cpp.
#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/udmabuf.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

// a dma-buf of size bytes and an fd to map it on the CPU: udmabuf over a memfd, else a vgem dumb buffer (both kernel
// modules the CI loads); false with the reason when neither is there
static bool MakeDmaBuf(uint64_t size, int& buf, int& mapfd, std::string& why)
{
    buf = mapfd = -1;
    const int dev = open("/dev/udmabuf", O_RDWR);
    if (dev >= 0) {
        const int mfd = memfd_create("pm", MFD_ALLOW_SEALING);
        if (mfd >= 0 && ftruncate(mfd, off_t(size)) == 0 && fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) == 0) {
            udmabuf_create cr{};
            cr.memfd = uint32_t(mfd); cr.flags = UDMABUF_FLAGS_CLOEXEC; cr.offset = 0; cr.size = size;
            buf = ioctl(dev, UDMABUF_CREATE, &cr);
        }
        close(dev);
        if (buf >= 0) { mapfd = mfd; why = "udmabuf"; return true; }
        if (mfd >= 0) close(mfd);
    }
    for (int i = 0; i < 8; ++i) {
        const std::string path = "/dev/dri/card" + std::to_string(i);
        const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        char name[32] = {};
        drm_version ver{};
        ver.name = name; ver.name_len = sizeof(name) - 1;
        if (ioctl(fd, DRM_IOCTL_VERSION, &ver) == 0 && std::strcmp(name, "vgem") == 0) {
            drm_mode_create_dumb cd{};
            cd.width = uint32_t((size + 4095) / 4096); cd.height = 4096 / 4; cd.bpp = 32;   // width x 4096 bytes >= size
            if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0) {
                drm_prime_handle ph{};
                ph.handle = cd.handle; ph.flags = DRM_CLOEXEC | DRM_RDWR;
                if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0) { buf = ph.fd; mapfd = dup(ph.fd); close(fd); why = "vgem"; return true; }
            }
        }
        close(fd);
    }
    why = "no /dev/udmabuf and no vgem device";
    return false;
}
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
    const long page = sysconf(_SC_PAGESIZE);
    const uint64_t size = (L.size + uint64_t(page) - 1) / uint64_t(page) * uint64_t(page);
    int buf = -1, mfd = -1;
    std::string kind;
    if (!MakeDmaBuf(size, buf, mfd, kind)) return ZeroCopySkip(kind, must);
    std::printf("zero copy: dma-buf from %s\n", kind.c_str());
    const int dev = -1;
    std::string err;
    const int id = e->Import(buf, size, err);
    if (id < 0) { close(buf); close(mfd); return ZeroCopySkip("import failed: " + err, must); }
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
    close(buf); close(mfd); (void)dev;
#endif
}

// in place into host memory (VK_EXT_external_memory_host; Kodi's system-memory frame buffers on Windows and X11)
static void HostImport()
{
    pmk::EngineCfg c;
    c.width = W; c.height = H; c.bits = 8; c.threads = 2; c.quality = 1; c.backend = 2;
    std::string why, err;
    auto e = pmk::OpenEngine(c, why);
    if (!e) { std::printf("host import: SKIP (no GPU engine)\n"); return; }
    const uint64_t align = e->HostAlignment();
    if (!align) { std::printf("host import: SKIP (no VK_EXT_external_memory_host)\n"); return; }
    pmk::OutLayout L;
    CHECK(e->Layout(L));
    const uint64_t size = (L.size + align - 1) / align * align;
    std::vector<unsigned char> raw(size_t(size + align));
    unsigned char* p = reinterpret_cast<unsigned char*>((reinterpret_cast<uintptr_t>(raw.data()) + align - 1) / align * align);
    const int id = e->ImportHost(p, size, err);
    if (id < 0) { std::printf("host import: SKIP (%s)\n", err.c_str()); return; }
    std::vector<unsigned char> y(size_t(W) * H), u(size_t(W / 2) * (H / 2)), v(u.size());
    pm_yuv_out o{{y.data(), u.data(), v.data()}, {W, W / 2, W / 2}};
    for (int f = 0; f < 3; ++f) {
        Frame(f, 40 + 8 * f, 220);
        pm_yuv sv{{Y[f & 1], U[f & 1], V[f & 1]}, {W, W / 2, W / 2}};
        CHECK(e->Push(sv, f > 0, err));
    }
    CHECK(e->Generate(0.5f, o, err));
    CHECK(e->GenerateInto(0.5f, id, err));
    long diff = 0;
    for (int r = 0; r < H; ++r) diff += std::memcmp(p + size_t(r) * size_t(L.pitchY), y.data() + size_t(r) * W, W) != 0;
    std::printf("host import: frame written in place into host memory, %ld rows differ\n", diff);
    CHECK(diff == 0);
    e->ReleaseImport(id);
}

// NV12 / P010 output (Windows with Kodi 22): the interleaved layout must hold exactly the planar output's samples, and
// 10-bit samples sit in the high bits
static void SemiPlanar(int bits)
{
    pmk::EngineCfg c;
    c.width = W; c.height = H; c.bits = bits; c.threads = 2; c.quality = 1; c.backend = 2;
    std::string why, err;
    auto planar = pmk::OpenEngine(c, why);
    c.semiPlanar = true;
    auto semi = pmk::OpenEngine(c, why);
    if (!planar || !semi || !semi->IsGpu()) { std::printf("semi-planar %d-bit: SKIP (no GPU engine)\n", bits); return; }
    const int bps = bits > 8 ? 2 : 1;
    std::vector<uint8_t> py[2], pu[2], pv[2];
    for (int f = 0; f < 3; ++f) {
        Frame(f, 40 + 8 * f, 220);
        // 8-bit test frames widened to 16-bit containers for 10 bits (value x 4)
        for (int k = 0; k < 2 && f == 0; ++k) { py[k].resize(size_t(W) * H * bps); pu[k].resize(size_t(W / 2) * (H / 2) * bps); pv[k].resize(pu[k].size()); }
        std::vector<uint8_t>& yy = py[f & 1]; std::vector<uint8_t>& uu = pu[f & 1]; std::vector<uint8_t>& vv = pv[f & 1];
        for (int i = 0; i < W * H; ++i) { if (bps == 1) yy[i] = Y[f & 1][i]; else { uint16_t s = uint16_t(Y[f & 1][i] * 4); std::memcpy(&yy[size_t(i) * 2], &s, 2); } }
        for (int i = 0; i < W * H / 4; ++i) {
            if (bps == 1) { uu[i] = U[f & 1][i]; vv[i] = V[f & 1][i]; }
            else { uint16_t a = uint16_t(U[f & 1][i] * 4), b = uint16_t(V[f & 1][i] * 4); std::memcpy(&uu[size_t(i) * 2], &a, 2); std::memcpy(&vv[size_t(i) * 2], &b, 2); }
        }
        pm_yuv sv{{yy.data(), uu.data(), vv.data()}, {W * bps, W / 2 * bps, W / 2 * bps}};
        CHECK(planar->Push(sv, f > 0, err));
        CHECK(semi->Push(sv, f > 0, err));
    }
    std::vector<uint8_t> y1(size_t(W) * H * bps), u1(size_t(W / 2) * (H / 2) * bps), v1(u1.size());
    std::vector<uint8_t> y2(y1.size()), uv2(u1.size() * 2);
    pm_yuv_out o1{{y1.data(), u1.data(), v1.data()}, {W * bps, W / 2 * bps, W / 2 * bps}};
    pm_yuv_out o2{{y2.data(), uv2.data(), nullptr}, {W * bps, W * bps, 0}};
    CHECK(planar->Generate(0.5f, o1, err));
    CHECK(semi->Generate(0.5f, o2, err));
    long bad = 0;
    const int sh = bits > 8 ? 16 - bits : 0;
    auto at = [&](const std::vector<uint8_t>& b, size_t i) { return bps == 1 ? unsigned(b[i]) : unsigned(b[2 * i] | (b[2 * i + 1] << 8)); };
    for (size_t i = 0; i < size_t(W) * H; ++i) bad += at(y2, i) != (at(y1, i) << sh);
    for (size_t i = 0; i < size_t(W / 2) * (H / 2); ++i) bad += at(uv2, 2 * i) != (at(u1, i) << sh) || at(uv2, 2 * i + 1) != (at(v1, i) << sh);
    std::printf("semi-planar %d-bit: %ld samples differ from the planar output\n", bits, bad);
    CHECK(bad == 0);
}

// Kodi 21 HDR to SDR: neutral greys stay neutral, black stays black, brighter PQ codes give brighter SDR codes, and the
// 1000 cd/m2 peak lands near SDR white
#include "tonemap.h"
static void ToneMap()
{
    pmk::ToneMapper tm;
    tm.Configure(true, 1000);
    const int w = 8, h = 2;
    const uint16_t codes[4] = {64, 400, 572, 769};   // black, ~20, 203 and 1000 cd/m2 (PQ, 10-bit limited)
    int prev = -1;
    for (uint16_t c : codes) {
        std::vector<uint16_t> y(size_t(w) * h, c), u(size_t(w / 2), 512), v(size_t(w / 2), 512);
        std::vector<uint8_t> oy(size_t(w) * h), ou(size_t(w / 2)), ov(size_t(w / 2));
        const uint16_t* sp[3] = {y.data(), u.data(), v.data()};
        const ptrdiff_t ss[3] = {w * 2, w, w};
        uint8_t* const dp[3] = {oy.data(), ou.data(), ov.data()};
        const ptrdiff_t ds[3] = {w, w / 2, w / 2};
        tm.Run(sp, ss, 10, w, h, dp, ds, 2);
        std::printf("tone map: PQ code %u -> SDR Y %u Cb %u Cr %u\n", unsigned(c), unsigned(oy[0]), unsigned(ou[0]), unsigned(ov[0]));
        CHECK(std::abs(int(ou[0]) - 128) <= 1 && std::abs(int(ov[0]) - 128) <= 1);
        CHECK(int(oy[0]) > prev);
        if (c == 64) CHECK(oy[0] <= 17);
        if (c == 769) CHECK(oy[0] >= 225);
        prev = oy[0];
    }
}
