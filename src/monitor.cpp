#include "monitor.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace mon {
namespace {

constexpr size_t kMaxHistory = 4000;   // > 30 min at 500 ms
constexpr size_t kMaxEvents = 500;
constexpr double kHyst = 0.3;          // A

std::mutex g_mx;
std::condition_variable g_cv;
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_resetPeaks{false};
std::atomic<int> g_csvRequest{-1};     // -1 none, 0 off, 1 on
HWND g_notify = nullptr;
UINT g_msg = 0;

Snapshot g_snap;
std::deque<HistPoint> g_hist;
std::deque<Event> g_events;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring NowStr() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[32];
    swprintf(b, 32, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return b;
}

uint64_t g_eventSeq = 0;

void AddEvent(int level, const std::wstring& text) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_events.push_back({++g_eventSeq, NowStr(), level, text});
    while (g_events.size() > kMaxEvents) g_events.pop_front();
}

struct Csv {
    FILE* f = nullptr;
    std::wstring path;
    int pending = 0;
    void Open() {
        if (f) return;
        std::wstring dir = ExeDir() + L"\\logs";
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t name[64];
        swprintf(name, 64, L"\\rigsense_%04u%02u%02u_%02u%02u%02u.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        path = dir + name;
        f = _wfopen(path.c_str(), L"w");
        if (!f) { path.clear(); return; }
        fprintf(f, "time,pins_ok");
        for (int i = 1; i <= 6; ++i) fprintf(f, ",p%d_V", i);
        for (int i = 1; i <= 6; ++i) fprintf(f, ",p%d_A", i);
        fprintf(f, ",connector_W,total_A,board_W,gpu_util,core_mhz,core_C,hotspot_C,vram_C\n");
    }
    void Close() { if (f) { fclose(f); f = nullptr; } }
    void Write(const Snapshot& s, double connW, double totalA) {
        if (!f) return;
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u,%d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, s.pinsOk ? 1 : 0);
        for (int i = 0; i < 6; ++i) fprintf(f, ",%.3f", s.pinsOk ? s.volts[i] : 0.0);
        for (int i = 0; i < 6; ++i) fprintf(f, ",%.3f", s.pinsOk ? s.amps[i] : 0.0);
        fprintf(f, ",%.1f,%.3f,%.1f,%d,%d,%d,%.1f,%.1f\n", connW, totalA, s.stats.powerW, s.stats.gpuUtil,
                s.stats.coreMHz, s.stats.tempC, s.hotspotC, s.vramC);
        if (++pending >= 10) { fflush(f); pending = 0; }
    }
};

