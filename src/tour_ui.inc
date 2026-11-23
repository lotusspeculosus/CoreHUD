// Guided tour (offered on first start, and any time via the "Tour" button or Settings) + the alarm banner
// in the main window. Included inside app.cpp's anonymous namespace (after tablet_ui.inc).

// ---------- alarm banner in the main window ----------
void DrawAlarmBanner(float W) {
    int a = AlarmActive();
    if (!a) return;
    bool on = (GetTickCount64() / 500) % 2 == 0;
    D2D1_RECT_F r = R(0, 48, W, 40);
    Fill(r, kCrit, 0, on ? 0.95f : 0.75f);
    g_hits.push_back({r, 0, [] {}});   // clicks on the banner don't reach the buttons underneath
    std::wstring t;
    if (a == 2) {
        int worst = 0;
        for (int i = 1; i < g_snap.pinCount && i < pins::kMaxPins; ++i) if (g_snap.amps[i] > g_snap.amps[worst]) worst = i;
        t = F(L"⚠  12V-2x6 OVER LIMIT — pin %d at %.2f A (limit %.1f A). Lower the GPU load now, then check the connector.", worst + 1,
              g_snap.amps[worst], g_snap.cfg.limitA);
    } else {
        t = L"TEST ALARM — open panels and the tablet show the red alarm too (the tablet also beeps). It stops by itself.";
    }
    Text(t, D2D1::RectF(16, 48, W - 16, 88), Fmt(FitSize(t, 2, 15, W - 32), 2, 1), 0xFFFFFF);
}

// ---------- tour ----------
int g_tour = -1;          // current step, -1 = not running
bool g_tourOffer = false; // the welcome card ("Take a quick tour?")
bool g_tourFirstRun = false;

struct TourStep { int tab; const char* anchor; const wchar_t* title; const wchar_t* body; };
const TourStep kTour[] = {
    {TAB_DASH, "kpis", L"Your power connector at a glance",
     L"These three cards show what goes through your graphics card's 12V-2x6 power cable right now: the power (watts), the current (amps) "
     L"and the voltage."},
    {TAB_DASH, "pins", L"Every pin, live",
     L"The cable has 6 power pins. Each bar is one pin. Green is fine, amber is a warning, red means over the limit. If a pin goes red, "
     L"RigSense beeps, shows a notification and puts a big red alarm on every screen (panels and tablet too). "
     L"No sensor on your card? A WireView Pro II, PMD2X or MSI Ai1300TS/Ai1600TS power supply adds one."},
    {TAB_DASH, "gpu", L"GPU, CPU and more",
     L"Temperatures, clocks, usage and power of your graphics card and PC. Works with NVIDIA, AMD and Intel."},
    {TAB_CHART, "chart", L"Performance chart",
     L"See the last 1, 5 or 30 minutes. Handy after a gaming session: what did the pins and the card do while you were playing?"},
    {TAB_PANEL, "layouts", L"Your own sensor panels",
     L"Design screens with gauges, graphs and values. Keep as many layouts as you like: one for a small screen in your case, one for a tablet, "
     L"one for a second monitor. Click a layout to edit it; “+ New” makes another one."},
    {TAB_PANEL, "preview", L"Edit by dragging",
     L"Click a widget to select it, drag it to move it, and drag its corner to make it bigger or smaller. Delete removes it, the arrow keys nudge it."},
    {TAB_PANEL, "sensorcol", L"Pick what to show",
     L"Every sensor RigSense can read is in this list. Click one to add it, or to change the selected widget. Type in the box to search (for example “fps”)."},
    {TAB_PANEL, "panelcol", L"Show it anywhere",
     L"“Show on” picks where this layout appears: a floating window, or fullscreen on a second screen. Several layouts can be open at the same "
     L"time. “Tablet / phone…” turns an old tablet or phone into a wireless panel."},
    {TAB_EVENTS, "events", L"Event log",
     L"Every warning and alarm, with the time. When something alarming happens, the black box also saves what each pin did in the minute before and after it "
     L"(in the logs folder)."},
    {TAB_DASH, "settings", L"Settings",
     L"Change the limits, the alarms, the black box and the connector wear check (it learns how your pins share the load and warns if one slowly takes more), "
     L"and the automatic updates. Tip: Settings › Test alarm shows what an alarm looks like."},
    {TAB_DASH, "tour", L"That's it!",
     L"RigSense keeps watching in the background, also from the tray icon next to the clock. You can take this tour again any time with this button."},
};
constexpr int kTourSteps = int(sizeof kTour / sizeof kTour[0]);

void MarkTourSeen() { WritePrivateProfileStringW(L"Tour", L"Seen", L"1", g_ini.c_str()); }

void InitTour(bool firstRun) {
    g_tourFirstRun = firstRun;
    g_tourOffer = GetPrivateProfileIntW(L"Tour", L"Seen", 0, g_ini.c_str()) == 0;
}

void GoTourStep(int i) {
    if (i < 0 || i >= kTourSteps) { g_tour = -1; return; }
    g_tour = i;
    g_tab = kTour[i].tab;
}

void StartTour() {
    g_tourOffer = false;
    MarkTourSeen();
    CommitFocus();
    GoTourStep(0);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void EndTour() {
    g_tour = -1;
    g_tourOffer = false;
    MarkTourSeen();
}

// Wrapping text format (the normal ones are single-line). Stored in g_fmts so it's released at exit.
IDWriteTextFormat* WrapFmt(float size, int weight = 0) {
    int key = 30000000 + int(size * 2) * 10 + weight;
    auto it = g_fmts.find(key);
    if (it != g_fmts.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(L"Segoe UI", nullptr, weight ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (!f) return Fmt(size, weight);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g_fmts[key] = f;
    return f;
}
float WrappedHeight(const std::wstring& s, IDWriteTextFormat* f, float width) {
    IDWriteTextLayout* l = nullptr;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), UINT32(s.size()), f, width, 2000, &l))) return 40;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    l->Release();
    return m.height;
}

