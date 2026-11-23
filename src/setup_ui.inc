// Component check overlay ("Components" button / shown at start-up when something is missing).
// Included inside app.cpp's anonymous namespace.

bool g_setupOpen = false;
std::vector<comp::Component> g_comps;
DWORD g_compsTick = 0;
bool g_winget = false;

bool SetupShowAtStart() { return GetPrivateProfileIntW(L"UI", L"ShowComponentCheck", 1, g_ini.c_str()) != 0; }
void SetSetupShowAtStart(bool on) { WritePrivateProfileStringW(L"UI", L"ShowComponentCheck", on ? L"1" : L"0", g_ini.c_str()); }

void RecheckComponents() {
    g_comps = comp::Check();
    g_winget = comp::WingetAvailable();
    g_compsTick = GetTickCount();
}

void OpenSetup() {
    RecheckComponents();
    g_setupOpen = true;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Called once at start-up: pop up if the driver is missing, or anything is missing and the user hasn't opted out.
void MaybeShowSetupAtStart() {
    RecheckComponents();
    bool driverMissing = false, anyMissing = false;
    for (auto& c : g_comps) {
        if (c.required && !c.installed) driverMissing = true;
        if (!c.installed) anyMissing = true;
    }
    if (driverMissing || (anyMissing && SetupShowAtStart())) g_setupOpen = true;
}

void DrawSetupOverlay(float W, float H) {
    g_hits.clear();   // modal: nothing underneath is clickable
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0, 0.62f);
    float cw = std::min(900.f, W - 40), rowH = 104;
    float ch = std::min(H - 40, 86 + rowH * g_comps.size() + 70);
    float x0 = (W - cw) / 2, y0 = (H - ch) / 2;
    D2D1_RECT_F card = R(x0, y0, cw, ch);
    Fill(card, kSurface, 12);
    Stroke(card, kBorder, 12);
    Text(L"Components", R(x0 + 24, y0 + 16, cw - 48, 30), Fmt(19, 1), kInk);
    Text(L"RigSense " RIGSENSE_VERSION, R(x0 + 24, y0 + 16, cw - 48, 30), Fmt(12, 0, 2), kMuted);
    Text(L"RigSense works on its own; these add data. Install what you want — “Install latest” uses winget (built into Windows).",
         R(x0 + 24, y0 + 46, cw - 48, 20), Fmt(12.5f), kInk2);
    float y = y0 + 80;
    for (auto& c : g_comps) {
        D2D1_RECT_F row = R(x0 + 16, y, cw - 32, rowH - 10);
        Fill(row, kSurface2, 8);
        std::wstring st;
        uint32_t sc;
        if (c.installed && (c.running || !c.needsRunning)) { st = c.needsRunning ? L"Running" : L"Installed"; sc = kGood; }
        else if (c.installed) { st = L"Installed · not running"; sc = kWarn; }
        else if (c.required) { st = L"Missing"; sc = kCrit; }
        else { st = L"Not installed"; sc = kMuted; }
        float nx = row.left + 16;
        float nw = Measure(c.name, Fmt(15, 1));
        Text(c.name, R(nx, row.top + 10, nw + 4, 22), Fmt(15, 1), kInk);
        auto bf = Fmt(11.5f, 1);
        float bw = Measure(st, bf) + 28;
        D2D1_RECT_F badge = R(nx + nw + 12, row.top + 11, bw, 20);
        Fill(badge, sc, 10, 0.14f);
        Stroke(badge, sc, 10, 1.f, 0.7f);
        Circle(badge.left + 10, badge.top + 10, 3, sc);
        Text(st, R(badge.left + 18, badge.top, bw - 18, 20), bf, kInk);
        float textW = row.right - row.left - 16 - 360;
        Text(c.purpose, R(nx, row.top + 36, textW, 20), Fmt(12.5f), kInk2);
        Text(c.hint, R(nx, row.top + 58, row.right - row.left - 32, 20), Fmt(11.5f), kMuted);

        // buttons (right side)
        float bx = row.right - 14;
        auto btn = [&](const std::wstring& label, bool primary, std::function<void()> fn) {
            float w = Measure(label, Fmt(12.5f, 1, 1)) + 26;
            bx -= w;
            Btn(R(bx, row.top + 30, w, 28), label, primary, std::move(fn));
            bx -= 8;
        };
        comp::Component cc = c;
        btn(L"Download page", false, [cc] { comp::OpenUrl(cc.url); });
        if (!c.installed && g_winget && !c.wingetId.empty())
            btn(L"Install latest", true, [cc] {
                if (!comp::InstallWithWinget(cc)) MessageBoxW(g_hwnd, L"Could not start winget.", L"RigSense", MB_ICONERROR);
            });
        if (c.installed && c.needsRunning && !c.running && !c.exePath.empty())
            btn(L"Start", true, [cc] { comp::Launch(cc); g_compsTick = 0; });
        y += rowH;
    }
    // footer
    float fy = y0 + ch - 54;
    Text(g_winget ? L"winget found — “Install latest” opens a console window that shows the progress (it may ask for permission)."
                  : L"winget not found — use “Download page” (or install “App Installer” from the Microsoft Store).",
         R(x0 + 24, fy - 26, cw - 48, 18), Fmt(11.5f), kMuted);
    bool showAtStart = SetupShowAtStart();
    Btn(R(x0 + 24, fy, 200, 32), showAtStart ? L"Show at start-up: on" : L"Show at start-up: off", showAtStart,
        [showAtStart] { SetSetupShowAtStart(!showAtStart); });
    Btn(R(x0 + cw - 24 - 100, fy, 100, 32), L"Close", true, [] { g_setupOpen = false; });
    Btn(R(x0 + cw - 24 - 100 - 8 - 110, fy, 110, 32), L"Re-check", false, [] { RecheckComponents(); });
}