void ThreadMain(Config cfg) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    Snapshot s;
    s.cfg = cfg;
    s.started = true;

    std::string err;
    s.nvapiOk = nvapi::Init(err);
    if (!err.empty()) s.initError = err;
    nvapi::Gpu gpu;
    if (s.nvapiOk) {
        auto gpus = nvapi::EnumGpus();
        int pick = -1;
        if (cfg.gpuIndex >= 0 && cfg.gpuIndex < int(gpus.size())) pick = cfg.gpuIndex;
        for (size_t i = 0; pick < 0 && i < gpus.size(); ++i)
            if (nvapi::IsKnownAstral(gpus[i].subSystemId)) pick = int(i);
        if (pick < 0 && !gpus.empty()) pick = 0;
        if (pick >= 0) {
            gpu = gpus[size_t(pick)];
            s.haveGpu = true;
            s.gpuName = gpu.name;
            s.deviceId = gpu.deviceId;
            s.subSystemId = gpu.subSystemId;
            s.busId = gpu.busId;
            s.knownCard = nvapi::IsKnownAstral(gpu.subSystemId, &s.model);
        }
    }
    std::string nvmlErr;
    s.nvmlOk = nvml::Init(nvmlErr);
    nvml::Device dev = s.nvmlOk && s.haveGpu ? nvml::FindByBus(gpu.busId) : nullptr;
    if (!dev) s.nvmlOk = false;

    // Per-pin sources: the card's own sensor (ROG Astral) first, otherwise an external meter
    // (WireView Pro II / PMD2X / MSI GPU Safeguard+ PSU), which works with any GPU brand.
    const bool astralAvail = s.haveGpu && (s.knownCard || cfg.force);
    const bool useAstral = astralAvail && (cfg.pinSource == "auto" || cfg.pinSource == "astral");
    pins::Sources ext;
    ext.SetPreferred(useAstral || cfg.pinSource == "astral" ? "off" : cfg.pinSource);
    bool extSeen = false;
    uint16_t lastFaults = 0;
    s.sensorEnabled = useAstral;

    {
        wchar_t m[256];
        // Informational only: AMD/Intel GPUs and cards without a pin sensor are normal, not errors.
        if (s.haveGpu) {
            swprintf(m, 256, L"Monitoring %hs (subsystem %04X:%04X)", s.gpuName.c_str(), s.subSystemId & 0xFFFF, s.subSystemId >> 16);
            AddEvent(0, m);
            if (!useAstral)
                AddEvent(0, L"This card has no known per-pin connector sensor - looking for an external per-pin meter "
                            L"(WireView Pro II, PMD2X, MSI Ai1300TS/Ai1600TS)");
            if (!s.nvmlOk) AddEvent(1, L"NVIDIA NVML unavailable - GPU stats fall back to Windows counters");
        } else {
            AddEvent(0, L"No NVIDIA GPU - GPU stats come from Windows counters (works for AMD and Intel). "
                        L"Looking for an external per-pin meter (WireView Pro II, PMD2X, MSI Ai1300TS/Ai1600TS).");
        }
    }

    Csv csv;
    if (cfg.csv) csv.Open();
    bool lastPinsOk = true;
    bool firstRead = true;
    int spreadState = 0;
    std::chrono::steady_clock::time_point critSince{};
    bool critActive = false, shutdownIssued = false;

    while (!g_stop) {
        auto tStart = clock::now();

        if (g_resetPeaks.exchange(false)) {
            for (double& p : s.peakA) p = 0;
            s.peakConnW = 0;
            s.minVolts = 0;
            AddEvent(0, L"Peaks reset");
        }
        int csvReq = g_csvRequest.exchange(-1);
        if (csvReq == 1 && !csv.f) { csv.Open(); AddEvent(0, csv.f ? L"CSV logging started: " + csv.path : L"Could not create CSV log"); }
        if (csvReq == 0 && csv.f) { csv.Close(); AddEvent(0, L"CSV logging stopped"); }
        s.csvOn = csv.f != nullptr;
        s.csvPath = csv.path;

        // --- per-pin sensor (on-card or external meter) ---
        bool attempted = false, ok = false;
        std::string perr;
        auto r0 = clock::now();
        if (useAstral) {
            attempted = true;
            nvapi::PinReading pr;
            ok = nvapi::ReadPins(gpu.handle, uint8_t(cfg.i2cPort), uint8_t(cfg.i2cAddr), pr, perr);
            s.pinSource = L"ROG Astral on-card sensor (IT8915FN)";
            s.pinSourceId = "astral";
            if (ok) {
                s.pinCount = 6;
                s.perPinVolts = true;
                std::copy(pr.volts, pr.volts + 6, s.volts);
                std::copy(pr.amps, pr.amps + 6, s.amps);
            }
        } else if (cfg.pinSource != "off" && cfg.pinSource != "astral") {
            pins::Reading r;
            ok = ext.Read(r, perr);
            if (ext.Present() || ok) {
                if (!extSeen) AddEvent(0, L"External per-pin meter found: " + ext.Name());
                extSeen = true;
                s.pinSource = ext.Name();
                s.pinSourceId = ext.Id();
            }
            attempted = extSeen;   // once a meter was seen, losing it is an alarm, not "no sensor"
            if (!ok && extSeen && !ext.Present()) perr = "meter disconnected or taken by another app (" + perr + ")";
            if (ok) {
                s.pinCount = r.pinCount;
                s.perPinVolts = r.perPinVolts;
                std::copy(r.volts, r.volts + pins::kMaxPins, s.volts);
                std::copy(r.amps, r.amps + pins::kMaxPins, s.amps);
                s.pinExtras = r.extras;
                if (r.activeFaults != lastFaults) {
                    if (r.activeFaults) {
                        wchar_t m[160];
                        swprintf(m, 160, L"%s reports an active fault (code 0x%04X) - check the connector", ext.Name().c_str(), r.activeFaults);
                        AddEvent(2, m);
                    } else AddEvent(0, L"Per-pin meter fault cleared");
                    lastFaults = r.activeFaults;
                }
            }
        }
        s.sensorEnabled = attempted;
        if (attempted) {
            s.readMs = std::chrono::duration<double, std::milli>(clock::now() - r0).count();
            s.pinsOk = ok;
            if (ok) s.pinError.clear();
            else { s.pinError = perr; ++s.failures; }
            if (ok != lastPinsOk || firstRead) {
                if (!ok) AddEvent(2, L"Per-pin sensor read failed: " + Widen(perr));
                else if (!firstRead) AddEvent(0, L"Per-pin sensor readings recovered");
                else AddEvent(0, L"Per-pin monitoring active: " + s.pinSource);
                lastPinsOk = ok;
            }
            firstRead = false;
        } else {
            s.pinsOk = false;
        }
        for (int i = s.pinCount; i < pins::kMaxPins; ++i) { s.pinState[i] = 0; s.amps[i] = 0; s.volts[i] = 0; }

        // --- GPU stats ---
        if (s.nvmlOk) nvml::Read(dev, s.stats);
        if (s.haveGpu) {
            double v;
            s.coreV = nvapi::GetCoreVoltage(gpu.handle, v) ? v : -1;
            double hot = -1, vram = -1;
            if (nvapi::GetExtraTemps(gpu.handle, hot, vram)) { s.hotspotC = hot; s.vramC = vram; }
            else { s.hotspotC = s.vramC = -1; }
        }

        // --- derived values, thresholds ---
        double connW = 0, totalA = 0;
        if (s.pinsOk) {
            double minV = 1e9, spread = 0;
            for (int c = 0; c < s.pinCount / 6; ++c) {   // imbalance is judged per connector
                double lo = 1e9, hi = -1;
                for (int i = c * 6; i < c * 6 + 6; ++i) { lo = std::min(lo, s.amps[i]); hi = std::max(hi, s.amps[i]); }
                spread = std::max(spread, hi - lo);
            }
            for (int i = 0; i < s.pinCount; ++i) {
                connW += s.volts[i] * s.amps[i];
                totalA += s.amps[i];
                s.peakA[i] = std::max(s.peakA[i], s.amps[i]);
                minV = std::min(minV, s.volts[i]);
                wchar_t pinName[32];
                if (i < 6) swprintf(pinName, 32, L"Pin %d", i + 1);
                else swprintf(pinName, 32, L"Connector 2 pin %d", i - 5);

                int st = s.pinState[i], a = 0;
                double amp = s.amps[i];
                if (amp >= cfg.limitA || (st == 2 && amp > cfg.limitA - kHyst)) a = 2;
                else if (amp >= cfg.warnA || (st >= 1 && amp > cfg.warnA - kHyst)) a = 1;
                if (a != st) {
                    wchar_t m[160];
                    if (a == 2) swprintf(m, 160, L"%s at %.2f A - above the %.1f A limit", pinName, amp, cfg.limitA);
                    else if (a == 1 && st == 0) swprintf(m, 160, L"%s at %.2f A - above the %.1f A warning level", pinName, amp, cfg.warnA);
                    else if (a == 1) swprintf(m, 160, L"%s back below the limit (%.2f A)", pinName, amp);
                    else swprintf(m, 160, L"%s back to normal (%.2f A)", pinName, amp);
                    AddEvent(a == 2 ? 2 : (a > st ? 1 : 0), m);
                    if (a == 2 && cfg.beep) MessageBeep(MB_ICONHAND);
                    s.pinState[i] = a;
                }
            }
            // Emergency action: a pin stays above the limit for criticalSeconds.
            bool anyCrit = false;
            for (int st : s.pinState) anyCrit |= st == 2;
            if (anyCrit && !critActive) { critActive = true; critSince = clock::now(); }
            if (!anyCrit) critActive = false;
            if (critActive && cfg.criticalAction == 1 && !shutdownIssued &&
                clock::now() - critSince >= std::chrono::seconds(cfg.criticalSeconds)) {
                shutdownIssued = true;
                wchar_t cmd[] = L"shutdown.exe /s /t 60 /c \"RigSense: a 12V-2x6 pin stayed above its current limit. "
                                L"Shutting down in 60 s to protect the connector. Run 'shutdown /a' to cancel.\"";
                STARTUPINFOW si{sizeof si};
                PROCESS_INFORMATION pi{};
                if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    AddEvent(2, L"EMERGENCY: pin over limit for " + std::to_wstring(cfg.criticalSeconds) +
                                    L" s - Windows shutdown started (60 s). Cancel with: shutdown /a");
                } else {
                    AddEvent(2, L"EMERGENCY: could not start the shutdown command");
                }
            }
            s.peakConnW = std::max(s.peakConnW, connW);
            s.minVolts = s.minVolts == 0 ? minV : std::min(s.minVolts, minV);
            int sp = (spread >= cfg.spreadWarnA || (spreadState && spread > cfg.spreadWarnA - kHyst)) ? 1 : 0;
            if (sp != spreadState) {
                wchar_t m[160];
                if (sp) swprintf(m, 160, L"Pin imbalance: spread %.2f A (warning at %.1f A)", spread, cfg.spreadWarnA);
                else swprintf(m, 160, L"Pin imbalance cleared (spread %.2f A)", spread);
                AddEvent(sp ? 1 : 0, m);
                spreadState = sp;
            }
        }
        ++s.samples;
        if (csv.f) csv.Write(s, connW, totalA);

        HistPoint hp;
        hp.t = std::chrono::duration<double>(clock::now() - t0).count();
        hp.ok = s.pinsOk;
        for (int i = 0; i < 6; ++i) { hp.amps[i] = float(s.amps[i]); hp.volts[i] = float(s.volts[i]); }
        hp.connW = float(connW);
        hp.boardW = float(s.stats.powerW);

        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_snap = s;
            g_hist.push_back(hp);
            while (g_hist.size() > kMaxHistory) g_hist.pop_front();
        }
        if (g_notify) PostMessageW(g_notify, g_msg, 0, 0);

        std::unique_lock<std::mutex> lk(g_mx);
        g_cv.wait_until(lk, tStart + std::chrono::milliseconds(std::max(100, cfg.pollMs)), [] { return g_stop.load(); });
    }
    csv.Close();
    nvml::Shutdown();
    nvapi::Shutdown();
}

} // namespace

