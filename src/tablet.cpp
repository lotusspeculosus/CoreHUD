#define _CRT_RAND_S   // rand_s: fallback for the secret key
#include "tablet.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace tab {
namespace {

// The tablet page. Plain ES5 so it runs on old Android browsers / WebViews and old iPads.
// It asks for a frame at its exact pixel size, swaps two <img> elements (no flicker) and retries on errors.
const char* kPage = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="mobile-web-app-capable" content="yes"><meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent"><meta name="theme-color" content="#0b0e12">
<link rel="manifest" href="manifest.json?k=%KEY%"><link rel="icon" href="icon.png?k=%KEY%"><link rel="apple-touch-icon" href="icon.png?k=%KEY%">
<title>CoreHUD panel</title>
<style>
html,body{margin:0;padding:0;width:100%;height:100%;background:#0b0e12;overflow:hidden;-webkit-user-select:none;user-select:none}
img{position:absolute;left:0;top:0;width:100%;height:100%;display:block}
#s{position:absolute;left:0;right:0;top:40%;text-align:center;color:#aeb5bf;font:18px sans-serif}
#s small{color:#7d8590;font-size:14px}
</style></head><body>
<img id="a" alt=""><img id="b" alt="" style="visibility:hidden">
<div id="s">Connecting to CoreHUD&hellip;<br><small>Tap the screen for fullscreen</small></div>
<script>
var K='%KEY%',fails=0,front=document.getElementById('a'),back=document.getElementById('b'),st=document.getElementById('s');
function size(){var r=window.devicePixelRatio||1,w=Math.round(window.innerWidth*r),h=Math.round(window.innerHeight*r),m=2560;
 if(w>m){h=Math.round(h*m/w);w=m;} if(h>m){w=Math.round(w*m/h);h=m;} return [Math.max(w,160),Math.max(h,120)];}
function tick(){var s=size();
 back.onload=function(){back.style.visibility='visible';front.style.visibility='hidden';var t=front;front=back;back=t;fails=0;st.style.display='none';setTimeout(tick,450);};
 back.onerror=function(){fails++;if(fails>2){st.style.display='block';st.innerHTML='Reconnecting to CoreHUD&hellip;<br><small>Is CoreHUD running and the tablet panel switched on?</small>';}setTimeout(tick,1500);};
 back.src='frame.jpg?k='+K+'&w='+s[0]+'&h='+s[1]+'&t='+(new Date().getTime());}
// Keep the screen on: the Wake Lock API where the browser allows it (USB/localhost, newer browsers), plus a muted
// looping black video, which keeps the screen on in practically every browser - also over plain http (Wi-Fi).
// It fills the screen *behind* the panel images: browsers only hold the screen on for a visible, reasonably big video.
var v=document.createElement('video');v.muted=true;v.loop=true;
v.setAttribute('muted','');v.setAttribute('loop','');v.setAttribute('playsinline','');v.setAttribute('webkit-playsinline','');
v.style.cssText='position:absolute;left:0;top:0;width:100%;height:100%;object-fit:cover;background:#0b0e12;pointer-events:none';
v.onerror=function(){setTimeout(function(){v.src='keepawake.mp4?k='+K+'&r='+(new Date().getTime());play();},5000);};
v.src='keepawake.mp4?k='+K;document.body.insertBefore(v,document.body.firstChild);
function play(){try{var p=v.play();if(p&&p['catch'])p['catch'](function(){});}catch(e){}}
function lock(){try{if(navigator.wakeLock&&navigator.wakeLock.request){navigator.wakeLock.request('screen')['catch'](function(){});}}catch(e){}play();}
document.body.onclick=function(){var d=document.documentElement,f=d.requestFullscreen||d.webkitRequestFullscreen||d.mozRequestFullScreen||d.msRequestFullscreen;if(f){try{f.call(d);}catch(e){}}lock();};
document.addEventListener('visibilitychange',function(){if(!document.hidden)lock();});
setInterval(function(){if(v.paused)play();},10000);
// Connector alarm sound: CoreHUD says when to beep (the red overlay itself is part of the frame).
// Browsers only allow sound after a tap, so the first tap (fullscreen) also unlocks audio.
var ac=null,alarm=0;
function unlockAudio(){try{if(!ac){var C=window.AudioContext||window.webkitAudioContext;if(C)ac=new C();}if(ac&&ac.resume)ac.resume();}catch(e){}}
function beep(f,d){if(!ac)return;try{var o=ac.createOscillator(),g=ac.createGain();o.type='square';o.frequency.value=f;g.gain.value=0.25;
 o.connect(g);g.connect(ac.destination);var t=ac.currentTime;o.start(t);o.stop(t+d);}catch(e){}}
function pollAlarm(){var x=new XMLHttpRequest();x.onreadystatechange=function(){if(x.readyState==4){alarm=x.status==200?parseInt(x.responseText,10)||0:0;setTimeout(pollAlarm,1000);}};
 try{x.open('GET','alarm?k='+K+'&t='+(new Date().getTime()),true);x.send();}catch(e){setTimeout(pollAlarm,3000);}}
setInterval(function(){if(alarm){beep(1100,0.18);setTimeout(function(){beep(820,0.18);},230);}},700);
document.addEventListener('touchend',unlockAudio,false);document.addEventListener('click',unlockAudio,false);
pollAlarm();
lock();tick();
</script></body></html>)HTML";

std::string Replace(std::string s, const std::string& a, const std::string& b) {
    for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
    return s;
}

std::string QueryParam(const std::string& q, const char* name) {
    std::string key = std::string(name) + "=";
    size_t p = 0;
    while (p < q.size()) {
        size_t e = q.find('&', p);
        if (e == std::string::npos) e = q.size();
        if (q.compare(p, key.size(), key) == 0) return q.substr(p + key.size(), e - p - key.size());
        p = e + 1;
    }
    return {};
}

void SendAll(SOCKET s, const char* d, size_t n) {
    while (n) {
        int w = send(s, d, int(std::min<size_t>(n, 1 << 20)), 0);
        if (w <= 0) return;
        d += w;
        n -= size_t(w);
    }
}

void Respond(SOCKET s, int code, const char* status, const char* type, const void* body, size_t len) {
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                     code, status, type, len);
    SendAll(s, hdr, size_t(n));
    if (len) SendAll(s, static_cast<const char*>(body), len);
}

