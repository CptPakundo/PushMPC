// Linux port of PushFPS's VfgAmpereCompat (see nvl_ampere.h). Derived from PushFPS src/pipeline/VfgAmpereCompat.cpp
// (GPL-3.0), which is derived from Xchen991/RTX30-Broadcast-FrameGen (MIT, commit 07536ab3): Hook_cuDeviceGetAttribute /
// Hook_cuModuleLoadData. Linux changes: GOT slots of the one library instead of PE imports; mprotect instead of
// VirtualProtect; no SEH (the fatbin is read from the caller's image as CUDA itself would).
#include "nvl_ampere.h"
#ifndef _WIN32   // Linux: the GOT hooks; Windows on Arm (RTX Spark, SM 12.1) never needs them (stubs at the end)
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include "nvl_fatbin.h"
#include "nvl_sha256.h"

namespace pmvs::nvl {

namespace {

#if defined(__x86_64__)
constexpr uint32_t kGlobDat = R_X86_64_GLOB_DAT, kJumpSlot = R_X86_64_JUMP_SLOT, kAbs64 = R_X86_64_64;
#elif defined(__aarch64__)
constexpr uint32_t kGlobDat = R_AARCH64_GLOB_DAT, kJumpSlot = R_AARCH64_JUMP_SLOT, kAbs64 = R_AARCH64_ABS64;
#else
#error "nvl_ampere.cpp: x86-64 or AArch64"
#endif

struct RelroRange { uintptr_t base = 0, lo = 0, hi = 0; };
int PhdrCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* r = static_cast<RelroRange*>(data);
    if (info->dlpi_addr != r->base) return 0;
    for (int i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_GNU_RELRO) { r->lo = r->base + info->dlpi_phdr[i].p_vaddr; r->hi = r->lo + info->dlpi_phdr[i].p_memsz; }
    return 1;
}

using AttributeFn = int (*)(int*, int, int);
using LoadFn = int (*)(void**, const void*);
using CtxDeviceFn = int (*)(int*);
struct State {
    AttributeFn attribute = nullptr;
    LoadFn load = nullptr;
    CtxDeviceFn contextDevice = nullptr;
    int device = -1;
};
std::mutex g_lock;
std::shared_ptr<State> g_state;   // read by the hooks (atomic_load)
std::atomic<unsigned> g_patched{0}, g_spoofs{0}, g_rejected{0};

int HookDeviceGetAttribute(int* value, int attribute, int device)
{
    auto s = std::atomic_load(&g_state);
    if (!s) return 3;   // CUDA_ERROR_NOT_INITIALIZED
    const int r = s->attribute(value, attribute, device);
    if (!r && value) {
        const int reported = VfgReportedAttribute(*value, attribute, device, s->device, r);
        if (reported != *value) { *value = reported; ++g_spoofs; }
    }
    return r;
}

int HookModuleLoadData(void** module, const void* image)
{
    auto s = std::atomic_load(&g_state);
    if (!s) return 3;
    int current = -1;
    const int cr = s->contextDevice(&current);
    if (cr || current != s->device || !image) return s->load(module, image);
    const auto* bytes = static_cast<const uint8_t*>(image);
    uint32_t magic = 0; std::memcpy(&magic, bytes, 4);
    if (magic != kFatbinMagic) return s->load(module, image);
    const size_t size = FatbinSize(bytes);
    if (!size) { ++g_rejected; return 200; }   // CUDA_ERROR_INVALID_IMAGE
    try {
        std::vector<uint8_t> copy; FatbinChanges ch; std::string why;
        if (!RetargetVfgFatbin(bytes, size, copy, ch, why)) { ++g_rejected; return 200; }
        if (!ch.containers) return s->load(module, image);
        const int r = s->load(module, copy.data());   // CUDA copies the image during this synchronous call
        ++g_patched;
        return r;
    } catch (...) { ++g_rejected; return 2; }   // CUDA_ERROR_OUT_OF_MEMORY
}

bool WriteSlot(const GotSlot& slot, uintptr_t value)
{
    const long page = sysconf(_SC_PAGESIZE);
    const uintptr_t start = slot.address & ~uintptr_t(page - 1);
    if (mprotect(reinterpret_cast<void*>(start), size_t(page), PROT_READ | PROT_WRITE) != 0) return false;
    __atomic_store_n(reinterpret_cast<uintptr_t*>(slot.address), value, __ATOMIC_SEQ_CST);
    // RELRO pages go back to read-only; the others were read-write already
    if (slot.relro) mprotect(reinterpret_cast<void*>(start), size_t(page), PROT_READ);
    return *reinterpret_cast<uintptr_t*>(slot.address) == value;
}

} // namespace

