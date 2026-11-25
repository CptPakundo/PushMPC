// The few OS calls of the CUDA / Video Effects engine, so the same engine serves Linux (x86-64) and Windows on Arm
// (NVIDIA RTX Spark: the win_arm64 nvidia-vfx wheel, CUDA 13), where pushcore has no build.
//   Linux:   dlopen / dlsym, libcuda.so.1, the runtime's nvvfx/libs/*.so
//   Windows: LoadLibraryExW (the runtime folder added with AddDllDirectory), nvcuda.dll from System32, *.dll
#pragma once
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif

namespace pmvs::nvl::os {

#ifdef _WIN32
inline std::wstring Wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
inline std::string LastError() { return "Windows error " + std::to_string(GetLastError()); }
// a library by absolute path (its folder is searched for its own dependencies), or a system one by name (System32)
inline void* Open(const std::string& path, std::string& why)
{
    const bool absolute = path.find(':') != std::string::npos || path.rfind("\\\\", 0) == 0;
    HMODULE h = absolute ? LoadLibraryExW(Wide(path).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
                         : LoadLibraryExW(Wide(path).c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) why = path + ": " + LastError();
    return h;
}
inline void* Sym(void* lib, const char* name) { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name)); }
inline bool FileExists(const std::string& p) { const DWORD a = GetFileAttributesW(Wide(p).c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
// the runtime's library folder joins the DLL search path (the effect DLLs load each other by name)
inline void AddLibraryDir(const std::string& dir) { AddDllDirectory(Wide(dir).c_str()); SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); }
inline constexpr const char* kDriver = "nvcuda.dll";
inline constexpr char kSep = '\\';
#else
inline void* Open(const std::string& path, std::string& why)
{
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!h) { const char* e = dlerror(); why = path + ": " + (e ? e : "dlopen failed"); }
    return h;
}
inline void* Sym(void* lib, const char* name) { return dlsym(lib, name); }
inline bool FileExists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
inline void AddLibraryDir(const std::string&) {}
inline constexpr const char* kDriver = "libcuda.so.1";
inline constexpr char kSep = '/';
#endif

// the runtime's file names (the wheels name the same libraries differently per OS)
struct Names { const char* image; const char* core; const char* vsr; const char* hdr; const char* vfg; };
#ifdef _WIN32
inline constexpr Names kNames = { "NVCVImage.dll", "NVVideoEffects.dll", "nvVFXVideoSuperRes.dll", "nvVFXTrueHDR.dll", "nvVFXVideoFrameGeneration.dll" };
#else
inline constexpr Names kNames = { "libNVCVImage.so", "libVideoFX.so", "libnvVFXVideoSuperRes.so", "libnvVFXTrueHDR.so", "libnvVFXVideoFrameGeneration.so" };
#endif

} // namespace pmvs::nvl::os
