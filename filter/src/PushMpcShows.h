// PushMPC: per-show memory for the animation mode (anime round 5). A show is the series folder: the parent of a
// "Season 1" / "S01" / "Specials" folder, else the file's own folder. It is stored only on this PC, under
// HKCU\Software\PushMPC\Shows\<key>, where <key> is a hash of the folder path (never the name itself):
//   Class   DWORD  the picture class pushcore measured (1 feature, 2 TV limited, 3 mixed CG; PushCoreStats.animClass)
//   Choice  DWORD  the user's mode for this show (0 Full, 1 Camera only, 2 Off); absent = Auto
// Shows\Current (string) names the show of the file playing now, so the settings page can offer "this show".
#pragma once
#include <windows.h>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>

namespace pm {

inline bool IsSeasonFolder(const std::wstring& n)
{
    std::wstring s;
    for (wchar_t c : n) s.push_back(wchar_t(std::towlower(c)));
    if (s == L"specials" || s == L"extras" || s == L"ova" || s == L"ovas") return true;
    auto digits = [](const std::wstring& t, size_t from) { if (from >= t.size()) return false; for (size_t i = from; i < t.size(); ++i) if (!std::iswdigit(t[i])) return false; return true; };
    if (s.size() >= 2 && s[0] == L's' && digits(s, 1)) return true;                       // S01
    if (s.rfind(L"season", 0) == 0) { size_t i = 6; while (i < s.size() && (s[i] == L' ' || s[i] == L'_' || s[i] == L'.')) ++i; return digits(s, i); }
    return false;
}
// the show key of a media file: 16 hex digits of FNV-1a 64 over the lower-case series folder path; empty = no file
inline std::wstring ShowKeyFor(const std::wstring& file)
{
    if (file.empty()) return {};
    std::filesystem::path dir = std::filesystem::path(file).parent_path();
    if (dir.empty()) return {};
    if (IsSeasonFolder(dir.filename().wstring()) && dir.has_parent_path() && !dir.parent_path().filename().empty()) dir = dir.parent_path();
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : dir.wstring()) {
        const wchar_t l = c == L'/' ? L'\\' : wchar_t(std::towlower(c));
        h ^= uint64_t(l & 0xff); h *= 1099511628211ull; h ^= uint64_t(l >> 8); h *= 1099511628211ull;
    }
    wchar_t buf[17]; swprintf_s(buf, L"%016llx", static_cast<unsigned long long>(h));
    return buf;
}
inline constexpr const wchar_t* kShowsKey = L"Software\\PushMPC\\Shows";
struct ShowInfo { int cls = 0; int choice = -1; };
inline ShowInfo ReadShow(const std::wstring& key)
{
    ShowInfo s;
    if (key.empty()) return s;
    const std::wstring path = std::wstring(kShowsKey) + L"\\" + key;
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), L"Class", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && v <= 3) s.cls = int(v);
    sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), L"Choice", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && v <= 2) s.choice = int(v);
    return s;
}
inline void WriteShowDword(const std::wstring& key, const wchar_t* name, int value)   // value < 0 removes it
{
    if (key.empty()) return;
    const std::wstring path = std::wstring(kShowsKey) + L"\\" + key;
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (value < 0) RegDeleteValueW(k, name);
    else { const DWORD v = DWORD(value); RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)); }
    RegCloseKey(k);
}
inline void SetCurrentShow(const std::wstring& key)   // empty clears it
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kShowsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (key.empty()) RegDeleteValueW(k, L"Current");
    else RegSetValueExW(k, L"Current", 0, REG_SZ, reinterpret_cast<const BYTE*>(key.c_str()), DWORD((key.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}
inline std::wstring CurrentShow()
{
    wchar_t buf[32]{}; DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kShowsKey, L"Current", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return {};
    return buf;
}

}  // namespace pm