// Run a hidden process, capture its stdout+stderr (adb).
std::string Run(const std::wstring& cmd, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return {};
    }
    CloseHandle(wr);
    std::string out;
    char buf[4096];
    DWORD start = GetTickCount();
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            DWORD got = 0;
            if (ReadFile(rd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) && got) out.append(buf, got);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            DWORD got = 0;
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail && ReadFile(rd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) && got)
                out.append(buf, got);
            break;
        }
        if (GetTickCount() - start > timeoutMs) { TerminateProcess(pi.hProcess, 1); break; }
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return out;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

} // namespace

// ---------------- server ----------------
bool Server::Start(int port, bool lan, const std::string& key, std::string& err) {
    Stop();
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) { err = "Winsock unavailable"; return false; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = "cannot create socket"; return false; }
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(u_short(port));
    a.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);   // USB-only mode never listens on the network
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 8) != 0) {
        err = "port " + std::to_string(port) + " is in use - pick another port";
        closesocket(s);
        return false;
    }
    listen_ = uintptr_t(s);
    port_ = port;
    key_ = key;
    stop_ = false;
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void Server::Stop() {
    if (!running_ && !thread_.joinable()) return;
    stop_ = true;
    if (listen_ != ~uintptr_t(0)) closesocket(SOCKET(listen_));
    if (thread_.joinable()) thread_.join();
    listen_ = ~uintptr_t(0);
    running_ = false;
    std::lock_guard<std::mutex> lk(mx_);
    frame_.clear();
    peers_.clear();
    wantW_ = wantH_ = 0;
}

void Server::Loop() {
    while (!stop_) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(SOCKET(listen_), &rs);
        timeval tv{0, 300000};
        int r = select(0, &rs, nullptr, nullptr, &tv);
        if (stop_) break;
        if (r <= 0) continue;
        sockaddr_in peer{};
        int plen = sizeof peer;
        SOCKET c = accept(SOCKET(listen_), reinterpret_cast<sockaddr*>(&peer), &plen);
        if (c == INVALID_SOCKET) continue;
        char ip[64] = {};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        DWORD to = 3000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        Handle(uintptr_t(c), ip);
        shutdown(c, SD_BOTH);
        closesocket(c);
    }
}

