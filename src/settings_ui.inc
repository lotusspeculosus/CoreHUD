// Settings window (replaces editing RigSense.ini in Notepad) + auto-update state machine.
// Included inside app.cpp's anonymous namespace (after panel_ui.inc / setup_ui.inc).

constexpr const char* kRepo = "lotusspeculosus/RigSense";
constexpr UINT WM_APP_UPDATE = WM_APP + 4;
constexpr UINT_PTR TIMER_UPDATE = 7;

// ---------------- updates ----------------
struct UpdResult { bool ok = false; std::string err; upd::Release rel; bool newer = false; bool staged = false; bool install = false; };
struct UpdState {
    int status = 0;            // 0 idle, 1 checking, 2 up to date, 3 available, 4 downloading, 5 ready, 6 error
    upd::Release rel;
    std::wstring msg;
    std::wstring checkedAt;
    std::string notifiedVersion;
    bool applyPending = false;
};
UpdState g_upd;
std::atomic<bool> g_updBusy{false};

int UpdateMode() {
    wchar_t b[16] = {};
    GetPrivateProfileStringW(L"Updates", L"Mode", L"auto", b, 16, g_ini.c_str());
    if (!_wcsicmp(b, L"off")) return upd::MODE_OFF;
    if (!_wcsicmp(b, L"notify")) return upd::MODE_NOTIFY;
    return upd::MODE_AUTO;
}

std::wstring NowHM() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return F(L"%02u:%02u", st.wHour, st.wMinute);
}

// Runs on a worker thread; posts the result to the UI thread.
void StartUpdateCheck(bool install) {
    if (g_updBusy.exchange(true)) return;
    g_upd.status = install ? 4 : 1;
    int mode = UpdateMode();
    HWND hwnd = g_hwnd;
    std::thread([hwnd, install, mode] {
        auto* res = new UpdResult;
        res->install = install;
        res->ok = upd::FetchLatest(kRepo, res->rel, res->err);
        if (res->ok) {
            res->newer = upd::CompareVersions(res->rel.version, RIGSENSE_VERSION_STR) > 0;
            if (res->newer && (install || mode == upd::MODE_AUTO)) {
                if (upd::IsStaged(res->rel.version)) res->staged = true;
                else {
                    res->staged = upd::Stage(res->rel, res->err);
                    if (!res->staged) res->ok = false;
                }
            }
        }
        if (!PostMessageW(hwnd, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(res))) delete res;
    }).detach();
}

void TrayNotify(int level, const std::wstring& text);   // app.cpp
void StartTour();                                          // tour_ui.inc

bool PinCritical() {
    for (int i = 0; i < pins::kMaxPins; ++i) if (g_snap.pinState[i] == 2) return true;
    return false;
}

void TryApplyUpdate() {
    if (!g_upd.applyPending) return;
    if (PinCritical()) { g_upd.msg = L"Update ready - waiting until the connector alert clears"; return; }   // never restart mid-alarm
    g_upd.applyPending = false;
    std::string err;
    mon::LogEvent(0, L"Installing RigSense " + Wide(g_upd.rel.version) + L" and restarting");
    if (upd::ApplyStaged(err)) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);   // the new version is starting
    } else {
        g_upd.status = 6;
        g_upd.msg = L"Update failed: " + Wide(err);
        TrayNotify(1, g_upd.msg);
    }
}

