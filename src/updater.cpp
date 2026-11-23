#include "updater.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <sstream>
#include "json.h"
#include "monitor.h"
#include "version.h"

namespace upd {
namespace {

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

bool AllowedHost(const std::wstring& host) {
    std::wstring h = host;
    for (auto& c : h) c = wchar_t(towlower(c));
    auto ends = [&](const wchar_t* suf) {
        size_t n = wcslen(suf);
        return h.size() >= n && h.compare(h.size() - n, n, suf) == 0;
    };
    return h == L"api.github.com" || h == L"github.com" || ends(L".githubusercontent.com");
}

// HTTPS GET (follows redirects, GitHub hosts only). maxBytes guards against runaway downloads.
bool HttpsGet(const std::string& url, std::vector<unsigned char>& out, std::string& err, size_t maxBytes, bool apiJson) {
    std::wstring wurl = W(url);
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS || !AllowedHost(host)) {
        err = "refusing non-GitHub or non-HTTPS URL";
        return false;
    }
    std::wstring ua = L"RigSense/" RIGSENSE_VERSION;
    HINTERNET s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = "WinHTTP unavailable"; return false; }
    WinHttpSetTimeouts(s, 5000, 10000, 15000, 30000);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    std::wstring obj = std::wstring(path) + extra;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", obj.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    const wchar_t* hdr = apiJson ? L"Accept: application/vnd.github+json\r\n" : L"Accept: application/octet-stream\r\n";
    if (r && WinHttpSendRequest(r, hdr, DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        // after redirects, make sure we still ended up on a GitHub host
        wchar_t finalUrl[4096] = {};
        DWORD fl = sizeof finalUrl;
        if (WinHttpQueryOption(r, WINHTTP_OPTION_URL, finalUrl, &fl)) {
            URL_COMPONENTS fc{sizeof fc};
            wchar_t fh[256] = {};
            fc.lpszHostName = fh;
            fc.dwHostNameLength = 255;
            if (!WinHttpCrackUrl(finalUrl, 0, 0, &fc) || fc.nScheme != INTERNET_SCHEME_HTTPS || !AllowedHost(fh)) {
                err = "redirected to an unexpected host";
                goto done;
            }
        }
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) { err = "HTTP " + std::to_string(status); goto done; }
        out.clear();
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(r, &avail) && avail) {
            size_t off = out.size();
            if (off + avail > maxBytes) { err = "download too large"; goto done; }
            out.resize(off + avail);
            DWORD got = 0;
            if (!WinHttpReadData(r, out.data() + off, avail, &got)) { err = "read failed"; goto done; }
            out.resize(off + got);
        }
        ok = !out.empty();
        if (!ok) err = "empty response";
    } else if (err.empty()) {
        err = "no connection to GitHub (error " + std::to_string(GetLastError()) + ")";
    }
done:
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

bool WriteFileAll(const std::wstring& path, const std::vector<unsigned char>& d) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, d.data(), DWORD(d.size()), &w, nullptr) && w == d.size();
    CloseHandle(h);
    return ok;
}

bool LooksLikeExe(const std::vector<unsigned char>& d) {
    if (d.size() < 1024 || d[0] != 'M' || d[1] != 'Z') return false;
    uint32_t pe = uint32_t(d[0x3C]) | (uint32_t(d[0x3D]) << 8) | (uint32_t(d[0x3E]) << 16) | (uint32_t(d[0x3F]) << 24);
    return pe + 4 < d.size() && d[pe] == 'P' && d[pe + 1] == 'E' && d[pe + 2] == 0 && d[pe + 3] == 0;
}

std::wstring UpdateDir() { return mon::ExeDir() + L"\\update"; }

} // namespace

std::string Sha256Hex(const std::vector<unsigned char>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[32] = {};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(h, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0) == 0 && BCryptFinishHash(h, digest, 32, 0) == 0) {
                char b[3];
                for (unsigned char x : digest) { snprintf(b, sizeof b, "%02x", x); hex += b; }
            }
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return hex;
}

std::string ExpectedSha(const std::string& text, const std::string& fileName) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        size_t np = line.find_first_not_of(" \t*", sp);
        if (np == std::string::npos) continue;   // "<hash> *" with no file name
        std::string hash = line.substr(0, sp), name = line.substr(np);
        if (name != fileName || hash.size() != 64) continue;   // exact file name match (no "RigSense.exe.old" etc.)
        bool hex = true;
        for (auto& ch : hash) { ch = char(tolower(static_cast<unsigned char>(ch))); hex = hex && isxdigit(static_cast<unsigned char>(ch)); }
        if (hex) return hash;
    }
    return {};
}

int CompareVersions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& s) {
        std::vector<int> v;
        size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
        while (i < s.size() && v.size() < 4) {
            int n = 0;
            bool any = false;
            while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) { n = n * 10 + (s[i] - '0'); ++i; any = true; }
            if (!any) break;
            v.push_back(n);
            if (i < s.size() && s[i] == '.') ++i; else break;
        }
        while (v.size() < 3) v.push_back(0);
        return v;
    };
    auto x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