void Server::Handle(uintptr_t sock, const std::string& peer) {
    SOCKET s = SOCKET(sock);
    std::string req;
    char buf[2048];
    while (req.size() < 8192 && req.find("\r\n\r\n") == std::string::npos) {
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) return;
        req.append(buf, size_t(n));
    }
    size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    std::string method = req.substr(0, sp1), target = req.substr(sp1 + 1, sp2 - sp1 - 1);
    if (method != "GET") { Respond(s, 405, "Method Not Allowed", "text/plain", "GET only", 8); return; }
    size_t qm = target.find('?');
    std::string path = target.substr(0, qm), query = qm == std::string::npos ? "" : target.substr(qm + 1);
    if (path == "/favicon.ico") { Respond(s, 404, "Not Found", "text/plain", "", 0); return; }
    if (QueryParam(query, "k") != key_) {
        const char* msg = "<!doctype html><meta name=viewport content='width=device-width'><body style='background:#0b0e12;color:#aeb5bf;"
                          "font:18px sans-serif;text-align:center;padding-top:20%'>CoreHUD: wrong or missing key.<br>"
                          "Scan the QR code in CoreHUD (Sensor panel &rsaquo; Tablet / phone) again.</body>";
        Respond(s, 403, "Forbidden", "text/html; charset=utf-8", msg, strlen(msg));
        return;
    }
    if (path == "/" || path == "/index.html") {
        std::string page = Replace(kPage, "%KEY%", key_);
        Respond(s, 200, "OK", "text/html; charset=utf-8", page.data(), page.size());
    } else if (path == "/frame.jpg") {
        int w = atoi(QueryParam(query, "w").c_str()), h = atoi(QueryParam(query, "h").c_str());
        std::vector<unsigned char> f;
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (w >= 160 && h >= 120 && w <= 2560 && h <= 2560) { wantW_ = w; wantH_ = h; }
            lastRequest_ = GetTickCount();
            bool known = false;
            for (auto& p : peers_) if (p.addr == peer) { p.tick = lastRequest_; known = true; }
            if (!known) peers_.push_back({peer, lastRequest_});
            f = frame_;
        }
        if (f.empty()) Respond(s, 503, "Service Unavailable", "text/plain", "starting", 8);
        else Respond(s, 200, "OK", "image/jpeg", f.data(), f.size());
    } else if (path == "/alarm") {
        char a[4];
        int n = snprintf(a, sizeof a, "%d", alarm_.load());
        Respond(s, 200, "OK", "text/plain", a, size_t(n));
    } else if (path == "/manifest.json") {
        std::string m = Replace(R"({"name":"CoreHUD panel","short_name":"CoreHUD","start_url":"/?k=%KEY%","display":"fullscreen",)"
                                R"("orientation":"any","background_color":"#0b0e12","theme_color":"#0b0e12",)"
                                R"("icons":[{"src":"icon.png?k=%KEY%","sizes":"192x192","type":"image/png"}]})",
                                "%KEY%", key_);
        Respond(s, 200, "OK", "application/manifest+json", m.data(), m.size());
    } else if (path == "/keepawake.mp4") {
        std::vector<unsigned char> vid;
        {
            std::lock_guard<std::mutex> lk(mx_);
            vid = video_;
        }
        if (vid.empty()) { Respond(s, 404, "Not Found", "text/plain", "", 0); return; }
        // Range: bytes=a-b  (media players ask for byte ranges)
        std::string lower = req;
        for (auto& c : lower) c = char(tolower(static_cast<unsigned char>(c)));
        const char kRange[] = "\r\nrange: bytes=";
        size_t rp = lower.find(kRange);
        if (rp != std::string::npos) {
            size_t a = rp + sizeof kRange - 1, dash = lower.find('-', a), eol = lower.find("\r\n", a);
            size_t total = vid.size(), from = 0, to = total - 1;
            if (dash != std::string::npos && dash < eol) {
                std::string sa = lower.substr(a, dash - a), sb = lower.substr(dash + 1, eol - dash - 1);
                if (!sa.empty()) from = size_t(strtoull(sa.c_str(), nullptr, 10));
                if (!sb.empty()) to = std::min(total - 1, size_t(strtoull(sb.c_str(), nullptr, 10)));
                if (sa.empty() && !sb.empty()) { from = total - std::min(total, size_t(strtoull(sb.c_str(), nullptr, 10))); to = total - 1; }
            }
            if (from > to || from >= total) {
                char h[160];
                int n = snprintf(h, sizeof h, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%zu\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", total);
                SendAll(s, h, size_t(n));
                return;
            }
            char h[320];
            int n = snprintf(h, sizeof h,
                             "HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\nContent-Length: %zu\r\nContent-Range: bytes %zu-%zu/%zu\r\n"
                             "Accept-Ranges: bytes\r\nCache-Control: max-age=3600\r\nConnection: close\r\n\r\n",
                             to - from + 1, from, to, total);
            SendAll(s, h, size_t(n));
            SendAll(s, reinterpret_cast<const char*>(vid.data() + from), to - from + 1);
            return;
        }
        Respond(s, 200, "OK", "video/mp4", vid.data(), vid.size());
    } else if (path == "/icon.png") {
        std::vector<unsigned char> ic;
        {
            std::lock_guard<std::mutex> lk(mx_);
            ic = icon_;
        }
        Respond(s, ic.empty() ? 404 : 200, ic.empty() ? "Not Found" : "OK", "image/png", ic.data(), ic.size());
    } else {
        Respond(s, 404, "Not Found", "text/plain", "not found", 9);
    }
}