bool FindGotSlots(void* handle, const std::vector<std::string>& symbols, std::vector<GotSlot>& slots, std::string& why)
{
    slots.clear();
    link_map* lm = nullptr;
    if (!handle || dlinfo(handle, RTLD_DI_LINKMAP, &lm) != 0 || !lm || !lm->l_ld) { why = "no link map for the library"; return false; }
    const uintptr_t base = lm->l_addr;
    auto addr = [&](ElfW(Addr) v) { return v < base ? uintptr_t(v) + base : uintptr_t(v); };   // glibc relocates d_ptr in place
    const ElfW(Sym)* symtab = nullptr; const char* strtab = nullptr;
    const ElfW(Rela)* rela = nullptr; size_t relaSize = 0; const ElfW(Rela)* jmprel = nullptr; size_t jmpSize = 0; long pltrel = DT_RELA;
    for (const ElfW(Dyn)* d = lm->l_ld; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
        case DT_SYMTAB: symtab = reinterpret_cast<const ElfW(Sym)*>(addr(d->d_un.d_ptr)); break;
        case DT_STRTAB: strtab = reinterpret_cast<const char*>(addr(d->d_un.d_ptr)); break;
        case DT_RELA: rela = reinterpret_cast<const ElfW(Rela)*>(addr(d->d_un.d_ptr)); break;
        case DT_RELASZ: relaSize = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = reinterpret_cast<const ElfW(Rela)*>(addr(d->d_un.d_ptr)); break;
        case DT_PLTRELSZ: jmpSize = d->d_un.d_val; break;
        case DT_PLTREL: pltrel = long(d->d_un.d_val); break;
        default: break;
        }
    }
    if (!symtab || !strtab) { why = "no dynamic symbol table"; return false; }
    if (jmprel && pltrel != DT_RELA) { why = "PLT relocations are not RELA"; return false; }
    RelroRange relro; relro.base = base;
    dl_iterate_phdr(PhdrCallback, &relro);
    auto scan = [&](const ElfW(Rela)* r, size_t bytes) {
        if (!r) return;
        for (size_t i = 0; i < bytes / sizeof(ElfW(Rela)); ++i) {
            const uint32_t type = uint32_t(ELF64_R_TYPE(r[i].r_info)), si = uint32_t(ELF64_R_SYM(r[i].r_info));
            if (!si || (type != kGlobDat && type != kJumpSlot && type != kAbs64)) continue;
            const char* name = strtab + symtab[si].st_name;
            for (const auto& want : symbols)
                if (want == name) {
                    GotSlot s; s.symbol = want; s.address = base + uintptr_t(r[i].r_offset);
                    s.relro = s.address >= relro.lo && s.address < relro.hi;
                    slots.push_back(s);
                }
        }
    };
    scan(rela, relaSize);
    scan(jmprel, jmpSize);
    for (const auto& want : symbols) {
        size_t n = 0;
        for (const auto& s : slots) n += s.symbol == want;
        if (!n) { why = "no GOT slot for " + want; return false; }
        if (n > 4) { why = "ambiguous GOT slots for " + want; return false; }
    }
    return true;
}