bool FetchLatest(const std::string& repo, Release& out, std::string& err) {
    std::vector<unsigned char> body;
    if (!HttpsGet("https://api.github.com/repos/" + repo + "/releases/latest", body, err, 2 * 1024 * 1024, true)) return false;
    json::JVal root;
    if (!json::Parse(std::string(body.begin(), body.end()), root) || root.t != json::JVal::Obj) { err = "unexpected reply from GitHub"; return false; }
    Release r;
    if (const json::JVal* t = root.get("tag_name")) r.tag = t->str;
    if (const json::JVal* u = root.get("html_url")) r.pageUrl = u->str;
    if (const json::JVal* b = root.get("body")) {
        std::string n = b->str.substr(0, 600);
        r.notes = W(n);
    }
    if (r.tag.empty()) { err = "release has no tag"; return false; }
    r.version = (r.tag[0] == 'v' || r.tag[0] == 'V') ? r.tag.substr(1) : r.tag;
    if (const json::JVal* assets = root.get("assets"))
        for (auto& a : assets->arr) {
            const json::JVal* n = a.get("name");
            const json::JVal* u = a.get("browser_download_url");
            if (!n || !u) continue;
            if (n->str == "RigSense.exe") {
                r.exeUrl = u->str;
                const json::JVal* d = a.get("digest");   // GitHub: "sha256:<64 hex>"
                if (d && d->t == json::JVal::Str && d->str.rfind("sha256:", 0) == 0 && d->str.size() == 7 + 64) {
                    r.exeSha = d->str.substr(7);
                    for (auto& ch : r.exeSha) ch = char(tolower(static_cast<unsigned char>(ch)));
                }
            } else if (n->str == "SHA256SUMS.txt") r.sumsUrl = u->str;
        }
    out = r;
    return true;
}

bool Stage(const Release& r, std::string& err) {
    if (r.exeUrl.empty()) { err = "release has no RigSense.exe (update manually from the release page)"; return false; }
    // expected hash: GitHub's own digest for the asset, else the release's SHA256SUMS.txt
    std::string expected = r.exeSha;
    if (expected.empty()) {
        if (r.sumsUrl.empty()) { err = "release has no checksum - not installed (update manually from the release page)"; return false; }
        std::vector<unsigned char> sums;
        if (!HttpsGet(r.sumsUrl, sums, err, 64 * 1024, false)) { err = "checksums: " + err; return false; }
        expected = ExpectedSha(std::string(sums.begin(), sums.end()), "RigSense.exe");
    }
    std::vector<unsigned char> exe;
    if (!HttpsGet(r.exeUrl, exe, err, 64 * 1024 * 1024, false)) { err = "RigSense.exe: " + err; return false; }
    if (expected.size() != 64 || Sha256Hex(exe) != expected) { err = "RigSense.exe failed the SHA-256 check - not installed"; return false; }
    if (!LooksLikeExe(exe)) { err = "downloaded file is not a valid program - not installed"; return false; }
    std::wstring dir = UpdateDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!WriteFileAll(dir + L"\\RigSense.exe", exe)) { err = "cannot write to the RigSense folder (move RigSense to a folder you can write to, e.g. not Program Files)"; return false; }
    std::vector<unsigned char> ver(r.version.begin(), r.version.end());
    WriteFileAll(dir + L"\\version.txt", ver);
    return true;
}

bool IsStaged(const std::string& version) {
    std::wstring p = UpdateDir() + L"\\version.txt";
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    char b[32] = {};
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    return std::string(b, n) == version && GetFileAttributesW((UpdateDir() + L"\\RigSense.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ApplyStaged(std::string& err) {
    std::wstring dir = mon::ExeDir(), up = UpdateDir();
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe = self, old = exe + L".old";
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { err = "cannot rename the running program (folder not writable?)"; return false; }
    if (!MoveFileExW((up + L"\\RigSense.exe").c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);   // roll back
        err = "cannot move the new program into place";
        return false;
    }
    DeleteFileW((up + L"\\version.txt").c_str());
    RemoveDirectoryW(up.c_str());
    std::wstring cmd = L"\"" + exe + L"\" --updated --wait-pid " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        MoveFileExW(exe.c_str(), (up + L"\\RigSense.exe").c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        err = "cannot start the new version";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void CleanupOld() {
    std::wstring dir = mon::ExeDir();
    // just updated from 1.3.0 or older: the separate console exe is replaced by "RigSense.exe --cli"
    if (GetFileAttributesW((dir + L"\\RigSense.exe.old").c_str()) != INVALID_FILE_ATTRIBUTES) DeleteFileW((dir + L"\\rigsense-cli.exe").c_str());
    DeleteFileW((dir + L"\\RigSense.exe.old").c_str());
    DeleteFileW((dir + L"\\rigsense-cli.exe.old").c_str());
}

} // namespace upd