void OnUpdateResult(UpdResult* res) {
    g_updBusy = false;
    g_upd.checkedAt = NowHM();
    if (!res->ok) {
        g_upd.status = 6;
        g_upd.msg = Wide(res->err);
    } else if (!res->newer) {
        g_upd.status = 2;
        g_upd.rel = res->rel;
        g_upd.msg = L"You have the latest version";
    } else {
        g_upd.rel = res->rel;
        if (res->staged) {
            g_upd.status = 5;
            g_upd.msg = L"Version " + Wide(res->rel.version) + L" downloaded and verified";
            g_upd.applyPending = true;
            TryApplyUpdate();
        } else {
            g_upd.status = 3;
            g_upd.msg = L"Version " + Wide(res->rel.version) + L" is available";
            if (g_upd.notifiedVersion != res->rel.version) {
                g_upd.notifiedVersion = res->rel.version;
                TrayNotify(1, L"RigSense " + Wide(res->rel.version) + L" is available - open Settings to install it.");
            }
        }
    }
    delete res;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------- diagnostics (for bug reports) ----------------
bool CopyText(const std::wstring& w) {
    if (!OpenClipboard(g_hwnd)) return false;
    bool ok = false;
    EmptyClipboard();
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (void* p = hg ? GlobalLock(hg) : nullptr) {
        memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(hg);
        if (SetClipboardData(CF_UNICODETEXT, hg)) { hg = nullptr; ok = true; }   // the clipboard owns it now
    }
    if (hg) GlobalFree(hg);
    CloseClipboard();
    return ok;
}

std::wstring BuildDiagnostics() {
    std::wstring t = L"RigSense " RIGSENSE_VERSION L" (build " RIGSENSE_BUILD L")\r\n";
    using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW ov{sizeof ov};
    if (auto f = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) f(&ov);
    t += F(L"Windows %lu.%lu build %lu\r\n", ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
    t += L"GPU: " + (g_reg.gpuName.empty() ? std::wstring(L"none found") : g_reg.gpuName) +
         (g_reg.gpuViaWindows ? (g_reg.gpuFromLhm ? L" (Windows counters + LibreHardwareMonitor)" : L" (Windows counters)") : L" (NVIDIA NVML)") + L"\r\n";
    if (g_snap.haveGpu)
        t += F(L"NVIDIA PCI %04X:%04X, subsystem %04X:%04X, known per-pin card: %s\r\n", g_snap.deviceId & 0xFFFF, g_snap.deviceId >> 16,
               g_snap.subSystemId & 0xFFFF, g_snap.subSystemId >> 16, g_snap.knownCard ? L"yes" : L"no");
    t += L"Per-pin source: " + (g_snap.sensorEnabled ? g_snap.pinSource : std::wstring(L"none")) +
         (g_snap.sensorEnabled ? (g_snap.pinsOk ? std::wstring(L" - OK") : L" - ERROR: " + Wide(g_snap.pinError)) : std::wstring()) + L"\r\n";
    if (g_snap.pinsOk) {
        t += L"Pins:";
        for (int i = 0; i < g_snap.pinCount; ++i) t += F(L"  P%d %.2fA %.3fV", i + 1, g_snap.amps[i], g_snap.volts[i]);
        t += L"\r\n";
    }
    t += F(L"Limits: warn %.1f A, limit %.1f A, spread %.1f A, critical action %s\r\n", g_snap.cfg.warnA, g_snap.cfg.limitA,
           g_snap.cfg.spreadWarnA, g_snap.cfg.criticalAction ? L"shutdown" : L"notify");
    t += L"LibreHardwareMonitor: " + std::wstring(g_reg.lhmOk ? F(L"%d sensors via %s", g_reg.lhmCount, g_reg.lhmVia.c_str()) :
                                                   g_reg.lhmProcess ? L"running, no data (web server off?)" : L"not running") + L"\r\n";
    t += L"HWiNFO Gadget: " + std::wstring(g_reg.gadgetOk ? F(L"%d values", g_reg.gadgetCount) : L"not available") + L"\r\n";
    t += L"RTSS: " + std::wstring(g_reg.rtssOk ? (g_reg.rtssApp.empty() ? L"running" : (L"running, " + g_reg.rtssApp).c_str()) : L"not running") + L"\r\n";
    for (auto& sl : g_slots) {
        const auto& L = LayoutOf(*sl);
        t += F(L"Layout %d: %dx%d, grid %dx%d, %zu widgets, show on %s", sl->id, L.width, L.height, L.cols, L.rows, L.widgets.size(),
               L.display.c_str()) + std::wstring(sl->wnd ? (sl->virt ? L", open (window)" : L", open (fullscreen)") : L"") + L"\r\n";
    }
    if (g_snap.wearOn) t += L"Wear check: " + g_snap.wear.text + L"\r\n";
    t += L"\r\nSensors (group | name | value | key):\r\n";
    for (auto& s : g_reg.All()) {
        t += s.group + L" | " + s.name + L" | " + (s.ok ? F(L"%.*f", s.decimals, s.value) + L" " + s.unit : std::wstring(L"offline")) + L" | " +
             Wide(s.key) + L"\r\n";
    }
    return t;
}

// ---------------- settings window ----------------
bool g_settingsOpen = false;
struct SettingsDraft {
    std::wstring warnA, limitA, spreadA, critSec, pollMs, lhmPort, gpuIndex, i2cPort, i2cAddr;
    int critAction = 0, updMode = upd::MODE_AUTO;
    bool beep = true, csv = false, force = false, showComponents = true;
    bool blackBox = true, wear = true, alarmPanels = true, alarmSound = true;
    std::string pinSource = "auto";
};
SettingsDraft g_draft;
enum SettingsField { SF_WARN = 60, SF_LIMIT, SF_SPREAD, SF_CRITSEC, SF_POLL, SF_LHMPORT, SF_GPUIDX, SF_I2CPORT, SF_I2CADDR };

void OpenSettings() {
    mon::Config c = mon::LoadConfig(g_ini);
    g_draft = SettingsDraft{};
    g_draft.warnA = F(L"%g", c.warnA);
    g_draft.limitA = F(L"%g", c.limitA);
    g_draft.spreadA = F(L"%g", c.spreadWarnA);
    g_draft.critSec = std::to_wstring(c.criticalSeconds);
    g_draft.pollMs = std::to_wstring(c.pollMs);
    g_draft.lhmPort = std::to_wstring(c.lhmPort);
    g_draft.gpuIndex = std::to_wstring(c.gpuIndex);
    g_draft.i2cPort = std::to_wstring(c.i2cPort);
    g_draft.i2cAddr = F(L"0x%02X", c.i2cAddr);
    g_draft.critAction = c.criticalAction;
    g_draft.beep = c.beep;
    g_draft.csv = c.csv;
    g_draft.force = c.force;
    g_draft.pinSource = c.pinSource;
    g_draft.blackBox = c.blackBox;
    g_draft.wear = c.wearCheck;
    g_draft.alarmPanels = g_alarmPanels;
    g_draft.alarmSound = g_alarmTabletSound;
    g_draft.showComponents = SetupShowAtStart();
    g_draft.updMode = UpdateMode();
    g_settingsOpen = true;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// A field still being typed in: take its text directly (CommitFocus would only apply it on the next paint).
void ApplyFocusedField() {
    // A click on Save first commits the focused field (g_commitId/g_commitText), which TextField would only
    // apply on the next paint - take it from there, or from the still-focused edit buffer.
    int id = g_focus ? g_focus : g_commitId;
    const std::wstring& text = g_focus ? g_editBuf : g_commitText;
    std::wstring* f = nullptr;
    switch (id) {
    case SF_WARN: f = &g_draft.warnA; break;
    case SF_LIMIT: f = &g_draft.limitA; break;
    case SF_SPREAD: f = &g_draft.spreadA; break;
    case SF_CRITSEC: f = &g_draft.critSec; break;
    case SF_POLL: f = &g_draft.pollMs; break;
    case SF_LHMPORT: f = &g_draft.lhmPort; break;
    case SF_GPUIDX: f = &g_draft.gpuIndex; break;
    case SF_I2CPORT: f = &g_draft.i2cPort; break;
    case SF_I2CADDR: f = &g_draft.i2cAddr; break;
    default: break;
    }
    if (f) { *f = text; g_focus = 0; g_commitId = 0; }
    else CommitFocus();
}

void SaveSettings() {
    ApplyFocusedField();
    auto put = [](const wchar_t* sec, const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(sec, key, v.c_str(), g_ini.c_str()); };
    put(L"Monitor", L"WarnA", g_draft.warnA);
    put(L"Monitor", L"LimitA", g_draft.limitA);
    put(L"Monitor", L"SpreadWarnA", g_draft.spreadA);
    put(L"Monitor", L"CriticalSeconds", g_draft.critSec);
    put(L"Monitor", L"CriticalAction", g_draft.critAction == 1 ? L"shutdown" : L"notify");
    put(L"Monitor", L"Beep", g_draft.beep ? L"1" : L"0");
    put(L"Monitor", L"Csv", g_draft.csv ? L"1" : L"0");
    put(L"Monitor", L"PollMs", g_draft.pollMs);
    put(L"Monitor", L"BlackBox", g_draft.blackBox ? L"1" : L"0");
    put(L"Monitor", L"WearCheck", g_draft.wear ? L"1" : L"0");
    put(L"Alarm", L"Panels", g_draft.alarmPanels ? L"1" : L"0");
    put(L"Alarm", L"TabletSound", g_draft.alarmSound ? L"1" : L"0");
    LoadAlarmCfg();
    put(L"Sensor", L"PinSource", Wide(g_draft.pinSource));
    put(L"Sensor", L"LhmPort", g_draft.lhmPort);
    put(L"Sensor", L"GpuIndex", g_draft.gpuIndex);
    put(L"Sensor", L"I2CPort", g_draft.i2cPort);
    put(L"Sensor", L"I2CAddr", g_draft.i2cAddr);
    put(L"Sensor", L"Force", g_draft.force ? L"1" : L"0");
    put(L"Updates", L"Mode", g_draft.updMode == upd::MODE_OFF ? L"off" : g_draft.updMode == upd::MODE_NOTIFY ? L"notify" : L"auto");
    SetSetupShowAtStart(g_draft.showComponents);
    mon::Restart(mon::LoadConfig(g_ini));   // LoadConfig clamps out-of-range values
    g_settingsOpen = false;
}

void DrawSettingsOverlay(float W, float H) {
    g_hits.clear();
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0, 0.62f);
    float cw = std::min(1060.f, W - 40), ch = std::min(H - 40, 700.f);
    float x0 = (W - cw) / 2, y0 = (H - ch) / 2;
    D2D1_RECT_F card = R(x0, y0, cw, ch);
    Fill(card, kSurface, 12);
    Stroke(card, kBorder, 12);
    Text(L"Settings", R(x0 + 24, y0 + 16, 300, 30), Fmt(19, 1), kInk);
    Text(L"RigSense " RIGSENSE_VERSION L"  ·  build " RIGSENSE_BUILD, R(x0 + 24, y0 + 16, cw - 48, 30), Fmt(12, 0, 2), kMuted);

    const float rowH = ch < 690 ? 31.f : 34.f, ctl = rowH - 6, lab = 210;
    float colW = (cw - 72) / 2;
    std::wstring tmp;
    auto section = [&](float x, float& y, const wchar_t* t) {
        Text(t, R(x, y, colW, 24), Fmt(13.5f, 1), kAccent);
        y += 28;
    };
    auto label = [&](float x, float y, const wchar_t* t) { Text(t, R(x, y, lab, ctl), Fmt(12.5f), kInk2); return x + lab; };
    auto numField = [&](float x, float& y, const wchar_t* t, int id, std::wstring& val, const wchar_t* unit) {
        float cx = label(x, y, t);
        if (TextField(R(cx, y, 90, ctl), id, val, L"", tmp)) val = tmp;
        if (unit) Text(unit, R(cx + 98, y, 120, ctl), Fmt(12), kMuted);
        y += rowH;
    };
    auto toggle = [&](float x, float& y, const wchar_t* t, bool& v) {
        float cx = label(x, y, t);
        Btn(R(cx, y, 64, ctl), L"On", v, [&v] { v = true; });
        Btn(R(cx + 70, y, 64, ctl), L"Off", !v, [&v] { v = false; });
        y += rowH;
    };

    // ----- left column -----
    float xl = x0 + 24, yl = y0 + 62;
    section(xl, yl, L"12V-2x6 connector protection");
    numField(xl, yl, L"Warning level per pin", SF_WARN, g_draft.warnA, L"A");
    numField(xl, yl, L"Limit (critical) per pin", SF_LIMIT, g_draft.limitA, L"A");
    {
        double lim = _wtof(g_draft.limitA.c_str());
        if (lim > 9.5) Text(L"⚠ Above the 9.5 A terminal rating of 12V-2x6 - not recommended", R(xl, yl - 6, colW, 18), Fmt(11.5f), kWarn);
        if (lim > 9.5) yl += 14;
    }
    numField(xl, yl, L"Imbalance warning (spread)", SF_SPREAD, g_draft.spreadA, L"A");
    {
        float cx = label(xl, yl, L"When a pin stays over the limit");
        Btn(R(cx, yl, 110, ctl), L"Notify", g_draft.critAction == 0, [] { g_draft.critAction = 0; });
        Btn(R(cx + 116, yl, 150, ctl), L"Shut down PC", g_draft.critAction == 1, [] { g_draft.critAction = 1; });
        yl += rowH;
    }
    numField(xl, yl, L"Seconds over limit first", SF_CRITSEC, g_draft.critSec, g_draft.critAction == 1 ? L"s (60 s countdown)" : L"s");
    toggle(xl, yl, L"Beep on critical", g_draft.beep);
    yl += 8;
    section(xl, yl, L"Alarms & connector history");
    auto toggleBtn = [&](const wchar_t* t, bool& v, const wchar_t* extra, std::function<void()> fn, float extraW) {
        float cx = label(xl, yl, t);
        Btn(R(cx, yl, 64, ctl), L"On", v, [&v] { v = true; });
        Btn(R(cx + 70, yl, 64, ctl), L"Off", !v, [&v] { v = false; });
        if (extra) Btn(R(cx + 146, yl, extraW, ctl), extra, false, std::move(fn));
        yl += rowH;
    };
    toggleBtn(L"Red alarm on panels + tablet", g_draft.alarmPanels, GetTickCount64() < g_alarmTestUntil ? L"Testing…" : L"Test alarm",
              [] { g_alarmTestUntil = GetTickCount64() + 6000; }, 104);
    toggle(xl, yl, L"Alarm sound on the tablet", g_draft.alarmSound);
    toggleBtn(L"Black box recordings", g_draft.blackBox, L"Open folder", [] {
        std::wstring dir = mon::BlackBoxDir();
        CreateDirectoryW((mon::ExeDir() + L"\\logs").c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        ShellExecuteW(g_hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }, 104);
    toggleBtn(L"Connector wear check", g_draft.wear, L"New cable…", [] {
        if (MessageBoxW(g_hwnd, L"Start learning the pin balance again?\n\nDo this after you replaced the 12V-2x6 cable or reseated the connector, "
                                L"so the wear check compares against the new situation. (The old history is kept as a .old file.)",
                        L"RigSense", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            mon::ResetWear();
    }, 104);
    yl += 8;
    section(xl, yl, L"Monitoring");
    numField(xl, yl, L"Sample interval", SF_POLL, g_draft.pollMs, L"ms (100-10000)");
    toggle(xl, yl, L"CSV log at start", g_draft.csv);
    toggle(xl, yl, L"Components check at start", g_draft.showComponents);

    // ----- right column -----
    float xr = x0 + 48 + colW, yr = y0 + 62;
    section(xr, yr, L"Per-pin source");
    {
        const char* ids[] = {"auto", "astral", "wireview", "pmd2x", "msipsu", "off"};
        const wchar_t* names[] = {L"Auto", L"On-card", L"WireView", L"PMD2X", L"MSI PSU", L"Off"};
        float bw = (colW - 5 * 6) / 6;
        for (int i = 0; i < 6; ++i) {
            std::string id = ids[i];
            Btn(R(xr + i * (bw + 6), yr, bw, ctl), names[i], g_draft.pinSource == id, [id] { g_draft.pinSource = id; }, 11.5f);
        }
        yr += rowH;
        Text(L"Auto: the card's own sensor first, otherwise the first USB meter found.", R(xr, yr - 4, colW, 18), Fmt(11.5f), kMuted);
        yr += 22;
    }
    numField(xr, yr, L"LibreHardwareMonitor port", SF_LHMPORT, g_draft.lhmPort, L"(web server)");
    yr += 8;
    section(xr, yr, L"On-card sensor (advanced)");
    numField(xr, yr, L"GPU index (-1 = auto)", SF_GPUIDX, g_draft.gpuIndex, nullptr);
    numField(xr, yr, L"I2C port", SF_I2CPORT, g_draft.i2cPort, nullptr);
    numField(xr, yr, L"I2C address", SF_I2CADDR, g_draft.i2cAddr, L"(default 0x2B)");
    toggle(xr, yr, L"Force on unknown cards", g_draft.force);
    yr += 8;
    section(xr, yr, L"Updates");
    {
        float cx = label(xr, yr, L"When a new version is out");
        Btn(R(cx, yr, 96, ctl), L"Automatic", g_draft.updMode == upd::MODE_AUTO, [] { g_draft.updMode = upd::MODE_AUTO; });
        Btn(R(cx + 102, yr, 96, ctl), L"Notify me", g_draft.updMode == upd::MODE_NOTIFY, [] { g_draft.updMode = upd::MODE_NOTIFY; });
        Btn(R(cx + 204, yr, 60, ctl), L"Off", g_draft.updMode == upd::MODE_OFF, [] { g_draft.updMode = upd::MODE_OFF; });
        yr += rowH;
        static const wchar_t* st[] = {L"Not checked yet", L"Checking…", L"", L"", L"Downloading…", L"", L""};
        std::wstring s = g_upd.status == 2 || g_upd.status == 3 || g_upd.status == 5 || g_upd.status == 6 ? g_upd.msg : st[g_upd.status];
        if (!g_upd.checkedAt.empty() && g_upd.status != 1 && g_upd.status != 4) s += L"  ·  checked " + g_upd.checkedAt;
        uint32_t sc = g_upd.status == 6 ? kWarn : g_upd.status == 3 || g_upd.status == 5 ? kAccent : kInk2;
        Text(s, R(xr, yr, colW, 20), Fmt(12), sc);
        yr += 26;
        Btn(R(xr, yr, 120, ctl), L"Check now", false, [] { StartUpdateCheck(false); });
        if (g_upd.status == 3)
            Btn(R(xr + 128, yr, 170, ctl), L"Install " + Wide(g_upd.rel.version), true, [] { StartUpdateCheck(true); });
        if (!g_upd.rel.pageUrl.empty()) {
            std::string url = g_upd.rel.pageUrl;
            Btn(R(xr + colW - 130, yr, 130, ctl), L"Release notes", false, [url] { ShellExecuteW(g_hwnd, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
        }
        yr += rowH;
        Text(L"Only from this project's GitHub releases · SHA-256 verified before installing.",
             R(xr, yr - 4, colW, 18), Fmt(11.5f), kMuted);
        yr += 22;
    }
    section(xr, yr, L"Help & about");
    {
        float bw = (colW - 18) / 4;
        Btn(R(xr, yr, bw, ctl), L"Take the tour", false, [] { g_settingsOpen = false; StartTour(); });
        Btn(R(xr + (bw + 6), yr, bw, ctl), L"Copy diagnostics", false, [] {
            if (CopyText(BuildDiagnostics()))
                MessageBoxW(g_hwnd, L"Diagnostic info copied to the clipboard.\n\nPaste it into your GitHub issue (Ctrl+V). It contains your hardware and "
                                    L"sensor readings, nothing personal.", L"RigSense", MB_ICONINFORMATION);
            else
                MessageBoxW(g_hwnd, L"Couldn't open the clipboard (another program is using it). Please try again.", L"RigSense", MB_ICONWARNING);
        }, 12.f);
        Btn(R(xr + 2 * (bw + 6), yr, bw, ctl), L"GitHub page", false,
            [] { ShellExecuteW(g_hwnd, L"open", L"https://github.com/lotusspeculosus/RigSense", nullptr, nullptr, SW_SHOWNORMAL); });
        Btn(R(xr + 3 * (bw + 6), yr, bw, ctl), L"RigSense.ini", false,
            [] { ShellExecuteW(g_hwnd, L"open", L"notepad.exe", (L"\"" + g_ini + L"\"").c_str(), nullptr, SW_SHOWNORMAL); });
    }

    // ----- footer -----
    float fy = y0 + ch - 54;
    Btn(R(x0 + cw - 24 - 110, fy, 110, 32), L"Save", true, [] { SaveSettings(); });
    Btn(R(x0 + cw - 24 - 110 - 8 - 110, fy, 110, 32), L"Cancel", false, [] { g_focus = 0; g_commitId = 0; g_settingsOpen = false; });
    Text(L"Saving applies the settings immediately (the sensor restarts).", R(x0 + 24, fy, cw - 300, 32), Fmt(11.5f), kMuted);
}