bool AmpereCompat::Install(void* vfgHandle, const std::string& vfgPath, void* cudaLib, int device, std::string& why, bool checkIdentity)
{
    std::lock_guard<std::mutex> lk(g_lock);
    if (m_installed) return true;
    if (std::atomic_load(&g_state)) { why = "another VFG instance owns the compatibility hooks"; return false; }
    if (checkIdentity) {
        std::string hex; uint64_t size = 0;
        if (!Sha256::File(vfgPath, hex, size)) { why = "cannot read " + vfgPath; return false; }
        if (size != kKnownSize || hex != kKnownSha256) { why = "RTX 30 VFG compatibility does not recognise this VFG library (" + hex.substr(0, 12) + "..., " + std::to_string(size) + " bytes): left alone"; return false; }
    }
    auto s = std::make_shared<State>();
    s->attribute = reinterpret_cast<AttributeFn>(dlsym(cudaLib, "cuDeviceGetAttribute"));
    s->load = reinterpret_cast<LoadFn>(dlsym(cudaLib, "cuModuleLoadData"));
    s->contextDevice = reinterpret_cast<CtxDeviceFn>(dlsym(cudaLib, "cuCtxGetDevice"));
    s->device = device;
    if (!s->attribute || !s->load || !s->contextDevice) { why = "required CUDA exports missing"; return false; }
    if (!FindGotSlots(vfgHandle, { "cuDeviceGetAttribute", "cuModuleLoadData" }, m_slots, why)) return false;
    m_original.clear();
    for (const auto& slot : m_slots) {
        const uintptr_t cur = *reinterpret_cast<uintptr_t*>(slot.address);
        const uintptr_t want = slot.symbol == "cuDeviceGetAttribute" ? reinterpret_cast<uintptr_t>(s->attribute) : reinterpret_cast<uintptr_t>(s->load);
        // a lazily bound JUMP_SLOT still points into the PLT: that is fine (the hook replaces it), anything else foreign is not
        if (cur != want && slot.relro) { why = "the VFG library's " + slot.symbol + " slot is already redirected by another component"; return false; }
        m_original.push_back(cur);
    }
    std::atomic_store(&g_state, s);
    for (size_t i = 0; i < m_slots.size(); ++i) {
        const uintptr_t hook = m_slots[i].symbol == "cuDeviceGetAttribute" ? reinterpret_cast<uintptr_t>(&HookDeviceGetAttribute) : reinterpret_cast<uintptr_t>(&HookModuleLoadData);
        if (!WriteSlot(m_slots[i], hook)) {
            for (size_t j = 0; j < i; ++j) WriteSlot(m_slots[j], m_original[j]);
            std::atomic_store(&g_state, std::shared_ptr<State>());
            why = "cannot write the VFG library's GOT";
            return false;
        }
    }
    m_installed = true;
    return true;
}

bool AmpereCompat::Restore()
{
    std::lock_guard<std::mutex> lk(g_lock);
    if (!m_installed) return true;
    bool ok = true;
    for (size_t i = m_slots.size(); i-- > 0;) ok = WriteSlot(m_slots[i], m_original[i]) && ok;
    // the hooks keep the state alive if a restore failed (a call already in flight must still find it)
    if (ok) std::atomic_store(&g_state, std::shared_ptr<State>());
    m_installed = false;
    return ok;
}

unsigned AmpereCompat::PatchedLoads() { return g_patched.load(); }
unsigned AmpereCompat::AttributeSpoofs() { return g_spoofs.load(); }
unsigned AmpereCompat::RejectedLoads() { return g_rejected.load(); }

} // namespace pmvs::nvl

#else
namespace pmvs::nvl {
bool FindGotSlots(void*, const std::vector<std::string>&, std::vector<GotSlot>&, std::string& why) { why = "Linux only"; return false; }
bool AmpereCompat::Install(void*, const std::string&, void*, int, std::string& why, bool) { why = "the RTX 30 compatibility is Linux only here (Windows x64 uses pushcore's)"; return false; }
bool AmpereCompat::Restore() { return true; }
unsigned AmpereCompat::PatchedLoads() { return 0; }
unsigned AmpereCompat::AttributeSpoofs() { return 0; }
unsigned AmpereCompat::RejectedLoads() { return 0; }
} // namespace pmvs::nvl
#endif