bool Server::Wanted(int& w, int& h) {
    std::lock_guard<std::mutex> lk(mx_);
    w = wantW_;
    h = wantH_;
    return running_ && lastRequest_ && GetTickCount() - lastRequest_ < 5000 && w > 0;
}

void Server::SetFrame(std::vector<unsigned char>&& jpeg, int w, int h) {
    std::lock_guard<std::mutex> lk(mx_);
    frame_ = std::move(jpeg);
    frameW_ = w;
    frameH_ = h;
}

void Server::SetKeepAwakeVideo(std::vector<unsigned char>&& mp4) {
    std::lock_guard<std::mutex> lk(mx_);
    video_ = std::move(mp4);
}

void Server::SetIcon(std::vector<unsigned char>&& png) {
    std::lock_guard<std::mutex> lk(mx_);
    icon_ = std::move(png);
}

bool Server::LastSize(int& w, int& h) {
    std::lock_guard<std::mutex> lk(mx_);
    w = wantW_;
    h = wantH_;
    return w > 0 && h > 0;
}

int Server::Viewers() {
    std::lock_guard<std::mutex> lk(mx_);
    DWORD now = GetTickCount();
    int n = 0;
    for (auto& p : peers_) if (now - p.tick < 5000) ++n;
    return n;
}

// ---------------- helpers ----------------
std::vector<std::string> LocalIPv4() {
    std::vector<std::pair<int, std::string>> found;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (r != NO_ERROR) return {};
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::wstring desc = a->Description ? a->Description : L"";
        std::wstring lower = desc;
        for (auto& c : lower) c = wchar_t(towlower(c));
        bool virt = lower.find(L"virtual") != std::wstring::npos || lower.find(L"hyper-v") != std::wstring::npos ||
                    lower.find(L"vpn") != std::wstring::npos || lower.find(L"wsl") != std::wstring::npos;
        int score = (a->FirstGatewayAddress ? 2 : 0) + (virt ? 0 : 1) + (a->IfType == IF_TYPE_IEEE80211 || a->IfType == IF_TYPE_ETHERNET_CSMACD ? 1 : 0);
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char ip[64] = {};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
            if (!strncmp(ip, "169.254.", 8)) continue;   // link-local
            found.push_back({score, ip});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](auto& x, auto& y) { return x.first > y.first; });
    std::vector<std::string> out;
    for (auto& f : found) out.push_back(f.second);
    return out;
}

std::string RandomKey() {
    unsigned char b[8] = {};
    // the key is the panel's only protection on Wi-Fi: it must never come out predictable (all zeros)
    if (BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        for (int i = 0; i < 8; i += 4) {
            unsigned int r = 0;
            if (rand_s(&r) != 0) r = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ unsigned(reinterpret_cast<uintptr_t>(&r));
            memcpy(b + i, &r, 4);
        }
    }
    char hex[17] = {};
    for (int i = 0; i < 8; ++i) snprintf(hex + 2 * i, 3, "%02x", b[i]);
    return hex;
}