std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}

std::wstring DefaultIniPath() { return ExeDir() + L"\\RigSense.ini"; }

Config LoadConfig(const std::wstring& ini) {
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = _wfopen(ini.c_str(), L"w");
        if (f) {
            fputs("; RigSense settings. Press \"Restart sensor\" in the app after editing.\n"
                  "[Monitor]\n"
                  "; sample interval in milliseconds (min 100)\n"
                  "PollMs=500\n"
                  "; per-pin rated current in amps (12V-2x6 terminal rating ~9.2 A)\n"
                  "LimitA=9.2\n"
                  "; per-pin warning level in amps\n"
                  "WarnA=8.0\n"
                  "; warn when (highest pin - lowest pin) exceeds this, amps\n"
                  "SpreadWarnA=2.5\n"
                  "; beep when a pin reaches the limit (1/0)\n"
                  "Beep=1\n"
                  "; start with CSV logging on (logs\\ folder next to the exe)\n"
                  "Csv=0\n"
                  "; what to do when a pin stays above LimitA for CriticalSeconds:\n"
                  ";   notify   = beep + Windows notification (default)\n"
                  ";   shutdown = also start a 60 s Windows shutdown countdown (cancel with: shutdown /a)\n"
                  "CriticalAction=notify\n"
                  "CriticalSeconds=5\n"
                  "\n[Sensor]\n"
                  "; -1 = auto-pick the first known ROG Astral card\n"
                  "GpuIndex=-1\n"
                  "; NVAPI I2C port and 7-bit address of the IT8915FN pin monitor\n"
                  "I2CPort=1\n"
                  "I2CAddr=0x2B\n"
                  "; 1 = also read on cards not in the known ROG Astral list (read-only, at your own risk)\n"
                  "Force=0\n"
                  "; per-pin source: auto (card sensor, else external meter) | astral | wireview | pmd2x | msipsu | off\n"
                  "PinSource=auto\n"
                  "; LibreHardwareMonitor web server port (enable it in LHM: Options > Remote Web Server > Run)\n"
                  "LhmPort=8085\n", f);
            fclose(f);
        }
    }
    auto num = [&](const wchar_t* sec, const wchar_t* key, double def) {
        wchar_t b[64];
        GetPrivateProfileStringW(sec, key, L"", b, 64, ini.c_str());
        if (!b[0]) return def;
        wchar_t* end = nullptr;
        double v = (b[0] == L'0' && (b[1] == L'x' || b[1] == L'X')) ? double(wcstol(b, &end, 16)) : wcstod(b, &end);
        return end == b ? def : v;
    };
    Config c;
    c.pollMs = std::clamp(int(num(L"Monitor", L"PollMs", c.pollMs)), 100, 10000);
    c.limitA = std::clamp(num(L"Monitor", L"LimitA", c.limitA), 1.0, 20.0);
    c.warnA = std::clamp(num(L"Monitor", L"WarnA", c.warnA), 0.5, c.limitA);
    c.spreadWarnA = std::clamp(num(L"Monitor", L"SpreadWarnA", c.spreadWarnA), 0.1, 20.0);
    c.beep = num(L"Monitor", L"Beep", 1) != 0;
    c.csv = num(L"Monitor", L"Csv", 0) != 0;
    {
        wchar_t b[32];
        GetPrivateProfileStringW(L"Monitor", L"CriticalAction", L"notify", b, 32, ini.c_str());
        c.criticalAction = _wcsicmp(b, L"shutdown") == 0 ? 1 : 0;
    }
    c.criticalSeconds = std::clamp(int(num(L"Monitor", L"CriticalSeconds", 5)), 1, 120);
    c.gpuIndex = int(num(L"Sensor", L"GpuIndex", -1));
    c.i2cPort = std::clamp(int(num(L"Sensor", L"I2CPort", c.i2cPort)), 0, 15);
    c.i2cAddr = std::clamp(int(num(L"Sensor", L"I2CAddr", c.i2cAddr)), 0x03, 0x77);
    c.force = num(L"Sensor", L"Force", 0) != 0;
    c.lhmPort = std::clamp(int(num(L"Sensor", L"LhmPort", 8085)), 1, 65535);
    {
        wchar_t b[32];
        GetPrivateProfileStringW(L"Sensor", L"PinSource", L"auto", b, 32, ini.c_str());
        std::string v;
        for (wchar_t* p = b; *p; ++p) v += char(towlower(*p));
        if (v == "auto" || v == "astral" || v == "wireview" || v == "pmd2x" || v == "msipsu" || v == "off") c.pinSource = v;
    }
    return c;
}