void DrawTourCard(float W, float H, const D2D1_RECT_F* A, const std::wstring& stepLabel, const std::wstring& title, const std::wstring& body,
                  bool isOffer) {
    float cw = std::min(440.f, W - 40);
    float tw = cw - 40;
    IDWriteTextFormat* bf = WrapFmt(13.5f);
    float bh = WrappedHeight(body, bf, tw);
    float ch = 20 + (stepLabel.empty() ? 0 : 20) + 30 + bh + 18 + 34 + 18;
    float x, y;
    if (!A) { x = (W - cw) / 2; y = (H - ch) / 2; }
    else {
        float acx = (A->left + A->right) / 2;
        x = std::clamp(acx - cw / 2, 20.f, W - cw - 20);
        if (A->bottom + 14 + ch < H - 10) y = A->bottom + 14;
        else if (A->top - 14 - ch > 52) y = A->top - 14 - ch;
        else {   // big area: put the card inside it, near the top-left
            x = std::clamp(A->left + 24, 20.f, W - cw - 20);
            y = std::clamp(A->top + 24, 52.f, H - ch - 10);
        }
    }
    D2D1_RECT_F card = R(x, y, cw, ch);
    Fill(card, kSurface, 12);
    Stroke(card, kAccent, 12, 1.5f, 0.9f);
    float cy = y + 18;
    if (!stepLabel.empty()) {
        Text(stepLabel, R(x + 20, cy, tw, 18), Fmt(11.5f, 1), kAccent);
        cy += 20;
    }
    Text(title, R(x + 20, cy, tw, 28), Fmt(FitSize(title, 2, 17, tw), 2), kInk);
    cy += 30;
    g_br->SetColor(Col(kInk2));
    g_rt->DrawTextW(body.c_str(), UINT32(body.size()), bf, R(x + 20, cy, tw, bh + 4), g_br);
    float by = y + ch - 18 - 34;
    if (isOffer) {
        Btn(R(x + cw - 20 - 150, by, 150, 34), L"Show me around", true, [] { StartTour(); }, 13);
        Btn(R(x + cw - 20 - 150 - 8 - 110, by, 110, 34), L"Not now", false, [] { EndTour(); }, 13);
    } else {
        bool last = g_tour == kTourSteps - 1;
        Btn(R(x + cw - 20 - 110, by, 110, 34), last ? L"Finish" : L"Next  ›", true, [last] { if (last) EndTour(); else GoTourStep(g_tour + 1); }, 13);
        if (g_tour > 0) Btn(R(x + cw - 20 - 110 - 8 - 90, by, 90, 34), L"‹  Back", false, [] { GoTourStep(g_tour - 1); }, 13);
        if (!last) Btn(R(x + 20, by, 100, 34), L"Skip tour", false, [] { EndTour(); }, 12.5f);
    }
}

void DrawTour(float W, float H) {
    if (g_tour < 0 && !g_tourOffer) return;
    if (g_tour >= 0 && g_tab != kTour[g_tour].tab) g_tab = kTour[g_tour].tab;   // the tab is painted next frame
    g_hits.clear();   // modal: only the tour's buttons work
    if (g_tour < 0) {
        Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0, 0.62f);
        std::wstring body = g_tourFirstRun
            ? L"RigSense watches your graphics card's power connector pin by pin, shows your PC's sensors and lets you build your own sensor panels. "
              L"Want a quick tour? It takes about a minute."
            : L"New in this version: several sensor panel layouts at once, a red alarm on every screen, the connector black box and the wear check. "
              L"Want a quick tour of the whole app? It takes about a minute.";
        DrawTourCard(W, H, nullptr, L"", g_tourFirstRun ? L"Welcome to RigSense" : L"RigSense " RIGSENSE_VERSION, body, true);
        return;
    }
    const TourStep& st = kTour[g_tour];
    auto it = g_anchors.find(st.anchor);
    const D2D1_RECT_F* A = it != g_anchors.end() ? &it->second : nullptr;
    if (A) {
        D2D1_RECT_F a = D2D1::RectF(A->left - 6, A->top - 6, A->right + 6, A->bottom + 6);
        Fill(D2D1::RectF(0, 0, W, a.top), 0x000000, 0, 0.62f);
        Fill(D2D1::RectF(0, a.bottom, W, H), 0x000000, 0, 0.62f);
        Fill(D2D1::RectF(0, a.top, a.left, a.bottom), 0x000000, 0, 0.62f);
        Fill(D2D1::RectF(a.right, a.top, W, a.bottom), 0x000000, 0, 0.62f);
        Stroke(a, kAccent, 10, 2.f);
    } else {
        Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0, 0.62f);
    }
    DrawTourCard(W, H, A, F(L"STEP %d OF %d", g_tour + 1, kTourSteps), st.title, st.body, false);
}

// Keyboard while the tour is showing: arrows / Enter step, Esc ends. Returns true if handled.
bool TourKey(WPARAM wp) {
    if (g_tour < 0 && !g_tourOffer) return false;
    if (wp == VK_ESCAPE) EndTour();
    else if (g_tour < 0) { if (wp == VK_RETURN) StartTour(); }
    else if (wp == VK_RIGHT || wp == VK_RETURN || wp == VK_SPACE) { if (g_tour == kTourSteps - 1) EndTour(); else GoTourStep(g_tour + 1); }
    else if (wp == VK_LEFT) GoTourStep(g_tour - 1 < 0 ? 0 : g_tour - 1);
    return true;
}
bool TourActive() { return g_tour >= 0 || g_tourOffer; }