std::wstring FindAdb() {
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    wchar_t local[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%", local, MAX_PATH);
    std::wstring cands[] = {std::wstring(local) + L"\\Microsoft\\WinGet\\Links\\adb.exe", std::wstring(local) + L"\\Android\\Sdk\\platform-tools\\adb.exe"};
    for (auto& c : cands) if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    // winget portable package folder
    std::wstring base = std::wstring(local) + L"\\Microsoft\\WinGet\\Packages\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"Google.PlatformTools*").c_str(), &fd);
    std::wstring res;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring p = base + fd.cFileName + L"\\platform-tools\\adb.exe";
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) { res = p; break; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return res;
}

// ---------------- adb (USB) ----------------
void Adb::Start(int port, const std::string& key, bool stayOn) {
    Stop();
    port_ = port;
    key_ = key;
    stayOn_ = stayOn;
    stop_ = false;
    thread_ = std::thread([this] { Loop(); });
}

void Adb::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    // undo what we changed on the tablets
    std::vector<AdbDevice> devs;
    {
        std::lock_guard<std::mutex> lk(mx_);
        devs = devices_;
        devices_.clear();
    }
    for (auto& d : devs) {
        if (!d.reversed || adb_.empty()) continue;
        std::wstring s = L"\"" + adb_ + L"\" -s " + Widen(d.serial);
        Run(s + L" reverse --remove tcp:" + std::to_wstring(port_), 4000);
        if (stayOn_) Run(s + L" shell svc power stayon false", 4000);
    }
}

std::vector<AdbDevice> Adb::Devices() {
    std::lock_guard<std::mutex> lk(mx_);
    return devices_;
}
std::wstring Adb::AdbPath() {
    std::lock_guard<std::mutex> lk(mx_);
    return adb_;
}
std::string Adb::Status() {
    std::lock_guard<std::mutex> lk(mx_);
    return status_;
}
void Adb::OpenOnTablets() { reopen_ = true; }

void Adb::Loop() {
    DWORD lastFind = 0;
    while (!stop_) {
        std::wstring adb;
        {
            std::lock_guard<std::mutex> lk(mx_);
            adb = adb_;
        }
        if (adb.empty() && (GetTickCount() - lastFind > 10000 || !lastFind)) {
            lastFind = GetTickCount();
            adb = FindAdb();
            std::lock_guard<std::mutex> lk(mx_);
            adb_ = adb;
            if (adb.empty()) status_ = "adb not installed";
        }
        if (!adb.empty()) {
            std::string out = Run(L"\"" + adb + L"\" devices -l", 15000);
            std::vector<AdbDevice> now;
            std::istringstream in(out);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line.rfind("List of", 0) == 0 || line[0] == '*') continue;
                std::istringstream ls(line);
                AdbDevice d;
                ls >> d.serial >> d.state;
                if (d.serial.empty() || d.state.empty()) continue;
                std::string tok;
                while (ls >> tok) if (tok.rfind("model:", 0) == 0) { d.model = tok.substr(6); std::replace(d.model.begin(), d.model.end(), '_', ' '); }
                now.push_back(d);
            }
            std::vector<AdbDevice> prev;
            {
                std::lock_guard<std::mutex> lk(mx_);
                prev = devices_;
            }
            bool reopen = reopen_.exchange(false);
            for (auto& d : now) {
                for (auto& p : prev) if (p.serial == d.serial && p.state == "device") d.reversed = p.reversed;
                if (d.state != "device") continue;
                std::wstring s = L"\"" + adb + L"\" -s " + Widen(d.serial);
                bool fresh = !d.reversed;
                if (fresh) {
                    std::string r = Run(s + L" reverse tcp:" + std::to_wstring(port_) + L" tcp:" + std::to_wstring(port_), 8000);
                    d.reversed = r.find("error") == std::string::npos;
                    if (d.reversed && stayOn_) Run(s + L" shell svc power stayon usb", 6000);
                }
                if (d.reversed && (fresh || reopen))
                    Run(s + L" shell am start -a android.intent.action.VIEW -d \"http://localhost:" + std::to_wstring(port_) + L"/?k=" + Widen(key_) + L"\"", 8000);
            }
            std::lock_guard<std::mutex> lk(mx_);
            devices_ = now;
            int ok = 0;
            for (auto& d : now) ok += d.reversed;
            status_ = now.empty() ? "no tablet connected" : ok ? "connected" : "waiting for tablet";
        }
        for (int i = 0; i < 30 && !stop_ && !reopen_; ++i) Sleep(100);
    }
}

} // namespace tab