void Start(const Config& cfg, void* hwnd, unsigned msg) {
    if (g_thread.joinable()) return;
    g_notify = static_cast<HWND>(hwnd);
    g_msg = msg;
    g_stop = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_snap = Snapshot{};
        g_snap.cfg = cfg;
        g_hist.clear();
    }
    g_thread = std::thread(ThreadMain, cfg);
}

void Stop() {
    if (!g_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_stop = true;
    }
    g_cv.notify_all();
    g_thread.join();
}

void Restart(const Config& cfg) {
    HWND h = g_notify;
    UINT m = g_msg;
    Stop();
    AddEvent(0, L"Sensor restarted, settings reloaded");
    Start(cfg, h, m);
}

void ResetPeaks() { g_resetPeaks = true; }
void SetCsv(bool on) { g_csvRequest = on ? 1 : 0; }

void Get(Snapshot& out) {
    std::lock_guard<std::mutex> lk(g_mx);
    out = g_snap;
}

void GetHistory(std::vector<HistPoint>& out) {
    std::lock_guard<std::mutex> lk(g_mx);
    out.assign(g_hist.begin(), g_hist.end());
}

void GetEvents(std::vector<Event>& out) {
    std::lock_guard<std::mutex> lk(g_mx);
    out.assign(g_events.begin(), g_events.end());
}

} // namespace mon
