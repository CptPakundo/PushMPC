// PushMPC installer and updater: downloads over WinHTTP (resumable, redirects, SHA-256) and file copies.
#include "pm_common.h"

#include <winhttp.h>
#include <shlwapi.h>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")

namespace pm {

static std::wstring g_ua = L"PushMPC";
void SetUserAgent(const std::wstring& ua) { g_ua = ua; }

bool IsHttpUrl(const std::wstring& u) { return StartsWithI(u, L"http://") || StartsWithI(u, L"https://"); }
bool IsFileUrl(const std::wstring& u) { return StartsWithI(u, L"file:"); }
std::wstring FileUrlToPath(const std::wstring& u) {
    if (!IsFileUrl(u)) return u;
    wchar_t buf[2048]; DWORD n = 2048;
    if (PathCreateFromUrlW(u.c_str(), buf, &n, 0) == S_OK) return buf;
    return u.substr(5);
}

namespace {
struct Http {
    HINTERNET s = nullptr, c = nullptr, r = nullptr;
    ~Http() { if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); if (s) WinHttpCloseHandle(s); }
};
// opens a GET request and receives the response headers; status out; rangeFrom > 0 adds a Range header
bool Open(Http& h, const std::wstring& url, uint64_t rangeFrom, DWORD& status, std::wstring& err) {
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[512]{}, path[4096]{};
    uc.lpszHostName = host; uc.dwHostNameLength = 512; uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    wchar_t extra[4096]{}; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = L"bad URL " + url; return false; }
    h.s = WinHttpOpen(g_ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.s) h.s = WinHttpOpen(g_ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.s) { err = L"WinHttpOpen: " + ErrorText(GetLastError()); return false; }
    WinHttpSetTimeouts(h.s, 15000, 15000, 30000, 30000);
    DWORD proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x2000 /* TLS 1.3 */;
    if (!WinHttpSetOption(h.s, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof(proto))) {
        proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(h.s, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof(proto));
    }
    h.c = WinHttpConnect(h.s, host, uc.nPort, 0);
    if (!h.c) { err = L"connect: " + ErrorText(GetLastError()); return false; }
    std::wstring object = std::wstring(path) + extra;
    h.r = WinHttpOpenRequest(h.c, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!h.r) { err = L"request: " + ErrorText(GetLastError()); return false; }
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(h.r, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
    std::wstring hdr = L"Accept: */*\r\n";
    if (StartsWithI(url, L"https://api.github.com/")) hdr = L"Accept: application/vnd.github+json\r\n";
    if (rangeFrom) hdr += Format(L"Range: bytes=%llu-\r\n", rangeFrom);
    if (!WinHttpSendRequest(h.r, hdr.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(h.r, nullptr)) {
        err = L"download: " + ErrorText(GetLastError()); return false;
    }
    DWORD sz = sizeof(status);
    WinHttpQueryHeaders(h.r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    return true;
}
uint64_t ContentLength(Http& h) {
    wchar_t buf[64]{}; DWORD sz = sizeof(buf);
    if (!WinHttpQueryHeaders(h.r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buf, &sz, WINHTTP_NO_HEADER_INDEX)) return 0;
    return _wcstoui64(buf, nullptr, 10);
}
uint64_t ContentRangeTotal(Http& h) {
    wchar_t buf[128]{}; DWORD sz = sizeof(buf);
    if (!WinHttpQueryHeaders(h.r, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, buf, &sz, WINHTTP_NO_HEADER_INDEX)) return 0;
    const wchar_t* slash = wcschr(buf, L'/');
    return slash ? _wcstoui64(slash + 1, nullptr, 10) : 0;
}
bool CheckSha(const std::wstring& file, const std::string& sha, std::atomic<bool>* cancel) {
    if (sha.empty()) return true;
    std::string h;
    return Sha256File(file, h, cancel) && _stricmp(h.c_str(), sha.c_str()) == 0;
}
}

DlResult Download(const std::wstring& url0, const std::wstring& dest, const std::string& sha256, const Progress& pr, std::wstring& err) {
    EnsureDir(ParentDir(dest));
    if (FileExists(dest) && !sha256.empty() && CheckSha(dest, sha256, pr.cancel)) {
        if (pr.onBytes) pr.onBytes(FileSize(dest), FileSize(dest));
        return DlResult::Ok;
    }
    const std::wstring part = dest + L".part";
    // ----- a file or file:// URL: copy with progress
    if (!IsHttpUrl(url0)) {
        std::wstring src = FileUrlToPath(url0);
        if (!FileExists(src)) { err = L"not found: " + src; return DlResult::Failed; }
        struct Ctx { const Progress* pr; } ctx{ &pr };
        BOOL cancelFlag = FALSE;
        auto cb = [](LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID p) -> DWORD {
            auto* c = static_cast<Ctx*>(p);
            if (c->pr->onBytes) c->pr->onBytes(uint64_t(done.QuadPart), uint64_t(total.QuadPart));
            return (c->pr->cancel && c->pr->cancel->load()) ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
        };
        if (!CopyFileExW(src.c_str(), part.c_str(), cb, &ctx, &cancelFlag, 0)) {
            DWORD e = GetLastError(); DeleteFileW(part.c_str());
            if (e == ERROR_REQUEST_ABORTED) return DlResult::Cancelled;
            err = L"copy " + src + L": " + ErrorText(e); return DlResult::Failed;
        }
    } else {
        // ----- http(s): resumable into <dest>.part
        int attempts = 0;
        for (;;) {
            if (pr.cancel && pr.cancel->load()) return DlResult::Cancelled;
            uint64_t have = FileSize(part);
            Http h; DWORD status = 0; std::wstring e;
            bool opened = Open(h, url0, have, status, e);
            if (opened && status == 416 && have) {
                // the part may already be complete; else start over
                if (CheckSha(part, sha256, pr.cancel) && !sha256.empty()) break;
                DeleteFileW(part.c_str()); continue;
            }
            if (!opened || (status != 200 && status != 206)) {
                if (opened) e = Format(L"HTTP %lu for ", status) + url0;
                if (opened && status >= 400 && status < 500 && status != 408 && status != 429) { err = e; return DlResult::Failed; }
                if (++attempts >= 4) { err = e; return DlResult::Failed; }
                Log::Writef(L"download retry %d: %s", attempts, e.c_str());
                Sleep(2000u * attempts);
                continue;
            }
            bool append = status == 206 && have;
            uint64_t total = append ? ContentRangeTotal(h) : ContentLength(h);
            if (!append) have = 0;
            HANDLE f = CreateFileW(part.c_str(), FILE_GENERIC_WRITE, FILE_SHARE_READ, nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) { err = L"cannot write " + part + L": " + ErrorText(GetLastError()); return DlResult::Failed; }
            if (append) { LARGE_INTEGER z{}; SetFilePointerEx(f, z, nullptr, FILE_END); }
            std::vector<char> buf(1 << 18);
            bool netErr = false;
            uint64_t done = have;
            ULONGLONG lastCb = 0;
            for (;;) {
                if (pr.cancel && pr.cancel->load()) { CloseHandle(f); return DlResult::Cancelled; }
                DWORD got = 0;
                if (!WinHttpReadData(h.r, buf.data(), DWORD(buf.size()), &got)) { netErr = true; e = L"read: " + ErrorText(GetLastError()); break; }
                if (!got) break;
                DWORD put = 0;
                if (!WriteFile(f, buf.data(), got, &put, nullptr) || put != got) { CloseHandle(f); err = L"write failed: " + ErrorText(GetLastError()); return DlResult::Failed; }
                done += got;
                ULONGLONG now = GetTickCount64();
                if (pr.onBytes && now - lastCb > 100) { pr.onBytes(done, total); lastCb = now; }
            }
            CloseHandle(f);
            if (pr.onBytes) pr.onBytes(done, total);
            if (netErr || (total && done < total)) {
                if (++attempts >= 6) { err = netErr ? e : L"the connection closed early"; return DlResult::Failed; }
                Log::Writef(L"download interrupted at %llu of %llu bytes, resuming (%s)", done, total, e.c_str());
                Sleep(1500u * attempts);
                continue;
            }
            break;
        }
    }
    if (!CheckSha(part, sha256, pr.cancel)) {
        if (pr.cancel && pr.cancel->load()) return DlResult::Cancelled;
        DeleteFileW(part.c_str());
        err = L"the download does not match its SHA-256 (" + Widen(sha256) + L"), so it was deleted";
        return DlResult::Failed;
    }
    if (!MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) { err = L"rename: " + ErrorText(GetLastError()); return DlResult::Failed; }
    return DlResult::Ok;
}

bool Fetch(const std::wstring& url, std::string& out, std::wstring& err, size_t maxBytes) {
    out.clear();
    if (!IsHttpUrl(url)) {
        std::wstring p = FileUrlToPath(url);
        if (!ReadFileBytes(p, out)) { err = L"cannot read " + p; return false; }
        return true;
    }
    for (int attempt = 1;; attempt++) {
        Http h; DWORD status = 0;
        if (!Open(h, url, 0, status, err)) { if (attempt >= 3) return false; Sleep(1500); continue; }
        if (status != 200) {
            err = Format(L"HTTP %lu for ", status) + url;
            if (status >= 500 && attempt < 3) { Sleep(1500); continue; }
            return false;
        }
        char buf[16384];
        DWORD got = 0;
        while (WinHttpReadData(h.r, buf, sizeof(buf), &got) && got) {
            out.append(buf, got);
            if (out.size() > maxBytes) { err = L"response too large: " + url; return false; }
        }
        return true;
    }
}

std::wstring ResolveUrl(const std::wstring& feedUrl, const Release& rel, const std::string& url) {
    std::wstring u = Widen(url);
    if (IsHttpUrl(u) || IsFileUrl(u) || (u.size() > 2 && (u[1] == L':' || (u[0] == L'\\' && u[1] == L'\\')))) return u;
    if (IsHttpUrl(feedUrl) && !rel.tag.empty() && !rel.repository.empty()) {
        std::wstring repo = Widen(rel.repository);
        while (!repo.empty() && repo.back() == L'/') repo.pop_back();
        return repo + L"/releases/download/" + Widen(rel.tag) + L"/" + u;
    }
    std::wstring base = feedUrl;
    size_t i = base.find_last_of(L"/\\");
    base = i == std::wstring::npos ? L"" : base.substr(0, i + 1);
    if (IsHttpUrl(base) || IsFileUrl(base)) return base + u;
    return JoinPath(base.empty() ? L"." : base.substr(0, base.size() - 1), u);
}

} // namespace pm
